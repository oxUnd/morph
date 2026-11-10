#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/plan.h"
#include "agent/tools/multi_agents.h"
#include "agent/tools/plan.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "session.h"
#include "util/error.h"
}

#include <cstring>
#include <memory>
#include <string>

class AgentPlanControlTest : public testing::Test
{
      protected:
	AgentHttpServer server;
	struct db database{};
	struct session session{};
	struct tool_registry tools{};
	std::unique_ptr<struct plan_registry> plans;
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
		ASSERT_EQ(session_create(&database, "root-plan", "root-model", &session), 0);
		tool_registry_init(nullptr, &tools);
		std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
		model = model_llm_create(nullptr, "openai", "root-model", url.c_str(), "test-key");
		ASSERT_NE(model, nullptr);
		model->retry_count = 0;
		tokenizer = tokenizer_create(nullptr, "root-model", 128000);
		ASSERT_NE(tokenizer, nullptr);
		plans = std::make_unique<struct plan_registry>();
		ASSERT_EQ(plan_registry_bind(plans.get(), &database, session.id), 0);
		const char *steps[] = {"Integrate child evidence"};
		ASSERT_NE(plan_add_borrow(plans.get(), "root-plan", "root goal", steps, 1),
			  nullptr);
		ASSERT_EQ(plan_registry_save(plans.get()), 0);
		ASSERT_EQ(plan_tool_init(&tools, plans.get(), model), 0);
		tool_borrow(&tools, "plan")->flags |= TOOL_FLAG_READONLY;
		config.enabled = 1;
		config.max_threads = 6;
		config.max_depth = 2;
		std::strcpy(config.multi_agent_mode, "proactive");
		Reopen();
		runtime.db = &database;
		runtime.memory_session_id = session.id;
		runtime.agent_id = "/root";
	}

	void Reopen()
	{
		agent_control_destroy(control);
		control =
			agent_control_create(&tools, model, tokenizer, nullptr, &config, &database);
		ASSERT_NE(control, nullptr);
		ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
		ASSERT_EQ(agent_control_set_plan_initializer(control, plan_tool_rebind), 0);
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

	int Call(const char *operation, const char *json)
	{
		cJSON *args = cJSON_Parse(json);
		cJSON *out = nullptr;
		int rc = agent_control_call(control, operation, args, &runtime, &out);
		cJSON_Delete(args);
		cJSON_Delete(out);
		return rc;
	}

	int64_t ChildSession()
	{
		struct agent_info *items = nullptr;
		int count = 0;
		EXPECT_EQ(agent_control_list_copy(control, session.id, &items, &count), 0);
		int64_t id = count ? items[0].child_session_id : 0;
		agent_control_free_list(items, count);
		return id;
	}
};

static std::string plan_call(const char *id, const char *arguments, const char *name = "plan")
{
	cJSON *delta = cJSON_CreateObject();
	cJSON *calls = cJSON_AddArrayToObject(delta, "tool_calls");
	cJSON *call = cJSON_CreateObject();
	cJSON_AddItemToArray(calls, call);
	cJSON_AddNumberToObject(call, "index", 0);
	cJSON_AddStringToObject(call, "id", id);
	cJSON_AddStringToObject(call, "type", "function");
	cJSON *function = cJSON_AddObjectToObject(call, "function");
	cJSON_AddStringToObject(function, "name", name);
	cJSON_AddStringToObject(function, "arguments", arguments);
	char *json = cJSON_PrintUnformatted(delta);
	std::string result = json ? json : "{}";
	cJSON_free(json);
	cJSON_Delete(delta);
	return result;
}

