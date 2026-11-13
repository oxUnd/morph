#include "runtime_test_support.hpp"
#include "nb/err.h"

extern "C" {
#include "models/llm.h"
#include "runtime/runtime_internal.h"
#include "runtime/checkpoint.h"
#include "permission/context_internal.h"
#include "agent/tools/exec_tool.h"
#include "agent/tool_runtime.h"
#include "nb/cleanup.h"
#include "cJSON.h"
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct RuntimeScriptedModel {
	struct model *model = nullptr;
	std::string response = "Thought: complete\nFinal: scripted answer";
	std::mutex mutex;
	std::condition_variable condition;
	bool block = false;
	bool entered = false;
	bool released = false;
	int error = 0;
	int calls = 0;
	void *observed_usage_user_data = nullptr;
	std::vector<std::string> observed_messages;
	std::vector<std::string> stream_chunks;
	std::function<void()> after_stream_chunk;
	std::vector<std::pair<std::string, std::string>> actions;
	size_t action_cursor = 0;
};

static int runtime_scripted_chat(struct model *self, const char *, const char **messages, int count,
				 const struct model_chat_options *, sse_callback callback,
				 void *user_data)
{
	auto *state = static_cast<RuntimeScriptedModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->calls++;
		state->observed_usage_user_data =
			self->usage_sink ? self->usage_sink->user_data : nullptr;
		for (int i = 0; i < count; i++)
			state->observed_messages.emplace_back(messages[i] ? messages[i] : "");
		state->entered = true;
		state->condition.notify_all();
		state->condition.wait(lock, [&] { return !state->block || state->released; });
	}
	if (state->error)
		return state->error;
	if (callback)
		callback(state->response.c_str(), user_data);
	return 200;
}

static int runtime_scripted_chat_with_tools(struct model *self, const char *,
					    struct chat_message *messages, int count,
					    struct tool_desc *, int, struct chat_response *response,
					    sse_callback callback, void *user_data)
{
	auto *state = static_cast<RuntimeScriptedModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->calls++;
		state->observed_usage_user_data =
			self->usage_sink ? self->usage_sink->user_data : nullptr;
		for (int i = 0; i < count; i++) {
			std::string item = messages[i].role ? messages[i].role : "";
			item += ":";
			item += messages[i].content ? messages[i].content : "";
			state->observed_messages.push_back(std::move(item));
		}
		state->entered = true;
		state->condition.notify_all();
		state->condition.wait(lock, [&] { return !state->block || state->released; });
	}
	if (state->error)
		return state->error;
	for (const auto &chunk : state->stream_chunks) {
		if (callback) {
			int rc = callback(chunk.c_str(), user_data);
			if (rc != 0)
				return rc;
		}
		if (state->after_stream_chunk)
			state->after_stream_chunk();
	}
	if (state->action_cursor < state->actions.size()) {
		const auto &action = state->actions[state->action_cursor++];

		response->tool_calls = static_cast<struct tool_call *>(
			std::calloc(1, sizeof(*response->tool_calls)));
		if (!response->tool_calls)
			return NB_ERR_NOMEM;
		response->tool_call_count = 1;
		auto &call = response->tool_calls[0];
		call.id = strdup(("checkpoint_test_" +
		std::to_string(state->action_cursor)).c_str());
		call.name = strdup(action.first.c_str());
		call.input_kind = action.first == "apply_patch" ? TOOL_INPUT_TEXT : TOOL_INPUT_JSON;
		call.arguments = strdup(action.second.c_str());
		return call.id && call.name && call.arguments ? 200 : NB_ERR_NOMEM;
	}
	response->content = strdup(state->response.c_str());
	return response->content ? 200 : NB_ERR_NOMEM;
}

static void runtime_scripted_model_destroy(struct model *model)
{
	std::free(model);
}

static int runtime_install_scripted_model(struct react_context *react,
					   void *user_data)
{
	auto *state = static_cast<RuntimeScriptedModel *>(user_data);
	react->llm_model = state->model;
	return 0;
}

static struct model *runtime_create_scripted_model(RuntimeScriptedModel *state)
{
	auto *model = static_cast<struct model *>(
		std::calloc(1, sizeof(struct model)));
	if (!model)
		return nullptr;
	std::strncpy(model->provider, "runtime-test", sizeof(model->provider) - 1);
	std::strncpy(model->model_id, "scripted", sizeof(model->model_id) - 1);
	std::strncpy(model->api_key, "test-key", sizeof(model->api_key) - 1);
	model->context_limit = 128000;
	model->max_tokens = 1024;
	model->chat = runtime_scripted_chat;
	model->chat_with_tools = runtime_scripted_chat_with_tools;
	model->destroy = runtime_scripted_model_destroy;
	model->handle = state;
	state->model = model;
	return model;
}

class RuntimeExecuteTest : public RuntimeFacadeTest {
protected:
	RuntimeScriptedModel scripted;

