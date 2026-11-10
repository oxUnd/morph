#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/tools/multi_agents.h"
#include "agent/tools/apply_patch.h"
#include "agent/tools/exec_tool.h"
#include "agent/react.h"
#include "agent/react_request.h"
#include "agent/react_internal.h"
#include "agent/tokenizer.h"
#include "agent/turn.h"
#include "agent/history.h"
#include "models/llm.h"
#include "runtime/runtime.h"
#include "permission/context.h"
#include "session.h"
#include "util/json.h"
}

#include <arpa/inet.h>
#include <atomic>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
extern "C" {
#include "util/error.h"
}
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AgentControlTest : public testing::Test
{
      protected:
	AgentHttpServer server;
	struct db database{};
	struct session session{};
	struct tool_registry tools{};
	struct model *model = nullptr;
	struct tokenizer *tokenizer = nullptr;
	struct agent_control *control = nullptr;
	struct tool_runtime_context runtime{};
	struct config_agents config{};

	void SetUp() override
	{
		server.start();
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "root", "test-model", &session), 0);
		tool_registry_init(nullptr, &tools);
		std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
		model = model_llm_create(nullptr, "openai", "test-model", url.c_str(), "test-key");
		ASSERT_NE(model, nullptr);
		model->retry_count = 0;
		tokenizer = tokenizer_create(nullptr, "test-model", 128000);
		config.enabled = 1;
		config.max_threads = 6;
		config.max_depth = 2;
		strcpy(config.multi_agent_mode, "proactive");
		reopen();
		runtime.db = &database;
		runtime.memory_session_id = session.id;
		runtime.agent_id = "/root";
	}

	void reopen()
	{
		agent_control_destroy(control);
		control =
			agent_control_create(&tools, model, tokenizer, nullptr, &config, &database);
		ASSERT_NE(control, nullptr);
		ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
		ASSERT_EQ(multi_agents_register(&tools, control), 0);
	}

	void TearDown() override
	{
		agent_control_destroy(control);
		tool_registry_cleanup(&tools);
		model_destroy(model);
		tokenizer_destroy(tokenizer);
		db_close(&database);
	}

	int call(const char *operation, const char *json, cJSON **out = nullptr)
	{
		cJSON *args = cJSON_Parse(json);
		cJSON *result = nullptr;
		int rc = agent_control_call(control, operation, args, &runtime, &result);
		cJSON_Delete(args);
		if (out)
			*out = result;
		else
			cJSON_Delete(result);
		return rc;
	}

	std::string receive(const char *id = "/root", bool ack = true)
	{
		char *text = nullptr;
		int64_t cursor = 0;
		EXPECT_EQ(agent_control_receive(control, id, session.id, &text, &cursor), 0);
		std::string result = text ? text : "";
		free(text);
		if (ack && cursor)
			EXPECT_EQ(agent_control_ack(control, id, session.id, cursor, &database), 0);
		return result;
	}
};

TEST_F(AgentControlTest, ReplacesLegacyToolsAndUsesCodexSchemas)
{
	EXPECT_EQ(tool_borrow(&tools, "delegate"), nullptr);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"a","message":"task","fork_context":true})"),
		  -EINVAL);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"a","message":"task","fork_turns":3})"),
		  -EINVAL);
	EXPECT_EQ(
		call("spawn_agent",
		     R"({"task_name":"a","message":"task","fork_turns":"none","reasoning_effort":""})"),
		-EINVAL);

	EXPECT_EQ(tool_borrow(&tools, "fanout"), nullptr);
	EXPECT_EQ(tool_borrow(&tools, "agent_status"), nullptr);
	for (const char *name : {"spawn_agent", "send_message", "followup_task", "interrupt_agent",
				 "list_agents", "wait_agent"})
		EXPECT_NE(tool_borrow(&tools, name), nullptr);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"../bad","message":"task"})"), -EINVAL);
	EXPECT_EQ(
		call("spawn_agent", R"({"task_name":"a","message":"task","fork_turns":"summary"})"),
		-EINVAL);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"a","message":"task","model":"other"})"),
		  -EINVAL);
}

TEST_F(AgentControlTest, SpawnsIndependentClientsAndDeliversAttributedResult)
{
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"read files","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	EXPECT_EQ(agent_control_active(control, session.id), 0);
	std::string mail = receive();
	EXPECT_NE(mail.find("Sender: /root/reader"), std::string::npos);
	EXPECT_NE(mail.find("FINAL_ANSWER"), std::string::npos);
	EXPECT_NE(mail.find("child result"), std::string::npos);
	EXPECT_TRUE(receive().empty());
	ASSERT_EQ(server.snapshot().size(), 1u);
	EXPECT_TRUE(model->last_error[0] == '\0');
}