TEST_F(AgentPlanControlTest, ChildPlansAndPlannerModelStayIsolatedAcrossRestoredFollowups)
{
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("You are a task planner") != std::string::npos)
			return R"({"content":"1. Inspect child files\n2. Validate child result"})";
		if (body.find("continue-child-plan") != std::string::npos) {
			if (body.find("child_update") == std::string::npos)
				return plan_call("child_update", R"({"command":"update",
					"plan":"child-plan","step_id":1,"status":"completed"})");
			return R"({"content":"child plan updated"})";
		}
		if (body.find("child_create") == std::string::npos)
			return plan_call("child_create", R"({"command":"create","name":"child-plan",
				"goal":"Inspect and validate child files"})");
		return R"({"content":"child plan created"})";
	};
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"planner","message":"make a child plan",
		"fork_turns":"none","model":"child-model"})"),
		  0);
	agent_control_join(control);
	int64_t child_session = ChildSession();
	ASSERT_GT(child_session, 0);
	auto child = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(child.get(), &database, child_session), 0);
	ASSERT_EQ(child->count, 1);
	EXPECT_STREQ(child->plans[0].name, "child-plan");
	EXPECT_STREQ(child->plans[0].steps[0].status, "in_progress");
	EXPECT_EQ(plans->count, 1);
	EXPECT_STREQ(plans->plans[0].name, "root-plan");
	EXPECT_STREQ(plans->plans[0].steps[0].status, "in_progress");
	bool planner_called = false;
	for (const auto &body : server.snapshot()) {
		cJSON *request = cJSON_Parse(body.c_str());
		const char *requested_model =
			cJSON_GetStringValue(cJSON_GetObjectItem(request, "model"));
		EXPECT_STREQ(requested_model, "child-model");
		planner_called |= body.find("You are a task planner") != std::string::npos;
		cJSON_Delete(request);
	}
	EXPECT_TRUE(planner_called);

	Reopen();
	ASSERT_EQ(Call("followup_task", R"({"target":"planner",
		"message":"continue-child-plan: complete the first step"})"),
		  0);
	agent_control_join(control);
	child = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(child.get(), &database, child_session), 0);
	ASSERT_EQ(child->count, 1);
	EXPECT_STREQ(child->plans[0].steps[0].status, "completed");
	auto root = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(root.get(), &database, session.id), 0);
	ASSERT_EQ(root->count, 1);
	EXPECT_STREQ(root->plans[0].name, "root-plan");
	EXPECT_STREQ(root->plans[0].steps[0].status, "in_progress");
}

class UpdatePlanToolTest : public testing::Test
{
      protected:
	struct db database{};
	struct session session{};
	struct tool_registry tools{};
	std::unique_ptr<struct plan_registry> plans;
	struct tool_result result{};
	struct tool_runtime_context runtime{};
	struct morph_event_recorder events{};

	void SetUp() override
	{
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "update-plan", "test-model", &session), 0);
		plans = std::make_unique<struct plan_registry>();
		ASSERT_EQ(plan_registry_bind(plans.get(), &database, session.id), 0);
		tool_registry_init(nullptr, &tools);
		ASSERT_EQ(plan_tool_init(&tools, plans.get(), nullptr), 0);
		ASSERT_EQ(morph_event_recorder_init(&events), 0);
		runtime.event_cb = morph_event_recorder_cb;
		runtime.event_user_data = &events;
		tool_result_init(&result);
	}

	void TearDown() override
	{
		tool_result_cleanup(&result);
		tool_registry_cleanup(&tools);
		morph_event_recorder_cleanup(&events);
		db_close(&database);
	}

	int Update(const char *json)
	{
		result.runtime = &runtime;
		return tool_exec(&tools, "update_plan", json, &result);
	}
};

