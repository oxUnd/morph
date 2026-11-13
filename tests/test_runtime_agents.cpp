#include "runtime_test_support.hpp"
#include "nb/err.h"
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/tool.h"
#include "runtime/runtime_internal.h"
#include "nb/cleanup.h"
#include "cJSON.h"
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class RuntimeAgentGate
{
      public:
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool released = false;

	void Wait()
	{
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		changed.notify_all();
		changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
	}

	bool Await()
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
	}

	void Release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		released = true;
		changed.notify_all();
	}
};

class RuntimeAgentsTest : public RuntimeFacadeTest
{
      protected:
	AgentHttpServer server;
	RuntimeAgentGate gate;
	std::mutex identity_mutex;
	std::vector<std::pair<std::string, std::string>> identities;
	std::atomic<int> scoped_events{0};
	std::atomic<int> request_events{0};
	std::atomic<int> usage_observations{0};
	int64_t root_session = 0;

	void Start()
	{
		server.start();
		ASSERT_EQ(setenv("MORPH_RUNTIME_AGENTS_TEST_KEY", "test-key", 1), 0);
		WriteConfig("[features]\nhooks=false\n[memory]\nenabled=false\n"
			    "[model.text]\nprovider=\"openai\"\nmodel=\"test-model\"\n"
			    "api_base=\"http://127.0.0.1:" +
			    std::to_string(server.port) +
			    "/v1\"\napi_key_env=\"MORPH_RUNTIME_AGENTS_TEST_KEY\"\nretry_count=0\n"
			    "[agents]\nmulti_agent_mode=\"proactive\"\nmax_total_tokens=0\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.enable_multi_agent = 1;
		options.event_cb = [](const struct morph_event *event, void *opaque) -> int {
			auto *self = static_cast<RuntimeAgentsTest *>(opaque);
			if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(
				    event->data, "agent_event_sequence")))
				self->scoped_events.fetch_add(1);
			return 0;
		};
		options.event_user_data = this;
		options.usage_observer = [](const struct model_usage *, void *opaque) {
			static_cast<RuntimeAgentsTest *>(opaque)->usage_observations.fetch_add(1);
		};
		options.usage_observer_user_data = this;
		Open(&options);
		ASSERT_EQ(runtime_session_create_and_select(instance, "agents", nullptr), 0);
		ASSERT_EQ(runtime_session_current_id(instance, &root_session), 0);
		struct tool_spec identity{};
		identity.name = "capture_identity";
		identity.description = "Record independent worker ownership";
		identity.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
		identity.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
		identity.flags = TOOL_FLAG_READONLY;
		identity.user_data = this;
		identity.exec = [](const char *, struct tool_result *result, void *opaque) -> int {
			auto *self = static_cast<RuntimeAgentsTest *>(opaque);
			std::lock_guard<std::mutex> lock(self->identity_mutex);
			self->identities.emplace_back(result->runtime->user_id,
						      result->runtime->credit_session_id);
			return tool_result_success_text_copy(result, "Identity captured");
		};
		ASSERT_EQ(tool_register_borrow(&instance->context.tools, &identity), 0);
		struct tool_spec hold{};
		hold.name = "hold_child";
		hold.description = "Wait for deterministic lifecycle coordination";
		hold.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
		hold.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
		hold.flags = TOOL_FLAG_READONLY;
		hold.user_data = &gate;
		hold.exec = [](const char *, struct tool_result *result, void *opaque) -> int {
			static_cast<RuntimeAgentGate *>(opaque)->Wait();
			return tool_result_success_text_copy(result, "Gate released");
		};
		ASSERT_EQ(tool_register_borrow(&instance->context.tools, &hold), 0);
		server.respond = [](const std::string &body) -> std::string {
			if (body.find("You are `/root/blocker`") != std::string::npos &&
			    body.find("call_hold") == std::string::npos)
				return R"({"tool_calls":[{"index":0,"id":"call_hold","type":"function",)"
				       R"("function":{"name":"hold_child","arguments":"{}"}}]})";
			if (body.find("You are `/root/worker`") != std::string::npos &&
			    body.find("call_identity") == std::string::npos)
				return R"({"tool_calls":[{"index":0,"id":"call_identity","type":"function",)"
				       R"("function":{"name":"capture_identity","arguments":"{}"}}]})";
			return R"({"content":"Verified result"})";
		};
		ASSERT_EQ(Execute(), 0);
	}

	void TearDown() override
	{
		gate.Release();
		RuntimeFacadeTest::TearDown();
		unsetenv("MORPH_RUNTIME_AGENTS_TEST_KEY");
	}

	int Execute()
	{
		struct runtime_request request{};
		struct runtime_result result{};
		request.session_id = root_session;
		request.model_input = "Continue independent work";
		request.user_id = "facade-user";
		request.event_cb = [](const struct morph_event *, void *opaque) -> int {
			static_cast<std::atomic<int> *>(opaque)->fetch_add(1);
			return 0;
		};
		request.event_user_data = &request_events;
		return runtime_execute_turn(instance, &request, &result);
	}

	int Spawn(const char *name)
	{
		struct runtime_agent_actor actor{};
		int rc = runtime_agent_actor_begin(instance, &actor);
		if (rc != 0)
			return rc;
		cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
		if (!args || !cJSON_AddStringToObject(args, "task_name", name) ||
		    !cJSON_AddStringToObject(args, "message", "Inspect independently") ||
		    !cJSON_AddStringToObject(args, "fork_turns", "none"))
			rc = NB_ERR_NOMEM;
		else
			rc = agent_control_call(instance->context.agents, "spawn_agent", args,
						&actor.context, &out);
		runtime_agent_actor_end(instance);
		return rc;
	}

	bool AwaitPhase(struct runtime *owner, int phase)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			nb_mutex_lock(owner->context.agent_interaction_lock);
			bool found = owner->context.agent_phase == phase;
			nb_mutex_unlock(owner->context.agent_interaction_lock);
			if (found)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
};

