#include "http/request.h"
#include <gtest/gtest.h>
#include <nb/file.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <cstdlib>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

class HttpTest : public ::testing::Test {
protected:
  nb_http_client *client = nullptr;
  nb_http_call *call = nullptr;

  void SetUp() override
  {
	  ASSERT_EQ(http_client_open(nullptr, &client), NB_OK);
  }
  void TearDown() override
  {
	  nb_http_call_free(call);
	  nb_http_client_free(client);
  }
};

TEST_F(HttpTest, DefaultsPreserveApplicationPolicy)
{
	nb_http_request request;
	http_request_init(&request, "GET", "http://localhost/");
	EXPECT_EQ(request.redirect, NB_HTTP_REDIRECT_MANUAL);
	EXPECT_EQ(request.timeouts.connect_ms, 10000);
	EXPECT_EQ(request.timeouts.total_ms, 30000);
	EXPECT_EQ(request.timeouts.response_ms, 0);
	EXPECT_EQ(request.timeouts.idle_ms, 0);
	EXPECT_EQ(request.max_body_bytes, 0u);
}

TEST_F(HttpTest, RejectsInvalidRequestsWithoutQueueing)
{
	nb_http_request request;
	http_request_init(&request, "GET", nullptr);
	EXPECT_EQ(http_request_run(nullptr, client, &request, nullptr, &call), NB_ERR_INVAL);
	EXPECT_EQ(call, nullptr);
	EXPECT_EQ(nb_http_client_pending(client), 0u);
	http_request_init(&request, "GET", "file:///etc/passwd");
	EXPECT_EQ(http_request_run(nullptr, client, &request, nullptr, &call), NB_ERR_INVAL);
	EXPECT_EQ(call, nullptr);
}

TEST_F(HttpTest, PreservesOccupiedCallWithoutStartingTransfer)
{
	nb_http_request request;
	http_request_init(&request, "GET", "http://127.0.0.1:1/");
	ASSERT_EQ(nb_http_client_send(client, &request, nullptr, &call, nullptr), NB_OK);
	auto *saved = call;
	EXPECT_EQ(http_request_run(nullptr, client, &request, nullptr, &call), NB_ERR_EXISTS);
	EXPECT_EQ(call, saved);
	EXPECT_EQ(nb_http_call_get_state(call), NB_HTTP_CALL_QUEUED);
}

TEST_F(HttpTest, ConfiguredHeadersKeepDuplicateNamesAndRejectInjection)
{
	nb_vec headers NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&headers, sizeof(nb_http_header));
	const char *lines[] = {"Authorization: Bearer token", "X-Test: first", "x-test: second"};
	ASSERT_EQ(http_headers_parse(&headers, lines, 3), NB_OK);
	auto *values = reinterpret_cast<nb_http_header *>(headers.data);
	EXPECT_TRUE(nb_str_eq_cstr(values[0].name, "Authorization"));
	EXPECT_TRUE(nb_str_eq_cstr(values[0].value, "Bearer token"));
	EXPECT_TRUE(nb_str_eq_cstr(values[2].value, "second"));
	nb_http_request request;
	http_request_init(&request, "GET", "http://127.0.0.1:1/");
	values[0].value = NB_STR_LIT("token\r\nInjected: bad");
	request.headers = {values, headers.len};
	EXPECT_EQ(http_request_run(nullptr, client, &request, nullptr, &call), NB_ERR_INVAL);
	EXPECT_EQ(call, nullptr);
}

class SilentPeer
{
      public:
	int listener = -1;
	unsigned port = 0;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable condition;
	bool finished = false;

	bool open()
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener < 0)
			return false;

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t size = sizeof(address);
		if (bind(listener, reinterpret_cast<sockaddr *>(&address), size) ||
		    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) ||
		    listen(listener, 1))
			return false;

		port = ntohs(address.sin_port);
		worker = std::thread([this] {
			int peer = accept(listener, nullptr, nullptr);
			std::unique_lock<std::mutex> lock(mutex);
			condition.wait_for(lock, std::chrono::seconds(4),
					   [this] { return finished; });
			if (peer >= 0)
				close(peer);
		});
		return true;
	}

	~SilentPeer()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		condition.notify_all();
		if (listener >= 0)
			shutdown(listener, SHUT_RDWR);

		if (worker.joinable())
			worker.join();

		if (listener >= 0)
			close(listener);
	}
};

