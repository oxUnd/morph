#include "exec/process.h"
#include "agent/tools/exec_tool.h"
#include "agent/tool.h"
#include "agent/tool_context.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

struct ExecApprovalState {
	int calls = 0;
	std::string command;
	std::string reason;
	std::vector<std::string> programs;
};

static enum tool_operation_verdict approve_exec_operation(
	const struct tool_operation *op, void *user_data)
{
	auto *state = static_cast<ExecApprovalState *>(user_data);

	state->calls++;
	state->command = op->action ? op->action : "";
	state->reason = op->reason ? op->reason : "";
	for (int i = 0; i < op->programs_count; i++)
		state->programs.emplace_back(op->programs[i]);
	return TOOL_OP_ALLOW;
}

class ProcessTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		struct process_manager_config config = {};

		config.max_session_output = 64 * 1024;
		config.max_inline_output = 4096;
		config.kill_grace_ms = 100;
		config.shell = "/bin/bash";
		ASSERT_EQ(process_manager_create(&config, &manager), 0);
	}

	void TearDown() override
	{
		process_manager_destroy(manager);
		manager = nullptr;
	}

	struct process_manager *manager = nullptr;
};

TEST(ExecToolTest, RegistersExecAndProcessTools)
{
	struct tool_registry registry;

	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	EXPECT_NE(tool_lookup(&registry, "exec"), nullptr);
	EXPECT_NE(tool_lookup(&registry, "process"), nullptr);
	tool_registry_cleanup(&registry);
}

TEST(ExecToolTest, CompoundCommandRequestsOneClearApproval)
{
	char work_template[] = "/tmp/morph_exec_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"printf first && pwd\","
		"\"workdir\":\"/tmp\",\"yield_time_ms\":1000}",
		&result), 0);
	EXPECT_EQ(approval.calls, 1);
	EXPECT_EQ(approval.command, "printf first && pwd");
	EXPECT_FALSE(approval.reason.empty());
	ASSERT_EQ(approval.programs.size(), 2u);
	EXPECT_EQ(approval.programs[0], "printf");
	EXPECT_EQ(approval.programs[1], "pwd");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(workdir);
}

TEST(ExecToolTest, QuotedPythonOperatorsDoNotTriggerComplexSyntaxError)
{
	char work_template[] = "/tmp/morph_exec_python_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"python3 -c \\\"print(1 << 2, 'x >> y')\\\"\","
		"\"workdir\":\"/tmp\",\"yield_time_ms\":1000}",
		&result), 0);
	EXPECT_EQ(approval.calls, 1);
	EXPECT_EQ(approval.programs, std::vector<std::string>({"python3"}));
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(result.data, "stdout")), "4 x >> y\n");
	EXPECT_EQ(cJSON_GetNumberValue(
		cJSON_GetObjectItem(result.data, "exit_code")), 0);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(workdir);
}

TEST(ExecToolTest, ApprovalIncludesNestedAstCommands)
{
	char work_template[] = "/tmp/morph_exec_nested_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"printf '%s' \\\"$(echo nested)\\\"\","
		"\"workdir\":\"/tmp\",\"yield_time_ms\":1000}",
		&result), 0);
	EXPECT_EQ(approval.calls, 1);
	EXPECT_EQ(approval.programs,
		std::vector<std::string>({"printf", "echo"}));
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(result.data, "stdout")), "nested");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(workdir);
}

TEST(ExecToolTest, DynamicCommandIsRejectedBeforeApproval)
{
	char work_template[] = "/tmp/morph_exec_dynamic_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"$COMMAND argument\","
		"\"workdir\":\"/tmp\",\"yield_time_ms\":1000}",
		&result), -EPERM);
	EXPECT_EQ(approval.calls, 0);
	ASSERT_NE(result.envelope, nullptr);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(workdir);
}