TEST_F(RuntimeAgentsTest, IdleFollowupRetainsOwnedIdentityAndPersistentEventSink)
{
	Start();
	ASSERT_EQ(Spawn("worker"), 0);
	agent_control_join(instance->context.agents);
	char **events = nullptr;
	int count = 0;
	int64_t frontier = 0;
	ASSERT_EQ(runtime_agent_subscribe(instance, "/root/worker", &events, &count, &frontier), 0);
	runtime_agent_events_free(events, count);
	int observed = usage_observations.load();
	int temporary_events = request_events.load();
	ASSERT_EQ(db_exec(&instance->context.database, "DELETE FROM credit_events"), 0);
	ASSERT_EQ(runtime_agent_followup(instance, "/root/worker", "Continue after root finished"),
		  0);
	agent_control_join(instance->context.agents);
	EXPECT_GT(scoped_events.load(), 0);
	EXPECT_EQ(request_events.load(), temporary_events);
	EXPECT_GT(usage_observations.load(), observed);
	ASSERT_FALSE(identities.empty());
	for (const auto &identity : identities) {
		EXPECT_EQ(identity.first, "facade-user");
		EXPECT_FALSE(identity.second.empty());
	}
	struct runtime_agent_actor actor{};
	ASSERT_EQ(runtime_agent_actor_begin(instance, &actor), 0);
	EXPECT_STREQ(actor.context.user_id, "facade-user");
	EXPECT_EQ(identities.front().second, actor.credit_session_id);
	std::string credit_session = actor.credit_session_id;
	runtime_agent_actor_end(instance);
	sqlite3_stmt *raw = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(
			  instance->context.database.handle,
			  "SELECT user_id,session_id,input_tokens,output_tokens FROM credit_events",
			  -1, &raw, nullptr),
		  SQLITE_OK);
	std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
	int records = 0;
	int step;
	while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
		EXPECT_STREQ(
			reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0)),
			"facade-user");
		EXPECT_EQ(reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1)),
			  credit_session);
		EXPECT_EQ(sqlite3_column_int(statement.get(), 2), 12);
		EXPECT_EQ(sqlite3_column_int(statement.get(), 3), 5);
		records++;
	}
	EXPECT_EQ(step, SQLITE_DONE);
	EXPECT_GT(records, 0);
}

TEST_F(RuntimeAgentsTest, FinishingRejectsIdleRestartsAndStillAcceptsRunningWorkerSteering)
{
	Start();
	ASSERT_EQ(Spawn("worker"), 0);
	agent_control_join(instance->context.agents);
	ASSERT_EQ(Spawn("blocker"), 0);
	ASSERT_TRUE(gate.Await());
	size_t next_request = server.snapshot().size() + 1;
	auto primary = std::async(std::launch::async, [&] { return Execute(); });
	ASSERT_TRUE(server.await_requests(next_request));
	/* Successful finalization first collects active agents inside ReAct. Cancel
	 * the primary to exercise outer shutdown while the child tool still runs. */
	runtime_cancel_turn(instance);
	ASSERT_TRUE(AwaitPhase(instance, RUNTIME_AGENTS_FINISHING));
	auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(runtime_agent_followup(instance, "/root/worker", "Restart during finish"),
		  NB_ERR_BUSY);
	EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
	EXPECT_EQ(runtime_agent_followup(instance, "/root/blocker", "Steer active work"), 0);
	gate.Release();
	EXPECT_EQ(primary.get(), NB_ERR_CANCELLED);
	EXPECT_EQ(runtime_agent_followup(instance, "/root/worker", "Restart after finish"), 0);
	agent_control_join(instance->context.agents);
}

