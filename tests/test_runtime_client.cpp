#include "runtime/client.h"

#ifdef REACT_H
#error "The runtime client protocol must not expose the ReAct implementation"
#endif

#include "runtime_test_support.hpp"

extern "C" {
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "db/scheduled_task.h"
#include "persistence/runtime_store.h"
#include <nb/err.h>
}

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <sqlite3.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using RuntimeClientJson = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using RuntimeClientMessage =
	std::unique_ptr<runtime_message, decltype(&runtime_message_destroy)>;

static RuntimeClientJson runtime_client_test_json()
{
	return RuntimeClientJson(cJSON_CreateObject(), cJSON_Delete);
}

static std::string runtime_client_test_text(const cJSON *object, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
	return text ? text : "";
}

static std::string runtime_client_test_delta(const char *text)
{
	auto json = runtime_client_test_json();
	cJSON_AddStringToObject(json.get(), "content", text);
	char *encoded = cJSON_PrintUnformatted(json.get());
	std::string result = encoded ? encoded : "{}";
	cJSON_free(encoded);
	return result;
}

static std::string runtime_client_test_tool(const char *name, const char *id,
					   const char *arguments)
{
	auto json = runtime_client_test_json();
	cJSON *calls = cJSON_AddArrayToObject(json.get(), "tool_calls");
	cJSON *call = cJSON_CreateObject();
	cJSON_AddItemToArray(calls, call);
	cJSON_AddNumberToObject(call, "index", 0);
	cJSON_AddStringToObject(call, "id", id);
	cJSON_AddStringToObject(call, "type", "function");
	cJSON *function = cJSON_AddObjectToObject(call, "function");
	cJSON_AddStringToObject(function, "name", name);
	cJSON_AddStringToObject(function, "arguments", arguments);
	char *encoded = cJSON_PrintUnformatted(json.get());
	std::string result = encoded ? encoded : "{}";
	cJSON_free(encoded);
	return result;
}

/* Each accepted connection has its own responder so a held model request cannot
 * make a second session appear serialized by the test server itself. */
class RuntimeClientHttpServer {
public:
	std::function<std::string(const std::string &)> respond;
	int port = 0;

	void Start()
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listener, 0);

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
			  0);
		ASSERT_EQ(listen(listener, 16), 0);

		socklen_t size = sizeof(address);
		ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size), 0);
		port = ntohs(address.sin_port);
		acceptor = std::thread([this] {
			while (!stopped.load()) {
				pollfd pending{listener, POLLIN, 0};
				if (poll(&pending, 1, 50) <= 0)
					continue;

				int client = accept(listener, nullptr, nullptr);
				if (client < 0)
					break;

				workers.emplace_back([this, client] { Serve(client); });
			}
		});
	}

	bool Await(const std::string &text)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(8), [&] {
			for (const auto &request : requests) {
				if (request.find(text) != std::string::npos)
					return true;
			}

			return false;
		});
	}

	void Hold()
	{
		std::unique_lock<std::mutex> lock(mutex);
		changed.wait_for(lock, std::chrono::seconds(20), [&] { return released; });
	}

	void Release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		released = true;
		changed.notify_all();
	}

	std::vector<std::string> Snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return requests;
	}

	~RuntimeClientHttpServer()
	{
		Release();
		stopped.store(true);
		if (acceptor.joinable())
			acceptor.join();

		if (listener >= 0)
			close(listener);

		for (auto &worker : workers) {
			if (worker.joinable())
				worker.join();
		}
	}

private:
	int listener = -1;
	std::atomic<bool> stopped{false};
	std::thread acceptor;
	std::vector<std::thread> workers;
	std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> requests;
	bool released = false;

	void Serve(int client)
	{
#ifdef SO_NOSIGPIPE
		int no_sigpipe = 1;
		(void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
				 sizeof(no_sigpipe));
#endif
		timeval timeout{5, 0};
		(void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		std::string request;
		char buffer[BUFSIZ];
		size_t body_start = std::string::npos;
		size_t body_size = 0;
		for (;;) {
			ssize_t count = recv(client, buffer, sizeof(buffer), 0);
			if (count <= 0)
				break;

			request.append(buffer, static_cast<size_t>(count));
			body_start = request.find("\r\n\r\n");
			if (body_start == std::string::npos)
				continue;

			auto length = request.find("Content-Length:");
			if (length != std::string::npos)
				body_size = std::stoul(request.substr(length + 15));

			if (request.size() >= body_start + 4 + body_size)
				break;
		}

		std::string body = body_start == std::string::npos
			? "{}" : request.substr(body_start + 4, body_size);
		{
			std::lock_guard<std::mutex> lock(mutex);
			requests.push_back(body);
			changed.notify_all();
		}

		std::string delta = respond ? respond(body) : runtime_client_test_delta("Done");
		std::string payload =
			"data: {\"choices\":[{\"index\":0,\"delta\":" + delta +
			",\"finish_reason\":null}]}\n\n"
			"data: {\"choices\":[{\"index\":0,\"delta\":{},"
			"\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":12,"
			"\"completion_tokens\":5,\"total_tokens\":17}}\n\n"
			"data: [DONE]\n\n";
		std::string response =
			"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
			"Connection: close\r\nContent-Length: " +
			std::to_string(payload.size()) + "\r\n\r\n" + payload;
		size_t sent = 0;
		while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
			const int flags = MSG_NOSIGNAL;
#else
			const int flags = 0;
#endif
			ssize_t count = send(client, response.data() + sent, response.size() - sent,
					 flags);
			if (count <= 0)
				break;

			sent += static_cast<size_t>(count);
		}

		close(client);
	}
};

class RuntimeClientTest : public RuntimeFacadeTest {
protected:
	RuntimeClientHttpServer server;
	struct runtime_client *client = nullptr;
	std::deque<RuntimeClientMessage> pending;
	std::atomic<int> writes{0};
	std::mutex workdir_mutex;
	std::vector<std::string> observed_workdirs;
	unsigned request_sequence = 0;

	void Start(const std::string &permissions = "", size_t event_capacity = 0,
		bool patches = false, bool agents = false, bool commands = false)
	{
		server.respond = [this](const std::string &body) {
			if (body.find("CLIENT_PATCH") != std::string::npos &&
			    body.find("call_client_patch") == std::string::npos) {
				bool denied = body.find("CLIENT_PATCH_DENIED") != std::string::npos;
				std::string patch = denied
					? "*** Begin Patch\n*** Update File: protected.txt\n@@\n"
					  "-External contents must survive\n"
					  "+Changed contents\n*** End Patch"
					: "*** Begin Patch\n*** Add File: async-client.txt\n"
					  "+Created by an authorized runtime turn\n*** End Patch";
				return runtime_client_test_tool("apply_patch", "call_client_patch",
					patch.c_str());
			}

			if (body.find("CLIENT_WORKDIR") != std::string::npos) {
				bool second = body.find("second settings") != std::string::npos;
				const char *call_id = second ? "call_client_workdir_second"
					: "call_client_workdir_first";
				if (body.find(call_id) == std::string::npos)
					return runtime_client_test_tool("client_test_workdir",
						call_id, "{}");
			}

			if (body.find("CLIENT_QUESTION") != std::string::npos &&
			    body.find("call_client_question") == std::string::npos)
				return runtime_client_test_tool("ask_user", "call_client_question",
					"{\"question\":\"Choose a release\","
					"\"choices\":[\"Stable\","
					"\"Preview\"],\"response_policy\":\"required\"}");

			if (body.find("CLIENT_WRITE") != std::string::npos &&
			    body.find("call_client_write") == std::string::npos)
				return runtime_client_test_tool("client_test_write",
					"call_client_write", "{}");

			if (body.find("CLIENT_HOLD") != std::string::npos &&
			    body.find("CLIENT_STEER") == std::string::npos)
				server.Hold();

			return runtime_client_test_delta(
				body.find("CLIENT_SECOND") != std::string::npos
					? "Second session completed" : "First session completed");
		};
		server.Start();
		ASSERT_EQ(setenv("MORPH_RUNTIME_CLIENT_TEST_KEY", "test-key", 1), 0);
		WriteConfig("[features]\nhooks=false\n[memory]\nenabled=false\n"
			"[general]\noutput_dir=\"" + directory + "/output\"\n"
			"[model.text]\nprovider=\"openai\"\nmodel=\"test-model\"\n"
			"api_base=\"http://127.0.0.1:" + std::to_string(server.port) +
			"/v1\"\napi_key_env=\"MORPH_RUNTIME_CLIENT_TEST_KEY\"\nretry_count=0\n" +
			permissions);
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.enable_apply_patch = patches;
		options.enable_multi_agent = agents;
		options.enable_exec = commands;
		options.platform_tools_cb = [](struct tool_registry *tools, struct tool_context *,
					       void *opaque) -> int {
			struct tool_spec spec{};
			spec.name = "client_test_write";
			spec.description = "Perform an observable test write after authorization";
			spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
			spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
			spec.user_data = opaque;
			spec.exec = [](const char *, struct tool_result *result,
				void *state) -> int {
				static_cast<RuntimeClientTest *>(state)->writes.fetch_add(1);
				return tool_result_success_text_copy(result, "Write completed");
			};
			int rc = tool_register_borrow(tools, &spec);
			if (rc != 0)
				return rc;

			spec.name = "client_test_workdir";
			spec.description = "Inspect the captured session working directory";
			spec.flags = TOOL_FLAG_READONLY;
			spec.exec = [](const char *, struct tool_result *result,
				void *state) -> int {
				auto *self = static_cast<RuntimeClientTest *>(state);
				const char *workdir = result->runtime && result->runtime->workdir
					? result->runtime->workdir : "";
				{
					std::lock_guard<std::mutex> lock(self->workdir_mutex);
					self->observed_workdirs.emplace_back(workdir);
				}

				return tool_result_success_text_copy(result, workdir);
			};
			return tool_register_borrow(tools, &spec);
		};
		options.platform_tools_user_data = this;
		Open(&options);
		struct runtime_client_options client_options{};
		client_options.event_capacity = event_capacity;
		ASSERT_EQ(runtime_client_open(instance, &client_options, &client), 0);
	}