TEST_F(UpdatePlanToolTest, UpdatesExistingPlanStateWithoutRequiringAModel)
{
	ASSERT_EQ(Update(R"({"explanation":"Split implementation and verification","plan":[
		{"step":"Implement parser","status":"in_progress"},
		{"step":"Verify parser","status":"pending"}]})"),
		  0);
	ASSERT_EQ(plans->count, 1);
	EXPECT_STREQ(plans->plans[0].name, "Task plan");
	EXPECT_EQ(plans->plans[0].active_step, 0);
	std::string id = plans->plans[0].id;
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "text")),
		     "Plan updated");
	EXPECT_EQ(cJSON_GetObjectItem(result.data, "plans"), nullptr);
	ASSERT_EQ(morph_event_recorder_count(&events), 1u);
	cJSON *event = cJSON_Parse(morph_event_recorder_borrow(&events, 0));
	ASSERT_NE(event, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(event, "name")), "plan.updated");
	cJSON *data = cJSON_GetObjectItem(event, "data");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(data, "plan")), 2);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "explanation")),
		     "Split implementation and verification");
	cJSON_Delete(event);
	ASSERT_EQ(Update(R"({"plan":[{"step":"Implement parser","status":"completed"},
		{"step":"Verify parser","status":"in_progress"}]})"),
		  0);
	EXPECT_STREQ(plans->plans[0].id, id.c_str());
	EXPECT_EQ(plans->plans[0].active_step, 1);
	auto restored = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(restored.get(), &database, session.id), 0);
	ASSERT_EQ(restored->count, 1);
	EXPECT_STREQ(restored->plans[0].steps[0].status, "completed");
	EXPECT_EQ(restored->plans[0].active_step, 1);
}

TEST_F(UpdatePlanToolTest, InvalidInputsNeverReplaceTheSavedPlanOrEmitUpdates)
{
	ASSERT_EQ(Update(R"({"plan":[{"step":"Keep this step","status":"pending"}]})"), 0);
	std::string id = plans->plans[0].id;
	for (const char *invalid :
	     {R"({})", R"({"plan":null})", R"({"plan":[],"explanation":42})",
	      R"({"plan":[{"step":"","status":"pending"}]})",
	      R"({"plan":[{"step":"  ","status":"pending"}]})",
	      R"({"plan":[{"step":"bad","status":"failed"}]})",
	      R"({"plan":[{"step":"bad","status":"pending","unknown":true}]})",
	      R"({"plan":[],"unknown":true})", R"({"plan":[],"plan":[]})",
	      R"({"plan":[{"step":"one","status":"in_progress"},
			{"step":"two","status":"in_progress"}]})"}) {
		EXPECT_EQ(Update(invalid), -EINVAL) << invalid;
		EXPECT_STREQ(plans->plans[0].id, id.c_str());
		EXPECT_STREQ(plans->plans[0].steps[0].description, "Keep this step");
		EXPECT_EQ(morph_event_recorder_count(&events), 1u);
	}
	std::string long_step =
		"{\"plan\":[{\"step\":\"" + std::string(300, 'x') + "\",\"status\":\"pending\"}]}";
	EXPECT_EQ(Update(long_step.c_str()), -EINVAL);
	std::string oversized = "{\"plan\":[";
	for (int i = 0; i <= PLAN_MAX_STEPS; i++) {
		if (i)
			oversized += ",";
		oversized += R"({"step":"Step","status":"pending"})";
	}
	oversized += "]}";
	EXPECT_EQ(Update(oversized.c_str()), -EINVAL);
	EXPECT_STREQ(plans->plans[0].steps[0].description, "Keep this step");
}

TEST_F(UpdatePlanToolTest, PersistenceFailureRollsBackMemoryAndDoesNotPublishAnEvent)
{
	ASSERT_EQ(Update(R"({"plan":[{"step":"Original","status":"pending"}]})"), 0);
	ASSERT_EQ(sqlite3_exec(database.handle,
			       "CREATE TRIGGER reject_plan_update BEFORE UPDATE ON session_plans "
			       "BEGIN SELECT RAISE(ABORT,'test persistence failure'); END;",
			       nullptr, nullptr, nullptr),
		  SQLITE_OK);
	EXPECT_EQ(Update(R"({"plan":[{"step":"Replacement","status":"in_progress"}]})"),
		  MORPH_ERR_DB);
	EXPECT_STREQ(plans->plans[0].steps[0].description, "Original");
	EXPECT_EQ(morph_event_recorder_count(&events), 1u);
	auto restored = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(restored.get(), &database, session.id), 0);
	EXPECT_STREQ(restored->plans[0].steps[0].description, "Original");
}

