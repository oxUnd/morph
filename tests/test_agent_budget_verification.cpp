#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/react.h"
#include "agent/turn.h"
#include "agent/tokenizer.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/multi_agents.h"
#include "models/llm.h"
#include "permission/context.h"
#include "session.h"
#include "util/cleanup.h"
#include "util/error.h"
}

#include <chrono>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <thread>

class AgentBudgetVerificationTest : public testing::Test
{
      protected:
	AgentHttpServer server;
	struct db database{};
	struct session session{};
	struct config config{};
	struct tool_registry tools{};
	struct model *model = nullptr;
	struct tokenizer *tokenizer = nullptr;
	struct agent_control *control = nullptr;
	struct tool_runtime_context runtime{};

	void SetUp() override
	{
		server.start();
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "budget root", "test-model", &session), 0);
		config_set_defaults(nullptr, &config);
		tool_registry_init(nullptr, &tools);
		std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
		model = model_llm_create(nullptr, "openai", "test-model", url.c_str(), "test-key");
		ASSERT_NE(model, nullptr);
		model->retry_count = 0;
		model->max_tokens = 64;
		tokenizer = tokenizer_create(nullptr, "test-model", 128000);
		ASSERT_NE(tokenizer, nullptr);
		Reopen();
		runtime.db = &database;
		runtime.memory_session_id = session.id;
		runtime.agent_id = "/root";
	}

	void Reopen()
	{
		agent_control_destroy(control);
		control = agent_control_create(&tools, model, tokenizer, nullptr, &config.agents,
					       &database);
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

	int Call(const char *operation, const char *json)
	{
		cJSON *args = cJSON_Parse(json);
		cJSON *out = nullptr;
		int rc = agent_control_call(control, operation, args, &runtime, &out);
		cJSON_Delete(args);
		cJSON_Delete(out);
		return rc;
	}

	void Usage(int64_t input, int64_t cached, int64_t output)
	{
		struct model_usage usage{};
		usage.input_tokens = input;
		usage.cached_tokens = cached;
		usage.output_tokens = output;
		model_report_usage(model, &usage);
	}
};

TEST_F(AgentBudgetVerificationTest, WeightedUsageExcludesCacheAndPersistsAcrossTurnsAndColdRestore)
{
	config.agents.budget_input_weight = 2;
	config.agents.budget_output_weight = 4;
	Reopen();
	agent_control_begin_turn(control, nullptr);
	Usage(100, 80, 10);
	agent_control_end_turn(control);
	agent_control_begin_turn(control, nullptr);
	Usage(10, 0, 5);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 120);
	EXPECT_EQ(budget.input_tokens, 110);
	EXPECT_EQ(budget.cached_tokens, 80);
	EXPECT_EQ(budget.output_tokens, 15);
	agent_control_end_turn(control);
	Reopen();
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 120);
	struct session other{};
	ASSERT_EQ(session_create(&database, "other budget", "test-model", &other), 0);
	ASSERT_EQ(agent_control_set_parent_session(control, other.id), 0);
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 0);
}

TEST_F(AgentBudgetVerificationTest, ChildAllocationStopsDispatchWithoutCancellingPrimaryReserve)
{
	config.agents.max_total_tokens = 1000;
	Reopen();
	agent_control_begin_turn(control, nullptr);
	Usage(800, 0, 0);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_TRUE(budget.dispatch_paused);
	EXPECT_FALSE(budget.exhausted);
	EXPECT_EQ(budget.primary_reserve, 200);
	EXPECT_EQ(Call("spawn_agent", R"({"task_name":"extra","message":"more work"})"),
		  MORPH_ERR_AGENT_BUDGET);
	EXPECT_EQ(agent_control_budget_check_for(control, "/root/worker", 1),
		  MORPH_ERR_AGENT_BUDGET);
	EXPECT_EQ(agent_control_budget_check_for(control, "/root", 200), 0);
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_FALSE(budget.exhausted);
	Usage(200, 0, 0);
	EXPECT_EQ(agent_control_budget_check_for(control, "/root", 0), MORPH_ERR_TOKEN_BUDGET);
	agent_control_end_turn(control);
}