	void TearDown() override
	{
		server.Release();
		if (client)
			runtime_client_close(client);

		client = nullptr;
		pending.clear();
		RuntimeFacadeTest::TearDown();
		unsetenv("MORPH_RUNTIME_CLIENT_TEST_KEY");
	}

	RuntimeClientMessage Wait(const std::function<bool(const runtime_message *)> &matches,
				  int timeout_ms = 10000)
	{
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			if (matches(it->get())) {
				auto result = std::move(*it);
				pending.erase(it);
				return result;
			}
		}

		auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeout_ms);
		while (std::chrono::steady_clock::now() < deadline) {
			struct runtime_message *raw = nullptr;
			int rc = runtime_client_receive(client, 100, &raw);
			if (rc == NB_ERR_TIMEOUT || rc == NB_ERR_AGAIN)
				continue;

			if (rc != 0)
				return RuntimeClientMessage(nullptr, runtime_message_destroy);

			RuntimeClientMessage message(raw, runtime_message_destroy);
			if (matches(message.get()))
				return message;

			pending.push_back(std::move(message));
		}

		return RuntimeClientMessage(nullptr, runtime_message_destroy);
	}

	RuntimeClientMessage Call(const char *method, const cJSON *params = nullptr)
	{
		std::string id = "request-" + std::to_string(++request_sequence);
		struct runtime_command command{id.c_str(), method, params};
		EXPECT_EQ(runtime_client_request_copy(client, &command), 0);
		return Wait([&](const runtime_message *message) {
			return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
				id == message->id;
		});
	}

	int64_t Create(const char *name)
	{
		auto params = runtime_client_test_json();
		cJSON_AddStringToObject(params.get(), "name", name);
		auto response = Call("session.create", params.get());
		EXPECT_NE(response, nullptr);
		if (!response)
			return 0;

		EXPECT_EQ(response->error_code, 0);
		const cJSON *session = cJSON_GetObjectItemCaseSensitive(response->data, "session");
		if (!session)
			session = response->data;

		const cJSON *id = cJSON_GetObjectItemCaseSensitive(session, "id");
		if (!id)
			id = cJSON_GetObjectItemCaseSensitive(session, "session_id");

		EXPECT_TRUE(cJSON_IsNumber(id));
		return static_cast<int64_t>(cJSON_GetNumberValue(id));
	}

	RuntimeClientMessage Request(const char *method, int64_t session_id,
				     const char *turn_id = nullptr, const char *input = nullptr)
	{
		auto params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id",
			static_cast<double>(session_id));
		if (turn_id)
			cJSON_AddStringToObject(params.get(), "turn_id", turn_id);

		if (input)
			cJSON_AddStringToObject(params.get(), "input", input);

		return Call(method, params.get());
	}

	RuntimeClientMessage Completed(int64_t session_id, int timeout_ms = 10000)
	{
		return Wait([&](const runtime_message *message) {
			return message->kind == RUNTIME_MESSAGE_EVENT &&
				message->session_id == session_id && message->method &&
				std::strcmp(message->method, "turn.completed") == 0;
		}, timeout_ms);
	}

	std::string StartTurn(int64_t session_id, const char *input)
	{
		auto response = Request("turn.start", session_id, nullptr, input);
		EXPECT_NE(response, nullptr);
		if (!response)
			return {};

		EXPECT_EQ(response->error_code, 0);
		return runtime_client_test_text(response->data, "turn_id");
	}
};

TEST_F(RuntimeClientTest, CopiesRequestAndReturnsOwnedResponse)
{
	Start();
	auto params = runtime_client_test_json();
	cJSON_AddStringToObject(params.get(), "name", "Original name");
	char id[] = "owned-request";
	char method[] = "session.create";
	struct runtime_command command{id, method, params.get()};
	ASSERT_EQ(runtime_client_request_copy(client, &command), 0);
	std::memset(id, 'x', sizeof(id) - 1);
	std::memset(method, 'x', sizeof(method) - 1);
	cJSON_ReplaceItemInObjectCaseSensitive(params.get(), "name",
		cJSON_CreateString("Changed after submission"));
	params.reset();
	auto response = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			std::strcmp(message->id, "owned-request") == 0;
	});
	ASSERT_NE(response, nullptr);
	ASSERT_EQ(response->error_code, 0);
	const cJSON *session = cJSON_GetObjectItemCaseSensitive(response->data, "session");
	if (!session)
		session = response->data;

	EXPECT_EQ(runtime_client_test_text(session, "name"), "Original name");
	auto listed = Call("session.list");
	ASSERT_NE(listed, nullptr);
	EXPECT_EQ(listed->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(session, "name"), "Original name");
}

TEST_F(RuntimeClientTest, RunsSessionsConcurrentlyAndKeepsHistorySeparate)
{
	Start();
	int64_t first = Create("Concurrent first");
	int64_t second = Create("Concurrent second");
	ASSERT_GT(first, 0);
	ASSERT_GT(second, 0);
	ASSERT_FALSE(StartTurn(first, "CLIENT_HOLD first-only").empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD first-only"));
	ASSERT_FALSE(StartTurn(second, "CLIENT_SECOND second-only").empty());
	auto second_done = Completed(second);
	ASSERT_NE(second_done, nullptr);
	EXPECT_EQ(second_done->session_id, second);
	auto first_status = Request("turn.get", first);
	ASSERT_NE(first_status, nullptr);
	EXPECT_EQ(first_status->error_code, 0);
	EXPECT_NE(runtime_client_test_text(first_status->data, "status"), "completed");
	server.Release();
	ASSERT_NE(Completed(first), nullptr);
	auto first_result = Request("turn.get", first);
	auto second_result = Request("turn.get", second);
	ASSERT_NE(first_result, nullptr);
	ASSERT_NE(second_result, nullptr);
	EXPECT_EQ(runtime_client_test_text(first_result->data, "final_text"),
		  "First session completed");
	EXPECT_EQ(runtime_client_test_text(second_result->data, "final_text"),
		  "Second session completed");
	auto first_history = Request("history.list", first);
	auto second_history = Request("history.list", second);
	ASSERT_NE(first_history, nullptr);
	ASSERT_NE(second_history, nullptr);
	ASSERT_EQ(first_history->error_code, 0);
	ASSERT_EQ(second_history->error_code, 0);
	char *first_json = cJSON_PrintUnformatted(first_history->data);
	char *second_json = cJSON_PrintUnformatted(second_history->data);
	ASSERT_NE(first_json, nullptr);
	ASSERT_NE(second_json, nullptr);
	EXPECT_NE(std::strstr(first_json, "first-only"), nullptr);
	EXPECT_EQ(std::strstr(first_json, "second-only"), nullptr);
	EXPECT_NE(std::strstr(second_json, "second-only"), nullptr);
	EXPECT_EQ(std::strstr(second_json, "first-only"), nullptr);
	cJSON_free(first_json);
	cJSON_free(second_json);
}

TEST_F(RuntimeClientTest, HistoryPaginationIsStableAndDoesNotReplayExecution)
{
	Start();
	int64_t session_id = Create("Paged history");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_SECOND PAGE_FIRST").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_SECOND PAGE_SECOND").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	size_t model_requests = server.Snapshot().size();
	std::set<int64_t> sequences;
	int64_t before = 0;
	bool has_more = true;
	bool first_input = false, second_input = false;
	for (int page = 0; has_more && page < 100; page++) {
		auto params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id",
			static_cast<double>(session_id));
		cJSON_AddNumberToObject(params.get(), "limit", 1);
		if (before > 0)
			cJSON_AddNumberToObject(params.get(), "before_sequence",
				static_cast<double>(before));

		auto response = Call("history.list", params.get());
		ASSERT_NE(response, nullptr);
		ASSERT_EQ(response->error_code, 0);
		EXPECT_EQ(runtime_client_test_text(response->data, "order"), "newest_first");
		const cJSON *items = cJSON_GetObjectItemCaseSensitive(response->data, "items");
		ASSERT_TRUE(cJSON_IsArray(items));
		ASSERT_EQ(cJSON_GetArraySize(items), 1);
		const cJSON *item = cJSON_GetArrayItem(items, 0);
		int64_t sequence = static_cast<int64_t>(cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(item, "sequence_no")));
		EXPECT_TRUE(sequences.insert(sequence).second);
		if (before > 0)
			EXPECT_LT(sequence, before);

		std::string content = runtime_client_test_text(item, "content");
		first_input = first_input || content.find("PAGE_FIRST") != std::string::npos;
		second_input = second_input || content.find("PAGE_SECOND") != std::string::npos;
		before = static_cast<int64_t>(cJSON_GetNumberValue(
			cJSON_GetObjectItemCaseSensitive(response->data, "next_before_sequence")));
		EXPECT_EQ(before, sequence);
		has_more = cJSON_IsTrue(
			cJSON_GetObjectItemCaseSensitive(response->data, "has_more"));
	}

	EXPECT_FALSE(has_more);
	EXPECT_TRUE(first_input);
	EXPECT_TRUE(second_input);
	EXPECT_GE(sequences.size(), 4u);
	EXPECT_EQ(server.Snapshot().size(), model_requests);
}