	void OpenScripted(bool checkpoint_tools = false)
	{
		ASSERT_NE(runtime_create_scripted_model(&scripted), nullptr);
		WriteConfig("[memory]\nenabled = false\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.after_models_cb = runtime_install_scripted_model;
		options.after_models_user_data = &scripted;
		options.enable_apply_patch = checkpoint_tools;
		Open(&options);
	}

	void TearDown() override
	{
		RuntimeFacadeTest::TearDown();
		if (scripted.model)
			model_destroy(scripted.model);
		scripted.model = nullptr;
	}

	int Execute(const char *input, struct runtime_result *result)
	{
		struct runtime_request request{};
		request.model_input = input;
		request.stored_user_input = input;
		request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
		return runtime_execute_turn(instance, &request, result);
	}

	void ExpectCancelledRequestInNextInput(const char *original, const char *steering)
	{
		cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
		ASSERT_EQ(runtime_session_last_request_copy(instance, &snapshot), 0);
		const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "input_json"));
		ASSERT_NE(input, nullptr);
		cJSON *parsed NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(input);
		ASSERT_NE(parsed, nullptr);
		cJSON *messages = cJSON_GetObjectItem(parsed, "messages");
		ASSERT_TRUE(cJSON_IsArray(messages));
		int original_count = 0;
		int steering_count = 0;
		int continuation_count = 0;
		cJSON *message = nullptr;
		cJSON_ArrayForEach(message, messages) {
			const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
			const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "content"));
			ASSERT_NE(role, nullptr);
			EXPECT_STRNE(role, "tool");
			EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(message, "tool_calls")), 0);
			if (!text)
				continue;
			EXPECT_EQ(strstr(text, "PARTIAL_CANCELLED_RESPONSE"), nullptr);
			if (strncmp(text, original, strlen(original)) == 0) {
				original_count++;
				EXPECT_STREQ(role, "user");
				EXPECT_NE(strstr(text, "Historical turn execution ended: cancelled"),
					nullptr);
				EXPECT_NE(strstr(text, "Tool side effects are not rolled back"), nullptr);
			}
			if (steering && strncmp(text, steering, strlen(steering)) == 0) {
				steering_count++;
				EXPECT_STREQ(role, "user");
				EXPECT_NE(strstr(text, "cancelled"), nullptr);
			}
			if (strcmp(text, "continue") == 0)
				continuation_count++;
		}
		EXPECT_EQ(original_count, 1);
		EXPECT_EQ(steering_count, steering ? 1 : 0);
		EXPECT_EQ(continuation_count, 1);
	}
};

static int runtime_count_events(const struct morph_event *, void *user_data)
{
	(*static_cast<int *>(user_data))++;
	return 0;
}

static int runtime_count_action_drains(void *user_data,
			       struct react_action *, int)
{
	(*static_cast<int *>(user_data))++;
	return 0;
}

struct RuntimePendingSteering {
	bool delivered = false;
};

static int runtime_cancel_test_steering(void *user_data, struct react_action *out, int)
{
	auto *state = static_cast<RuntimePendingSteering *>(user_data);
	if (state->delivered)
		return 0;
	state->delivered = true;
	out->type = "prompt";
	out->payload_json = "{\"text\":\"preserve component padding\"}";
	return 1;
}

static void runtime_cancel_test_messages_cleanup(struct message **messages)
{
	runtime_session_messages_free(*messages);
	*messages = nullptr;
}

TEST_F(RuntimeExecuteTest, SuccessfulTurnPersistsUserAndAssistantMessages)
{
	struct runtime_result result{};
	OpenScripted();
	ASSERT_EQ(Execute("hello runtime", &result), 0);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	ASSERT_NE(result.final_text, nullptr);
	EXPECT_STREQ(result.final_text, "scripted answer");
	int count = 0;
	struct message *messages NB_CLEANUP(runtime_cancel_test_messages_cleanup) =
		runtime_session_messages_current_copy(instance, &count);
	ASSERT_NE(messages, nullptr);
	EXPECT_EQ(count, 2);
	EXPECT_STREQ(messages->role, "user");
	EXPECT_STREQ(messages->content, "hello runtime");
	ASSERT_NE(messages->next, nullptr);
	EXPECT_STREQ(messages->next->role, "assistant");
	EXPECT_STREQ(messages->next->content, "scripted answer");
}

TEST_F(RuntimeExecuteTest, SecondTurnReceivesEarlierSessionHistory)
{
	struct runtime_result result{};
	OpenScripted();
	ASSERT_EQ(Execute("first question", &result), 0);
	ASSERT_EQ(Execute("second question", &result), 0);
	EXPECT_EQ(scripted.calls, 2);
	bool saw_first = false;
	for (const auto &message : scripted.observed_messages) {
		if (message.find("first question") != std::string::npos)
			saw_first = true;
	}
	EXPECT_TRUE(saw_first);
	int count = 0;
	struct message *messages = runtime_session_messages_current_copy(instance, &count);
	EXPECT_EQ(count, 4);
	runtime_session_messages_free(messages);
}