TEST(ExecToolTest, ReadsFilesOutsideWorkspaceByDefault)
{
	char path[PATH_MAX];
	char args[PATH_MAX + 128];
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(realpath(__FILE__, path), nullptr);

	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_GT(snprintf(args, sizeof(args),
			   "{\"command\":\"cat %s\",\"workdir\":\"/tmp\","
			   "\"yield_time_ms\":1000}", path), 0);
	ASSERT_EQ(tool_exec(&registry, "exec", args, &result), 0);
	ASSERT_NE(result.data, nullptr);
	const char *output = cJSON_GetStringValue(
		cJSON_GetObjectItem(result.data, "stdout"));
	ASSERT_NE(output, nullptr);
	EXPECT_NE(strstr(output, "#include \"exec/process.h\""), nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(
		cJSON_GetObjectItem(result.data, "exit_code")), 0);

	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
}

TEST_F(ProcessTest, CapturesSeparateStreamsAndExitCode)
{
	struct process_spawn_options options = {};
	struct process_snapshot snapshot = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};

	options.command = "printf out; printf err >&2; exit 7";
	options.yield_time_ms = 1000;
	ASSERT_EQ(process_spawn(manager, &options, session_id, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(snapshot.exit_code, 7);
	EXPECT_STREQ(snapshot.stdout_text, "out");
	EXPECT_STREQ(snapshot.stderr_text, "err");
	process_snapshot_cleanup(&snapshot);
}

TEST_F(ProcessTest, ReportsInvalidWorkdir)
{
	struct process_spawn_options options = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};

	options.command = "true";
	options.workdir = "/path/that/does/not/exist";
	EXPECT_EQ(process_spawn(manager, &options, session_id, nullptr), -ENOENT);
}

TEST_F(ProcessTest, YieldReturnsRunningSessionAndPollIsIncremental)
{
	struct process_spawn_options options = {};
	struct process_snapshot snapshot = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};
	std::string output;

	options.command = "printf first; sleep 1; printf second";
	options.yield_time_ms = 50;
	ASSERT_EQ(process_spawn(manager, &options, session_id, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_RUNNING);
	output += snapshot.stdout_text;
	process_snapshot_cleanup(&snapshot);

	for (int i = 0; i < 30; i++) {
		ASSERT_EQ(process_session_snapshot(manager, session_id, &snapshot), 0);
		output += snapshot.stdout_text;
		if (snapshot.state >= PROCESS_EXITED)
			break;
		process_snapshot_cleanup(&snapshot);
		usleep(50000);
	}
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(output, "firstsecond");
	process_snapshot_cleanup(&snapshot);
}

TEST_F(ProcessTest, WritesToStdin)
{
	struct process_spawn_options options = {};
	struct process_snapshot snapshot = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};

	options.command = "read line; printf 'got:%s' \"$line\"";
	options.yield_time_ms = 50;
	ASSERT_EQ(process_spawn(manager, &options, session_id, &snapshot), 0);
	process_snapshot_cleanup(&snapshot);
	ASSERT_EQ(process_session_write(manager, session_id, "hello\n", 6), 0);
	for (int i = 0; i < 20; i++) {
		ASSERT_EQ(process_session_snapshot(manager, session_id, &snapshot), 0);
		if (snapshot.state >= PROCESS_EXITED)
			break;
		process_snapshot_cleanup(&snapshot);
		usleep(25000);
	}
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_STREQ(snapshot.stdout_text, "got:hello");
	process_snapshot_cleanup(&snapshot);
}

TEST_F(ProcessTest, TimeoutKillsProcessGroup)
{
	struct process_spawn_options options = {};
	struct process_snapshot snapshot = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};

	options.command = "sleep 10 & wait";
	options.timeout_ms = 50;
	options.yield_time_ms = 1000;
	ASSERT_EQ(process_spawn(manager, &options, session_id, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_TIMED_OUT);
	process_snapshot_cleanup(&snapshot);
}

}  /* namespace */