TEST_F(RuntimeClientTest, ScheduledTasksUseRuntimeClientWithoutAFrontendRunner)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n");
	int64_t source_session = Create("Scheduled source");
	struct db storage NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_open(nullptr, &storage, database.c_str()), 0);
	struct scheduled_task_input input{};
	input.source_session_id = source_session;
	input.title = "Headless scheduled execution";
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = static_cast<int64_t>(std::time(nullptr)) - 1;
	input.timeout_at = static_cast<int64_t>(std::time(nullptr)) + 60;
	input.max_attempts = 1;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"CLIENT_SECOND scheduled work\"}";
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
	ASSERT_EQ(scheduled_task_create(&storage, &input, &task), 0);
	struct NotificationCapture {
		int count = 0;
		int64_t session_id = 0;
		std::string body;
	} capture;
	int ran = runtime_tasks_run_due_for_runtime(instance, 1, nullptr, nullptr,
		[](const struct notification *notification, void *opaque) {
			auto *result = static_cast<NotificationCapture *>(opaque);
			result->count++;
			result->session_id = notification->session_id;
			result->body = notification->body ? notification->body : "";
		}, &capture);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(capture.count, 1);
	EXPECT_GT(capture.session_id, 0);
	EXPECT_NE(capture.session_id, source_session);
	EXPECT_EQ(capture.body, "Second session completed");
	struct scheduled_task finished NB_CLEANUP(scheduled_task_cleanup) = {};
	ASSERT_EQ(scheduled_task_get(&storage, task.id, &finished), 0);
	EXPECT_STREQ(finished.status, "completed");
	EXPECT_EQ(finished.latest_session_id, capture.session_id);
	auto result = Request("turn.get", capture.session_id);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(result->data, "status"), "completed");
	EXPECT_TRUE(server.Await("CLIENT_SECOND scheduled work"));
}

TEST_F(RuntimeClientTest, ContextInspectionAndHistoryRepairUseTheNamedSessionActor)
{
	Start();
	int64_t first = Create("Actor history under repair");
	int64_t second = Create("Unrelated actor history");
	ASSERT_FALSE(StartTurn(first, "CLIENT_SECOND ACTOR_FIRST_INPUT").empty());
	ASSERT_NE(Completed(first), nullptr);
	ASSERT_FALSE(StartTurn(second, "CLIENT_SECOND OTHER_ACTOR_INPUT").empty());
	ASSERT_NE(Completed(second), nullptr);
	auto context = Request("context.get", first);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(context->error_code, 0);
	double active_items = cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(context->data, "active_items"));
	EXPECT_GE(active_items, 2);
	EXPECT_GE(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(context->data, "transcript_count")), 2);
	size_t model_calls = server.Snapshot().size();
	auto repaired = Request("history.repair", first);
	ASSERT_NE(repaired, nullptr);
	ASSERT_EQ(repaired->error_code, 0);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(repaired->data, "changed")));
	EXPECT_EQ(server.Snapshot().size(), model_calls);
	context = Request("context.get", first);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(context->data, "active_items")), active_items);
	ASSERT_FALSE(StartTurn(first, "CLIENT_SECOND ACTOR_NEXT_INPUT").empty());
	ASSERT_NE(Completed(first), nullptr);
	auto bodies = server.Snapshot();
	ASSERT_FALSE(bodies.empty());
	EXPECT_NE(bodies.back().find("ACTOR_FIRST_INPUT"), std::string::npos);
	EXPECT_NE(bodies.back().find("ACTOR_NEXT_INPUT"), std::string::npos);
	EXPECT_EQ(bodies.back().find("OTHER_ACTOR_INPUT"), std::string::npos);
	context = Request("context.get", first);
	ASSERT_NE(context, nullptr);
	EXPECT_GE(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(context->data, "active_items")), active_items + 2);
}

TEST_F(RuntimeClientTest, ShutdownCancelsCompressionRunningOnAManagementWorker)
{
	Start();
	int64_t session_id = Create("Compression shutdown owner");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_SECOND compression fixture").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	auto default_response = server.respond;
	server.respond = [this, default_response](const std::string &body) {
		if (body.find("The supplied conversation is historical evidence") ==
		    std::string::npos)
			return default_response(body);

		server.Hold();
		return runtime_client_test_delta("{\"objective\":\"Inspect\","
			"\"constraints\":\"None\","
			"\"completed_work\":\"One turn\",\"working_state\":\"Idle\","
			"\"pending_work\":\"None\",\"verification\":\"Mock response\"}");
	};
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	struct runtime_command command{"shutdown-compression", "history.compress", params.get()};
	ASSERT_EQ(runtime_client_request_copy(client, &command), 0);
	ASSERT_TRUE(server.Await("The supplied conversation is historical evidence"));
	auto started = std::chrono::steady_clock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
	auto response = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			!std::strcmp(message->id, "shutdown-compression");
	});
	ASSERT_NE(response, nullptr);
	EXPECT_NE(response->error_code, 0);
	server.Release();
}

TEST_F(RuntimeClientTest, SessionSettingsSelectActualModelAndWorkingDirectory)
{
	Start();
	std::string workspace_root = std::filesystem::canonical(directory).string();
	std::string first_workdir = workspace_root + "/workspace-a";
	std::string second_workdir = workspace_root + "/workspace-b";
	ASSERT_TRUE(std::filesystem::create_directory(first_workdir));
	ASSERT_TRUE(std::filesystem::create_directory(second_workdir));
	auto params = runtime_client_test_json();
	cJSON_AddStringToObject(params.get(), "name", "Explicit session settings");
	cJSON_AddStringToObject(params.get(), "model", "model-A");
	cJSON_AddStringToObject(params.get(), "workdir", first_workdir.c_str());
	auto created = Call("session.create", params.get());
	ASSERT_NE(created, nullptr);
	ASSERT_EQ(created->error_code, 0);
	int64_t session_id = static_cast<int64_t>(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(created->data, "id")));
	ASSERT_GT(session_id, 0);
	EXPECT_EQ(runtime_client_test_text(created->data, "model"), "model-A");
	EXPECT_EQ(runtime_client_test_text(created->data, "workdir"), first_workdir);
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_WORKDIR first settings").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	{
		std::lock_guard<std::mutex> lock(workdir_mutex);
		ASSERT_EQ(observed_workdirs.size(), 1u);
		EXPECT_EQ(observed_workdirs[0], first_workdir);
	}

	params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "model", "model-B");
	cJSON_AddStringToObject(params.get(), "workdir", second_workdir.c_str());
	auto updated = Call("session.update", params.get());
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(updated->error_code, 0);
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_WORKDIR second settings").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	auto second_result = Request("turn.get", session_id);
	ASSERT_NE(second_result, nullptr);
	std::unique_ptr<char, decltype(&cJSON_free)> diagnostic(
		cJSON_PrintUnformatted(second_result->data), cJSON_free);
	ASSERT_EQ(runtime_client_test_text(second_result->data, "status"), "completed")
		<< (diagnostic ? diagnostic.get() : "No result snapshot");
	std::string request_summary;
	for (const auto &body : server.Snapshot()) {
		RuntimeClientJson parsed(cJSON_Parse(body.c_str()), cJSON_Delete);
		request_summary += "\nmodel=" + runtime_client_test_text(parsed.get(), "model");
		const cJSON *messages = cJSON_GetObjectItemCaseSensitive(parsed.get(), "messages");
		const cJSON *message;
		cJSON_ArrayForEach(message, messages) {
			std::string role = runtime_client_test_text(message, "role");
			if (role == "system" || role == "developer")
				continue;

			std::unique_ptr<char, decltype(&cJSON_free)> encoded(
				cJSON_PrintUnformatted(message), cJSON_free);
			request_summary += "\n" + std::string(encoded ? encoded.get() : "{}");
		}
	}

	{
		std::lock_guard<std::mutex> lock(workdir_mutex);
		EXPECT_EQ(observed_workdirs.size(), 2u) << request_summary;
		if (observed_workdirs.size() >= 2)
			EXPECT_EQ(observed_workdirs[1], second_workdir);
	}

	bool used_first_model = false;
	bool used_second_model = false;
	for (const auto &body : server.Snapshot()) {
		RuntimeClientJson parsed(cJSON_Parse(body.c_str()), cJSON_Delete);
		ASSERT_NE(parsed, nullptr);
		std::string model = runtime_client_test_text(parsed.get(), "model");
		if (body.find("first settings") != std::string::npos && model == "model-A")
			used_first_model = true;

		if (body.find("second settings") != std::string::npos && model == "model-B")
			used_second_model = true;
	}

	EXPECT_TRUE(used_first_model);
	EXPECT_TRUE(used_second_model);
}

TEST_F(RuntimeClientTest, RejectsOverlappingTurnAndStaleCancellation)
{
	Start();
	int64_t session_id = Create("Named interruption");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	auto duplicate = Request("turn.start", session_id, nullptr, "Do not start");
	ASSERT_NE(duplicate, nullptr);
	EXPECT_NE(duplicate->error_code, 0);
	auto stale = Request("turn.interrupt", session_id, "wrong-turn");
	ASSERT_NE(stale, nullptr);
	EXPECT_NE(stale->error_code, 0);
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "turn_id", turn.c_str());
	cJSON_AddStringToObject(params.get(), "preserve_processes", "true");
	auto malformed = Call("turn.interrupt", params.get());
	ASSERT_NE(malformed, nullptr);
	EXPECT_EQ(malformed->error_code, NB_ERR_INVAL);
	auto running = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(running, nullptr);
	EXPECT_EQ(runtime_client_test_text(running->data, "status"), "running");
	auto interrupted = Request("turn.interrupt", session_id, turn.c_str());
	ASSERT_NE(interrupted, nullptr);
	EXPECT_EQ(interrupted->error_code, 0);
	ASSERT_NE(Completed(session_id), nullptr);
	auto status = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(status, nullptr);
	EXPECT_EQ(runtime_client_test_text(status->data, "status"), "interrupted");
}

TEST_F(RuntimeClientTest, SteeringTargetsCurrentTurnAndSurvivesModelInterruption)
{
	Start();
	int64_t session_id = Create("Steering");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	auto stale = Request("turn.steer", session_id, "wrong-turn", "Ignored");
	ASSERT_NE(stale, nullptr);
	EXPECT_NE(stale->error_code, 0);
	auto steering = Request("turn.steer", session_id, turn.c_str(), "CLIENT_STEER");
	ASSERT_NE(steering, nullptr);
	EXPECT_EQ(steering->error_code, 0);
	ASSERT_NE(Completed(session_id), nullptr);
	EXPECT_TRUE(server.Await("CLIENT_STEER"));
	auto status = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(status, nullptr);
	EXPECT_EQ(runtime_client_test_text(status->data, "status"), "completed");
}