TEST_F(UpdatePlanToolTest, EmptyChecklistKeepsOtherNamedPlansAndStableTaskIdentity)
{
	ASSERT_EQ(Update(R"({"plan":[{"step":"Original","status":"completed"}]})"), 0);
	std::string id = plans->plans[0].id;
	const char *steps[] = {"Independent existing plan"};
	ASSERT_NE(plan_add_borrow(plans.get(), "Other plan", "Goal", steps, 1), nullptr);
	ASSERT_EQ(Update(R"({"plan":[]})"), 0);
	ASSERT_EQ(plans->count, 2);
	EXPECT_STREQ(plans->plans[0].id, id.c_str());
	EXPECT_EQ(plans->plans[0].step_count, 0);
	EXPECT_EQ(plans->plans[0].active_step, -1);
	EXPECT_STREQ(plans->plans[1].steps[0].description, "Independent existing plan");
}

TEST_F(AgentPlanControlTest, UpdatePlanUsesPrivateChildStateAcrossColdResumeWithoutExtraInference)
{
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("resume-checklist") != std::string::npos) {
			if (body.find("checklist_second") == std::string::npos)
				return plan_call("checklist_second", R"({"plan":[
					{"step":"Inspect child","status":"completed"},
					{"step":"Verify child","status":"in_progress"}]})",
						 "update_plan");
			return R"({"content":"child checklist resumed"})";
		}
		if (body.find("checklist_first") == std::string::npos)
			return plan_call("checklist_first", R"({"plan":[
				{"step":"Inspect child","status":"in_progress"},
				{"step":"Verify child","status":"pending"}]})",
					 "update_plan");
		return R"({"content":"child checklist created"})";
	};
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"checklist","message":"Plan child work",
		"fork_turns":"none"})"),
		  0);
	agent_control_join(control);
	int64_t child_session = ChildSession();
	ASSERT_GT(child_session, 0);
	auto child = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(child.get(), &database, child_session), 0);
	ASSERT_EQ(child->count, 1);
	EXPECT_STREQ(child->plans[0].name, "Task plan");
	EXPECT_EQ(child->plans[0].active_step, 0);
	EXPECT_EQ(plans->count, 1);
	EXPECT_STREQ(plans->plans[0].name, "root-plan");
	ASSERT_EQ(server.snapshot().size(), 2u);
	EXPECT_NE(server.snapshot().back().find("Durable task checkpoint"), std::string::npos);
	EXPECT_EQ(server.snapshot().back().find("root-plan"), std::string::npos);
	char **events = nullptr;
	int count = 0;
	ASSERT_EQ(agent_control_events_copy(control, "/root/checklist", &events, &count), 0);
	bool updated = false;
	for (int i = 0; i < count; i++)
		updated |= std::strstr(events[i], "plan.updated") != nullptr;
	agent_control_free_events(events, count);
	EXPECT_TRUE(updated);

	Reopen();
	ASSERT_EQ(Call("followup_task", R"({"target":"checklist",
		"message":"resume-checklist and verify"})"),
		  0);
	agent_control_join(control);
	child = std::make_unique<struct plan_registry>();
	ASSERT_EQ(plan_registry_bind(child.get(), &database, child_session), 0);
	ASSERT_EQ(child->count, 1);
	EXPECT_STREQ(child->plans[0].steps[0].status, "completed");
	EXPECT_EQ(child->plans[0].active_step, 1);
	EXPECT_EQ(server.snapshot().size(), 4u);
	EXPECT_EQ(plans->count, 1);
	EXPECT_STREQ(plans->plans[0].steps[0].status, "in_progress");
}