TEST_F(AgentControlTest, MessagesDoNotWakeIdleAgentsAndFollowupRetainsHistory)
{
	ASSERT_EQ(
		call("spawn_agent",
		     R"({"task_name":"reader","message":"first assignment","fork_turns":"none"})"),
		0);
	agent_control_join(control);
	ASSERT_EQ(call("send_message", R"({"target":"reader","message":"additional evidence"})"),
		  0);
	EXPECT_EQ(server.snapshot().size(), 1u);
	ASSERT_EQ(call("followup_task", R"({"target":"reader","message":"second assignment"})"), 0);
	agent_control_join(control);
	auto requests = server.snapshot();
	ASSERT_EQ(requests.size(), 2u);
	EXPECT_NE(requests[1].find("first assignment"), std::string::npos);
	EXPECT_NE(requests[1].find("additional evidence"), std::string::npos);
	EXPECT_NE(requests[1].find("second assignment"), std::string::npos);
}

TEST_F(AgentControlTest, RestoresAgentsAndUnreadMessagesWithoutRerunningTools)
{
	ASSERT_EQ(
		call("spawn_agent",
		     R"({"task_name":"reader","message":"first assignment","fork_turns":"none"})"),
		0);
	agent_control_join(control);
	std::string before = receive("/root", false);
	reopen();
	EXPECT_EQ(receive(), before);
	ASSERT_EQ(
		call("followup_task", R"({"target":"reader","message":"continue after restore"})"),
		0);
	agent_control_join(control);
	EXPECT_EQ(server.snapshot().size(), 2u);
}

TEST_F(AgentControlTest, IdleIdentitiesDoNotConsumeExecutionCapacity)
{
	config.max_threads = 2;
	reopen();
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"task"})"), 0);
	agent_control_join(control);
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"second","message":"task"})"), 0);
	agent_control_join(control);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"duplicate"})"), -EEXIST);
	EXPECT_EQ(call("followup_task", R"({"target":"reader","message":"reuse"})"), 0);
	agent_control_join(control);
}

TEST_F(AgentControlTest, ForksHistoryWithoutPendingToolExchange)
{
	struct model_history_insert item{};
	item.session_id = session.id;
	item.kind = "user_message";
	item.role = "user";
	item.content = "original user constraint";
	item.active = 1;
	ASSERT_EQ(model_history_add(&database, &item, nullptr), 0);
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.content = nullptr;
	item.payload_json =
		R"({"tool_calls":[{"id":"pending","name":"spawn_agent","arguments":"{}"}]})";
	ASSERT_EQ(model_history_add(&database, &item, nullptr), 0);
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"inherited task"})"), 0);
	agent_control_join(control);
	auto requests = server.snapshot();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_NE(requests[0].find("original user constraint"), std::string::npos);
	EXPECT_EQ(requests[0].find("\"id\":\"pending\""), std::string::npos);
}

TEST_F(AgentControlTest, ExplicitAndProactiveInstructionsAreIndependentOfReasoning)
{
	morph_buf_t prompt{};
	ASSERT_EQ(morph_buf_init(&prompt, 1024), 0);
	ASSERT_EQ(agent_control_prompt(control, "/root", &prompt), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&prompt), "Proactive multi-agent"), nullptr);
	morph_buf_cleanup(&prompt);
	strcpy(config.multi_agent_mode, "explicit");
	reopen();
	ASSERT_EQ(morph_buf_init(&prompt, 1024), 0);
	ASSERT_EQ(agent_control_prompt(control, "/root", &prompt), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&prompt), "Only spawn agents"), nullptr);
	morph_buf_cleanup(&prompt);
}

TEST_F(AgentControlTest, WaitTimesOutAndCancellationIsObserved)
{
	ASSERT_EQ(call("wait_agent", R"({"timeout_ms":1})"), 0);
	morph_cancel_token token{};
	morph_cancel_token_cancel(&token);
	runtime.http.cancel_token = &token;
	EXPECT_EQ(call("wait_agent", R"({"timeout_ms":30000})"), -ECANCELED);
	EXPECT_EQ(call("wait_agent", R"({"timeout_ms":1.5})"), -EINVAL);
}

TEST_F(AgentControlTest, UltraEffortSelectsProactiveModeAsInCodex)
{
	strcpy(config.multi_agent_mode, "auto");
	strcpy(model->extra_body_json, R"({"reasoning_effort":"ultra"})");
	reopen();
	morph_buf_t prompt{};
	ASSERT_EQ(morph_buf_init(&prompt, 1024), 0);
	ASSERT_EQ(agent_control_prompt(control, "/root", &prompt), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&prompt), "Proactive multi-agent"), nullptr);
	morph_buf_cleanup(&prompt);
	strcpy(model->extra_body_json, R"({"reasoning_effort":"high"})");
	ASSERT_EQ(morph_buf_init(&prompt, 1024), 0);
	ASSERT_EQ(agent_control_prompt(control, "/root", &prompt), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&prompt), "Only spawn agents"), nullptr);
	morph_buf_cleanup(&prompt);
}