TEST_F(RuntimeClientTest, RepliesToRequiredQuestionOnceWhileAnotherSessionRuns)
{
	Start();
	int64_t first = Create("Question");
	int64_t second = Create("Independent work");
	ASSERT_FALSE(StartTurn(first, "CLIENT_QUESTION").empty());
	auto question = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_SERVER_REQUEST;
	});
	ASSERT_NE(question, nullptr);
	ASSERT_NE(question->id, nullptr);
	EXPECT_EQ(question->session_id, first);
	ASSERT_FALSE(StartTurn(second, "CLIENT_SECOND").empty());
	ASSERT_NE(Completed(second), nullptr);
	auto result = runtime_client_test_json();
	cJSON *answers = cJSON_AddArrayToObject(result.get(), "answers");
	cJSON_AddItemToArray(answers, cJSON_CreateString("Stable"));
	struct runtime_reply reply{question->id, result.get(), 0};
	EXPECT_EQ(runtime_client_reply_copy(client, &reply), 0);
	EXPECT_NE(runtime_client_reply_copy(client, &reply), 0);
	ASSERT_NE(Completed(first), nullptr);
	EXPECT_NE(runtime_client_reply_copy(client, &reply), 0);
}

TEST_F(RuntimeClientTest, DisconnectedControllerRequiresExplicitAuthorizedHandoff)
{
	Start();
	int64_t session_id = Create("Question handoff");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_QUESTION").empty());
	auto original = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_SERVER_REQUEST;
	});
	ASSERT_NE(original, nullptr);
	ASSERT_NE(original->id, nullptr);
	std::string interaction_id = original->id;
	runtime_client_close(client);
	client = nullptr;
	pending.clear();
	struct runtime_client_options observer{};
	observer.capabilities = RUNTIME_CLIENT_READ;
	ASSERT_EQ(runtime_client_open(instance, &observer, &client), 0);
	auto denied = Request("session.claim_interactions", session_id);
	ASSERT_NE(denied, nullptr);
	EXPECT_NE(denied->error_code, 0);
	runtime_client_close(client);
	client = nullptr;
	pending.clear();
	ASSERT_EQ(runtime_client_open(instance, nullptr, &client), 0);
	auto subscribed = Request("session.subscribe", session_id);
	ASSERT_NE(subscribed, nullptr);
	ASSERT_EQ(subscribed->error_code, 0);
	for (const auto &message : pending)
		EXPECT_NE(message->kind, RUNTIME_MESSAGE_SERVER_REQUEST);

	auto claimed = Request("session.claim_interactions", session_id);
	ASSERT_NE(claimed, nullptr);
	ASSERT_EQ(claimed->error_code, 0);
	auto transferred = Wait([&](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_SERVER_REQUEST && message->id &&
			interaction_id == message->id;
	});
	ASSERT_NE(transferred, nullptr);
	EXPECT_EQ(transferred->session_id, session_id);
	auto result = runtime_client_test_json();
	cJSON *answers = cJSON_AddArrayToObject(result.get(), "answers");
	cJSON_AddItemToArray(answers, cJSON_CreateString("Preview"));
	struct runtime_reply reply{interaction_id.c_str(), result.get(), 0};
	ASSERT_EQ(runtime_client_reply_copy(client, &reply), 0);
	ASSERT_NE(Completed(session_id), nullptr);
	EXPECT_NE(runtime_client_reply_copy(client, &reply), 0);

	struct runtime_client *idle_observer = nullptr;
	ASSERT_EQ(runtime_client_open(instance, nullptr, &idle_observer), 0);
	std::unique_ptr<runtime_client, decltype(&runtime_client_close)>
		idle_observer_owner(idle_observer, runtime_client_close);
	auto idle_params = runtime_client_test_json();
	cJSON_AddNumberToObject(idle_params.get(), "session_id", static_cast<double>(session_id));
	struct runtime_command view{"idle-view", "permission.get", idle_params.get()};
	ASSERT_EQ(runtime_client_request_copy(idle_observer, &view), 0);
	struct runtime_message *received = nullptr;
	ASSERT_EQ(runtime_client_receive(idle_observer, 10000, &received), 0);
	RuntimeClientMessage viewed(received, runtime_message_destroy);
	ASSERT_NE(viewed, nullptr);
	ASSERT_EQ(viewed->kind, RUNTIME_MESSAGE_RESPONSE);
	ASSERT_STREQ(viewed->id, "idle-view");
	ASSERT_EQ(viewed->error_code, 0);

	struct runtime_command claim{"idle-claim", "session.claim_interactions", idle_params.get()};
	ASSERT_EQ(runtime_client_request_copy(idle_observer, &claim), 0);
	received = nullptr;
	ASSERT_EQ(runtime_client_receive(idle_observer, 10000, &received), 0);
	RuntimeClientMessage rejected(received, runtime_message_destroy);
	ASSERT_NE(rejected, nullptr);
	ASSERT_EQ(rejected->kind, RUNTIME_MESSAGE_RESPONSE);
	ASSERT_STREQ(rejected->id, "idle-claim");
	EXPECT_EQ(rejected->error_code, NB_ERR_BUSY);
}

TEST_F(RuntimeClientTest, ClientsKeepIndependentChildStreamsAfterDistinctSubscriptions)
{
	Start("[agents]\nmulti_agent_mode=\"proactive\"\nmax_total_tokens=0\n", 0, false, true);
	server.respond = [this](const std::string &body) {
		if (body.find("You are `/root/one`") != std::string::npos) {
			server.Hold();
			return runtime_client_test_delta("First child evidence");
		}

		if (body.find("You are `/root/two`") != std::string::npos) {
			server.Hold();
			return runtime_client_test_delta("Second child evidence");
		}

		if (body.find("call_spawn_one") == std::string::npos)
			return runtime_client_test_tool("spawn_agent", "call_spawn_one",
				"{\"task_name\":\"one\",\"message\":\"Inspect independently\","
				"\"fork_turns\":\"none\"}");

		if (body.find("call_spawn_two") == std::string::npos)
			return runtime_client_test_tool("spawn_agent", "call_spawn_two",
				"{\"task_name\":\"two\",\"message\":\"Verify independently\","
				"\"fork_turns\":\"none\"}");

		return runtime_client_test_delta("Collected independent evidence");
	};
	int64_t session_id = Create("Independent child observers");
	ASSERT_FALSE(StartTurn(session_id, "Delegate both independent checks").empty());
	ASSERT_TRUE(server.Await("You are `/root/one`"));
	ASSERT_TRUE(server.Await("You are `/root/two`"));
	auto budget = Request("agent.budget", session_id);
	ASSERT_NE(budget, nullptr);
	ASSERT_EQ(budget->error_code, 0);
	for (const char *field : {"used", "limit", "request_tokens", "input_tokens",
		"cached_tokens", "output_tokens", "reserved"})
		EXPECT_TRUE(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(budget->data, field)));

	EXPECT_GT(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(budget->data, "input_tokens")), 0);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(budget->data, "limit")), 0);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(budget->data, "exhausted")));
	struct runtime_client *observer = nullptr;
	ASSERT_EQ(runtime_client_open(instance, nullptr, &observer), 0);
	std::unique_ptr<runtime_client, decltype(&runtime_client_close)>
		observer_owner(observer, runtime_client_close);
	std::deque<RuntimeClientMessage> observer_pending;
	auto observer_call = [&](const char *method, const cJSON *params) {
		std::string id = "observer-" + std::to_string(++request_sequence);
		struct runtime_command command{id.c_str(), method, params};
		EXPECT_EQ(runtime_client_request_copy(observer, &command), 0);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			struct runtime_message *received = nullptr;
			int rc = runtime_client_receive(observer, 100, &received);
			if (rc == NB_ERR_TIMEOUT)
				continue;

			EXPECT_EQ(rc, 0);
			RuntimeClientMessage message(received, runtime_message_destroy);
			if (!message)
				break;

			if (message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			    id == message->id)
				return message;

			observer_pending.push_back(std::move(message));
		}

		return RuntimeClientMessage(nullptr, runtime_message_destroy);
	};
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	auto subscribed = observer_call("session.subscribe", params.get());
	ASSERT_NE(subscribed, nullptr);
	ASSERT_EQ(subscribed->error_code, 0);
	cJSON_AddStringToObject(params.get(), "agent_id", "/root/one");
	auto first_subscription = Call("agent.subscribe", params.get());
	ASSERT_NE(first_subscription, nullptr);
	ASSERT_EQ(first_subscription->error_code, 0);
	cJSON_ReplaceItemInObjectCaseSensitive(params.get(), "agent_id",
		cJSON_CreateString("/root/two"));
	auto second_subscription = observer_call("agent.subscribe", params.get());
	ASSERT_NE(second_subscription, nullptr);
	ASSERT_EQ(second_subscription->error_code, 0);
	server.Release();
	for (const char *agent : {"/root/one", "/root/two"}) {
		auto final = Wait([&](const runtime_message *message) {
			const cJSON *data = cJSON_GetObjectItemCaseSensitive(message->data, "data");
			return message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
				!std::strcmp(message->method, "react.final") &&
				runtime_client_test_text(data, "agent_id") == agent;
		});
		ASSERT_NE(final, nullptr) << agent;
	}

	std::set<std::string> observer_finals;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (observer_finals.size() < 2 && std::chrono::steady_clock::now() < deadline) {
		struct runtime_message *received = nullptr;
		int rc = runtime_client_receive(observer, 100, &received);
		if (rc == NB_ERR_TIMEOUT)
			continue;

		ASSERT_EQ(rc, 0);
		RuntimeClientMessage message(received, runtime_message_destroy);
		ASSERT_NE(message, nullptr);
		if (message->kind != RUNTIME_MESSAGE_EVENT || !message->method ||
		    std::strcmp(message->method, "react.final"))
			continue;

		const cJSON *data = cJSON_GetObjectItemCaseSensitive(message->data, "data");
		std::string agent = runtime_client_test_text(data, "agent_id");
		if (agent == "/root/one" || agent == "/root/two")
			observer_finals.insert(agent);
	}

	EXPECT_EQ(observer_finals, (std::set<std::string>{"/root/one", "/root/two"}));
	ASSERT_NE(Completed(session_id), nullptr);
	auto parent = Request("turn.get", session_id);
	ASSERT_NE(parent, nullptr);
	EXPECT_EQ(runtime_client_test_text(parent->data, "status"), "completed");
	std::string parent_stream = runtime_client_test_text(parent->data, "stream_text");
	EXPECT_EQ(parent_stream.find("First child evidence"), std::string::npos);
	EXPECT_EQ(parent_stream.find("Second child evidence"), std::string::npos);
}

