#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/react.h"
#include "agent/tokenizer.h"
#include "agent/tools/multi_agents.h"
#include "agent/tools/skill_activate.h"
#include "agent/tools/dynamic_tools.h"
#include "agent/tools/dynamic_tool_internal.h"
#include "agent/tools/scheduled_tasks.h"
#include "agent/tools/ask_user.h"
#include "db/database.h"
#include "models/llm.h"
#include "runtime/bootstrap.h"
#include "session.h"
#include "skill/skill.h"
#include "errors.h"
}

#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <memory>
#include <string>

class AgentChildRuntimeTest : public testing::Test
{
      protected:
	AgentHttpServer server;
	struct db database{};
	struct session session{};
	struct tool_registry tools{};
	struct skill_registry root_skills{};
	std::unique_ptr<struct config> config;
	struct model *model = nullptr;
	struct tokenizer *tokenizer = nullptr;
	struct agent_control *control = nullptr;
	struct tool_runtime_context runtime{};
	char directory[PATH_MAX]{};

	void SetUp() override
	{
		server.start();
		std::strcpy(directory, "/tmp/morph-child-runtime-XXXXXX");
		ASSERT_NE(mkdtemp(directory), nullptr);
		std::string skills = std::string(directory) + "/.agents/skills/child-check";
		std::filesystem::create_directories(skills);
		std::ofstream(skills + "/SKILL.md")
			<< "---\nname: child-check\ndescription: Inspect a child's result\n---\n"
			   "CHILD_PRIVATE_SKILL_INSTRUCTIONS: verify the assigned result.\n";
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "root-context", "root-model", &session), 0);
		tool_registry_init(nullptr, &tools);
		skill_registry_init(nullptr, &root_skills);
		ASSERT_EQ(skill_discover(&root_skills,
					 (std::string(directory) + "/.agents/skills").c_str()),
			  1);
		ASSERT_EQ(skill_activate_init(&tools, &root_skills), 0);
		tool_borrow(&tools, "activate_skill")->flags |= TOOL_FLAG_READONLY;
		std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
		model = model_llm_create(nullptr, "openai", "root-model", url.c_str(), "test-key");
		ASSERT_NE(model, nullptr);
		model->retry_count = 0;
		tokenizer = tokenizer_create(nullptr, "root-model", 128000);
		ASSERT_NE(tokenizer, nullptr);
		config = std::make_unique<struct config>();
		config_set_defaults(nullptr, config.get());
		std::snprintf(config->skill.dir, sizeof(config->skill.dir), "%s/.agents/skills",
			      directory);
		config->agents.rollout_budget.enabled = 0;
		std::strcpy(config->agents.multi_agent_mode, "proactive");
		runtime.db = &database;
		runtime.config = config.get();
		runtime.workdir = directory;
		runtime.memory_session_id = session.id;
		runtime.agent_id = "/root";
		Reopen();
	}

	void Reopen()
	{
		agent_control_destroy(control);
		control = agent_control_create(&tools, model, tokenizer, nullptr, &config->agents,
					       &database);
		ASSERT_NE(control, nullptr);
		ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
		ASSERT_EQ(agent_control_set_child_initializer(control, runtime_bootstrap_child,
							      nullptr),
			  0);
		ASSERT_EQ(multi_agents_register(&tools, control), 0);
	}

	void TearDown() override
	{
		agent_control_destroy(control);
		tool_registry_cleanup(&tools);
		skill_registry_cleanup(&root_skills);
		model_destroy(model);
		tokenizer_destroy(tokenizer);
		db_close(&database);
		std::filesystem::remove_all(directory);
	}

	int Call(const char *operation, const char *text)
	{
		cJSON *args = cJSON_Parse(text);
		cJSON *out = nullptr;
		int rc = agent_control_call(control, operation, args, &runtime, &out);
		cJSON_Delete(args);
		cJSON_Delete(out);
		return rc;
	}

	struct agent_info Info()
	{
		struct agent_info *items = nullptr;
		int count = 0;
		EXPECT_EQ(agent_control_list_copy(control, session.id, &items, &count), 0);
		struct agent_info info{};
		if (count) {
			info.status = items[0].status;
			info.error_code = items[0].error_code;
			info.child_session_id = items[0].child_session_id;
		}
		agent_control_free_list(items, count);
		return info;
	}
};