TEST_F(AgentControlTest, RootCollectsChildResultBeforeFinalAnswer)
{
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("You are `/root/reader`") != std::string::npos)
			return R"({"content":"verified child result"})";
		if (body.find("verified child result") != std::string::npos)
			return R"({"content":"integrated child result"})";
		if (body.find("call_spawn") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_spawn","type":"function","function":{"name":"spawn_agent","arguments":"{\"task_name\":\"reader\",\"message\":\"inspect files\",\"fork_turns\":\"none\"}"}}]})";
		return R"({"content":"premature root answer"})";
	};
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->llm_model = model;
	react->agents = control;
	react->max_iterations = 10;
	react_set_tool_runtime_context(react, &runtime);
	struct agent_session_runtime session_runtime{};
	session_runtime.db = &database;
	session_runtime.session_id = session.id;
	session_runtime.react = react;
	session_runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn turn{};
	struct agent_turn_input input{};
	input.model_input = "Coordinate the investigation";
	ASSERT_EQ(agent_turn_begin(&turn, &session_runtime, &input), 0);
	EXPECT_EQ(react_run(react, input.model_input, nullptr, nullptr), 0);
	EXPECT_STREQ(react->final_answer, "integrated child result");
	EXPECT_EQ(agent_turn_finish(&turn, nullptr), 0);
	agent_control_join(control);
	EXPECT_TRUE(receive().empty());
	react_context_destroy(react);
}

TEST_F(AgentControlTest, RunningFollowupAndMessagesReachTheSameAgent)
{
	std::mutex gate_mutex;
	std::condition_variable gate;
	bool released = false;
	server.respond = [&](const std::string &body) -> std::string {
		if (body.find("new evidence") == std::string::npos) {
			std::unique_lock<std::mutex> lock(gate_mutex);
			gate.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
		}
		return R"({"content":"work completed"})";
	};
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"initial work","fork_turns":"none"})"),
		  0);
	ASSERT_TRUE(server.await_requests(1));
	EXPECT_EQ(call("followup_task", R"({"target":"reader","message":"new evidence"})"), 0);
	{
		std::lock_guard<std::mutex> lock(gate_mutex);
		released = true;
		gate.notify_all();
	}
	agent_control_join(control);
	auto requests = server.snapshot();
	ASSERT_GE(requests.size(), 2u);
	EXPECT_NE(requests.back().find("new evidence"), std::string::npos);
}

TEST_F(AgentControlTest, InterruptPreservesIdentityForLaterWork)
{
	std::mutex gate_mutex;
	std::condition_variable gate;
	bool released = false;
	server.respond = [&](const std::string &) -> std::string {
		std::unique_lock<std::mutex> lock(gate_mutex);
		gate.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
		return R"({"content":"response"})";
	};
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"slow work","fork_turns":"none"})"),
		  0);
	ASSERT_TRUE(server.await_requests(1));
	EXPECT_EQ(call("interrupt_agent", R"({"target":"reader"})"), 0);
	{
		std::lock_guard<std::mutex> lock(gate_mutex);
		released = true;
		gate.notify_all();
	}
	agent_control_join(control);
	struct agent_info *agents = nullptr;
	int count = 0;
	ASSERT_EQ(agent_control_list_copy(control, session.id, &agents, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(agents[0].status, AGENT_CANCELLED);
	agent_control_free_list(agents, count);
	ASSERT_EQ(call("followup_task", R"({"target":"reader","message":"retry with new scope"})"),
		  0);
	agent_control_join(control);
}

TEST_F(AgentControlTest, SessionsHaveIndependentAgentNamespacesAndMailboxes)
{
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"first session"})"), 0);
	agent_control_join(control);
	struct session other{};
	ASSERT_EQ(session_create(&database, "other", "test-model", &other), 0);
	ASSERT_EQ(agent_control_set_parent_session(control, other.id), 0);
	EXPECT_EQ(call("send_message", R"({"target":"reader","message":"wrong session"})"),
		  -ENOENT);
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"second session","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
	EXPECT_NE(receive().find("child result"), std::string::npos);
}

TEST_F(AgentControlTest, DepthLimitAndSiblingCommunicationUseCanonicalPaths)
{
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"root task"})"), 0);
	agent_control_join(control);
	runtime.agent_id = "/root/reader";
	runtime.agent_depth = 1;
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"nested","message":"child task","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	runtime.agent_id = "/root/reader/nested";
	runtime.agent_depth = 2;
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"deep","message":"too deep"})"), -ELOOP);
	EXPECT_EQ(call("send_message", R"({"target":"/root","message":"nested finding"})"), 0);
	EXPECT_NE(receive().find("nested finding"), std::string::npos);
}