TEST_F(RuntimeClientTest, DeniedToolApprovalLeavesSideEffectsUntouched)
{
	Start("[permissions]\naccess=\"workspace\"\napproval=\"on_request\"\n"
		"reviewer=\"user\"\n");
	int64_t session_id = Create("Denied operation");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_WRITE").empty());
	auto approval = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_SERVER_REQUEST;
	});
	ASSERT_NE(approval, nullptr);
	EXPECT_EQ(writes.load(), 0);
	auto decision = runtime_client_test_json();
	cJSON_AddStringToObject(decision.get(), "decision", "deny");
	struct runtime_reply reply{approval->id, decision.get(), 0};
	ASSERT_EQ(runtime_client_reply_copy(client, &reply), 0);
	ASSERT_NE(Completed(session_id), nullptr);
	EXPECT_EQ(writes.load(), 0);
}

TEST_F(RuntimeClientTest, FullPermissionExecutesWithoutHumanApproval)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n");
	int64_t session_id = Create("Full operation");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_WRITE").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	EXPECT_EQ(writes.load(), 1);
	for (const auto &message : pending)
		EXPECT_NE(message->kind, RUNTIME_MESSAGE_SERVER_REQUEST);
}

TEST_F(RuntimeClientTest, FullPatchWritesFileAndRetainsCheckpointWithoutHumanCallback)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n", 0, true);
	int64_t session_id = Create("Full tracked patch");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_PATCH").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	std::ifstream file(directory + "/async-client.txt");
	ASSERT_TRUE(file.is_open());
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_EQ(text, "Created by an authorized runtime turn\n");
	auto checkpoints = Request("checkpoint.list", session_id);
	ASSERT_NE(checkpoints, nullptr);
	EXPECT_EQ(checkpoints->error_code, 0);
	for (const auto &message : pending)
		EXPECT_NE(message->kind, RUNTIME_MESSAGE_SERVER_REQUEST);
}

TEST_F(RuntimeClientTest, DeniedConcretePatchPreservesAnAccessibleFile)
{
	Start("[permissions]\naccess=\"read_only\"\napproval=\"on_request\"\n"
		"reviewer=\"user\"\n", 0, true);
	std::ofstream(directory + "/protected.txt") << "External contents must survive\n";
	int64_t session_id = Create("Denied tracked patch");
	ASSERT_GT(session_id, 0);
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_PATCH_DENIED").empty());
	auto approval = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_SERVER_REQUEST;
	});
	ASSERT_NE(approval, nullptr);
	auto decision = runtime_client_test_json();
	cJSON_AddStringToObject(decision.get(), "decision", "deny");
	struct runtime_reply reply{approval->id, decision.get(), 0};
	ASSERT_EQ(runtime_client_reply_copy(client, &reply), 0);
	ASSERT_NE(Completed(session_id), nullptr);
	std::ifstream file(directory + "/protected.txt");
	ASSERT_TRUE(file.is_open());
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_EQ(text, "External contents must survive\n");
}

TEST_F(RuntimeClientTest, WorkspaceMutationWaitIsTargetedAndDoesNotBlockOtherSessions)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n", 0, true);
	int64_t first = Create("Workspace mutation owner");
	int64_t second = Create("Waiting workspace mutation");
	int64_t third = Create("Independent model work");
	ASSERT_FALSE(StartTurn(first, "CLIENT_PATCH CLIENT_HOLD").empty());
	ASSERT_TRUE(server.Await("call_client_patch"));
	ASSERT_TRUE(std::filesystem::exists(directory + "/async-client.txt"));
	std::string second_turn = StartTurn(second, "CLIENT_PATCH second contender");
	ASSERT_FALSE(second_turn.empty());
	ASSERT_TRUE(server.Await("second contender"));
	ASSERT_FALSE(StartTurn(third, "CLIENT_SECOND independent").empty());
	ASSERT_NE(Completed(third), nullptr);
	auto independent = Request("turn.get", third);
	ASSERT_NE(independent, nullptr);
	EXPECT_EQ(runtime_client_test_text(independent->data, "status"), "completed");
	EXPECT_EQ(Completed(second, 100), nullptr);
	auto interrupted = Request("turn.interrupt", second, second_turn.c_str());
	ASSERT_NE(interrupted, nullptr);
	ASSERT_EQ(interrupted->error_code, 0);
	ASSERT_NE(Completed(second), nullptr);
	auto cancelled = Request("turn.get", second, second_turn.c_str());
	ASSERT_NE(cancelled, nullptr);
	EXPECT_EQ(runtime_client_test_text(cancelled->data, "status"), "interrupted");
	std::ifstream file(directory + "/async-client.txt");
	ASSERT_TRUE(file.is_open());
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_EQ(text, "Created by an authorized runtime turn\n");
	size_t contender_requests = 0;
	for (const auto &body : server.Snapshot()) {
		if (body.find("second contender") != std::string::npos)
			contender_requests++;
	}

	EXPECT_EQ(contender_requests, 1u);
	server.Release();
	ASSERT_NE(Completed(first), nullptr);
	auto owner = Request("turn.get", first);
	ASSERT_NE(owner, nullptr);
	EXPECT_EQ(runtime_client_test_text(owner->data, "status"), "completed");
}

TEST_F(RuntimeClientTest, SessionLifecyclePreservesRunningServiceAndWorkspaceClaim)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n", 0, true, false, true);
	auto default_response = server.respond;
	server.respond = [default_response](const std::string &body) {
		if (body.find("CLIENT_SERVICE") == std::string::npos)
			return default_response(body);

		if (body.find("call_service") == std::string::npos)
			return runtime_client_test_tool("exec", "call_service",
				"{\"command\":\"printf SERVICE_READY; sleep 30\","
				"\"service\":true}");

		return runtime_client_test_delta("Service remains available");
	};
	int64_t owner = Create("Service lifecycle owner");
	int64_t contender = Create("Waiting behind service");
	ASSERT_FALSE(StartTurn(owner, "CLIENT_SERVICE").empty());
	ASSERT_NE(Completed(owner), nullptr);
	auto jobs = Request("process.list", owner);
	ASSERT_NE(jobs, nullptr);
	ASSERT_EQ(jobs->error_code, 0);
	ASSERT_TRUE(cJSON_IsArray(jobs->data));
	ASSERT_EQ(cJSON_GetArraySize(jobs->data), 1);
	const cJSON *service = cJSON_GetArrayItem(jobs->data, 0);
	ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(service, "service")));
	std::string process_id = runtime_client_test_text(service, "session_id");
	ASSERT_FALSE(process_id.empty());
	for (const char *method : {"session.update", "session.delete", "session.unload"}) {
		auto params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(owner));
		cJSON_AddStringToObject(params.get(), "name", "Must not close live service");
		auto rejected = Call(method, params.get());
		ASSERT_NE(rejected, nullptr);
		EXPECT_EQ(rejected->error_code, NB_ERR_BUSY) << method;
	}

	auto preserved = Request("session.get", owner);
	ASSERT_NE(preserved, nullptr);
	ASSERT_EQ(preserved->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(preserved->data, "name"), "Service lifecycle owner");
	ASSERT_FALSE(StartTurn(contender, "CLIENT_PATCH").empty());
	ASSERT_TRUE(server.Await("CLIENT_PATCH"));
	EXPECT_EQ(Completed(contender, 100), nullptr);
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(owner));
	cJSON_AddStringToObject(params.get(), "process_id", process_id.c_str());
	auto stopped = Call("process.stop", params.get());
	ASSERT_NE(stopped, nullptr);
	ASSERT_EQ(stopped->error_code, 0);
	ASSERT_NE(Completed(contender), nullptr);
	EXPECT_TRUE(std::filesystem::exists(directory + "/async-client.txt"));
	auto unloaded = Request("session.unload", owner);
	ASSERT_NE(unloaded, nullptr);
	EXPECT_EQ(unloaded->error_code, 0);
	auto deleted = Request("session.delete", owner);
	ASSERT_NE(deleted, nullptr);
	EXPECT_EQ(deleted->error_code, 0);
}

