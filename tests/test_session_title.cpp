#include "runtime_test_support.hpp"

extern "C" {
#include "runtime/runtime_internal.h"
#include "runtime/session_title.h"
#include "util/utf8.h"
#include "credits.h"
}

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using TitleClock = std::chrono::steady_clock;

/* A gated HTTP response makes background/rename races deterministic. */
class SessionTitleTest : public RuntimeFacadeTest {
protected:
	int listener = -1;
	std::thread server;
	std::mutex lock;
	std::condition_variable condition;
	bool received = false;
	bool released = false;
	std::string request_body;
	std::string answer = R"({"title":"Fix session naming"})";
	std::vector<std::string> title_phases;

	void TearDown() override
	{
		Release();
		if (server.joinable()) {
			shutdown(listener, SHUT_RDWR);
			server.join();
		}
		if (listener >= 0)
			close(listener);
		RuntimeFacadeTest::TearDown();
		http_cleanup();
	}

	void Start()
	{
		ASSERT_EQ(http_init(), 0);
		listener = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listener, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address),
			sizeof(address)), 0);
		ASSERT_EQ(listen(listener, 2), 0);
		socklen_t size = sizeof(address);
		ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address),
			&size), 0);
		WriteConfig("[memory]\nenabled=false\n[model.text]\nprovider=\"openai\"\n"
			"model=\"title-test\"\napi_key=\"test-key\"\napi_base=\"http://127.0.0.1:" +
			std::to_string(ntohs(address.sin_port)) + "/v1\"\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.event_user_data = this;
		options.event_cb = [](const struct morph_event *event, void *opaque) {
			auto *self = static_cast<SessionTitleTest *>(opaque);
			if (event->name && strcmp(event->name, "session.title") == 0) {
				std::lock_guard<std::mutex> guard(self->lock);
				self->title_phases.emplace_back(event->phase);
			}
			return 0;
		};
		Open(&options);
		ASSERT_NE(instance->context.llm, nullptr);
		ASSERT_STREQ(instance->context.llm->api_key, "test-key");
		ASSERT_EQ(std::string(instance->context.llm->api_base).find("http://127.0.0.1:"), 0u);
		server = std::thread([this] {
			struct pollfd ready{listener, POLLIN, 0};
			if (poll(&ready, 1, 3000) <= 0)
				return;
			int client = accept(listener, nullptr, nullptr);
			if (client < 0)
				return;
			int no_sigpipe = 1;
#ifdef SO_NOSIGPIPE
			setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
				sizeof(no_sigpipe));
#else
			(void)no_sigpipe;
#endif
			std::string request;
			char buffer[BUFSIZ];
			while (true) {
				ssize_t count = recv(client, buffer, sizeof(buffer), 0);
				if (count <= 0)
					break;
				request.append(buffer, static_cast<size_t>(count));
				auto headers = request.find("\r\n\r\n");
				auto length = request.find("Content-Length:");
				if (headers != std::string::npos && length != std::string::npos &&
				    request.size() >= headers + 4 + std::stoul(request.substr(length + 15)))
					break;
			}
			{
				std::unique_lock<std::mutex> guard(lock);
				request_body = request.substr(request.find("\r\n\r\n") + 4);
				received = true;
				condition.notify_all();
				condition.wait(guard, [this] { return released; });
			}
			cJSON *delta = cJSON_CreateObject();
			cJSON_AddStringToObject(delta, "content", answer.c_str());
			char *json = cJSON_PrintUnformatted(delta);
			std::string body = "data: {\"choices\":[{\"delta\":" + std::string(json) +
				",\"finish_reason\":null}]}\n\n"
				"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
				"\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8,"
				"\"total_tokens\":28}}\n\ndata: [DONE]\n\n";
			free(json);
			cJSON_Delete(delta);
			std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
				"Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
				"\r\n\r\n" + body;
			int flags = 0;
#ifdef MSG_NOSIGNAL
			flags = MSG_NOSIGNAL;
#endif
			(void)send(client, response.data(), response.size(), flags);
			close(client);
		});
	}

	void Release()
	{
		std::lock_guard<std::mutex> guard(lock);
		released = true;
		condition.notify_all();
	}

	bool WaitRequest()
	{
		std::unique_lock<std::mutex> guard(lock);
		return condition.wait_for(guard, std::chrono::seconds(3),
			[this] { return received; });
	}

	int Generate(const std::string &prompt = "Fix session titles")
	{
		struct runtime_request request{};
		request.session_id = instance->context.current_session.id;
		request.stored_user_input = prompt.c_str();
		pthread_mutex_lock(&instance->context.execution_lock);
		int rc = runtime_title_start(instance, &request);
		pthread_mutex_unlock(&instance->context.execution_lock);
		return rc;
	}

	bool WaitName(int64_t id, const std::string &name)
	{
		auto deadline = TitleClock::now() + std::chrono::seconds(3);
		do {
			struct session session{};
			if (session_get_by_id(&instance->context.database, id, &session) == 0 &&
			    session.name == name)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (TitleClock::now() < deadline);
		return false;
	}
};