TEST_F(RuntimeExecuteTest, CapturesDispatchedInputWithoutTheLaterAssistantAnswer)
{
	struct runtime_result result{};
	OpenScripted();
	ASSERT_EQ(Execute("inspect actual model input", &result), 0);
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_session_last_request_copy(instance, &snapshot), 0);
	const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "input_json"));
	ASSERT_NE(input, nullptr);
	EXPECT_NE(strstr(input, "inspect actual model input"), nullptr);
	EXPECT_EQ(strstr(input, "scripted answer"), nullptr);
	EXPECT_NE(strstr(input, "messages"), nullptr);
	EXPECT_NE(strstr(input, "tools"), nullptr);
	cJSON_DeleteOwner(&snapshot);
}

TEST_F(RuntimeExecuteTest, MemoryMaintenanceNeverEntersTheDispatchedPrompt)
{
	OpenScripted();
	struct session session{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "receipt-policy", &session), 0);
	struct db handle{};
	ASSERT_EQ(db_open(NULL, &handle, database.c_str()), 0);
	struct model_history_insert item{};
	item.session_id = session.id;
	item.kind = "background_receipt";
	item.role = "system";
	item.active = 1;
	item.content = "[Background receipt] memory_consolidation: memory consolidation queued";
	item.payload_json = "{\"task\":\"memory_consolidation\",\"phase\":\"ready\"}";
	ASSERT_EQ(model_history_add(&handle, &item, nullptr), 0);
	item.content = "[Background receipt] build: archive created";
	item.payload_json = "{\"task\":\"build\",\"phase\":\"end\"}";
	ASSERT_EQ(model_history_add(&handle, &item, nullptr), 0);
	struct runtime_result result{};
	ASSERT_EQ(Execute("continue after the background build", &result), 0);
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_session_last_request_copy(instance, &snapshot), 0);
	const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "input_json"));
	ASSERT_NE(input, nullptr);
	EXPECT_EQ(strstr(input, "memory_consolidation"), nullptr);
	EXPECT_EQ(strstr(input, "memory consolidation queued"), nullptr);
	EXPECT_NE(strstr(input, "archive created"), nullptr);
	EXPECT_EQ(model_history_count(&handle, session.id, 0), 4);
	EXPECT_EQ(model_history_count(&handle, session.id, 1), 3);
	cJSON_DeleteOwner(&snapshot);
	db_close(&handle);
}

TEST_F(RuntimeExecuteTest, InputSnapshotFailureDoesNotPreventModelDispatch)
{
	struct runtime_result result{};
	struct db database_handle{};
	OpenScripted();
	ASSERT_EQ(db_open(NULL, &database_handle, database.c_str()), 0);
	ASSERT_EQ(db_exec(&database_handle, "CREATE TRIGGER fail_input_snapshot BEFORE INSERT ON "
		"model_request_snapshots BEGIN SELECT RAISE(ABORT,'test failure'); END"), 0);
	ASSERT_EQ(Execute("still execute this request", &result), 0);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_EQ(scripted.calls, 1);
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_EQ(runtime_session_last_request_copy(instance, &snapshot), NB_ERR_NO_DATA);
	db_close(&database_handle);
}

TEST_F(RuntimeExecuteTest, TurnStatusIsADeepCopiedSnapshot)
{
	struct runtime_result result{};
	struct runtime_turn_status first{};
	OpenScripted();
	ASSERT_EQ(Execute("snapshot one", &result), 0);
	ASSERT_EQ(runtime_turn_status_copy(instance, &first), 0);
	ASSERT_NE(first.final_answer, nullptr);
	EXPECT_STREQ(first.final_answer, "scripted answer");
	scripted.response = "Thought: complete\nFinal: changed answer";
	ASSERT_EQ(Execute("snapshot two", &result), 0);
	EXPECT_STREQ(first.final_answer, "scripted answer");
	runtime_turn_status_cleanup(&first);
	EXPECT_EQ(first.final_answer, nullptr);
	EXPECT_EQ(first.step_count, 0);
}

TEST_F(RuntimeExecuteTest, PerRequestEventCallbackReceivesTurnEvents)
{
	struct runtime_result result{};
	struct runtime_request request{};
	int events = 0;
	OpenScripted();
	request.model_input = "emit events";
	request.stored_user_input = "emit events";
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	request.event_cb = runtime_count_events;
	request.event_user_data = &events;
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_GT(events, 0);
}

TEST_F(RuntimeExecuteTest, PerRequestActionDrainIsBoundForOneTurn)
{
	struct runtime_result result{};
	struct runtime_request request{};
	int drains = 0;
	OpenScripted();
	request.model_input = "drain actions";
	request.stored_user_input = "drain actions";
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	request.action_drain_fn = runtime_count_action_drains;
	request.action_drain_user_data = &drains;
	request.override_action_drain = 1;
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_GT(drains, 0);
	int first_turn_drains = drains;
	ASSERT_EQ(Execute("no action drain", &result), 0);
	EXPECT_EQ(drains, first_turn_drains);
}