TEST_F(AgentBudgetVerificationTest, InflightReservationsWaitAndReleaseWithoutStealingPrimaryPool)
{
	config.agents.max_total_tokens = 1000;
	Reopen();
	agent_control_begin_turn(control, nullptr);
	int64_t primary = 0;
	int64_t child = 0;
	ASSERT_EQ(agent_control_budget_reserve(control, "/root", 200, 0, &primary), 0);
	ASSERT_EQ(agent_control_budget_reserve(control, "/root/worker", 800, 0, &child), 0);
	auto next = std::async(std::launch::async, [&] {
		int64_t reservation = 0;
		int rc = agent_control_budget_reserve(control, "/root/second", 20, 0, &reservation);
		agent_control_budget_release_for(control, "/root/second", reservation);
		return rc;
	});
	EXPECT_EQ(next.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
	agent_control_budget_release_for(control, "/root/worker", child);
	EXPECT_EQ(next.get(), 0);
	agent_control_budget_release(control, primary);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.reserved, 0);
	EXPECT_EQ(budget.child_reserved, 0);
	agent_control_end_turn(control);
}

TEST_F(AgentBudgetVerificationTest, ReservationWaitObservesCancellation)
{
	config.agents.max_total_tokens = 1000;
	Reopen();
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	agent_control_begin_turn(control, react);
	int64_t held = 0;
	ASSERT_EQ(agent_control_budget_reserve(control, "/root", 1000, 0, &held), 0);
	auto next = std::async(std::launch::async, [&] {
		int64_t reservation = 0;
		return agent_control_budget_reserve(control, "/root", 1, 0, &reservation);
	});
	EXPECT_EQ(next.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
	react_cancel(react);
	EXPECT_EQ(next.get(), -ECANCELED);
	agent_control_budget_release(control, held);
	agent_control_end_turn(control);
	react_context_destroy(react);
}

TEST_F(AgentBudgetVerificationTest, RebindingActiveSessionPreservesInflightReservations)
{
	config.agents.max_total_tokens = 1000;
	Reopen();
	agent_control_begin_turn(control, nullptr);
	int64_t reservation = 0;
	ASSERT_EQ(agent_control_budget_reserve(control, "/root", 100, 0, &reservation), 0);
	ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.reserved, 100);
	struct session another{};
	ASSERT_EQ(session_create(&database, "another root", "test-model", &another), 0);
	EXPECT_EQ(agent_control_set_parent_session(control, another.id), -EBUSY);
	agent_control_budget_release(control, reservation);
	agent_control_end_turn(control);
	ASSERT_EQ(agent_control_set_parent_session(control, another.id), 0);
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.reserved, 0);
}

TEST_F(AgentBudgetVerificationTest, WarnOnlyLimitAccountsAndRemindsWithoutStopping)
{
	config.agents.max_total_tokens = 100;
	std::strcpy(config.agents.budget_enforcement, "warn");
	Reopen();
	agent_control_begin_turn(control, nullptr);
	Usage(500, 0, 0);
	EXPECT_EQ(agent_control_budget_check_for(control, "/root/worker", 1000), 0);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 500);
	EXPECT_FALSE(budget.exhausted);
	EXPECT_FALSE(budget.dispatch_paused);
	EXPECT_EQ(budget.reminder_level, 3);
	agent_control_end_turn(control);
}