TEST_F(RuntimeAgentsTest, IdleWorkerProtectsSessionNavigationAndShutdownCancelsAndJoins)
{
	Start();
	ASSERT_EQ(Spawn("blocker"), 0);
	ASSERT_TRUE(gate.Await());
	EXPECT_EQ(runtime_session_select(instance, "another", nullptr, nullptr), NB_ERR_BUSY);
	EXPECT_EQ(runtime_session_create_and_select(instance, "another", nullptr), NB_ERR_BUSY);
	EXPECT_EQ(runtime_session_delete_and_update(instance, root_session), NB_ERR_BUSY);
	int64_t current = 0;
	EXPECT_EQ(runtime_session_current_id(instance, &current), 0);
	EXPECT_EQ(current, root_session);
	struct runtime *owner = instance;
	instance = nullptr;
	auto closing = std::async(std::launch::async, [owner] { runtime_close(owner); });
	ASSERT_TRUE(AwaitPhase(owner, RUNTIME_AGENTS_CLOSING));
	EXPECT_EQ(runtime_agent_followup(owner, "/root/blocker", "Too late"), NB_ERR_CANCELLED);
	EXPECT_EQ(closing.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
	gate.Release();
	closing.get();
}

TEST_F(RuntimeAgentsTest, ColdStartupLoadsPersistedWorkersBeforeAnyNewPrimaryTurn)
{
	Start();
	ASSERT_EQ(Spawn("worker"), 0);
	agent_control_join(instance->context.agents);
	const size_t requests = server.snapshot().size();
	runtime_close(instance);
	instance = nullptr;
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.default_session = "agents";
	options.enable_multi_agent = 1;
	Open(&options);
	EXPECT_EQ(server.snapshot().size(), requests);
	struct agent_info *workers = nullptr;
	int count = 0;
	ASSERT_EQ(runtime_agent_list(instance, root_session, &workers, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(workers[0].id, "/root/worker");
	EXPECT_EQ(workers[0].parent_session_id, root_session);
	runtime_agent_list_free(workers, count);
	char **events = nullptr;
	int64_t sequence = 0;
	ASSERT_EQ(runtime_agent_subscribe(instance, "/root/worker", &events, &count, &sequence), 0);
	EXPECT_GT(count, 0);
	EXPECT_GT(sequence, 0);
	runtime_agent_events_free(events, count);
}

TEST_F(RuntimeAgentsTest, DetachedPrimaryRequestRestoresSelectedWorkerTreeAndIdentity)
{
	for (const char *name : {"selected-check", "detached-check"}) {
		const std::string path = directory + "/.agents/skills/" + name;
		std::filesystem::create_directories(path);
		std::ofstream(path + "/SKILL.md")
			<< "---\nname: " << name
			<< "\ndescription: Session isolation check\n---\nSession instructions.\n";
	}
	Start();
	ASSERT_EQ(runtime_skill_set_active(instance, "selected-check", 1, nullptr), 0);
	ASSERT_EQ(Spawn("worker"), 0);
	agent_control_join(instance->context.agents);
	struct agent_info *worker = nullptr;
	ASSERT_EQ(runtime_agent_read(instance, "/root/worker", &worker), 0);
	ASSERT_NE(worker, nullptr);
	const int64_t child_session = worker->child_session_id;
	runtime_agent_list_free(worker, 1);
	struct session detached{};
	ASSERT_EQ(runtime_session_create_detached(instance, "detached-request", &detached), 0);
	const std::string activate = "INSERT INTO session_skill_activations VALUES(" +
				     std::to_string(detached.id) + ",'detached-check')";
	ASSERT_EQ(db_exec(&instance->context.database, activate.c_str()), 0);
	struct runtime_request request{};
	struct runtime_result result{};
	request.session_id = detached.id;
	request.model_input = "Inspect the independent session";
	request.user_id = "other-user";
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_EQ(instance->context.current_session.id, root_session);
	EXPECT_EQ(instance->context.react->tool_runtime.memory_session_id, root_session);
	struct skill_entry skill{};
	ASSERT_EQ(runtime_skill_find_borrow(instance, "selected-check", &skill), 0);
	EXPECT_TRUE(skill.activated);
	ASSERT_EQ(runtime_skill_find_borrow(instance, "detached-check", &skill), 0);
	EXPECT_FALSE(skill.activated);
	worker = nullptr;
	ASSERT_EQ(runtime_agent_read(instance, "/root/worker", &worker), 0);
	ASSERT_NE(worker, nullptr);
	EXPECT_EQ(worker->parent_session_id, root_session);
	EXPECT_EQ(worker->child_session_id, child_session);
	runtime_agent_list_free(worker, 1);
	char **events = nullptr;
	int count = 0;
	int64_t sequence = 0;
	ASSERT_EQ(runtime_agent_subscribe(instance, "/root/worker", &events, &count, &sequence), 0);
	EXPECT_GT(count, 0);
	runtime_agent_events_free(events, count);
	struct runtime_agent_actor actor{};
	ASSERT_EQ(runtime_agent_actor_begin(instance, &actor), 0);
	EXPECT_EQ(actor.context.memory_session_id, root_session);
	EXPECT_STREQ(actor.context.user_id, "facade-user");
	EXPECT_NE(std::string(actor.context.credit_session_id), std::string(detached.display_id));
	runtime_agent_actor_end(instance);
	ASSERT_EQ(runtime_agent_followup(instance, "/root/worker", "Continue selected work"), 0);
	agent_control_join(instance->context.agents);
}