TEST_F(RuntimeExecuteTest, ModelFailureReturnsFailedOutcomeAndErrorSnapshot)
{
	struct runtime_result result{};
	OpenScripted();
	scripted.error = NB_ERR_IO;
	EXPECT_EQ(Execute("fail", &result), NB_ERR_IO);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_FAILED);
	char *json = runtime_turn_error_json_copy(instance);
	ASSERT_NE(json, nullptr);
	EXPECT_NE(std::strstr(json, "llm_error"), nullptr);
	std::free(json);
}

TEST_F(RuntimeExecuteTest, ConcurrentTurnIsBusyAndCancellationIsObserved)
{
	struct runtime_result first{};
	struct runtime_result second{};
	int first_rc = 0;
	OpenScripted();
	scripted.block = true;
	std::thread worker([&] { first_rc = Execute("blocking", &first); });
	bool entered;
	{
		std::unique_lock<std::mutex> lock(scripted.mutex);
		entered = scripted.condition.wait_for(lock, std::chrono::seconds(2),
			[&] { return scripted.entered; });
	}
	EXPECT_TRUE(entered);
	if (entered) {
		EXPECT_EQ(Execute("concurrent", &second), NB_ERR_BUSY);
		EXPECT_EQ(second.outcome, RUNTIME_OUTCOME_BUSY);
	}
	runtime_cancel_turn(instance);
	{
		std::lock_guard<std::mutex> lock(scripted.mutex);
		scripted.released = true;
	}
	scripted.condition.notify_all();
	worker.join();
	EXPECT_NE(first_rc, 0);
	EXPECT_EQ(first.outcome, RUNTIME_OUTCOME_CANCELLED);
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	ASSERT_EQ(Execute("continue", &second), 0);
	ExpectCancelledRequestInNextInput("blocking", nullptr);
}

TEST_F(RuntimeExecuteTest, CancelledStreamPreservesRequestAndSteeringInNextDispatch)
{
	OpenScripted();
	scripted.stream_chunks = {"PARTIAL_CANCELLED_RESPONSE", "discarded stream chunk"};
	scripted.after_stream_chunk = [&] { runtime_cancel_turn(instance); };
	RuntimePendingSteering steering;
	struct runtime_request request{};
	request.model_input = "complete the current feed page";
	request.stored_user_input = request.model_input;
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	request.override_action_drain = 1;
	request.action_drain_fn = runtime_cancel_test_steering;
	request.action_drain_user_data = &steering;
	struct runtime_result cancelled{};
	EXPECT_EQ(runtime_execute_turn(instance, &request, &cancelled), NB_ERR_CANCELLED);
	EXPECT_EQ(cancelled.outcome, RUNTIME_OUTCOME_CANCELLED);
	EXPECT_TRUE(steering.delivered);
	scripted.stream_chunks.clear();
	scripted.after_stream_chunk = {};
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	struct runtime_result continued{};
	ASSERT_EQ(Execute("continue", &continued), 0);
	EXPECT_EQ(continued.outcome, RUNTIME_OUTCOME_COMPLETED);
	ExpectCancelledRequestInNextInput(request.model_input, "preserve component padding");
	int count = 0;
	struct message *messages NB_CLEANUP(runtime_cancel_test_messages_cleanup) =
		runtime_session_messages_current_copy(instance, &count);
	int original_count = 0;
	for (auto *message = messages; message; message = message->next) {
		if (strcmp(message->role, "user") == 0 &&
		    strcmp(message->content, request.model_input) == 0)
			original_count++;
	}
	EXPECT_EQ(original_count, 1);
}

TEST_F(RuntimeExecuteTest, CancelledToolExecutionKeepsRequestWithoutReplayingToolChain)
{
	OpenScripted();
	struct CancelState {
		struct runtime *instance;
		std::atomic<int> executions{0};
	} state{instance};
	struct tool_spec tool{};
	tool.name = "cancel_fixture";
	tool.description = "Cancel the current turn during a tool execution";
	tool.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	tool.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	tool.flags = TOOL_FLAG_READONLY;
	tool.user_data = &state;
	tool.exec = [](const char *, struct tool_result *result, void *user_data) -> int {
		auto *context = static_cast<CancelState *>(user_data);
		context->executions.fetch_add(1);
		int rc = tool_result_error_copy(result, "CANCELLED", "Cancelled fixture execution");
		if (rc != 0)
			return rc;
		runtime_cancel_turn(context->instance);
		return NB_ERR_CANCELLED;
	};
	ASSERT_EQ(tool_register_borrow(&instance->context.tools, &tool), 0);
	scripted.actions.emplace_back("cancel_fixture", "{}");
	RuntimePendingSteering steering;
	struct runtime_request request{};
	request.model_input = "finish the tool component implementation";
	request.stored_user_input = request.model_input;
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	request.override_action_drain = 1;
	request.action_drain_fn = runtime_cancel_test_steering;
	request.action_drain_user_data = &steering;
	struct runtime_result cancelled{};
	EXPECT_EQ(runtime_execute_turn(instance, &request, &cancelled), NB_ERR_CANCELLED);
	EXPECT_EQ(cancelled.outcome, RUNTIME_OUTCOME_CANCELLED);
	EXPECT_EQ(state.executions.load(), 1);
	EXPECT_TRUE(steering.delivered);
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	struct runtime_result continued{};
	ASSERT_EQ(Execute("continue", &continued), 0);
	EXPECT_EQ(continued.outcome, RUNTIME_OUTCOME_COMPLETED);
	ExpectCancelledRequestInNextInput(request.model_input, "preserve component padding");
	EXPECT_EQ(state.executions.load(), 1);
}