TEST_F(AgentControlTest, ModelOverridesAreScopedToIsolatedAgents)
{
	ASSERT_EQ(
		call("spawn_agent",
		     R"({"task_name":"reader","message":"task","fork_turns":"none","model":"other-model","reasoning_effort":"low"})"),
		0);
	agent_control_join(control);
	auto requests = server.snapshot();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_NE(requests[0].find("other-model"), std::string::npos);
	EXPECT_NE(requests[0].find("low"), std::string::npos);
	EXPECT_STREQ(model->model_id, "test-model");
	EXPECT_TRUE(model->extra_body_json[0] == '\0');
}

TEST_F(AgentControlTest, PersistsChildEventsWithoutMixingTheirOutputIntoRoot)
{
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"task","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	char **events = nullptr;
	int count = 0;
	ASSERT_EQ(agent_control_events_copy(control, "/root/reader", &events, &count), 0);
	EXPECT_GT(count, 0);
	for (int i = 0; i < count; i++)
		EXPECT_NE(strstr(events[i], "/root/reader"), nullptr);
	agent_control_free_events(events, count);
}

TEST_F(AgentControlTest, ChildToolCallsShareThePermissionBoundary)
{
	struct tool_context *permissions = tool_context_create(nullptr, "/tmp", "/tmp");
	ASSERT_NE(permissions, nullptr);
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	tools.permission_context = permissions;
	int executed = 0;
	struct tool_spec spec{};
	spec.name = "write_external";
	spec.description = "Opaque external write";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.user_data = &executed;
	spec.exec = [](const char *, struct tool_result *result, void *opaque) -> int {
		++*static_cast<int *>(opaque);
		return tool_result_success_text_copy(result, "written");
	};
	ASSERT_EQ(tool_register_borrow(&tools, &spec), 0);
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("call_write") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_write","type":"function","function":{"name":"write_external","arguments":"{}"}}]})";
		return R"({"content":"write denied"})";
	};
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"try writing","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	EXPECT_EQ(executed, 0);
	EXPECT_NE(receive().find("write denied"), std::string::npos);
	tools.permission_context = nullptr;
	tool_context_destroy(permissions);
}

TEST_F(AgentControlTest, RuntimeFacadeRunsCollaborationWithSeparateDatabaseConnections)
{
	char directory[] = "/tmp/morph-multi-agent-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/data.db";
	std::string config_path = std::string(directory) + "/config.toml";
	{
		std::ofstream file(config_path);
		file << "[memory]\nenabled=false\n[features]\nhooks=false\n";
	}
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("You are `/root/reader`") != std::string::npos)
			return R"({"content":"runtime child evidence"})";
		if (body.find("runtime child evidence") != std::string::npos)
			return R"({"content":"runtime integrated result"})";
		if (body.find("call_spawn") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_spawn","type":"function","function":{"name":"spawn_agent","arguments":"{\"task_name\":\"reader\",\"message\":\"inspect\",\"fork_turns\":\"none\"}"}}]})";
		return R"({"content":"wait for evidence"})";
	};
	struct runtime_options options{};
	options.db_path = path.c_str();
	options.config_path = config_path.c_str();
	options.workdir = directory;
	options.enable_multi_agent = 1;
	options.after_models_user_data = model;
	options.after_models_cb = [](struct react_context *react, void *opaque) -> int {
		auto *source = static_cast<struct model *>(opaque);
		strcpy(react->llm_model->api_base, source->api_base);
		strcpy(react->llm_model->api_key, "test-key");
		strcpy(react->llm_model->model_id, "test-model");
		react->llm_model->retry_count = 0;
		return 0;
	};
	struct runtime *instance = nullptr;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	struct session root{};
	ASSERT_EQ(runtime_session_current(instance, &root), 0);
	struct runtime_request request{};
	request.session_id = root.id;
	request.model_input = "Coordinate the task";
	struct runtime_result result{};
	EXPECT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_STREQ(result.final_text, "runtime integrated result");
	struct agent_info *agents = nullptr;
	int count = 0;
	EXPECT_EQ(runtime_agent_list(instance, root.id, &agents, &count), 0);
	EXPECT_EQ(count, 1);
	runtime_agent_list_free(agents, count);
	runtime_close(instance);
	std::filesystem::remove_all(directory);
}

TEST_F(AgentControlTest, ResponsesReasoningOverridePreservesOtherOptions)
{
	strcpy(model->adapter, "openai-responses");
	strcpy(model->extra_body_json,
	       R"({"reasoning":{"effort":"ultra","summary":"auto"},"store":false})");
	ASSERT_EQ(model_llm_set_reasoning_effort(model, "low"), 0);
	cJSON *extra = cJSON_Parse(model->extra_body_json);
	ASSERT_NE(extra, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(extra, "reasoning_effort"), nullptr);
	const cJSON *reasoning = cJSON_GetObjectItem(extra, "reasoning");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(reasoning, "effort")), "low");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(reasoning, "summary")), "auto");
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(extra, "store")));
	cJSON_Delete(extra);
}

