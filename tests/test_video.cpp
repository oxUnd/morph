#include <gtest/gtest.h>

extern "C" {
#include "agent/tool.h"
#include "agent/tools/vid_gen.h"
#include "cJSON.h"
#include "models/llm.h"
#include "models/video_gen.h"
#include "models/video_provider.h"
#include "util/error.h"
#include "util/base64.h"
#include <unistd.h>
}

#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

static struct model make_video_model(const char *provider,
				     const char *adapter,
				     const char *model_id)
{
	struct model model{};

	std::snprintf(model.provider, sizeof(model.provider), "%s", provider);
	std::snprintf(model.adapter, sizeof(model.adapter), "%s", adapter);
	std::snprintf(model.model_id, sizeof(model.model_id), "%s", model_id);
	return model;
}

TEST(VideoAdapterTest, ResolvesExplicitAndInferredAdapter)
{
	struct model inferred = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct model explicit_adapter = make_video_model(
		"gateway", "volcengine-videos", "doubao-seedance-2-0-260128");
	struct model unsupported = make_video_model(
		"gateway", "unknown-videos", "doubao-seedance-2-0-260128");

	EXPECT_STREQ(video_gen_adapter_name(&inferred), "volcengine-videos");
	EXPECT_STREQ(video_gen_adapter_name(&explicit_adapter),
		     "volcengine-videos");
	EXPECT_EQ(video_gen_adapter_name(&unsupported), nullptr);
	EXPECT_TRUE(video_gen_adapter_supported("volcengine", ""));
	EXPECT_TRUE(video_gen_adapter_supported("gateway", "volcengine-videos"));
	EXPECT_FALSE(video_gen_adapter_supported("gateway", ""));
}

TEST(VideoAdapterTest, CapabilitiesFollowSeedanceVersion)
{
	struct model one = make_video_model(
		"volcengine", "", "doubao-seedance-1-0-pro-fast-251015");
	struct model one_five = make_video_model(
		"volcengine", "", "doubao-seedance-1-5-pro-251215");
	struct model two = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct model fast = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-fast-260128");
	struct model mini = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-mini-260615");
	struct model endpoint = make_video_model("volcengine", "", "ep-test");
	struct video_capabilities caps{};

	ASSERT_EQ(video_gen_capabilities(&one, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_images);
	EXPECT_FALSE(caps.supports_multi_reference_images);
	EXPECT_FALSE(caps.supports_generate_audio);
	EXPECT_FALSE(caps.supports_reference_videos);
	EXPECT_FALSE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&one_five, &caps), 0);
	EXPECT_TRUE(caps.supports_generate_audio);
	EXPECT_FALSE(caps.supports_reference_videos);

	ASSERT_EQ(video_gen_capabilities(&two, &caps), 0);
	EXPECT_TRUE(caps.supports_multi_reference_images);
	EXPECT_TRUE(caps.supports_generate_audio);
	EXPECT_TRUE(caps.supports_reference_videos);
	EXPECT_TRUE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&fast, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_videos);
	EXPECT_TRUE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&mini, &caps), 0);
	EXPECT_TRUE(caps.supports_multi_reference_images);
	EXPECT_TRUE(caps.supports_generate_audio);
	EXPECT_TRUE(caps.supports_reference_videos);
	EXPECT_TRUE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&endpoint, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_images);
	EXPECT_FALSE(caps.supports_multi_reference_images);
	EXPECT_FALSE(caps.supports_reference_videos);
	EXPECT_FALSE(caps.supports_reference_audios);
}

TEST(VideoAdapterTest, EmptyModelDoesNotUseHiddenFallback)
{
	struct model model = make_video_model("volcengine", "", "");
	struct video_capabilities caps{};
	struct video_result result{};

	EXPECT_EQ(video_gen_capabilities(&model, &caps),
		  MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0,
				   nullptr, 0, -1, 5, nullptr, &result),
		  MORPH_ERR_NOT_CONFIGURED);
	EXPECT_NE(std::strstr(result.error_msg, "no model configured"), nullptr);
}