TEST_F(RuntimeExecuteTest, InvalidTurnArgumentsAreRejected)
{
	struct runtime_result result{};
	struct runtime_request request{};
	OpenScripted();
	EXPECT_EQ(runtime_execute_turn(nullptr, &request, &result), NB_ERR_INVAL);
	EXPECT_EQ(runtime_execute_turn(instance, nullptr, &result), NB_ERR_INVAL);
	EXPECT_EQ(runtime_execute_turn(instance, &request, &result), NB_ERR_INVAL);
}

TEST_F(RuntimeFacadeTest, PathPoliciesAndCustomSkillsLoadWithoutExec)
{
	std::string skills = directory + "-skills";
	std::string trusted = directory + "-trusted";
	std::filesystem::create_directories(trusted);
	WriteConfig("[memory]\nenabled=false\n[skill]\ndir=\"" + skills +
		"\"\n[react.permissions]\nactive_profile=\"test\"\n"
		"[[react.permission_profiles]]\nname=\"test\"\nwrite_paths=[\"" + trusted + "\"]\n");
	struct State { std::string skills; std::string trusted; bool called = false; } state;
	state.skills = skills;
	state.trusted = trusted;
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_exec = 0;
	options.platform_tools_user_data = &state;
	options.platform_tools_cb = [](struct tool_registry *tools,
		struct tool_context *tctx, void *data) {
		auto *state = static_cast<State *>(data);
		state->called = true;
		EXPECT_EQ(tool_borrow(tools, "exec"), nullptr);
		EXPECT_GT(tctx->builtin_paths.len, 32u);
		EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
			(state->skills + "/SKILL.md").c_str(), NULL, 0), 0);
		EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
			(state->trusted + "/config").c_str(), NULL, 0), 0);
		/* Temp directories have an independent built-in delete grant. A write
		 * profile must not populate the configured delete roots. */
		EXPECT_EQ(tctx->exec_profile_delete_dirs_count, 0);
		return 0;
	};
	Open(&options);
	EXPECT_TRUE(state.called);
	runtime_close(instance);
	instance = nullptr;
	std::filesystem::remove_all(skills);
	std::filesystem::remove_all(trusted);
}

TEST_F(RuntimeExecuteTest, TrustedStopCommandRunsThroughRuntimeAndPersistsOneAnswer)
{
	ASSERT_NE(runtime_create_scripted_model(&scripted), nullptr);
	std::string script = directory + "/completion.py";
	{
		std::ofstream file(script);
		file << "import json,sys\nx=json.load(sys.stdin)\n"
		     << "print(json.dumps({} if x['stop_hook_active'] else "
		     << "{'decision':'block','reason':'Verify once more.'}))\n";
	}
	WriteConfig("[memory]\nenabled=false\n[[hooks.Stop]]\n[[hooks.Stop.hooks]]\n"
		"type='command'\ncommand='python3 " + script + "'\ntimeout=3\n");
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.after_models_cb = runtime_install_scripted_model;
	options.after_models_user_data = &scripted;
	Open(&options);
	ASSERT_NE(instance, nullptr);
	cJSON *hooks NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_hooks_snapshot_copy(instance, &hooks), 0);
	const cJSON *row;
	int trusted = 0;
	cJSON_ArrayForEach(row, hooks) {
		const cJSON *source = cJSON_GetObjectItem(row, "source");
		if (source && std::filesystem::canonical(config_path) ==
		    std::filesystem::canonical(source->valuestring)) {
			ASSERT_EQ(runtime_hooks_manage(instance, "trust",
				cJSON_GetObjectItem(row, "id")->valuestring,
				cJSON_GetObjectItem(row, "digest")->valuestring), 0);
			trusted++;
		}
	}
	cJSON_DeleteOwner(&hooks);
	ASSERT_EQ(trusted, 1);
	struct runtime_result result{};
	ASSERT_EQ(Execute("finish the work", &result), 0);
	EXPECT_EQ(scripted.calls, 2);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	int count = 0, assistant_count = 0;
	struct message *messages = runtime_session_messages_current_copy(instance, &count);
	for (auto *msg = messages; msg; msg = msg->next) {
		if (strcmp(msg->role, "assistant") == 0)
			assistant_count++;
	}
	EXPECT_EQ(assistant_count, 1);
	runtime_session_messages_free(messages);
}