TEST_F(RuntimeClientTest, SessionLifecyclePreservesAnActiveChildFollowup)
{
	Start("[agents]\nmulti_agent_mode=\"proactive\"\nmax_total_tokens=0\n", 0, false, true);
	server.respond = [this](const std::string &body) {
		if (body.find("You are `/root/lifecycle`") != std::string::npos) {
			if (body.find("LIFECYCLE_FOLLOWUP") != std::string::npos)
				server.Hold();

			return runtime_client_test_delta("Child lifecycle result");
		}

		if (body.find("call_spawn_lifecycle") == std::string::npos)
			return runtime_client_test_tool("spawn_agent", "call_spawn_lifecycle",
				"{\"task_name\":\"lifecycle\","
				"\"message\":\"Inspect independently\","
				"\"fork_turns\":\"none\"}");

		return runtime_client_test_delta("Root lifecycle result");
	};
	int64_t session_id = Create("Child followup owner");
	ASSERT_FALSE(StartTurn(session_id, "Coordinate one child").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	pending.clear();
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "agent_id", "/root/lifecycle");
	cJSON_AddStringToObject(params.get(), "message", "LIFECYCLE_FOLLOWUP");
	auto followup = Call("agent.followup", params.get());
	ASSERT_NE(followup, nullptr);
	ASSERT_EQ(followup->error_code, 0);
	ASSERT_TRUE(server.Await("LIFECYCLE_FOLLOWUP"));
	for (const char *method : {"session.unload", "session.update", "session.delete"}) {
		params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id",
			static_cast<double>(session_id));
		cJSON_AddStringToObject(params.get(), "name", "Must preserve active child");
		auto rejected = Call(method, params.get());
		ASSERT_NE(rejected, nullptr);
		EXPECT_EQ(rejected->error_code, NB_ERR_BUSY) << method;
	}

	server.Release();
	auto completed = Wait([&](const runtime_message *message) {
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(message->data, "data");
		return message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
			!std::strcmp(message->method, "agent.completed") &&
			runtime_client_test_text(data, "agent_id") == "/root/lifecycle";
	});
	ASSERT_NE(completed, nullptr);
	auto unloaded = Request("session.unload", session_id);
	ASSERT_NE(unloaded, nullptr);
	EXPECT_EQ(unloaded->error_code, 0);
}

TEST_F(RuntimeClientTest, MalformedDestructiveParametersPreserveCodeHistoryAndPreferences)
{
	Start("[permissions]\naccess=\"full\"\napproval=\"never\"\n"
		"reviewer=\"user\"\n", 0, true);
	int64_t session_id = Create("Protected destructive parameters");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_PATCH").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	auto before = Request("history.list", session_id);
	ASSERT_NE(before, nullptr);
	auto checkpoints = Request("checkpoint.list", session_id);
	ASSERT_NE(checkpoints, nullptr);
	const cJSON *items = cJSON_GetObjectItemCaseSensitive(checkpoints->data, "checkpoints");
	ASSERT_GT(cJSON_GetArraySize(items), 0);
	double checkpoint_id = cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 0), "id"));
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddNumberToObject(params.get(), "checkpoint_id", checkpoint_id);
	cJSON_AddNumberToObject(params.get(), "mode", 123);
	auto invalid_rewind = Call("checkpoint.restore", params.get());
	ASSERT_NE(invalid_rewind, nullptr);
	EXPECT_EQ(invalid_rewind->error_code, NB_ERR_INVAL);
	EXPECT_TRUE(std::filesystem::exists(directory + "/async-client.txt"));
	auto after = Request("history.list", session_id);
	ASSERT_NE(after, nullptr);
	EXPECT_TRUE(cJSON_Compare(before->data, after->data, true));
	params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "scope", "session");
	cJSON_AddStringToObject(params.get(), "key", "response.detail");
	cJSON_AddStringToObject(params.get(), "value", "Preserve this exact detail preference");
	auto preference = Call("preference.update", params.get());
	ASSERT_NE(preference, nullptr);
	ASSERT_EQ(preference->error_code, 0);
	auto preferences_before = Request("preference.read", session_id);
	ASSERT_NE(preferences_before, nullptr);
	std::string expected = runtime_client_test_text(preferences_before->data, "text");
	ASSERT_NE(expected.find("Preserve this exact detail preference"), std::string::npos);
	for (const char *scope : {"\"typo\"", "-1", "0.5", "true", "null"}) {
		params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id",
			static_cast<double>(session_id));
		cJSON_AddItemToObject(params.get(), "scope", cJSON_Parse(scope));
		auto invalid_clear = Call("memory.clear", params.get());
		ASSERT_NE(invalid_clear, nullptr);
		EXPECT_NE(invalid_clear->error_code, 0) << scope;
		auto preferences_after = Request("preference.read", session_id);
		ASSERT_NE(preferences_after, nullptr);
		EXPECT_EQ(runtime_client_test_text(preferences_after->data, "text"), expected);
	}
}

TEST_F(RuntimeClientTest, SessionPermissionTighteningSurvivesActorReload)
{
	Start();
	int64_t session_id = Create("Restricted session");
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "access", "read_only");
	cJSON_AddStringToObject(params.get(), "approval", "never");
	cJSON_AddStringToObject(params.get(), "reviewer", "user");
	cJSON_AddStringToObject(params.get(), "scope", "session");
	auto changed = Call("permission.update", params.get());
	ASSERT_NE(changed, nullptr);
	ASSERT_EQ(changed->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(changed->data, "access"), "read_only");
	auto unloaded = Request("session.unload", session_id);
	ASSERT_NE(unloaded, nullptr);
	ASSERT_EQ(unloaded->error_code, 0);
	auto restored = Request("permission.get", session_id);
	ASSERT_NE(restored, nullptr);
	ASSERT_EQ(restored->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(restored->data, "access"), "read_only");
	params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "name", "Renamed restricted session");
	auto renamed = Call("session.update", params.get());
	ASSERT_NE(renamed, nullptr);
	ASSERT_EQ(renamed->error_code, 0);
	restored = Request("permission.get", session_id);
	ASSERT_NE(restored, nullptr);
	ASSERT_EQ(restored->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(restored->data, "access"), "read_only");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_WRITE").empty());
	ASSERT_NE(Completed(session_id), nullptr);
	EXPECT_EQ(writes.load(), 0);
	for (const auto &message : pending)
		EXPECT_NE(message->kind, RUNTIME_MESSAGE_SERVER_REQUEST);
}

TEST_F(RuntimeClientTest, AnotherTrustedIdentityCannotSteerOrCancelAnOwnedTurn)
{
	Start();
	int64_t session_id = Create("Owned execution");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	struct runtime_client *owner = client;
	client = nullptr;
	struct runtime_client_options options{};
	options.user_id = "different-user";
	options.capabilities = RUNTIME_CLIENT_READ | RUNTIME_CLIENT_EXECUTE;
	ASSERT_EQ(runtime_client_open(instance, &options, &client), 0);
	std::deque<RuntimeClientMessage> owner_pending;
	pending.swap(owner_pending);
	auto steered = Request("turn.steer", session_id, turn.c_str(), "Unauthorized steering");
	ASSERT_NE(steered, nullptr);
	EXPECT_EQ(steered->error_code, NB_ERR_NOT_PERMITTED);
	auto interrupted = Request("turn.interrupt", session_id, turn.c_str());
	ASSERT_NE(interrupted, nullptr);
	EXPECT_EQ(interrupted->error_code, NB_ERR_NOT_PERMITTED);
	runtime_client_close(client);
	client = owner;
	pending.clear();
	pending.swap(owner_pending);
	interrupted = Request("turn.interrupt", session_id, turn.c_str());
	ASSERT_NE(interrupted, nullptr);
	EXPECT_EQ(interrupted->error_code, 0);
	ASSERT_NE(Completed(session_id), nullptr);
}

TEST_F(RuntimeClientTest, DeletedSessionCannotExposeItsCachedTurn)
{
	Start();
	int64_t session_id = Create("Deleted execution");
	std::string turn = StartTurn(session_id, "CLIENT_SECOND private result");
	ASSERT_FALSE(turn.empty());
	ASSERT_NE(Completed(session_id), nullptr);
	auto deleted = Request("session.delete", session_id);
	ASSERT_NE(deleted, nullptr);
	ASSERT_EQ(deleted->error_code, 0);
	auto status = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(status, nullptr);
	EXPECT_EQ(status->error_code, NB_ERR_NOT_FOUND);
	auto subscription = Request("session.subscribe", session_id);
	ASSERT_NE(subscription, nullptr);
	EXPECT_EQ(subscription->error_code, NB_ERR_NOT_FOUND);
}

TEST_F(RuntimeClientTest, UnsubscribeDoesNotCancelExecution)
{
	Start();
	int64_t session_id = Create("Detached view");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	auto unsubscribed = Request("session.unsubscribe", session_id);
	ASSERT_NE(unsubscribed, nullptr);
	ASSERT_EQ(unsubscribed->error_code, 0);
	auto status = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(status, nullptr);
	EXPECT_NE(runtime_client_test_text(status->data, "status"), "interrupted");
	auto subscribed = Request("session.subscribe", session_id);
	ASSERT_NE(subscribed, nullptr);
	ASSERT_EQ(subscribed->error_code, 0);
	server.Release();
	ASSERT_NE(Completed(session_id), nullptr);
}

TEST_F(RuntimeClientTest, ReadOnlyClientCannotStartExecution)
{
	Start();
	int64_t session_id = Create("Read only observer");
	runtime_client_close(client);
	client = nullptr;
	struct runtime_client_options options{};
	options.capabilities = RUNTIME_CLIENT_READ;
	ASSERT_EQ(runtime_client_open(instance, &options, &client), 0);
	auto snapshot = Request("session.get", session_id);
	ASSERT_NE(snapshot, nullptr);
	EXPECT_EQ(snapshot->error_code, 0);
	auto rejected = Request("turn.start", session_id, nullptr, "CLIENT_SECOND");
	ASSERT_NE(rejected, nullptr);
	EXPECT_NE(rejected->error_code, 0);
	EXPECT_TRUE(server.Snapshot().empty());
}

TEST_F(RuntimeClientTest, InvalidRequestsAndReceiveSlotsPreserveOwners)
{
	Start();
	struct runtime_command malformed{"bad", nullptr, nullptr};
	EXPECT_EQ(runtime_client_request_copy(client, &malformed), NB_ERR_INVAL);
	auto unknown = Call("unknown.operation");
	ASSERT_NE(unknown, nullptr);
	EXPECT_NE(unknown->error_code, 0);
	struct runtime_message *occupied = unknown.get();
	EXPECT_EQ(runtime_client_try_receive(client, &occupied), NB_ERR_EXISTS);
	EXPECT_EQ(occupied, unknown.get());
	struct runtime_message *empty = nullptr;
	EXPECT_EQ(runtime_client_receive(client, 1, &empty), NB_ERR_TIMEOUT);
	EXPECT_EQ(empty, nullptr);
}