TEST(SessionTitleParser, StrictJsonAndSafeNormalization)
{
	const char *invalid_responses[] = {nullptr, "title", "[]", "{}", "{\"title\":3}",
		"{\"title\":\"\"}", "{\"title\":\"x\",\"extra\":1}",
		"{\"title\":\"x\"} trailing", "{\"title\":\"\\u001b[31mred\"}"};
	for (const char *invalid : invalid_responses) {
		char *title = runtime_title_parse(invalid);
		EXPECT_EQ(title, nullptr);
		free(title);
	}
	char *title = runtime_title_parse(R"({"title":"  \"Fix\n  session titles!\"  "})");
	ASSERT_NE(title, nullptr);
	EXPECT_STREQ(title, "Fix session titles");
	free(title);
}

TEST(SessionTitleParser, TruncatesByUnicodeCharacters)
{
	std::string input;
	for (int i = 0; i < 50; i++)
		input += "猫";
	char *title = runtime_title_parse(("{\"title\":\"" + input + "\"}").c_str());
	ASSERT_NE(title, nullptr);
	EXPECT_EQ(utf8valid(title), nullptr);
	EXPECT_EQ(utf8len(title), SESSION_TITLE_MAX_CHARS);
	free(title);
}

TEST_F(SessionTitleTest, SavesToOriginAfterSwitchAndRecordsSeparateUsage)
{
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	struct session other{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "Other task", &other), 0);
	Release();
	ASSERT_TRUE(WaitName(origin.id, "Fix session naming"));
	EXPECT_STREQ(runtime_session_current_name(instance), "Other task");
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 0);
	struct credit_summary summary{};
	ASSERT_EQ(credit_summary_session(&instance->context.database, origin.display_id,
		&summary), 0);
	EXPECT_EQ(summary.event_count, 1);
	ASSERT_EQ(runtime_session_select_existing(instance, origin.id, nullptr), 0);
	EXPECT_STREQ(runtime_session_current_name(instance), "Fix session naming");
}

TEST_F(SessionTitleTest, StoresModelSummaryRatherThanPromptPrefix)
{
	answer = R"({"title":"改用模型总结会话标题"})";
	Start();
	const std::string input =
		"我还是希望不要文本直接截取做name，而是靠总结，"
		"现在还是截取，感觉很生硬！";
	ASSERT_EQ(Generate(input), 0);
	ASSERT_TRUE(WaitRequest());
	cJSON *body = cJSON_Parse(request_body.c_str());
	ASSERT_NE(body, nullptr);
	const cJSON *messages = cJSON_GetObjectItem(body, "messages");
	const char *instructions = cJSON_GetStringValue(cJSON_GetObjectItem(
		cJSON_GetArrayItem(messages, 0), "content"));
	ASSERT_NE(instructions, nullptr);
	EXPECT_NE(strstr(instructions, "do not simply copy or truncate"), nullptr);
	cJSON_Delete(body);
	Release();
	ASSERT_TRUE(WaitName(instance->context.current_session.id,
		"改用模型总结会话标题"));
	struct session current{};
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_NE(input.find(current.name), 0u);
}

TEST_F(SessionTitleTest, ManualRenameWinsEvenWhenNameIsUnchanged)
{
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(runtime_session_rename_and_update(instance, origin.id, origin.name), 0);
	Release();
	runtime_title_stop(instance);
	EXPECT_TRUE(WaitName(origin.id, origin.name));
}

TEST_F(SessionTitleTest, DeletedSessionsAreNotRecreated)
{
	Start();
	int64_t id = instance->context.current_session.id;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(runtime_session_delete_and_update(instance, id), 0);
	Release();
	runtime_title_stop(instance);
	struct session session{};
	EXPECT_EQ(session_get_by_id(&instance->context.database, id, &session), -ENOENT);
}