TEST_F(HttpTest, CancellationInterruptsSilentPeersPromptly)
{
	for (int kind = 0; kind < 3; kind++) {
		for (int signal = 0; signal < 3; signal++) {
			SCOPED_TRACE(kind);
			SCOPED_TRACE(signal);
			SilentPeer server;
			ASSERT_TRUE(server.open());
			nb_cancel token{};
			nb_atomic_int flag = 0;
			std::atomic<bool> interrupted{false};
			http_control control{};
			control.cancel_token = &token;
			control.cancel_flag = &flag;
			control.interrupt_user_data = &interrupted;
			control.interrupt_check = [](void *ctx) -> int {
				return static_cast<std::atomic<bool> *>(ctx)->load();
			};
			char url[128];
			snprintf(url, sizeof(url), "http://127.0.0.1:%u/", server.port);
			nb_http_request request;
			http_request_init(&request, "POST", url);
			nb_http_part part{};
			part.name = "text";
			part.data = NB_STR_LIT("test");
			if (kind == 2) {
				request.parts = &part;
				request.part_count = 1;
			} else {
				request.body = NB_STR_LIT("{}");
			}

			nb_http_handler handler{};
			if (kind == 1)
				handler.on_data = [](nb_http_call *, nb_str, void *, nb_err *) {
					return NB_OK;
				};

			std::thread canceller([&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (signal == 0)
					nb_cancel_request(&token);
				else if (signal == 1)
					__atomic_store_n(&flag, 1, __ATOMIC_RELAXED);
				else
					interrupted.store(true);
			});
			auto start = std::chrono::steady_clock::now();
			int rc = http_request_run(&control, client, &request, &handler, &call);
			auto elapsed = std::chrono::steady_clock::now() - start;
			canceller.join();
			EXPECT_EQ(rc, NB_ERR_CANCELLED);
			EXPECT_LT(elapsed, std::chrono::milliseconds(500));
			nb_http_call_free(call);
			call = nullptr;
		}
	}
}

TEST_F(HttpTest, BufferedStreamingAndMultipartPreserveTimeoutErrors)
{
	for (int kind = 0; kind < 3; kind++) {
		SCOPED_TRACE(kind);
		SilentPeer server;
		ASSERT_TRUE(server.open());
		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%u/", server.port);
		nb_http_request request;
		http_request_init(&request, "POST", url);
		request.timeouts.total_ms = 100;
		nb_http_part part{};
		part.name = "text";
		part.data = NB_STR_LIT("test");
		if (kind == 2) {
			request.parts = &part;
			request.part_count = 1;
		} else {
			request.body = NB_STR_LIT("{}");
		}

		nb_http_handler handler{};
		if (kind == 1)
			handler.on_data = [](nb_http_call *, nb_str, void *, nb_err *) {
				return NB_OK;
			};

		EXPECT_EQ(http_request_run(nullptr, client, &request, &handler, &call),
			  NB_ERR_TIMEOUT);
		nb_http_call_free(call);
		call = nullptr;
	}
}

TEST_F(HttpTest, AlreadyCancelledNeverStartsTransfer)
{
	nb_cancel token{};
	nb_cancel_request(&token);
	http_control control{};
	control.cancel_token = &token;
	nb_http_request request;
	http_request_init(&request, "GET", "http://127.0.0.1:1/");
	EXPECT_EQ(http_request_run(&control, client, &request, nullptr, &call), NB_ERR_CANCELLED);
	EXPECT_EQ(call, nullptr);
	EXPECT_EQ(nb_http_client_pending(client), 0u);
}

TEST(CancelTokenTest, ResetCancelAndQuery)
{
	nb_cancel token{};
	EXPECT_FALSE(nb_cancel_requested(&token));
	nb_cancel_request(&token);
	EXPECT_TRUE(nb_cancel_requested(&token));
	nb_cancel_reset(&token);
	EXPECT_FALSE(nb_cancel_requested(&token));
}

TEST(CancelTokenTest, RetryWaitStopsWhenCancelled)
{
	nb_cancel token{};
	http_control control{};
	control.cancel_token = &token;
	std::thread canceller([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		nb_cancel_request(&token);
	});
	auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(http_wait_cancelable(&control, 5000), NB_ERR_CANCELLED);
	EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
	canceller.join();
}

class SavedHttpEnvironment
{
      public:
	const char *name;
	std::optional<std::string> value;

	explicit SavedHttpEnvironment(const char *key) : name(key)
	{
		if (const char *current = getenv(name))
			value = current;
	}

	~SavedHttpEnvironment()
	{
		if (value)
			setenv(name, value->c_str(), 1);
		else
			unsetenv(name);
	}
};

TEST_F(HttpTest, CertificateEnvironmentPriorityIsPreserved)
{
	SavedHttpEnvironment curl_ca("CURL_CA_BUNDLE");
	SavedHttpEnvironment ssl_ca("SSL_CERT_FILE");
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(nb_file_temp(&path, nullptr), NB_OK);
	nb_http_client *configured NB_CLEANUP(http_client_cleanup) = nullptr;
	const char *missing = "/morph-missing-test-ca-file";

	setenv("CURL_CA_BUNDLE", missing, 1);
	setenv("SSL_CERT_FILE", nb_buf_cstr(&path), 1);
	EXPECT_EQ(http_client_open(nullptr, &configured), NB_ERR_NOT_FOUND);
	EXPECT_EQ(configured, nullptr);
	setenv("CURL_CA_BUNDLE", nb_buf_cstr(&path), 1);
	setenv("SSL_CERT_FILE", missing, 1);
	EXPECT_EQ(http_client_open(nullptr, &configured), NB_OK);
	http_client_cleanup(&configured);
	setenv("CURL_CA_BUNDLE", "", 1);
	EXPECT_EQ(http_client_open(nullptr, &configured), NB_ERR_NOT_FOUND);
	EXPECT_EQ(configured, nullptr);
	setenv("SSL_CERT_FILE", nb_buf_cstr(&path), 1);
	EXPECT_EQ(http_client_open(nullptr, &configured), NB_OK);
	EXPECT_EQ(nb_file_remove(nb_buf_cstr(&path), nullptr), NB_OK);
}