TEST_F(RuntimeFacadeTest, ProcessResultsSurviveRestartAndRespectSessionOwnership)
{
	struct Notices {
		std::mutex lock;
		std::condition_variable ready;
		int completed = 0;
	} notices;
	WriteConfig("[memory]\nenabled=false\n");
	runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_exec = 1;
	options.operation_approval_cb = [](const tool_operation *, void *) { return TOOL_OP_ALLOW; };
	options.event_user_data = &notices;
	options.event_cb = [](const morph_event *event, void *opaque) -> int {
		if (std::strcmp(event->name, "process.completed")) return 0;
		auto *n = static_cast<Notices *>(opaque);
		std::lock_guard<std::mutex> lock(n->lock);
		n->completed++;
		n->ready.notify_all();
		return 0;
	};
	Open(&options);
	int64_t owner = 0;
	ASSERT_EQ(runtime_session_current_id(instance, &owner), 0);
	tool_runtime_context context{};
	context.memory_session_id = owner;
	context.turn_id = "runtime-job-test";
	auto start = [&](const char *args) {
		tool_result result{};
		tool_result_init(&result);
		result.runtime = &context;
		int rc = tool_exec(&instance->context.tools, "exec", args, &result);
		EXPECT_EQ(rc, 0);
		const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
		std::string id = value ? value : "";
		tool_result_cleanup(&result);
		return id;
	};
	std::string finished = start(R"({"command":"printf persisted; exit 7","background":true})");
	{
		std::unique_lock<std::mutex> lock(notices.lock);
		ASSERT_TRUE(notices.ready.wait_for(lock, std::chrono::seconds(3),
			[&] { return notices.completed > 0; }));
	}
	std::string service = start(R"({"command":"sleep 20","service":true})");
	cJSON *jobs NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_process_list(instance, &jobs), 0);
	EXPECT_EQ(cJSON_GetArraySize(jobs), 2);
	cJSON_DeleteOwner(&jobs);
	runtime_close(instance);
	instance = nullptr;
	Open(&options);
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_process_read(instance, finished.c_str(), &data), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "stdout")), "persisted");
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "exit_code")), 7);
	EXPECT_EQ(cJSON_GetObjectItem(data, "stdout_path"), nullptr);
	cJSON_DeleteOwner(&data);
	ASSERT_EQ(runtime_process_read(instance, service.c_str(), &data), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "status")), "unavailable");
	cJSON_DeleteOwner(&data);
	EXPECT_EQ(runtime_process_stop(instance, service.c_str()), NB_ERR_NOT_FOUND);
	struct session other{};
	int created = 0;
	ASSERT_EQ(runtime_session_select(instance, "other-process-session", &other, &created), 0);
	ASSERT_EQ(runtime_process_list(instance, &jobs), 0);
	EXPECT_EQ(cJSON_GetArraySize(jobs), 0);
	cJSON_DeleteOwner(&jobs);
	EXPECT_EQ(runtime_process_read(instance, finished.c_str(), &data), NB_ERR_NOT_FOUND);
}