TEST_F(AgentChildRuntimeTest, SkillActivationIsPrivateAndRestoresAfterColdFollowup)
{
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("CHILD_PRIVATE_SKILL_INSTRUCTIONS") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"skill_call","type":"function",)"
			       R"("function":{"name":"activate_skill",)"
			       R"("arguments":"{\"name\":\"child-check\"}"}}]})";
		return R"({"content":"child skill received"})";
	};
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"reader","fork_turns":"none",
		"message":"Activate child-check and inspect the assigned result"})"),
		  0);
	agent_control_join(control);
	EXPECT_EQ(Info().status, AGENT_COMPLETED);
	EXPECT_FALSE(skill_borrow(&root_skills, "child-check")->activated);
	EXPECT_EQ(skill_borrow(&root_skills, "child-check")->body, nullptr);
	auto requests = server.snapshot();
	ASSERT_EQ(requests.size(), 2u);
	EXPECT_EQ(requests[0].find("CHILD_PRIVATE_SKILL_INSTRUCTIONS"), std::string::npos);
	EXPECT_NE(requests[1].find("CHILD_PRIVATE_SKILL_INSTRUCTIONS"), std::string::npos);
	struct skill_registry restored NB_CLEANUP(skill_registry_cleanup) = {};
	skill_registry_init(nullptr, &restored);
	ASSERT_EQ(skill_discover(&restored, config->skill.dir), 1);
	ASSERT_EQ(skill_restore_session(&restored, &database, Info().child_session_id), 0);
	EXPECT_TRUE(skill_borrow(&restored, "child-check")->activated);
	ASSERT_EQ(skill_restore_session(&restored, &database, session.id), 0);
	EXPECT_FALSE(skill_borrow(&restored, "child-check")->activated);
	Reopen();
	ASSERT_EQ(Call("followup_task", R"({"target":"reader","message":"resume_skill"})"), 0);
	agent_control_join(control);
	requests = server.snapshot();
	ASSERT_EQ(requests.size(), 3u);
	EXPECT_NE(requests[2].find("resume_skill"), std::string::npos);
	EXPECT_NE(requests[2].find("CHILD_PRIVATE_SKILL_INSTRUCTIONS"), std::string::npos);
	EXPECT_FALSE(skill_borrow(&root_skills, "child-check")->activated);
}

TEST_F(AgentChildRuntimeTest, ConfiguredInputGuardrailBlocksChildAndDisabledRuleAllowsIt)
{
	config->react.guardrail_ext_rule_count = 1;
	auto &rule = config->react.guardrail_ext_rules[0];
	std::strcpy(rule.name, "child_input_policy");
	std::strcpy(rule.hook, "input");
	std::strcpy(rule.ext_type, "exec");
	std::strcpy(rule.ext_entry, "/definitely-missing-morph-child-guardrail");
	std::strcpy(rule.on_error, "block");
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"blocked","message":"Inspect files"})"), 0);
	agent_control_join(control);
	EXPECT_EQ(Info().status, AGENT_FAILED);
	EXPECT_TRUE(server.snapshot().empty());
	config->react.guardrail_disabled_rule_count = 1;
	std::strcpy(config->react.guardrail_disabled_rules[0], "child_input_policy");
	ASSERT_EQ(Call("followup_task", R"({"target":"blocked","message":"Inspect files again"})"),
		  0);
	agent_control_join(control);
	EXPECT_EQ(Info().status, AGENT_COMPLETED);
	EXPECT_EQ(server.snapshot().size(), 1u);
}

TEST_F(AgentChildRuntimeTest, DrainingKeepsLateSteeringQueuedUntilAnExplicitRestart)
{
	struct Observer {
		std::function<int()> followup;
		struct agent_control *control;
		std::atomic<bool> followed{false};
	} observer{[this] {
			   return Call("followup_task",
				       R"({"target":"reader","message":"late-drain-steering"})");
		   },
		   control};
	ASSERT_EQ(agent_control_set_event_callback(
			  control,
			  [](const struct morph_event *event, void *opaque) -> int {
				  auto *state = static_cast<Observer *>(opaque);
				  if (event->name && !std::strcmp(event->name, "react.turn.end") &&
				      !state->followed.exchange(true)) {
					  agent_control_set_interactions_blocked(state->control, 1);
					  return state->followup();
				  }
				  return 0;
			  },
			  &observer),
		  0);
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"first task completed"})";
	};
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"reader","message":"Inspect files"})"), 0);
	ASSERT_TRUE(server.await_requests(1));
	ASSERT_EQ(agent_control_select(control, "/root/reader"), 0);
	release->set_value();
	agent_control_join(control);
	EXPECT_TRUE(observer.followed);
	EXPECT_EQ(Info().status, AGENT_COMPLETED);
	EXPECT_EQ(server.snapshot().size(), 1u);
	char *mail NB_CLEANUP(nb_charp_cleanup) = nullptr;
	int64_t cursor = 0;
	ASSERT_EQ(agent_control_receive(control, "/root/reader", session.id, &mail, &cursor), 0);
	ASSERT_NE(mail, nullptr);
	EXPECT_NE(std::strstr(mail, "late-drain-steering"), nullptr);
	agent_control_set_interactions_blocked(control, 0);
	ASSERT_EQ(Call("followup_task", R"({"target":"reader","message":"resume safely"})"), 0);
	agent_control_join(control);
	EXPECT_EQ(server.snapshot().size(), 2u);
	EXPECT_NE(server.snapshot().back().find("late-drain-steering"), std::string::npos);
	agent_control_set_event_callback(control, nullptr, nullptr);
}