TEST_F(RuntimeClientTest, RequestCapacityIncludesUnreadResponsesAndRejectsWithoutMutation)
{
	Start();
	runtime_client_close(client);
	client = nullptr;
	struct runtime_client_options options{};
	options.request_capacity = 1;
	ASSERT_EQ(runtime_client_open(instance, &options, &client), 0);
	struct runtime_command first{"capacity-first", "session.list", nullptr};
	ASSERT_EQ(runtime_client_request_copy(client, &first), 0);
	auto params = runtime_client_test_json();
	cJSON_AddStringToObject(params.get(), "name", "Rejected name remains owned");
	struct runtime_command second{"capacity-second", "session.create", params.get()};
	EXPECT_EQ(runtime_client_request_copy(client, &second), NB_ERR_BUSY);
	EXPECT_EQ(runtime_client_test_text(params.get(), "name"), "Rejected name remains owned");
	auto response = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			std::strcmp(message->id, "capacity-first") == 0;
	});
	ASSERT_NE(response, nullptr);
	ASSERT_EQ(runtime_client_request_copy(client, &second), 0);
	response = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			std::strcmp(message->id, "capacity-second") == 0;
	});
	ASSERT_NE(response, nullptr);
	EXPECT_EQ(response->error_code, 0);
}

TEST_F(RuntimeClientTest, FullRequestQueueStillAdmitsTargetedInterruption)
{
	Start();
	int64_t session_id = Create("Interrupt with unread response");
	runtime_client_close(client);
	client = nullptr;
	struct runtime_client_options options{};
	options.request_capacity = 1;
	ASSERT_EQ(runtime_client_open(instance, &options, &client), 0);
	std::string turn_id = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn_id.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	struct runtime_command fill{"unread-normal-response", "session.list", nullptr};
	ASSERT_EQ(runtime_client_request_copy(client, &fill), 0);
	struct runtime_command rejected{"normal-over-capacity", "session.list", nullptr};
	ASSERT_EQ(runtime_client_request_copy(client, &rejected), NB_ERR_BUSY);
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "turn_id", turn_id.c_str());
	struct runtime_command cancel{"reserved-interrupt", "turn.interrupt", params.get()};
	ASSERT_EQ(runtime_client_request_copy(client, &cancel), 0);
	auto response = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_RESPONSE && message->id &&
			std::strcmp(message->id, "reserved-interrupt") == 0;
	});
	ASSERT_NE(response, nullptr);
	EXPECT_EQ(response->error_code, 0);
	ASSERT_NE(Completed(session_id), nullptr);
	auto terminal = Request("turn.get", session_id, turn_id.c_str());
	ASSERT_NE(terminal, nullptr);
	EXPECT_EQ(runtime_client_test_text(terminal->data, "status"), "interrupted");
}

TEST_F(RuntimeClientTest, IdempotentTurnStartReusesPersistedResultAfterRestart)
{
	Start();
	int64_t session_id = Create("Idempotent turn");
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddStringToObject(params.get(), "input", "CLIENT_SECOND idempotent request");
	cJSON_AddStringToObject(params.get(), "idempotency_key", "stable-operation-key");
	auto first = Call("turn.start", params.get());
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(first->error_code, 0);
	std::string turn_id = runtime_client_test_text(first->data, "turn_id");
	ASSERT_FALSE(turn_id.empty());
	ASSERT_NE(Completed(session_id), nullptr);
	size_t requests_before = server.Snapshot().size();
	auto duplicate = Call("turn.start", params.get());
	ASSERT_NE(duplicate, nullptr);
	ASSERT_EQ(duplicate->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(duplicate->data, "turn_id"), turn_id);
	EXPECT_EQ(server.Snapshot().size(), requests_before);
	runtime_client_close(client);
	client = nullptr;
	pending.clear();
	runtime_close(instance);
	instance = nullptr;
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	ASSERT_EQ(runtime_client_open(instance, nullptr, &client), 0);
	duplicate = Call("turn.start", params.get());
	ASSERT_NE(duplicate, nullptr);
	ASSERT_EQ(duplicate->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(duplicate->data, "turn_id"), turn_id);
	EXPECT_EQ(runtime_client_test_text(duplicate->data, "status"), "completed");
	EXPECT_EQ(server.Snapshot().size(), requests_before);
}

TEST_F(RuntimeClientTest, RestartMarksAbandonedPersistedTurnInterruptedWithoutReplaying)
{
	Start();
	int64_t session_id = Create("Recovered execution");
	std::string turn = StartTurn(session_id, "CLIENT_SECOND crash fixture");
	ASSERT_FALSE(turn.empty());
	ASSERT_NE(Completed(session_id), nullptr);
	size_t requests_before = server.Snapshot().size();
	runtime_client_close(client);
	client = nullptr;
	pending.clear();
	runtime_close(instance);
	instance = nullptr;
	sqlite3 *storage = nullptr;
	ASSERT_EQ(sqlite3_open(database.c_str(), &storage), SQLITE_OK);
	std::string sql = "UPDATE runtime_turns SET status='running',owner_epoch='abandoned',"
		"execution_rc=0 WHERE turn_id='" + turn + "'";
	int updated = sqlite3_exec(storage, sql.c_str(), nullptr, nullptr, nullptr);
	sqlite3_close(storage);
	ASSERT_EQ(updated, SQLITE_OK);
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	ASSERT_EQ(runtime_client_open(instance, nullptr, &client), 0);
	auto recovered = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->error_code, 0);
	EXPECT_EQ(runtime_client_test_text(recovered->data, "status"), "interrupted");
	EXPECT_NE(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(recovered->data, "execution_rc")), 0);
	EXPECT_EQ(server.Snapshot().size(), requests_before);
}

TEST_F(RuntimeClientTest, MalformedDomainParametersLeaveSessionsAndHistoryUnchanged)
{
	Start();
	int64_t session_id = Create("Preserved session");
	auto before = Call("session.list");
	ASSERT_NE(before, nullptr);
	ASSERT_EQ(before->error_code, 0);
	const cJSON *before_items = cJSON_IsArray(before->data) ? before->data
		: cJSON_GetObjectItemCaseSensitive(before->data, "sessions");
	ASSERT_TRUE(cJSON_IsArray(before_items));
	int count_before = cJSON_GetArraySize(before_items);
	auto params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "name", 123);
	auto bad_create = Call("session.create", params.get());
	ASSERT_NE(bad_create, nullptr);
	EXPECT_NE(bad_create->error_code, 0);
	params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddItemToObject(params.get(), "model", cJSON_CreateArray());
	cJSON_AddStringToObject(params.get(), "name", "Must not rename");
	auto bad_update = Call("session.update", params.get());
	ASSERT_NE(bad_update, nullptr);
	EXPECT_NE(bad_update->error_code, 0);
	auto preserved = Request("session.get", session_id);
	ASSERT_NE(preserved, nullptr);
	EXPECT_EQ(runtime_client_test_text(preserved->data, "name"), "Preserved session");
	params = runtime_client_test_json();
	cJSON_AddNumberToObject(params.get(), "session_id", static_cast<double>(session_id));
	cJSON_AddNumberToObject(params.get(), "input", 123);
	auto bad_turn = Call("turn.start", params.get());
	ASSERT_NE(bad_turn, nullptr);
	EXPECT_NE(bad_turn->error_code, 0);
	for (const char *field : {"turn_flags", "idempotency_key", "stored_user_input",
		"generate_session_title"}) {
		params = runtime_client_test_json();
		cJSON_AddNumberToObject(params.get(), "session_id",
			static_cast<double>(session_id));
		cJSON_AddStringToObject(params.get(), "input", "Must not execute malformed input");
		cJSON_AddNumberToObject(params.get(), field, 0.5);
		bad_turn = Call("turn.start", params.get());
		ASSERT_NE(bad_turn, nullptr);
		EXPECT_EQ(bad_turn->error_code, NB_ERR_INVAL) << field;
	}

	EXPECT_TRUE(server.Snapshot().empty());
	auto after = Call("session.list");
	ASSERT_NE(after, nullptr);
	const cJSON *after_items = cJSON_IsArray(after->data) ? after->data
		: cJSON_GetObjectItemCaseSensitive(after->data, "sessions");
	ASSERT_TRUE(cJSON_IsArray(after_items));
	EXPECT_EQ(cJSON_GetArraySize(after_items), count_before);
	auto history = Request("history.list", session_id);
	ASSERT_NE(history, nullptr);
	ASSERT_EQ(history->error_code, 0);
	const cJSON *messages = cJSON_GetObjectItemCaseSensitive(history->data, "items");
	ASSERT_TRUE(cJSON_IsArray(messages));
	EXPECT_EQ(cJSON_GetArraySize(messages), 0);
}

TEST_F(RuntimeClientTest, SubscriptionSnapshotFrontierOrdersLiveEvents)
{
	Start();
	int64_t session_id = Create("Ordered observer");
	auto subscribed = Request("session.subscribe", session_id);
	ASSERT_NE(subscribed, nullptr);
	ASSERT_EQ(subscribed->error_code, 0);
	const cJSON *cursor = cJSON_GetObjectItemCaseSensitive(subscribed->data, "cursor");
	ASSERT_TRUE(cJSON_IsNumber(cursor));
	uint64_t frontier = static_cast<uint64_t>(cJSON_GetNumberValue(cursor));
	EXPECT_FALSE(runtime_client_test_text(subscribed->data, "runtime_epoch").empty());
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_SECOND").empty());
	auto completed = Completed(session_id);
	ASSERT_NE(completed, nullptr);
	uint64_t previous = frontier;
	for (const auto &message : pending) {
		if (message->kind != RUNTIME_MESSAGE_EVENT || message->session_id != session_id)
			continue;

		EXPECT_GT(message->sequence, previous);
		previous = message->sequence;
	}

	EXPECT_GT(completed->sequence, previous);
	auto refreshed = Request("session.subscribe", session_id);
	ASSERT_NE(refreshed, nullptr);
	ASSERT_EQ(refreshed->error_code, 0);
	cursor = cJSON_GetObjectItemCaseSensitive(refreshed->data, "cursor");
	ASSERT_TRUE(cJSON_IsNumber(cursor));
	EXPECT_GE(static_cast<uint64_t>(cJSON_GetNumberValue(cursor)), completed->sequence);
}