TEST(VideoAdapterTest, RejectsUnsupportedReferencesBeforeNetworkRequest)
{
	struct model model = make_video_model(
		"volcengine", "", "doubao-seedance-1-5-pro-251215");
	struct video_result result{};
	const char *videos[] = {"reference.mp4"};

	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, videos, 1,
				   nullptr, 0, -1, 5, nullptr, &result),
		  -ENOTSUP);
	EXPECT_NE(std::strstr(result.error_msg, "reference videos"), nullptr);
}

TEST(VideoAdapterTest, ValidatesReferenceAudioCombination)
{
	struct model model = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct video_result result{};
	const char *images[] = {"reference.png"};
	const char *audios[] = {"reference.mp3"};

	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0,
				   audios, 1, -1, 5, nullptr, &result),
		  -EINVAL);
	EXPECT_NE(std::strstr(result.error_msg, "requires a reference image"),
		  nullptr);

	memset(&result, 0, sizeof(result));
	EXPECT_EQ(video_gen_create(&model, "test", images, 1, nullptr, 0,
				   audios, 1, 0, 5, nullptr, &result),
		  -EINVAL);
	EXPECT_NE(std::strstr(result.error_msg, "generate_audio=true"), nullptr);
}

TEST(VideoToolTest, SchemaExposesAudioInputs)
{
	struct tool_registry registry;

	tool_registry_init(NULL, &registry);
	ASSERT_EQ(vid_gen_init(&registry, nullptr, nullptr), 0);
	struct tool_entry *entry = tool_borrow(&registry, "vid_gen");
	ASSERT_NE(entry, nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "reference_audios"),
		  nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "generate_audio"),
		  nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "\"maxItems\":3"),
		  nullptr);
	cJSON *schema = cJSON_Parse(entry->desc.input_schema);
	EXPECT_NE(schema, nullptr);
	cJSON_Delete(schema);
	tool_registry_cleanup(&registry);
}

TEST(VideoAdapterTest, RejectsOversizedLocalVideoBeforeNetworkRequest)
{
	char path[] = "/tmp/morph_video_limit_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ftruncate(fd, (off_t)MORPH_MEDIA_MAX_FILE_BYTES + 1), 0);
	close(fd);
	struct model model = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	strcpy(model.api_key, "mock");
	strcpy(model.api_base, "http://127.0.0.1:1");
	struct video_result result{};
	const char *videos[] = {path};
	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, videos, 1,
		nullptr, 0, -1, 5, nullptr, &result), -EFBIG);
	EXPECT_NE(strstr(result.error_msg, "media reference"), nullptr);
	unlink(path);
}

class VideoTaskServer
{
      public:
	enum Mode { WAITING, BLOCK_QUERY, FAILED };
	std::atomic<bool> submitted{false};
	std::atomic<bool> queried{false};
	std::string base;

	explicit VideoTaskServer(Mode mode) : mode(mode)
	{
	}

	bool start()
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener < 0)
			return false;
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
			return false;
		socklen_t length = sizeof(address);
		if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0 ||
		    listen(listener, 4) != 0)
			return false;
		base = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
		worker = std::thread([this] { serve(); });
		return true;
	}

	~VideoTaskServer()
	{
		stop = true;
		if (worker.joinable())
			worker.join();
		if (listener >= 0)
			close(listener);
	}

      private:
	Mode mode;
	int listener{-1};
	std::atomic<bool> stop{false};
	std::thread worker;

	void serve()
	{
		while (!stop) {
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(listener, &readable);
			timeval timeout{0, 50000};
			if (select(listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
				continue;
			int peer = accept(listener, nullptr, nullptr);
			if (peer < 0)
				continue;
			timeval receive_timeout{0, 100000};
			setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
				   sizeof(receive_timeout));
#ifdef SO_NOSIGPIPE
			int no_sigpipe = 1;
			setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
			std::string request;
			char bytes[4096];
			while (!stop) {
				ssize_t length = recv(peer, bytes, sizeof(bytes), 0);
				if (length <= 0)
					break;
				request.append(bytes, static_cast<size_t>(length));
				auto header_end = request.find("\r\n\r\n");
				if (header_end == std::string::npos)
					continue;
				auto content_header = request.find("Content-Length:");
				size_t body_size =
					content_header == std::string::npos
						? 0
						: static_cast<size_t>(std::strtoul(
							  request.c_str() + content_header + 15,
							  nullptr, 10));
				if (request.size() >= header_end + 4 + body_size)
					break;
			}
			bool post = request.rfind("POST ", 0) == 0;
			std::string body = "{\"id\":\"task\",\"status\":\"running\"}";
			if (!post) {
				queried = true;
				if (mode == BLOCK_QUERY) {
					while (!stop)
						std::this_thread::sleep_for(
							std::chrono::milliseconds(10));
					close(peer);
					continue;
				}
				if (mode == FAILED)
					body = "{\"status\":\"failed\",\"error\":{\"code\":\"bad_"
					       "task\","
					       "\"message\":\"provider rejected task\"}}";
			}
			std::string response =
				"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
				"Connection: close\r\nContent-Length: " +
				std::to_string(body.size()) + "\r\n\r\n" + body;
			(void)send(peer, response.data(), response.size(), 0);
			if (post)
				submitted = true;
			close(peer);
		}
	}
};