TEST_F(AgentChildRuntimeTest, FailedTurnNeverRetriesItsUnacknowledgedFollowupAutomatically)
{
	config->react.guardrail_ext_rule_count = 1;
	auto &rule = config->react.guardrail_ext_rules[0];
	std::strcpy(rule.name, "failing_input");
	std::strcpy(rule.hook, "input");
	std::strcpy(rule.ext_type, "exec");
	std::strcpy(rule.ext_entry, "/definitely-missing-morph-child-guardrail");
	std::strcpy(rule.on_error, "block");
	struct Observer {
		std::function<int()> followup;
		std::atomic<bool> followed{false};
	} observer{[this] {
		return Call("followup_task",
			    R"({"target":"reader","message":"recover-queued-work"})");
	}};
	ASSERT_EQ(agent_control_set_event_callback(
			  control,
			  [](const struct morph_event *event, void *opaque) -> int {
				  auto *state = static_cast<Observer *>(opaque);
				  if (event->name && !std::strcmp(event->name, "agent.started") &&
				      !state->followed.exchange(true))
					  return state->followup();
				  return 0;
			  },
			  &observer),
		  0);
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"reader","message":"Inspect files"})"), 0);
	agent_control_join(control);
	EXPECT_TRUE(observer.followed);
	EXPECT_EQ(Info().status, AGENT_FAILED);
	EXPECT_TRUE(server.snapshot().empty());
	config->react.guardrail_disabled_rule_count = 1;
	std::strcpy(config->react.guardrail_disabled_rules[0], "failing_input");
	ASSERT_EQ(Call("followup_task", R"({"target":"reader","message":"retry after fix"})"), 0);
	agent_control_join(control);
	EXPECT_EQ(Info().status, AGENT_COMPLETED);
	ASSERT_EQ(server.snapshot().size(), 1u);
	EXPECT_NE(server.snapshot()[0].find("recover-queued-work"), std::string::npos);
	agent_control_set_event_callback(control, nullptr, nullptr);
}

TEST_F(AgentChildRuntimeTest, GuardrailHooksAndCheckerModelUseTheChildConfiguration)
{
	config->react.guardrail_max_retries = 7;
	config->react.guardrail_max_empty_rounds = 5;
	config->react.guardrail_llm_rule_count = 1;
	std::strcpy(config->react.guardrail_llm_rules[0].name, "child_tool_rule");
	std::strcpy(config->react.guardrail_llm_rules[0].hook, "tool_output");
	std::strcpy(config->react.guardrail_llm_rules[0].description, "Validate child tool output");
	std::strcpy(config->react.guardrail_llm_model, "checker-model");
	config->react.guardrail_disabled_rule_count = 1;
	std::strcpy(config->react.guardrail_disabled_rules[0], "empty_answer");
	struct tool_registry child_tools{};
	tool_registry_init(nullptr, &child_tools);
	ASSERT_EQ(tool_registry_copy(&child_tools, &tools), 0);
	struct react_context *react =
		react_context_create(&child_tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	struct agent_child_state owner{};
	ASSERT_EQ(runtime_bootstrap_child(&child_tools, nullptr, react, model, &runtime, &owner,
					  nullptr),
		  0);
	EXPECT_TRUE(react->guardrail.enabled);
	EXPECT_EQ(react->guardrail.max_retries, 7);
	EXPECT_EQ(react->guardrail.max_empty_rounds, 5);
	auto *rule = guardrail_rule_borrow(&react->guardrail, "child_tool_rule");
	ASSERT_NE(rule, nullptr);
	EXPECT_EQ(rule->hook, GUARDRAIL_HOOK_TOOL_OUTPUT);
	EXPECT_FALSE(guardrail_rule_borrow(&react->guardrail, "empty_answer")->enabled);
	ASSERT_NE(react->guardrail.llm, nullptr);
	EXPECT_NE(react->guardrail.llm, model);
	EXPECT_STREQ(react->guardrail.llm->model_id, "checker-model");
	EXPECT_EQ(react->guardrail.llm->budget_sink, model->budget_sink);
	EXPECT_NE(react->skills, &root_skills);
	react_context_destroy(react);
	tool_registry_cleanup(&child_tools);
	owner.destroy(owner.data);
}

TEST_F(AgentChildRuntimeTest, SessionSkillRowsCascadeWithTheirSession)
{
	ASSERT_EQ(skill_activate_session(&root_skills, &database, session.id, "child-check", 1), 0);
	ASSERT_EQ(session_delete(&database, session.id), 0);
	sqlite3_stmt *statement = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(database.handle,
				     "SELECT COUNT(*) FROM session_skill_activations", -1,
				     &statement, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(statement, 0), 0);
	sqlite3_finalize(statement);
}

TEST(AgentChildGuardrailTest, CheckerRequestUsesTurnCancellationWithoutChangingItsClient)
{
	struct Checker {
		const struct nb_cancel *expected;
		bool called;
	};
	struct nb_cancel token{};
	struct Checker checked{&token, false};
	struct model client{};
	client.handle = &checked;
	client.chat = [](struct model *self, const char *, const char **, int,
			 const struct model_chat_options *, sse_callback callback,
			 void *opaque) -> int {
		auto *state = static_cast<Checker *>(self->handle);
		state->called = true;
		EXPECT_EQ(self->http.cancel_token, state->expected);
		return callback("{\"pass\":true}", opaque);
	};
	struct guardrail_config config{};
	config.enabled = 1;
	config.llm = &client;
	ASSERT_EQ(guardrail_rule_register(&config, "input_rule", GUARDRAIL_HOOK_INPUT,
					  GUARDRAIL_RULE_LLM, nullptr, "Validate child input",
					  nullptr, nullptr),
		  0);
	struct guardrail_eval_ctx eval{};
	eval.user_input = "child input";
	eval.cancel_token = &token;
	EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_INPUT, &eval).verdict, GUARDRAIL_PASS);
	EXPECT_TRUE(checked.called);
	EXPECT_EQ(client.http.cancel_token, nullptr);
}