TEST_F(AgentBudgetVerificationTest, ReservedPrimaryAllocationKeepsLateFollowupQueued)
{
	config.agents.max_total_tokens = 100000;
	Reopen();
	struct Observer {
		std::function<int()> queue;
		std::atomic<bool> followed{false};
	} observer{[this] {
		Usage(80000, 0, 0);
		return Call("followup_task",
			    R"({"target":"reader","message":"budget-queued-followup"})");
	}};
	ASSERT_EQ(agent_control_set_event_callback(
			  control,
			  [](const struct morph_event *event, void *opaque) -> int {
				  auto *state = static_cast<Observer *>(opaque);
				  if (event->name && !std::strcmp(event->name, "react.turn.end") &&
				      !state->followed.exchange(true))
					  return state->queue();
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
	EXPECT_EQ(server.snapshot().size(), 1u);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_TRUE(budget.dispatch_paused);
	EXPECT_FALSE(budget.exhausted);
	char *mail MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	int64_t cursor = 0;
	ASSERT_EQ(agent_control_receive(control, "/root/reader", session.id, &mail, &cursor), 0);
	ASSERT_NE(mail, nullptr);
	EXPECT_NE(std::strstr(mail, "budget-queued-followup"), nullptr);
	config.agents.max_total_tokens = 200000;
	Reopen();
	ASSERT_EQ(Call("followup_task",
		       R"({"target":"reader","message":"resume after budget update"})"),
		  0);
	agent_control_join(control);
	ASSERT_EQ(server.snapshot().size(), 2u);
	EXPECT_NE(server.snapshot().back().find("budget-queued-followup"), std::string::npos);
}

TEST_F(AgentBudgetVerificationTest, ReminderIsAckedOnlyAfterEnteringModelHistory)
{
	config.agents.max_total_tokens = 100000;
	Reopen();
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->llm_model = model;
	react->agents = control;
	react->max_iterations = 3;
	react_set_tool_runtime_context(react, &runtime);
	struct agent_session_runtime session_runtime{};
	session_runtime.db = &database;
	session_runtime.session_id = session.id;
	session_runtime.react = react;
	session_runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn turn{};
	struct agent_turn_input input{};
	input.model_input = "Integrate existing findings";
	ASSERT_EQ(agent_turn_begin(&turn, &session_runtime, &input), 0);
	agent_control_begin_turn(control, react);
	Usage(70000, 0, 0);
	sqlite3_stmt *query = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(
			  database.handle,
			  "SELECT consumed FROM agent_mailbox WHERE kind='BUDGET_REMINDER'", -1,
			  &query, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(query), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(query, 0), 0);
	sqlite3_finalize(query);
	EXPECT_EQ(react_run(react, input.model_input, nullptr, nullptr), 0);
	EXPECT_EQ(agent_turn_finish(&turn, nullptr), 0);
	agent_control_end_turn(control);
	auto requests = server.snapshot();
	ASSERT_FALSE(requests.empty());
	EXPECT_NE(requests.back().find("BUDGET_REMINDER"), std::string::npos);
	ASSERT_EQ(sqlite3_prepare_v2(
			  database.handle,
			  "SELECT consumed FROM agent_mailbox WHERE kind='BUDGET_REMINDER'", -1,
			  &query, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(query), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(query, 0), 1);
	sqlite3_finalize(query);
	react_context_destroy(react);
	Reopen();
	agent_control_begin_turn(control, nullptr);
	Usage(1, 0, 0);
	ASSERT_EQ(sqlite3_prepare_v2(
			  database.handle,
			  "SELECT count(*) FROM agent_mailbox WHERE kind='BUDGET_REMINDER'", -1,
			  &query, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(query), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(query, 0), 1);
	sqlite3_finalize(query);
	agent_control_end_turn(control);
}

TEST_F(AgentBudgetVerificationTest, RootGuardrailUsageSharesBudgetAndBorrowedSinksAreRestored)
{
	struct react_context *react = react_context_create(&tools, tokenizer, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->guardrail_model = model_llm_clone(model, nullptr);
	ASSERT_NE(react->guardrail_model, nullptr);
	EXPECT_EQ(react->guardrail_model->budget_sink, nullptr);
	agent_control_begin_turn(control, react);
	struct model_usage usage{};
	usage.input_tokens = 10;
	usage.output_tokens = 3;
	model_report_usage(react->guardrail_model, &usage);
	struct agent_budget budget{};
	ASSERT_EQ(agent_control_budget_copy(control, &budget), 0);
	EXPECT_EQ(budget.used, 13);
	agent_control_end_turn(control);
	EXPECT_EQ(react->guardrail_model->budget_sink, nullptr);
	react_context_destroy(react);
}

TEST_F(AgentBudgetVerificationTest,
       ScopedChildCompilesAndRunsCTestInPrivateScratchButCannotWriteSibling)
{
	char directory[] = "/tmp/morph-agent-verify-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::ofstream(std::string(directory) + "/CMakeLists.txt")
		<< "cmake_minimum_required(VERSION 3.20)\nproject(child_verify C)\n"
		   "enable_testing()\nadd_executable(verify main.c)\n"
		   "add_test(NAME verifies COMMAND verify)\n";
	std::ofstream(std::string(directory) + "/main.c") << "int main(void) { return 0; }\n";
	std::ofstream(std::string(directory) + "/sibling.txt") << "preserve\n";
	runtime.workdir = directory;
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"writer","message":"verify",
		"fork_turns":"none","write_paths":["main.c"]})"),
		  0);
	agent_control_join(control);
	auto *permissions = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(permissions, nullptr);
	struct permission_policy policy{PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	ASSERT_EQ(exec_tool_init(&tools, permissions, nullptr), 0);
	struct tool_runtime_context child = runtime;
	child.agents = control;
	child.agent_id = "/root/writer";
	child.agent_depth = 1;
	struct tool_result result{};
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&tools, "exec", R"({"command":"sleep 60","service":true})", &result),
		  -EACCES);
	EXPECT_EQ(
		tool_exec(&tools, "exec", R"({"command":"sleep 60","notify_only":true})", &result),
		-EACCES);
	ASSERT_EQ(tool_exec(&tools, "exec", R"({"command":
		"cmake -S . -B \"$MORPH_AGENT_SCRATCH/build\" >/dev/null && cmake --build \"$MORPH_AGENT_SCRATCH/build\" >/dev/null && ctest --test-dir \"$MORPH_AGENT_SCRATCH/build\" --output-on-failure && printf '\nSCRATCH=%s\n' \"$MORPH_AGENT_SCRATCH\"",
		"yield_time_ms":10000,"timeout_ms":20000})",
			    &result),
		  0);
	const char *output = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout"));
	const char *errors = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stderr"));
	ASSERT_NE(output, nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0)
		<< (errors ? errors : "") << output;
	EXPECT_NE(strstr(output, "100% tests passed"), nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "execution_mode")),
		     "sandbox");
	std::string scratch;
	const char *marker = strstr(output, "SCRATCH=");
	if (marker) {
		scratch = marker + 8;
		scratch = scratch.substr(0, scratch.find('\n'));
	}
	EXPECT_FALSE(scratch.empty());
	tool_result_cleanup(&result);
	tool_result_init(&result);
	result.runtime = &child;
	ASSERT_EQ(
		tool_exec(
			&tools, "exec",
			R"({"command":"(sleep 0.5; printf escaped > main.c) >/dev/null 2>&1 & printf launched","yield_time_ms":1000})",
			&result),
		0);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")), "launched");
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	std::ifstream protected_source(std::string(directory) + "/main.c");
	std::string content;
	std::getline(protected_source, content);
	EXPECT_EQ(content, "int main(void) { return 0; }");
	tool_result_cleanup(&result);
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(
		tool_exec(
			&tools, "exec",
			R"({"command":"printf denied > sibling.txt","yield_time_ms":1000,"host":true})",
			&result),
		0);
	EXPECT_NE(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	std::ifstream sibling(std::string(directory) + "/sibling.txt");
	std::getline(sibling, content);
	EXPECT_EQ(content, "preserve");
	tool_result_cleanup(&result);
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(
		tool_exec(
			&tools, "exec",
			R"({"command":"printf 'int main(void) { return 0; }\n' > main.c","yield_time_ms":1000})",
			&result),
		0);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	tool_result_cleanup(&result);
	policy.access = PERMISSION_READ_ONLY;
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	tool_result_init(&result);
	result.runtime = &child;
	int parent_readonly_rc =
		tool_exec(&tools, "exec",
			  R"({"command":"printf denied > main.c","yield_time_ms":1000})", &result);
	if (parent_readonly_rc == 0)
		EXPECT_NE(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	else
		EXPECT_EQ(parent_readonly_rc, -EACCES);
	tool_result_cleanup(&result);
	std::ifstream own_source(std::string(directory) + "/main.c");
	std::getline(own_source, content);
	EXPECT_EQ(content, "int main(void) { return 0; }");
	std::filesystem::path scratch_base = std::filesystem::path(scratch).parent_path();
	tool_registry_cleanup(&tools);
	if (!scratch.empty())
		EXPECT_FALSE(std::filesystem::exists(scratch_base));
	tool_registry_init(nullptr, &tools);
	tool_context_destroy(permissions);
	std::filesystem::remove_all(directory);
	runtime.workdir = nullptr;
}

TEST_F(AgentBudgetVerificationTest, AncestorShellCannotRetainBroadLeaseWhileNestedWriterOwnsAFile)
{
	char directory[] = "/tmp/morph-ancestor-verify-XXXXXX";
	char scratch[] = "/tmp/morph-ancestor-scratch-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	ASSERT_NE(mkdtemp(scratch), nullptr);
	std::filesystem::create_directory(std::string(directory) + "/owned");
	std::ofstream(std::string(directory) + "/owned/a.c") << "preserve\n";
	runtime.workdir = directory;
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	server.respond = [ready](const std::string &) {
		ready.wait_for(std::chrono::seconds(5));
		return R"({"content":"finished"})";
	};
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"parent","message":"coordinate",
		"fork_turns":"none","write_paths":["owned"]})"),
		  0);
	ASSERT_TRUE(server.await_requests(1));
	runtime.agent_id = "/root/parent";
	runtime.agent_depth = 1;
	ASSERT_EQ(Call("spawn_agent", R"({"task_name":"nested","message":"edit one file",
		"fork_turns":"none","write_paths":["owned/a.c"]})"),
		  0);
	struct agent_shell_scope parent_scope{};
	struct agent_shell_scope nested_scope{};
	ASSERT_EQ(agent_control_shell_scope_copy(control, "/root/parent", scratch, &parent_scope),
		  0);
	ASSERT_EQ(agent_control_shell_scope_copy(control, "/root/parent/nested", scratch,
						 &nested_scope),
		  0);
	EXPECT_EQ(parent_scope.write_path_count, 1);
	EXPECT_EQ(nested_scope.write_path_count, 2);
	auto *permissions = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(permissions, nullptr);
	struct permission_policy policy{PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), 0);
	ASSERT_EQ(exec_tool_init(&tools, permissions, nullptr), 0);
	struct tool_runtime_context child = runtime;
	child.agents = control;
	struct tool_result result{};
	tool_result_init(&result);
	result.runtime = &child;
	EXPECT_EQ(tool_exec(&tools, "exec",
			    R"({"command":"printf denied > owned/a.c","yield_time_ms":1000})",
			    &result),
		  0);
	EXPECT_NE(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	tool_result_cleanup(&result);
	release->set_value();
	agent_control_join(control);
	agent_shell_scope_cleanup(&parent_scope);
	agent_shell_scope_cleanup(&nested_scope);
	tool_registry_cleanup(&tools);
	tool_registry_init(nullptr, &tools);
	tool_context_destroy(permissions);
	std::filesystem::remove_all(directory);
	std::filesystem::remove_all(scratch);
	runtime.workdir = nullptr;
	runtime.agent_id = "/root";
	runtime.agent_depth = 0;
}