static model video_poll_model(const VideoTaskServer &server)
{
	model result = make_video_model("volcengine", "", "doubao-seedance-2-0-test");
	strcpy(result.api_key, "test");
	std::snprintf(result.api_base, sizeof(result.api_base), "%s", server.base.c_str());
	result.poll_interval_seconds = 1;
	result.poll_timeout_seconds = 5;
	return result;
}

TEST(VideoPollingTest, CancellationStopsWaitingAndBlockedQueries)
{
	for (bool block_query : {false, true}) {
		SCOPED_TRACE(block_query);
		VideoTaskServer server(block_query ? VideoTaskServer::BLOCK_QUERY
						   : VideoTaskServer::WAITING);
		ASSERT_TRUE(server.start());
		model model = video_poll_model(server);
		morph_cancel_token token{};
		model.http.cancel_token = &token;
		if (!block_query)
			model.poll_interval_seconds = 5;
		video_result result{};
		std::thread cancel([&] {
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!(block_query ? server.queried.load() : server.submitted.load()) &&
			       std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			morph_cancel_token_cancel(&token);
		});
		auto start = std::chrono::steady_clock::now();
		int rc = video_gen_create(&model, "test", nullptr, 0, nullptr, 0, nullptr, 0, -1, 5,
					  nullptr, &result);
		cancel.join();
		EXPECT_EQ(rc, -ECANCELED);
		EXPECT_TRUE(server.submitted);
		EXPECT_EQ(server.queried.load(), block_query);
		EXPECT_LT(std::chrono::steady_clock::now() - start,
			  std::chrono::milliseconds(block_query ? 2000 : 1000));
		EXPECT_EQ(strstr(result.error_msg, "timed out"), nullptr);
	}
}

TEST(VideoPollingTest, OverallDeadlineBoundsAnIntervalLongerThanTheBudget)
{
	VideoTaskServer server(VideoTaskServer::WAITING);
	ASSERT_TRUE(server.start());
	model model = video_poll_model(server);
	model.poll_interval_seconds = 5;
	model.poll_timeout_seconds = 1;
	video_result result{};
	auto start = std::chrono::steady_clock::now();
	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0, nullptr, 0, -1, 5,
				   nullptr, &result),
		  -ETIMEDOUT);
	EXPECT_TRUE(server.submitted);
	EXPECT_FALSE(server.queried);
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(1800));
	EXPECT_NE(strstr(result.error_msg, "timed out"), nullptr);
}

TEST(VideoPollingTest, ProviderFailureRetainsItsDetails)
{
	VideoTaskServer server(VideoTaskServer::FAILED);
	ASSERT_TRUE(server.start());
	model model = video_poll_model(server);
	video_result result{};

	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0, nullptr, 0, -1, 5,
				   nullptr, &result),
		  MORPH_ERR_API);
	EXPECT_NE(strstr(result.error_msg, "bad_task"), nullptr);
	EXPECT_NE(strstr(result.error_msg, "provider rejected task"), nullptr);
}