static void child_test_state_cleanup(struct agent_child_state *owner)
{
	if (owner->destroy)
		owner->destroy(owner->data);
}

TEST_F(AgentChildRuntimeTest, MutableToolBindingsRemainPrivateAndNeverBorrowAFrontend)
{
	ASSERT_EQ(dynamic_tools_init(&tools, nullptr, &config->dynamic_tools, "root-session"), 0);
	ASSERT_EQ(scheduled_tasks_tool_init(&tools, &database), 0);
	ASSERT_EQ(scheduled_tasks_tool_set_source_session(&tools, session.id), 0);
	int frontend_calls = 0;
	ASSERT_EQ(
		ask_user_init(
			&tools,
			[](const char *, const char *const *, int, const char *, int, int,
			   const struct ask_user_options *, char ***, int *, void *opaque) -> int {
				++*static_cast<int *>(opaque);
				return NB_ERR_CANCELLED;
			},
			&frontend_calls),
		0);
	struct session child_session{};
	ASSERT_EQ(session_create(&database, "child-context", "root-model", &child_session), 0);
	struct tool_runtime_context child = runtime;
	child.memory_session_id = child_session.id;
	struct agent_child_state owner NB_CLEANUP(child_test_state_cleanup) = {};
	struct tool_registry child_tools NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &child_tools);
	ASSERT_EQ(tool_registry_copy(&child_tools, &tools), 0);
	struct react_context *react NB_CLEANUP(react_context_clear) =
		react_context_create(&child_tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	ASSERT_EQ(runtime_bootstrap_child(&child_tools, nullptr, react, model, &child, &owner,
					  nullptr),
		  0);
	ASSERT_EQ(dynamic_tools_set_session_id(&child_tools, "child-updated"), 0);
	auto *root_dynamic = static_cast<struct dynamic_tools_context *>(
		tool_borrow(&tools, "tool_create")->user_data);
	auto *child_dynamic = static_cast<struct dynamic_tools_context *>(
		tool_borrow(&child_tools, "tool_create")->user_data);
	EXPECT_NE(root_dynamic, child_dynamic);
	EXPECT_EQ(root_dynamic->reg, &tools);
	EXPECT_EQ(child_dynamic->reg, &child_tools);
	EXPECT_STREQ(root_dynamic->session_id, "root-session");
	EXPECT_STREQ(child_dynamic->session_id, "child-updated");
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&child_tools, "ask_user", R"({"question":"Need clarification"})",
			    &result),
		  MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(frontend_calls, 0);
	EXPECT_EQ(tool_exec(&child_tools, "tasks", R"({"op":"create","title":"Child task",
		"prompt":"Inspect the child result","delay_seconds":10})",
			    &result),
		  0);
	sqlite3_stmt *statement = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(
			  database.handle,
			  "SELECT source_session_id FROM scheduled_tasks WHERE title='Child task'",
			  -1, &statement, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int64(statement, 0), child_session.id);
	sqlite3_finalize(statement);
}