TEST_F(RuntimeClientTest, ClientDisconnectKeepsAcceptedTurnAlive)
{
	Start();
	int64_t session_id = Create("Disconnected sender");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	runtime_client_close(client);
	client = nullptr;
	pending.clear();
	ASSERT_EQ(runtime_client_open(instance, nullptr, &client), 0);
	auto observed = Request("session.subscribe", session_id);
	ASSERT_NE(observed, nullptr);
	ASSERT_EQ(observed->error_code, 0);
	auto status = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(status, nullptr);
	EXPECT_NE(runtime_client_test_text(status->data, "status"), "interrupted");
	server.Release();
	ASSERT_NE(Completed(session_id), nullptr);
}

TEST_F(RuntimeClientTest, SlowObserverGetsRecoverySignalWithoutBlockingExecution)
{
	Start("", 2);
	int64_t session_id = Create("Slow observer");
	std::string turn = StartTurn(session_id, "CLIENT_HOLD");
	ASSERT_FALSE(turn.empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	struct runtime_client *observer = nullptr;
	ASSERT_EQ(runtime_client_open(instance, nullptr, &observer), 0);
	std::swap(client, observer);
	std::deque<RuntimeClientMessage> slow_pending;
	pending.swap(slow_pending);
	server.Release();
	bool finished = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline) {
		auto status = Request("turn.get", session_id, turn.c_str());
		if (status && runtime_client_test_text(status->data, "status") == "completed") {
			finished = true;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	EXPECT_TRUE(finished);
	runtime_client_close(client);
	client = observer;
	pending.clear();
	pending.swap(slow_pending);
	auto recovery = Wait([](const runtime_message *message) {
		return message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
			std::strcmp(message->method, "runtime.resync_required") == 0;
	});
	ASSERT_NE(recovery, nullptr);
	auto snapshot = Request("session.subscribe", session_id);
	ASSERT_NE(snapshot, nullptr);
	EXPECT_EQ(snapshot->error_code, 0);
	auto result = Request("turn.get", session_id, turn.c_str());
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(runtime_client_test_text(result->data, "status"), "completed");
}

TEST_F(RuntimeClientTest, ShutdownCancelsRunningModelsAndClosesRetainedClient)
{
	Start();
	int64_t session_id = Create("Shutdown execution");
	ASSERT_FALSE(StartTurn(session_id, "CLIENT_HOLD").empty());
	ASSERT_TRUE(server.Await("CLIENT_HOLD"));
	auto start = std::chrono::steady_clock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(8));
	int status = 0;
	for (int i = 0; i < 1000; i++) {
		struct runtime_message *raw = nullptr;
		status = runtime_client_receive(client, 100, &raw);
		runtime_message_destroy(raw);
		if (status == NB_ERR_CLOSED)
			break;
	}

	EXPECT_EQ(status, NB_ERR_CLOSED);
	struct runtime_command rejected{"after-close", "session.list", nullptr};
	EXPECT_EQ(runtime_client_request_copy(client, &rejected), NB_ERR_CLOSED);
}

class RuntimeStoreTest : public ::testing::Test {
protected:
	struct db storage{};
	std::string directory;
	int64_t session_id = 0;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-runtime-store-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		ASSERT_EQ(db_open(nullptr, &storage, (directory + "/data.db").c_str()), 0);
		ASSERT_EQ(db_init_schema(&storage), 0);
		ASSERT_EQ(runtime_store_init(&storage), 0);
		struct session session{};
		ASSERT_EQ(session_create_with_workdir(&storage, "Stored session", "model",
			directory.c_str(), &session), 0);
		session_id = session.id;
	}

	void TearDown() override
	{
		db_close(&storage);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	struct runtime_store_turn_input Turn(const char *id, const char *epoch,
		const char *status = "completed")
	{
		struct runtime_store_turn_input turn{};
		turn.turn_id = id;
		turn.session_id = session_id;
		turn.owner_epoch = epoch;
		turn.user_id = "store-user";
		turn.input = "Original input";
		turn.stored_input = "Original displayed input";
		turn.status = status;
		turn.final_text = "Original result";
		return turn;
	}
};

TEST_F(RuntimeStoreTest, ReturnedRecordsOwnTextAndRejectOccupiedOutputs)
{
	auto turn = Turn("stored-turn", "store-epoch");
	turn.idempotency_key = "stable-key";
	ASSERT_EQ(runtime_store_turn_put(&storage, &turn), 0);
	struct runtime_store_turn_record first NB_CLEANUP(runtime_store_turn_record_cleanup) = {};
	ASSERT_EQ(runtime_store_turn_copy(&storage, session_id, turn.turn_id, &first), 0);
	char *original_owner = first.final_text;
	EXPECT_EQ(runtime_store_turn_copy(&storage, session_id, turn.turn_id, &first),
		NB_ERR_EXISTS);
	EXPECT_EQ(first.final_text, original_owner);
	turn.final_text = "Updated result";
	ASSERT_EQ(runtime_store_turn_put(&storage, &turn), 0);
	EXPECT_STREQ(first.final_text, "Original result");
	struct runtime_store_turn_record second NB_CLEANUP(runtime_store_turn_record_cleanup) = {};
	ASSERT_EQ(runtime_store_turn_by_key_copy(&storage, session_id, "store-user", "stable-key",
		&second), 0);
	EXPECT_STREQ(second.final_text, "Updated result");
	struct runtime_store_turn_record missing NB_CLEANUP(runtime_store_turn_record_cleanup) = {};
	EXPECT_EQ(runtime_store_turn_copy(&storage, session_id, "missing", &missing),
		NB_ERR_NOT_FOUND);
	EXPECT_EQ(missing.turn_id, nullptr);
	EXPECT_EQ(missing.status, nullptr);
	EXPECT_EQ(missing.final_text, nullptr);
}

TEST_F(RuntimeStoreTest, SettingsRoundTripKeepsAbsentOverridesDistinct)
{
	struct runtime_store_session_settings settings{};
	settings.session_id = session_id;
	settings.has_ultra = 1;
	settings.ultra = 1;
	ASSERT_EQ(runtime_store_session_settings_put(&storage, &settings), 0);
	struct runtime_store_session_settings loaded{};
	ASSERT_EQ(runtime_store_session_settings_get(&storage, session_id, &loaded), 0);
	EXPECT_EQ(loaded.has_permission, 0);
	EXPECT_EQ(loaded.has_ultra, 1);
	EXPECT_EQ(loaded.ultra, 1);
	settings.has_permission = 1;
	settings.access = 2;
	settings.approval = 1;
	settings.reviewer = 0;
	settings.has_ultra = 0;
	ASSERT_EQ(runtime_store_session_settings_put(&storage, &settings), 0);
	ASSERT_EQ(runtime_store_session_settings_get(&storage, session_id, &loaded), 0);
	EXPECT_EQ(loaded.has_permission, 1);
	EXPECT_EQ(loaded.access, 2);
	EXPECT_EQ(loaded.approval, 1);
	EXPECT_EQ(loaded.reviewer, 0);
	EXPECT_EQ(loaded.has_ultra, 0);
}

TEST_F(RuntimeStoreTest, RecoveryCandidatesDoNotMutateOtherOwners)
{
	ASSERT_EQ(runtime_store_instance_register(&storage, "live-owner", (int64_t)getpid()), 0);
	ASSERT_EQ(runtime_store_instance_register(&storage, "closed-owner", (int64_t)getpid()), 0);
	ASSERT_EQ(runtime_store_instance_close(&storage, "closed-owner"), 0);
	auto live = Turn("live-turn", "live-owner", "running");
	auto closed = Turn("closed-turn", "closed-owner", "queued");
	auto missing = Turn("missing-turn", "missing-owner", "preparing");
	ASSERT_EQ(runtime_store_turn_put(&storage, &live), 0);
	ASSERT_EQ(runtime_store_turn_put(&storage, &closed), 0);
	ASSERT_EQ(runtime_store_turn_put(&storage, &missing), 0);
	struct runtime_store_instances owners NB_CLEANUP(runtime_store_instances_cleanup) = {};
	ASSERT_EQ(runtime_store_pending_instances_copy(&storage, &owners), 0);
	ASSERT_EQ(owners.count, 3u);
	bool found_live = false, found_closed = false, found_missing = false;
	for (size_t i = 0; i < owners.count; i++) {
		const auto &owner = owners.items[i];
		if (std::strcmp(owner.epoch, "live-owner") == 0)
			found_live = !owner.closed && !owner.missing &&
				owner.process_id == getpid();

		if (std::strcmp(owner.epoch, "closed-owner") == 0)
			found_closed = owner.closed && !owner.missing;

		if (std::strcmp(owner.epoch, "missing-owner") == 0)
			found_missing = owner.missing;
	}

	EXPECT_TRUE(found_live);
	EXPECT_TRUE(found_closed);
	EXPECT_TRUE(found_missing);
	ASSERT_EQ(runtime_store_recover_epoch(&storage, "closed-owner", NB_ERR_INTERRUPTED), 0);
	struct runtime_store_turn_record result NB_CLEANUP(runtime_store_turn_record_cleanup) = {};
	ASSERT_EQ(runtime_store_turn_copy(&storage, session_id, "closed-turn", &result), 0);
	EXPECT_STREQ(result.status, "interrupted");
	EXPECT_EQ(result.execution_rc, NB_ERR_INTERRUPTED);
	runtime_store_turn_record_cleanup(&result);
	ASSERT_EQ(runtime_store_turn_copy(&storage, session_id, "live-turn", &result), 0);
	EXPECT_STREQ(result.status, "running");
}