TEST_F(RuntimeExecuteTest, HookPreparationFailureRestoresBorrowedCallbacksAndUsage)
{
	OpenScripted();
	struct react_context *react = instance->context.react;
	morph_event_cb previous_event = react->event_cb;
	void *previous_event_data = react->event_user_data;
	react_action_drain_fn previous_drain = react->action_drain_fn;
	void *previous_drain_data = react->action_drain_user_data;
	int original_usage = 0;
	int temporary_usage = 0;
	int temporary_events = 0;
	int temporary_drains = 0;
	struct model_usage_sink sink{};
	sink.user_data = &original_usage;
	scripted.model->usage_sink = &sink;
	const std::string hooks_path = directory + "/hooks.json";
	std::ofstream(hooks_path) << "{invalid";

	struct runtime_request request{};
	struct runtime_result result{};
	request.model_input = "first request";
	request.event_cb = runtime_count_events;
	request.event_user_data = &temporary_events;
	request.override_action_drain = 1;
	request.action_drain_fn = runtime_count_action_drains;
	request.action_drain_user_data = &temporary_drains;
	request.bind_usage_user_data = 1;
	request.usage_user_data = &temporary_usage;
	EXPECT_LT(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_EQ(scripted.calls, 0);
	EXPECT_EQ(react->event_cb, previous_event);
	EXPECT_EQ(react->event_user_data, previous_event_data);
	EXPECT_EQ(react->action_drain_fn, previous_drain);
	EXPECT_EQ(react->action_drain_user_data, previous_drain_data);
	EXPECT_EQ(sink.user_data, &original_usage);
	EXPECT_EQ(instance->context.hooks.event_cb, nullptr);
	EXPECT_EQ(instance->context.hooks.cancel_token, nullptr);
	EXPECT_FALSE(instance->context.turn_scope.bound);
	EXPECT_FALSE(instance->context.turn_scope.request_bound);

	ASSERT_TRUE(std::filesystem::remove(hooks_path));
	const int failed_request_events = temporary_events;
	EXPECT_EQ(Execute("second request", &result), 0);
	EXPECT_EQ(scripted.calls, 1);
	EXPECT_EQ(temporary_events, failed_request_events);
	EXPECT_EQ(temporary_drains, 0);
	EXPECT_EQ(sink.user_data, &original_usage);
	scripted.model->usage_sink = nullptr;
}

TEST_F(RuntimeExecuteTest, UsageOverrideIsBoundOnceAndRestoredForTheNextTurn)
{
	OpenScripted();
	int original_usage = 0;
	int request_usage = 0;
	struct model_usage_sink sink{};
	sink.user_data = &original_usage;
	scripted.model->usage_sink = &sink;
	struct runtime_request request{};
	struct runtime_result result{};
	request.model_input = "request with separate usage owner";
	request.bind_usage_user_data = 1;
	request.usage_user_data = &request_usage;
	EXPECT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_EQ(scripted.observed_usage_user_data, &request_usage);
	EXPECT_EQ(sink.user_data, &original_usage);
	EXPECT_EQ(Execute("next request", &result), 0);
	EXPECT_EQ(scripted.observed_usage_user_data, instance);
	EXPECT_EQ(sink.user_data, &original_usage);
	scripted.model->usage_sink = nullptr;
}

static std::string runtime_checkpoint_read_file(const std::string &path)
{
	std::ifstream file(path);

	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

TEST_F(RuntimeExecuteTest, CheckpointsTrackRealToolEditsAndModelUndoPreservesDirtyBaseline)
{
	OpenScripted(true);
	const auto path = directory + "/owned.c";
	std::ofstream(path) << "user change\nold\n";
	scripted.actions = {
		{"apply_patch",
		 "*** Begin Patch\n*** Update File: owned.c\n@@\n user change\n-old\n+new\n"
		 "*** End Patch\n"}};
	struct runtime_result result{};
	ASSERT_EQ(Execute("edit the second line", &result), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(path), "user change\nnew\n");
	cJSON *list NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_list_copy(instance, &list), 0);
	auto *rows = cJSON_GetObjectItemCaseSensitive(list, "checkpoints");
	ASSERT_EQ(cJSON_GetArraySize(rows), 1);
	auto *row = cJSON_GetArrayItem(rows, 0);
	auto *id = cJSON_GetObjectItemCaseSensitive(row, "id");
	ASSERT_TRUE(cJSON_IsNumber(id));
	const auto target = static_cast<int64_t>(id->valuedouble);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "prompt")),
		     "edit the second line");
	EXPECT_EQ(instance->context.checkpoint.id, 0);
	EXPECT_EQ(instance->context.react->tool_runtime.checkpoint.id, 0);
	scripted.actions = {
		{"checkpoint",
		 "{\"action\":\"restore_code\",\"id\":" + std::to_string(target) + "}"},
		{"apply_patch",
		 "*** Begin Patch\n*** Add File: after-undo.c\n+fresh\n*** End Patch\n"}};
	scripted.action_cursor = 0;
	ASSERT_EQ(Execute("undo the prior edit then add a new file", &result), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(path), "user change\nold\n");
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/after-undo.c"), "fresh\n");
	int count = 0;
	struct message *messages = runtime_session_messages_current_copy(instance, &count);
	EXPECT_GE(count, 4);
	message_free_list(messages);
}

TEST_F(RuntimeExecuteTest, CheckpointsPersistAcrossReopenAndRequirePermissionToRestore)
{
	OpenScripted(true);
	scripted.actions = {
		{"apply_patch",
		 "*** Begin Patch\n*** Add File: created.c\n+created\n*** End Patch\n"}};
	struct runtime_result result{};
	ASSERT_EQ(Execute("create one file", &result), 0);
	cJSON *list NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_list_copy(instance, &list), 0);
	auto *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(list, "checkpoints"), 0);
	ASSERT_NE(row, nullptr);
	const auto target = static_cast<int64_t>(
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
	runtime_close(instance);
	instance = nullptr;
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_apply_patch = 1;
	Open(&options);
	cJSON *preview NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_preview_copy(instance, target, &preview), 0);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(preview, "files")), 1);
	auto policy = permission_policy_default();
	policy.access = PERMISSION_READ_ONLY;
	ASSERT_EQ(tool_context_set_permission_policy(instance->context.tctx, &policy), 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_LT(runtime_checkpoint_restore(instance, target, RUNTIME_REWIND_CODE, &report), 0);
	EXPECT_TRUE(std::filesystem::exists(directory + "/created.c"));
	policy.access = PERMISSION_WORKSPACE;
	ASSERT_EQ(tool_context_set_permission_policy(instance->context.tctx, &policy), 0);
	ASSERT_EQ(runtime_checkpoint_restore(instance, target, RUNTIME_REWIND_BOTH, &report), 0);
	EXPECT_FALSE(std::filesystem::exists(directory + "/created.c"));
	int count = -1;
	struct message *messages = runtime_session_messages_current_copy(instance, &count);
	EXPECT_EQ(count, 0);
	message_free_list(messages);
}

