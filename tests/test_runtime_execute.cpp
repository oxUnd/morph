#include "runtime_test_support.hpp"

extern "C" {
#include "models/llm.h"
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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
	std::vector<std::string> observed_messages;
};

static int runtime_scripted_chat(struct model *self, struct arena *,
				 const char *, const char **messages, int count,
				 const struct model_chat_options *,
				 sse_callback callback, void *user_data)
{
	auto *state = static_cast<RuntimeScriptedModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->calls++;
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

static int runtime_scripted_chat_with_tools(
	struct model *self, struct arena *, const char *,
	struct chat_message *messages, int count, struct tool_desc *, int,
	struct chat_response *response, sse_callback, void *)
{
	auto *state = static_cast<RuntimeScriptedModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->calls++;
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
	response->content = strdup(state->response.c_str());
	return response->content ? 200 : -ENOMEM;
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

	void OpenScripted()
	{
		ASSERT_NE(runtime_create_scripted_model(&scripted), nullptr);
		WriteConfig("[memory]\nenabled = false\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.after_models_cb = runtime_install_scripted_model;
		options.after_models_user_data = &scripted;
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

TEST_F(RuntimeExecuteTest, SuccessfulTurnPersistsUserAndAssistantMessages)
{
	struct runtime_result result{};
	OpenScripted();
	ASSERT_EQ(Execute("hello runtime", &result), 0);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	ASSERT_NE(result.final_text, nullptr);
	EXPECT_STREQ(result.final_text, "scripted answer");
	int count = 0;
	struct message *messages = runtime_session_messages_current(instance, &count);
	ASSERT_NE(messages, nullptr);
	EXPECT_EQ(count, 2);
	EXPECT_STREQ(messages->role, "user");
	EXPECT_STREQ(messages->content, "hello runtime");
	ASSERT_NE(messages->next, nullptr);
	EXPECT_STREQ(messages->next->role, "assistant");
	EXPECT_STREQ(messages->next->content, "scripted answer");
	runtime_session_messages_free(messages);
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
	struct message *messages = runtime_session_messages_current(instance, &count);
	EXPECT_EQ(count, 4);
	runtime_session_messages_free(messages);
}

TEST_F(RuntimeExecuteTest, CapturesDispatchedInputWithoutTheLaterAssistantAnswer)
{
	struct runtime_result result{};
	OpenScripted();
	ASSERT_EQ(Execute("inspect actual model input", &result), 0);
	cJSON *snapshot = nullptr;
	ASSERT_EQ(runtime_session_last_request(instance, &snapshot), 0);
	const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "input_json"));
	ASSERT_NE(input, nullptr);
	EXPECT_NE(strstr(input, "inspect actual model input"), nullptr);
	EXPECT_EQ(strstr(input, "scripted answer"), nullptr);
	EXPECT_NE(strstr(input, "messages"), nullptr);
	EXPECT_NE(strstr(input, "tools"), nullptr);
	cJSON_Delete(snapshot);
}

TEST_F(RuntimeExecuteTest, MemoryMaintenanceNeverEntersTheDispatchedPrompt)
{
	OpenScripted();
	struct session session{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "receipt-policy", &session), 0);
	struct db handle{};
	ASSERT_EQ(db_open(&handle, database.c_str()), 0);
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
	cJSON *snapshot = nullptr;
	ASSERT_EQ(runtime_session_last_request(instance, &snapshot), 0);
	const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "input_json"));
	ASSERT_NE(input, nullptr);
	EXPECT_EQ(strstr(input, "memory_consolidation"), nullptr);
	EXPECT_EQ(strstr(input, "memory consolidation queued"), nullptr);
	EXPECT_NE(strstr(input, "archive created"), nullptr);
	EXPECT_EQ(model_history_count(&handle, session.id, 0), 4);
	EXPECT_EQ(model_history_count(&handle, session.id, 1), 3);
	cJSON_Delete(snapshot);
	db_close(&handle);
}

TEST_F(RuntimeExecuteTest, InputSnapshotFailureDoesNotPreventModelDispatch)
{
	struct runtime_result result{};
	struct db database_handle{};
	OpenScripted();
	ASSERT_EQ(db_open(&database_handle, database.c_str()), 0);
	ASSERT_EQ(db_exec(&database_handle, "CREATE TRIGGER fail_input_snapshot BEFORE INSERT ON "
		"model_request_snapshots BEGIN SELECT RAISE(ABORT,'test failure'); END"), 0);
	ASSERT_EQ(Execute("still execute this request", &result), 0);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_EQ(scripted.calls, 1);
	cJSON *snapshot = nullptr;
	EXPECT_EQ(runtime_session_last_request(instance, &snapshot), -ENODATA);
	db_close(&database_handle);
}

TEST_F(RuntimeExecuteTest, TurnStatusIsADeepCopiedSnapshot)
{
	struct runtime_result result{};
	struct runtime_turn_status first{};
	OpenScripted();
	ASSERT_EQ(Execute("snapshot one", &result), 0);
	ASSERT_EQ(runtime_turn_status_get(instance, &first), 0);
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
	scripted.error = -EIO;
	EXPECT_EQ(Execute("fail", &result), -EIO);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_FAILED);
	char *json = runtime_turn_error_json(instance);
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
		EXPECT_EQ(Execute("concurrent", &second), -EBUSY);
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
}

TEST_F(RuntimeExecuteTest, InvalidTurnArgumentsAreRejected)
{
	struct runtime_result result{};
	struct runtime_request request{};
	OpenScripted();
	EXPECT_EQ(runtime_execute_turn(nullptr, &request, &result), -EINVAL);
	EXPECT_EQ(runtime_execute_turn(instance, nullptr, &result), -EINVAL);
	EXPECT_EQ(runtime_execute_turn(instance, &request, &result), -EINVAL);
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
		EXPECT_EQ(tool_lookup(tools, "exec"), nullptr);
		EXPECT_GT(tctx->builtin_paths.nelts, 32u);
		EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
			(state->skills + "/SKILL.md").c_str(), NULL, 0), 0);
		EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
			(state->trusted + "/config").c_str(), NULL, 0), 0);
		EXPECT_NE(tool_context_authorize_path(tctx, TOOL_PATH_DELETE,
			(state->trusted + "/config").c_str(), NULL, 0), 0);
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
	cJSON *hooks = nullptr;
	ASSERT_EQ(runtime_hooks_snapshot(instance, &hooks), 0);
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
	cJSON_Delete(hooks);
	ASSERT_EQ(trusted, 1);
	struct runtime_result result{};
	ASSERT_EQ(Execute("finish the work", &result), 0);
	EXPECT_EQ(scripted.calls, 2);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	int count = 0, assistant_count = 0;
	struct message *messages = runtime_session_messages_current(instance, &count);
	for (auto *msg = messages; msg; msg = msg->next) {
		if (strcmp(msg->role, "assistant") == 0)
			assistant_count++;
	}
	EXPECT_EQ(assistant_count, 1);
	runtime_session_messages_free(messages);
}