TEST_F(AgentControlTest, FailedHistoryInsertionLeavesMailboxUnread)
{
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"task","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->llm_model = model;
	react->agents = control;
	react_set_tool_runtime_context(react, &runtime);
	struct agent_session_runtime session_runtime{};
	session_runtime.db = &database;
	session_runtime.session_id = session.id;
	session_runtime.react = react;
	session_runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn turn{};
	struct agent_turn_input input{};
	input.model_input = "Integrate the result";
	ASSERT_EQ(agent_turn_begin(&turn, &session_runtime, &input), 0);
	ASSERT_EQ(db_exec(&database,
			  "CREATE TRIGGER reject_agent_mail BEFORE INSERT ON model_history_items "
			  "WHEN NEW.content LIKE 'Message Type:%' BEGIN "
			  "SELECT RAISE(ABORT,'injected history failure'); END;"),
		  0);
	EXPECT_LT(react_run(react, input.model_input, nullptr, nullptr), 0);
	EXPECT_NE(receive("/root", false).find("child result"), std::string::npos);
	EXPECT_EQ(db_exec(&database, "DROP TRIGGER reject_agent_mail"), 0);
	react_context_destroy(react);
}

TEST_F(AgentControlTest, RestoringActiveAgentMarksInterruptedWithoutModelCalls)
{
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"task","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	ASSERT_EQ(db_exec(&database, "UPDATE agent_threads SET status=1"), 0);
	reopen();
	struct agent_info *agents = nullptr;
	int count = 0;
	ASSERT_EQ(agent_control_list_copy(control, session.id, &agents, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(agents[0].status, AGENT_CANCELLED);
	EXPECT_EQ(agents[0].error_code, -ECANCELED);
	agent_control_free_list(agents, count);
	EXPECT_EQ(server.snapshot().size(), 1u);
}

TEST_F(AgentControlTest, WaitReturnsForUserSteeringWithoutTimingOut)
{
	runtime.input_pending_fn = [](void *) -> int { return 1; };
	cJSON *result = nullptr;
	ASSERT_EQ(call("wait_agent", R"({"timeout_ms":30000})", &result), 0);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(result, "timed_out")));
	cJSON_Delete(result);
}

TEST_F(AgentControlTest, SharedBudgetAggregatesRootAndDescendantsAndPersistsAcrossTurns)
{
	config.max_total_tokens = 100000;
	reopen();
	agent_control_begin_turn(control, nullptr);
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"inspect","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	struct model_usage usage{};
	usage.input_tokens = 20;
	usage.output_tokens = 3;
	model_report_usage(model, &usage);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 40);
	EXPECT_FALSE(budget.exhausted);
	usage.input_tokens = 100000;
	model_report_usage(model, &usage);
	EXPECT_EQ(agent_control_budget_check(control, 0), MORPH_ERR_TOKEN_BUDGET);
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"extra","message":"inspect"})"),
		  MORPH_ERR_TOKEN_BUDGET);
	agent_control_end_turn(control);
	agent_control_begin_turn(control, nullptr);
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_GE(budget.used, 100000);
	EXPECT_TRUE(budget.exhausted);
	agent_control_end_turn(control);
}

TEST_F(AgentControlTest, BudgetPreflightRejectsUnaffordableRequestsBeforeHttp)
{
	config.max_total_tokens = 1;
	reopen();
	agent_control_begin_turn(control, nullptr);
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"inspect","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	EXPECT_TRUE(server.snapshot().empty());
	struct agent_info *agents = nullptr;
	int count = 0;
	ASSERT_EQ(agent_control_list_copy(control, session.id, &agents, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(agents[0].status, AGENT_FAILED);
	EXPECT_EQ(agents[0].error_code, MORPH_ERR_AGENT_BUDGET);
	agent_control_free_list(agents, count);
	agent_control_end_turn(control);
}

TEST_F(AgentControlTest, WriteClaimsRejectOverlapTraversalAndRootConflictsUntilCompletion)
{
	char directory[] = "/tmp/morph-agent-scope-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::filesystem::create_directory(std::string(directory) + "/owned");
	runtime.workdir = directory;
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"scope complete"})";
	};
	ASSERT_EQ(
		call("spawn_agent",
		     R"({"task_name":"writer","message":"edit owned","fork_turns":"none","write_paths":["owned"]})"),
		0);
	ASSERT_TRUE(server.await_requests(1));
	EXPECT_EQ(
		call("spawn_agent",
		     R"({"task_name":"conflict","message":"overlap","write_paths":["owned/new.c"]})"),
		-EBUSY);
	EXPECT_EQ(call("spawn_agent",
		       R"({"task_name":"escape","message":"outside","write_paths":["../"]})"),
		  -EACCES);
	std::string owned = std::string(directory) + "/owned/new.c";
	std::string other = std::string(directory) + "/other.c";
	EXPECT_EQ(agent_control_write_check(control, "/root/writer", owned.c_str()), 0);
	EXPECT_EQ(agent_control_write_check(control, "/root/writer", other.c_str()), -EACCES);
	EXPECT_EQ(agent_control_write_check(control, "/root", owned.c_str()), -EBUSY);
	EXPECT_EQ(agent_control_write_check(control, "/root", other.c_str()), 0);
	EXPECT_TRUE(agent_control_shell_readonly(control, "/root"));
	EXPECT_EQ(call("followup_task",
		       R"({"target":"writer","message":"change scope","write_paths":[]})"),
		  -EBUSY);
	release->set_value();
	agent_control_join(control);
	EXPECT_EQ(agent_control_write_check(control, "/root", owned.c_str()), 0);
	EXPECT_FALSE(agent_control_shell_readonly(control, "/root"));
	EXPECT_TRUE(agent_control_shell_readonly(control, "/root/writer"));
	/* Restored identities cannot silently regain old writable claims. */
	reopen();
	EXPECT_EQ(agent_control_write_check(control, "/root/writer", owned.c_str()), -EACCES);
	std::filesystem::remove_all(directory);
	runtime.workdir = nullptr;
}