TEST_F(SessionTitleTest, DuplicateTitlesReceiveStableIdentitySuffix)
{
	Start();
	struct session origin = instance->context.current_session;
	struct session other{};
	ASSERT_EQ(runtime_session_create_detached(instance, "Fix session naming", &other), 0);
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	EXPECT_TRUE(WaitName(origin.id,
		"Fix session naming [" + std::string(origin.display_id) + "]"));
}

TEST_F(SessionTitleTest, InFlightRequestsAreDeduplicatedAndInputIsBounded)
{
	Start();
	std::string prompt;
	for (int i = 0; i < 1000; i++)
		prompt += "猫";
	ASSERT_EQ(Generate(prompt), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(Generate("A second request"), 0);
	cJSON *body = cJSON_Parse(request_body.c_str());
	ASSERT_NE(body, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(body, "tools"), nullptr);
	const cJSON *messages = cJSON_GetObjectItem(body, "messages");
	ASSERT_EQ(cJSON_GetArraySize(messages), 2);
	size_t bytes = 0;
	for (int i = 0; i < 2; i++) {
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(
			cJSON_GetArrayItem(messages, i), "content"));
		ASSERT_NE(text, nullptr);
		EXPECT_EQ(utf8valid(text), nullptr);
		bytes += strlen(text);
	}
	EXPECT_LE(bytes, SESSION_TITLE_PROMPT_MAX_BYTES);
	cJSON_Delete(body);
	Release();
	EXPECT_TRUE(WaitName(instance->context.current_session.id, "Fix session naming"));
}

TEST_F(SessionTitleTest, ShutdownCancelsAStalledRequest)
{
	Start();
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	auto before = TitleClock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(TitleClock::now() - before, std::chrono::seconds(3));
	EXPECT_EQ(title_phases, (std::vector<std::string>{"begin", "end"}));
}

static int foreground_reply(struct model *, struct arena *, const char *,
	struct chat_message *, int, struct tool_desc *, int,
	struct chat_response *response, sse_callback, void *)
{
	response->content = strdup("Task complete.");
	return response->content ? 200 : -ENOMEM;
}

TEST_F(SessionTitleTest, ForegroundTurnFinishesWhileTitleResponseIsBlocked)
{
	Start();
	instance->context.llm->chat_with_tools = foreground_reply;
	instance->context.llm->chat_with_tools_stream = nullptr;
	struct runtime_request request{};
	request.model_input = "Fix session titles";
	request.stored_user_input = request.model_input;
	request.generate_session_title = 1;
	struct runtime_result result{};
	auto before = TitleClock::now();
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_LT(TitleClock::now() - before, std::chrono::seconds(1));
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_STREQ(result.final_text, "Task complete.");
	ASSERT_TRUE(WaitRequest());
	struct session origin = instance->context.current_session;
	EXPECT_TRUE(WaitName(origin.id, origin.name));
	Release();
	ASSERT_TRUE(WaitName(origin.id, "Fix session naming"));
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 2);
	EXPECT_STREQ(runtime_session_current_name(instance), "Fix session naming");
}

TEST_F(SessionTitleTest, ExplicitlyNamedTurnsDoNotScheduleTitles)
{
	Start();
	instance->context.llm->chat_with_tools = foreground_reply;
	instance->context.llm->chat_with_tools_stream = nullptr;
	ASSERT_EQ(runtime_session_rename_and_update(instance,
		instance->context.current_session.id, "My chosen name"), 0);
	struct runtime_request request{};
	request.model_input = "Fix session titles";
	request.stored_user_input = request.model_input;
	struct runtime_result result{};
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	for (auto *job : instance->title_jobs)
		EXPECT_EQ(job, nullptr);
	EXPECT_STREQ(runtime_session_current_name(instance), "My chosen name");
}

TEST_F(SessionTitleTest, InvalidOutputDoesNotRenameOrEnterConversation)
{
	answer = "I cannot generate a title";
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	server.join();
	/* Wait for the isolated request to finish billing before joining its worker. */
	auto deadline = TitleClock::now() + std::chrono::seconds(3);
	struct credit_summary summary{};
	do {
		ASSERT_EQ(credit_summary_session(&instance->context.database,
			origin.display_id, &summary), 0);
		if (summary.event_count)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (TitleClock::now() < deadline);
	EXPECT_EQ(summary.event_count, 1);
	runtime_title_stop(instance);
	EXPECT_TRUE(WaitName(origin.id, origin.name));
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 0);
}