TEST_F(RuntimeExecuteTest, CheckpointConflictPreservesManualChangesAndConversationCanRewindAlone)
{
	OpenScripted(true);
	std::ofstream(directory + "/manual.c") << "before\n";
	scripted.actions = {{"apply_patch", "*** Begin Patch\n*** Update File: "
					    "manual.c\n@@\n-before\n+after\n*** End Patch\n"}};
	struct runtime_result result{};
	ASSERT_EQ(Execute("change the file", &result), 0);
	cJSON *list NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_list_copy(instance, &list), 0);
	auto *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(list, "checkpoints"), 0);
	ASSERT_NE(row, nullptr);
	const auto target = static_cast<int64_t>(
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
	std::ofstream(directory + "/manual.c") << "manual edit\n";
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_LT(runtime_checkpoint_restore(instance, target, RUNTIME_REWIND_BOTH, &report), 0);
	ASSERT_NE(report, nullptr);
	EXPECT_GT(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report, "conflicts")), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/manual.c"), "manual edit\n");
	cJSON_Delete(report);
	report = nullptr;
	ASSERT_EQ(
		runtime_checkpoint_restore(instance, target, RUNTIME_REWIND_CONVERSATION, &report),
		0);
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/manual.c"), "manual edit\n");
	EXPECT_EQ(instance->context.react->messages, nullptr);
	ASSERT_NE(instance->context.react->history_items, nullptr);
	EXPECT_STREQ(instance->context.react->history_items->kind, "background_receipt");
	EXPECT_NE(std::string(instance->context.react->history_items->content)
			  .find("Files were retained"),
		  std::string::npos);
}

TEST_F(RuntimeExecuteTest, FailedMultiFilePatchRetainsCheckpointForActualPartialWrites)
{
	OpenScripted(true);
	std::ofstream(directory + "/first.c") << "before\n";
	scripted.actions = {{"apply_patch",
			     "*** Begin Patch\n*** Update File: first.c\n@@\n-before\n+after\n"
			     "*** Update File: missing.c\n@@\n-not there\n+new\n*** End Patch\n"}};
	struct runtime_result result{};
	ASSERT_EQ(Execute("attempt a multi-file patch", &result), 0);
	ASSERT_EQ(runtime_checkpoint_read_file(directory + "/first.c"), "after\n");
	cJSON *list NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_list_copy(instance, &list), 0);
	auto *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(list, "checkpoints"), 0);
	ASSERT_NE(row, nullptr);
	const auto target = static_cast<int64_t>(
		cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_restore(instance, target, RUNTIME_REWIND_CODE, &report), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/first.c"), "before\n");
	EXPECT_FALSE(std::filesystem::exists(directory + "/missing.c"));
}

TEST_F(RuntimeExecuteTest, CheckpointFacadeRejectsBusyAndOccupiedOutputs)
{
	OpenScripted();
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(nb_mutex_lock(instance->context.execution_lock), 0);
	EXPECT_EQ(runtime_checkpoint_list_copy(instance, &out), NB_ERR_BUSY);
	EXPECT_EQ(runtime_checkpoint_preview_copy(instance, 1, &out), NB_ERR_BUSY);
	EXPECT_EQ(runtime_checkpoint_restore(instance, 1, RUNTIME_REWIND_BOTH, &out), NB_ERR_BUSY);
	nb_mutex_unlock(instance->context.execution_lock);
	out = cJSON_CreateObject();
	auto *owner = out;
	EXPECT_EQ(runtime_checkpoint_list_copy(instance, &out), NB_ERR_EXISTS);
	EXPECT_EQ(out, owner);
}

TEST_F(RuntimeExecuteTest, FailedCheckpointSealStopsFurtherTrackedEditsUntilNextTurn)
{
	OpenScripted(true);
	cJSON *empty NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_list_copy(instance, &empty), 0);
	ASSERT_EQ(sqlite3_exec(
			  instance->context.database.handle,
			  "CREATE TRIGGER reject_checkpoint_seal BEFORE UPDATE OF after_data "
			  "ON code_checkpoint_files BEGIN SELECT RAISE(ABORT,'seal rejected'); END",
			  nullptr, nullptr, nullptr),
		  SQLITE_OK);
	scripted.actions = {
		{"apply_patch", "*** Begin Patch\n*** Add File: first.c\n+first\n*** End Patch\n"},
		{"apply_patch",
		 "*** Begin Patch\n*** Add File: second.c\n+second\n*** End Patch\n"}};
	struct runtime_result result{};
	ASSERT_EQ(Execute("exercise a tracking failure", &result), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/first.c"), "first\n");
	EXPECT_FALSE(std::filesystem::exists(directory + "/second.c"));
	EXPECT_TRUE(instance->checkpoint_failed);
	ASSERT_EQ(sqlite3_exec(instance->context.database.handle,
			       "DROP TRIGGER reject_checkpoint_seal", nullptr, nullptr, nullptr),
		  SQLITE_OK);
	scripted.actions = {{"apply_patch",
			     "*** Begin Patch\n*** Add File: second.c\n+second\n*** End Patch\n"}};
	scripted.action_cursor = 0;
	ASSERT_EQ(Execute("start a new turn", &result), 0);
	EXPECT_EQ(runtime_checkpoint_read_file(directory + "/second.c"), "second\n");
	EXPECT_FALSE(instance->checkpoint_failed);
}