TEST_F(AgentControlTest, ActivePatchPreventsNewWritableAssignments)
{
	runtime.workdir = "/tmp";
	agent_control_patch_begin(control);
	EXPECT_EQ(call("spawn_agent",
		       R"({"task_name":"writer","message":"edit","write_paths":["/tmp"]})"),
		  -EBUSY);
	agent_control_patch_end(control);
	EXPECT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"inspect","write_paths":[]})"),
		  0);
	agent_control_join(control);
	EXPECT_EQ(agent_control_write_check(control, "/root/reader", "/tmp/not-owned"), -EACCES);
}

TEST_F(AgentControlTest, ChildPatchAndShellCannotBypassOwnershipEvenWithFullPermission)
{
	char directory[] = "/tmp/morph-agent-write-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	runtime.workdir = directory;
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"reader","message":"inspect","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	auto *permissions = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(permissions, nullptr);
	struct permission_policy policy{PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	ASSERT_EQ(apply_patch_init(&tools, permissions), 0);
	ASSERT_EQ(exec_tool_init(&tools, permissions, nullptr), 0);
	struct tool_runtime_context child = runtime;
	child.agents = control;
	child.agent_id = "/root/reader";
	child.agent_depth = 1;
	struct tool_result result{};
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&tools, "apply_patch",
			    "*** Begin Patch\n*** Add File: forbidden.txt\n+bad\n*** End Patch",
			    &result),
		  -EACCES);
	const char *diagnostic = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(result.envelope, "error"), "message"));
	ASSERT_NE(diagnostic, nullptr);
	EXPECT_NE(strstr(diagnostic, "Write ownership denied"), nullptr);
	EXPECT_NE(strstr(diagnostic, "forbidden.txt"), nullptr);
	EXPECT_NE(strstr(diagnostic, "/root/reader"), nullptr);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&tools, "exec",
			    R"({"command":"printf bad > forbidden.txt","yield_time_ms":1000})",
			    &result),
		  0);
	EXPECT_NE(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "execution_mode")),
		     "sandbox");
	EXPECT_FALSE(std::filesystem::exists(std::string(directory) + "/forbidden.txt"));
	tool_result_cleanup(&result);
	/* A new assignment can explicitly give a previously read-only agent a file. */
	ASSERT_EQ(
		call("followup_task",
		     R"({"target":"reader","message":"edit allowed","write_paths":["allowed.txt"]})"),
		0);
	agent_control_join(control);
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&tools, "apply_patch",
			    "*** Begin Patch\n*** Add File: allowed.txt\n+good\n*** End Patch",
			    &result),
		  0);
	EXPECT_TRUE(std::filesystem::exists(std::string(directory) + "/allowed.txt"));
	tool_result_cleanup(&result);
	tool_registry_cleanup(&tools);
	tool_registry_init(nullptr, &tools);
	tool_context_destroy(permissions);
	std::filesystem::remove_all(directory);
	runtime.workdir = nullptr;
}

TEST_F(AgentControlTest, BudgetFailureReplacesStaleToolErrorWithUsageAndRequestCost)
{
	config.max_total_tokens = 1;
	reopen();
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->llm_model = model;
	react->agents = control;
	react->max_iterations = 3;
	react_set_tool_runtime_context(react, &runtime);
	/* An earlier failed tool can leave a detail before the next LLM request. */
	react_set_event_callback(
		react,
		[](const struct morph_event *event, void *opaque) -> int {
			if (event->name && !strcmp(event->name, "react.thinking"))
				react_set_error_detail_copy(
					static_cast<struct react_context *>(opaque),
					"tool error: Invalid argument");
			return 0;
		},
		react);
	agent_control_begin_turn(control, react);
	EXPECT_EQ(react_run(react, "Investigate the project", nullptr, nullptr),
		  MORPH_ERR_TOKEN_BUDGET);
	ASSERT_NE(react->last_error_detail, nullptr);
	EXPECT_NE(strstr(react->last_error_detail, "0 / 1 weighted units used"), nullptr);
	EXPECT_NE(strstr(react->last_error_detail, "next request needs"), nullptr);
	EXPECT_NE(strstr(react->last_error_detail, "agents.max_total_tokens"), nullptr);
	EXPECT_EQ(strstr(react->last_error_detail, "Invalid argument"), nullptr);
	EXPECT_TRUE(server.snapshot().empty());
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_GT(budget.request_tokens, budget.limit);
	agent_control_end_turn(control);
	react_context_destroy(react);
}

TEST_F(AgentControlTest, SpawnAndIdleFollowupShareTheExecutionLimit)
{
	config.max_threads = 2;
	reopen();
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"idle","message":"task"})"), 0);
	agent_control_join(control);
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"finished"})";
	};
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"busy","message":"task"})"), 0);
	ASSERT_TRUE(server.await_requests(2));
	EXPECT_EQ(call("spawn_agent", R"({"task_name":"extra","message":"task"})"), -EAGAIN);
	EXPECT_EQ(call("followup_task", R"({"target":"idle","message":"more work"})"), -EAGAIN);
	release->set_value();
	agent_control_join(control);
	EXPECT_EQ(call("followup_task", R"({"target":"idle","message":"more work"})"), 0);
	agent_control_join(control);
}

TEST_F(AgentControlTest, HyphenatedTaskNamesWorkWithoutProbeAgents)
{
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"containers-buf-vec","message":"inspect"})"),
		  0);
	agent_control_join(control);
	EXPECT_NE(receive().find("/root/containers-buf-vec"), std::string::npos);
}

TEST_F(AgentControlTest, ParentIntegratesGrandchildEvidenceBeforePublishingItsResult)
{
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("You are `/root/parent/nested`") != std::string::npos)
			return R"({"content":"GRANDCHILD_VERIFIED_EVIDENCE"})";
		if (body.find("GRANDCHILD_VERIFIED_EVIDENCE") != std::string::npos)
			return R"({"content":"PARENT_INTEGRATED_GRANDCHILD_EVIDENCE"})";
		if (body.find("call_nested") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_nested","type":"function","function":{"name":"spawn_agent","arguments":"{\"task_name\":\"nested\",\"message\":\"verify independently\",\"fork_turns\":\"none\"}"}}]})";
		return R"({"content":"premature parent result"})";
	};
	ASSERT_EQ(call("spawn_agent",
		       R"({"task_name":"parent","message":"coordinate","fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	EXPECT_NE(receive().find("PARENT_INTEGRATED_GRANDCHILD_EVIDENCE"), std::string::npos);
	char *mail = nullptr;
	int64_t cursor = 0;
	EXPECT_EQ(agent_control_receive(control, "/root/parent", session.id, &mail, &cursor), 0);
	EXPECT_EQ(mail, nullptr);
	free(mail);
}

TEST_F(AgentControlTest, PrimaryReactWithDefaultIdentityCanApplyPatch)
{
	char directory[] = "/tmp/morph-root-patch-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	auto *permissions = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(permissions, nullptr);
	struct permission_policy policy{PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	ASSERT_EQ(apply_patch_init(&tools, permissions), 0);
	runtime.workdir = directory;
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("root_patch_call") != std::string::npos)
			return R"({"content":"patch finished"})";
		return R"({"tool_calls":[{"index":0,"id":"root_patch_call","type":"function","function":{"name":"apply_patch","arguments":"*** Begin Patch\n*** Add File: root.txt\n+root write works\n*** End Patch"}}]})";
	};
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->llm_model = model;
	react->agents = control;
	react->max_iterations = 3;
	react->workdir = strdup(directory);
	react_set_tool_runtime_context(react, &runtime);
	EXPECT_EQ(react->agent_id[0], '\0');
	EXPECT_EQ(react_run(react, "Write root.txt with apply_patch", nullptr, nullptr), 0);
	EXPECT_TRUE(std::filesystem::exists(std::string(directory) + "/root.txt"));
	EXPECT_EQ(agent_control_write_check(control, "",
					    (std::string(directory) + "/root.txt").c_str()),
		  0);
	react_context_destroy(react);
	tool_registry_cleanup(&tools);
	tool_registry_init(nullptr, &tools);
	tool_context_destroy(permissions);
	std::filesystem::remove_all(directory);
	runtime.workdir = nullptr;
}

TEST_F(AgentControlTest, NestedWriterBorrowsParentScopeUntilItsExecutionFinishes)
{
	char directory[] = "/tmp/morph-nested-scope-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::filesystem::create_directory(std::string(directory) + "/owned");
	runtime.workdir = directory;
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"finished scoped work"})";
	};
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"parent","message":"coordinate",
		"fork_turns":"none","write_paths":["owned"]})"),
		  0);
	ASSERT_TRUE(server.await_requests(1));
	runtime.agent_id = "/root/parent";
	runtime.agent_depth = 1;
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"nested","message":"edit one file",
		"fork_turns":"none","write_paths":["owned/a.c"]})"),
		  0);
	std::string path = std::string(directory) + "/owned/a.c";
	std::string other = std::string(directory) + "/owned/b.c";
	EXPECT_EQ(agent_control_write_check(control, "/root/parent/nested", path.c_str()), 0);
	EXPECT_EQ(agent_control_write_check(control, "/root/parent", path.c_str()), -EBUSY);
	EXPECT_EQ(agent_control_write_check(control, "/root", path.c_str()), -EBUSY);
	EXPECT_EQ(agent_control_write_check(control, "/root/parent", other.c_str()), 0);
	EXPECT_EQ(agent_control_write_check(control, "/root/parent/nested", other.c_str()),
		  -EACCES);
	release->set_value();
	agent_control_join(control);
	EXPECT_EQ(agent_control_write_check(control, "/root/parent", path.c_str()), 0);
	runtime.agent_id = "/root";
	runtime.agent_depth = 0;
	runtime.workdir = nullptr;
	std::filesystem::remove_all(directory);
}

TEST_F(AgentControlTest, ReadonlySiblingCannotReassignAnotherAgentsWriteAuthority)
{
	runtime.workdir = "/tmp";
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"worker","message":"inspect"})"), 0);
	agent_control_join(control);
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"reader","message":"inspect"})"), 0);
	agent_control_join(control);
	runtime.agent_id = "/root/reader";
	runtime.agent_depth = 1;
	EXPECT_EQ(call("followup_task", R"({"target":"/root/worker","message":"edit outside",
		"write_paths":["/tmp"]})"),
		  -EACCES);
	runtime.agent_id = "/root";
	runtime.agent_depth = 0;
	runtime.workdir = nullptr;
}

TEST_F(AgentControlTest, WriteOwnershipResolvesNewDirectoriesWithoutUnresolvedTraversal)
{
	char directory[] = "/tmp/morph-new-scope-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	runtime.workdir = directory;
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"writer","message":"create new module",
		"write_paths":["new_module"]})"),
		  0);
	agent_control_join(control);
	std::string path = std::string(directory) + "/new_module/src/new.c";
	EXPECT_EQ(agent_control_write_check(control, "/root/writer", path.c_str()), 0);
	EXPECT_EQ(agent_control_write_check(control, "", path.c_str()), 0);
	std::string ambiguous = std::string(directory) + "/missing/../new_module/x.c";
	EXPECT_EQ(agent_control_write_check(control, "/root/writer", ambiguous.c_str()), -EACCES);
	std::filesystem::remove_all(directory);
	runtime.workdir = nullptr;
}

TEST_F(AgentControlTest, QueuedFollowupNeverEmitsACompletedEventWithRunningStatus)
{
	struct Observer {
		struct agent_control *control;
		struct tool_runtime_context *runtime;
		std::atomic<bool> followed{false};
		std::atomic<int> invalid_completions{0};
		std::atomic<int> completions{0};
	} observer{control, &runtime};
	agent_control_set_event_callback(
		control,
		[](const struct morph_event *event, void *opaque) -> int {
			auto *state = static_cast<Observer *>(opaque);
			if (event->name && !strcmp(event->name, "react.turn.end") &&
			    !state->followed.exchange(true)) {
				cJSON *args = cJSON_Parse(
					R"({"target":"worker","message":"new followup evidence"})");
				cJSON *out = nullptr;
				int rc = agent_control_call(state->control, "followup_task", args,
							    state->runtime, &out);
				cJSON_Delete(args);
				cJSON_Delete(out);
				return rc;
			}
			if (event->name && !strcmp(event->name, "agent.completed")) {
				const char *status = cJSON_GetStringValue(
					cJSON_GetObjectItemCaseSensitive(event->data, "status"));
				state->completions++;
				if (status &&
				    (!strcmp(status, "running") || !strcmp(status, "pending")))
					state->invalid_completions++;
			}
			return 0;
		},
		&observer);
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"task result"})";
	};
	ASSERT_EQ(call("spawn_agent", R"({"task_name":"worker","message":"first task"})"), 0);
	ASSERT_TRUE(server.await_requests(1));
	ASSERT_EQ(agent_control_select(control, "/root/worker"), 0);
	release->set_value();
	agent_control_join(control);
	EXPECT_TRUE(observer.followed);
	EXPECT_EQ(observer.invalid_completions, 0);
	EXPECT_EQ(observer.completions, 1);
	agent_control_set_event_callback(control, nullptr, nullptr);
}
