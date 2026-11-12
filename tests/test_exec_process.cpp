#include "exec/process.h"
#include "config/config.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/exec_jobs.h"
#include "util/cleanup.h"
#include "agent/tools/request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/tool_context.h"
#include "permission/context.h"
#include "sandbox/sandbox.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/json.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <memory>
#include <unistd.h>
#include <fcntl.h>

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

class ProcessScratchDirectory {
public:
	ProcessScratchDirectory()
	{
		char name[] = "/tmp/morph_process_scratch_XXXXXX";
		if (mkdtemp(name)) {
			char *resolved MORPH_CLEANUP(morph_charp_cleanup) = realpath(name, nullptr);
			if (resolved) {
				path = resolved;
			}
		}
	}
	~ProcessScratchDirectory()
	{
		if (!path.empty())
			std::filesystem::remove_all(path);
	}
	std::string path;
};

TEST(ExecToolTest, RegistersExecAndBackgroundTools)
{
	struct tool_registry registry;

	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	EXPECT_NE(tool_borrow(&registry, "exec"), nullptr);
	EXPECT_NE(tool_borrow(&registry, "background"), nullptr);
	EXPECT_EQ(tool_borrow(&registry, "process"), nullptr);
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "process", R"({"action":"list"})", &result), -ENOENT);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::strstr(result.text.data, "deprecated"), nullptr);
	EXPECT_NE(std::strstr(result.text.data, "Use background"), nullptr);
	tool_registry_cleanup(&registry);
}

TEST(ExecToolTest, CommandIsTheOnlyCommandParameter)
{
	struct tool_registry registry MORPH_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	auto *entry = tool_borrow(&registry, "exec");
	ASSERT_NE(entry, nullptr);
	cJSON *schema MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(entry->desc.input_schema);
	ASSERT_NE(schema, nullptr);
	auto *properties = cJSON_GetObjectItem(schema, "properties");
	EXPECT_NE(cJSON_GetObjectItem(properties, "command"), nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(properties, "cmd"), nullptr);
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "exec", R"({"cmd":"printf must-not-run"})", &result),
		  -EINVAL);
	EXPECT_EQ(result.text.data ? std::strstr(result.text.data, "must-not-run") : nullptr,
		  nullptr);
}

TEST(ExecToolTest, HostExecutionRequiresUserConfigOptIn)
{
	struct tool_registry registry;
	struct tool_result result;

	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"printf must-not-run\",\"host\":true}", &result), -EACCES);
	ASSERT_NE(result.envelope, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		cJSON_GetObjectItem(result.envelope, "error"), "code")),
		"host_execution_disabled");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
}

TEST(ExecToolTest, ExplicitHostExecutionRetainsApprovalAndReportsMode)
{
	struct config config;
	config_set_defaults(NULL, &config);
	config.exec.allow_host_execution = 1;
	char work_template[] = "/tmp/morph_host_exec_XXXXXX";
	ASSERT_NE(mkdtemp(work_template), nullptr);
	auto *tctx = tool_context_create(NULL, work_template, work_template);
	ASSERT_NE(tctx, nullptr);
	ExecApprovalState approval;
	tool_context_set_operation_approval(tctx, approve_exec_operation, &approval);
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, &config.exec), 0);
	for (bool host : {false, true}) {
		cJSON *args = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "command", "printf host-probe");
		cJSON_AddStringToObject(args, "workdir", "/");
		cJSON_AddBoolToObject(args, "host", host);
		cJSON_AddNumberToObject(args, "yield_time_ms", 1000);
		char *json = cJSON_PrintUnformatted(args);
		struct tool_result result;
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&registry, "exec", json, &result), 0);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")),
			"host-probe");
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "execution_mode")),
			host ? "host" : "sandbox");
		tool_result_cleanup(&result);
		free(json);
		cJSON_Delete(args);
	}
	EXPECT_EQ(approval.calls, 2);
	EXPECT_NE(approval.reason.find("Host execution"), std::string::npos);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(work_template);
}

TEST(ExecToolTest, HostExecutionStillHonorsCommandDenial)
{
	struct config config;
	config_set_defaults(NULL, &config);
	config.exec.allow_host_execution = 1;
	auto *tctx = tool_context_create(NULL, "/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx,
		[](const struct tool_operation *, void *) { return TOOL_OP_DENY; }, nullptr);
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, &config.exec), 0);
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"printf must-not-run\",\"workdir\":\"/\",\"host\":true}",
		&result), -EACCES);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
}

TEST_F(ProcessTest, HostExecutionClosesInheritedDescriptors)
{
	int original = open("/dev/null", O_RDONLY);
	ASSERT_GE(original, 0);
	int inherited = fcntl(original, F_DUPFD, 200);
	close(original);
	ASSERT_GE(inherited, 200);
	std::string command = "test ! -e /dev/fd/" + std::to_string(inherited);
	struct process_spawn_options options = {};
	options.command = command.c_str();
	options.host_execution = 1;
	options.yield_time_ms = 1000;
	char session[PROCESS_SESSION_ID_MAX];
	struct process_snapshot snapshot = {};
	int rc = process_spawn(manager, &options, session, &snapshot);
	close(inherited);
	ASSERT_EQ(rc, 0);
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(snapshot.exit_code, 0);
	process_snapshot_cleanup(&snapshot);
}

#if defined(__APPLE__)
TEST(ExecToolTest, LiveHostAutomationReadsFrontmostApplication)
{
	if (!getenv("MORPH_TEST_HOST_AUTOMATION"))
		GTEST_SKIP() << "Requires an unlocked Mac and existing macOS privacy consent.";
	struct config config;
	config_set_defaults(NULL, &config);
	config.exec.allow_host_execution = 1;
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, &config.exec), 0);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"/usr/bin/osascript -e 'tell application \\\"System Events\\\" "
		"to get name of first application process whose frontmost is true'\","
		"\"host\":true,\"timeout_ms\":10000,\"yield_time_ms\":10000}", &result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0)
		<< cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stderr"));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "execution_mode")),
		"host");
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout"));
	ASSERT_NE(text, nullptr);
	EXPECT_GT(strlen(text), 1u);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
}
#endif

TEST(ExecToolTest, ReportsUnavailableApprovalWithoutClaimingItIsPending)
{
	char work_template[] = "/tmp/morph_exec_unavailable_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"printf test\",\"workdir\":\"/\","
		"\"yield_time_ms\":1000}",
		&result), -EPERM);
	ASSERT_NE(result.envelope, nullptr);
	cJSON *error = cJSON_GetObjectItem(result.envelope, "error");
	ASSERT_NE(error, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(error, "code")), "approval_unavailable");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(workdir);
}

TEST(ExecToolTest, CompoundCommandRequestsOneClearApproval)
{
	char work_template[] = "/tmp/morph_exec_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"printf first && pwd\","
		"\"workdir\":\"/\",\"yield_time_ms\":1000}",
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

TEST(ExecToolTest, BackgroundTimeoutIsOptIn)
{
	char work_template[] = "/tmp/morph_exec_background_XXXXXX";
	char *workdir = mkdtemp(work_template);
	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	ExecApprovalState approval;
	tool_context_set_operation_approval(tctx, approve_exec_operation, &approval);
	struct config_exec config = {};
	strcpy(config.shell, "/bin/bash");
	config.default_timeout_ms = 30;
	config.yield_time_ms = 500;
	config.max_inline_output = 4096;
	config.max_session_output = 4096;
	config.kill_grace_ms = 10;
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, &config), 0);
	const char *requests[] = {
		"{\"command\":\"sleep 0.2\",\"background\":true,\"yield_time_ms\":500}",
		"{\"command\":\"sleep 0.2\",\"background\":true,\"timeout_ms\":30,\"yield_time_ms\":500}",
		"{\"command\":\"sleep 0.2\",\"yield_time_ms\":500}"
	};
	const char *expected[] = {"exited", "timed_out", "timed_out"};
	for (size_t i = 0; i < 3; i++) {
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&registry, "exec", requests[i], &result), 0);
		std::string session = cJSON_GetStringValue(
			cJSON_GetObjectItem(result.data, "session_id"));
		std::string poll = "{\"action\":\"poll\",\"session_id\":\"" + session + "\"}";
		for (int attempt = 0; attempt < 100; attempt++) {
			const char *status = cJSON_GetStringValue(
				cJSON_GetObjectItem(result.data, "status"));
			if (status && strcmp(status, "running") != 0)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			tool_result_cleanup(&result);
			tool_result_init(&result);
			EXPECT_EQ(tool_exec(&registry, "background", poll.c_str(), &result), 0);
		}
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data,
			"status")), expected[i]);
		tool_result_cleanup(&result);
	}
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	std::filesystem::remove_all(workdir);
}

TEST(ExecToolTest, QuotedPythonOperatorsDoNotTriggerComplexSyntaxError)
{
	char work_template[] = "/tmp/morph_exec_python_policy_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"python3 -c \\\"print(1 << 2, 'x >> y')\\\"\","
		"\"workdir\":\"/\",\"yield_time_ms\":1000}",
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

TEST(ExecToolTest, ExecutesQuotedProgramPathWithSpaces)
{
	char work_template[] = "/tmp/morph_exec_quoted_program_XXXXXX";
	char *workdir = mkdtemp(work_template);
	char executable[PATH_MAX];
	char args[PATH_MAX + 160];
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(workdir, nullptr);
	ASSERT_GT(snprintf(executable, sizeof(executable),
			   "%s/quoted tool", workdir), 0);
	ASSERT_EQ(symlink("/bin/echo", executable), 0);
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_GT(snprintf(
		args, sizeof(args),
		"{\"command\":\"\\\"%s\\\" works\","
		"\"workdir\":\"/\",\"yield_time_ms\":1000}",
		executable), 0);
	ASSERT_EQ(tool_exec(&registry, "exec", args, &result), 0);
	EXPECT_EQ(approval.calls, 1);
	EXPECT_EQ(approval.programs,
		std::vector<std::string>({"quoted tool"}));
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(result.data, "stdout")), "works\n");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	unlink(executable);
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
	struct tool_context *tctx = tool_context_create(NULL, workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
		&registry, "exec",
		"{\"command\":\"printf '%s' \\\"$(echo nested)\\\"\","
		"\"workdir\":\"/\",\"yield_time_ms\":1000}",
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

TEST(ExecToolTest, DynamicScriptsExecuteInsideWorkspace)
{
	char work_template[] = "/tmp/morph_exec_dynamic_XXXXXX";
	ASSERT_NE(mkdtemp(work_template), nullptr);
	struct tool_registry registry;
	ExecApprovalState approval;
	auto *tctx = tool_context_create(NULL, work_template, work_template);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	const char *commands[] = {
		"COMMAND=printf; \"$COMMAND\" '%s' variable",
		"NAME=$(printf world); printf 'hello %s' \"$NAME\"",
		"for n in 1 2; do printf '%s' \"$n\"; done",
		"values=(alpha beta); printf '%s' \"${values[1]}\"",
		"x=42"
	};
	const char *expected[] = {"variable", "hello world", "12", "beta", ""};
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		cJSON *args = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "command", commands[i]);
		cJSON_AddStringToObject(args, "workdir", work_template);
		cJSON_AddNumberToObject(args, "yield_time_ms", 1000);
		char *json = cJSON_PrintUnformatted(args);
		struct tool_result result;
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&registry, "exec", json, &result), 0) << commands[i];
		ASSERT_NE(result.data, nullptr);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")),
			expected[i]);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
		tool_result_cleanup(&result);
		free(json);
		cJSON_Delete(args);
	}
	EXPECT_EQ(approval.calls, 0);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	rmdir(work_template);
}

TEST(ExecToolTest, ReadsOutsideWorkspaceRequireScopedAuthority)
{
	char path[PATH_MAX];
	char args[PATH_MAX + 128];
	struct tool_registry registry;
	struct tool_result result;
	ExecApprovalState approval;

	ASSERT_NE(realpath(__FILE__, path), nullptr);

	struct tool_context *tctx = tool_context_create(NULL, "/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(NULL, &registry);
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
	EXPECT_EQ(strstr(output, "#include"), nullptr);
	EXPECT_NE(cJSON_GetNumberValue(
		cJSON_GetObjectItem(result.data, "exit_code")), 0);
	char *slash = strrchr(path, '/');
	ASSERT_NE(slash, nullptr);
	*slash = '\0';
	struct tool_directory_access_request request = {};
	request.access = TOOL_PATH_ACCESS_READ;
	char principal[PATH_MAX];
	ASSERT_EQ(tool_context_command_principal("cat", principal, sizeof(principal)), 0);
	request.principal = principal;
	request.command = "Read the test source";
	request.path = path;
	request.session_scope = 1;
	ASSERT_EQ(tool_context_request_directory_access(tctx, &request, nullptr, 0), 0);
	ASSERT_EQ(tool_exec(&registry, "exec", args, &result), 0);
	output = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout"));
	ASSERT_NE(output, nullptr);
	EXPECT_NE(strstr(output, "#include"), nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);

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
	std::string received;
	for (int i = 0; i < 20; i++) {
		ASSERT_EQ(process_session_snapshot(manager, session_id, &snapshot), 0);
		received += snapshot.stdout_text ? snapshot.stdout_text : "";
		if (snapshot.state >= PROCESS_EXITED)
			break;
		process_snapshot_cleanup(&snapshot);
		usleep(25000);
	}
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(received, "got:hello");
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

TEST_F(ProcessTest, KillEscalatesWhenTheProcessIgnoresTerm)
{
	process_spawn_options options{};
	options.command = "trap '' TERM; printf ready; while :; do sleep .05; done";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot snapshot{};
	ASSERT_EQ(process_session_poll(manager, id, nullptr, 2000,
		nullptr, nullptr, &snapshot), 0);
	ASSERT_STREQ(snapshot.stdout_text, "ready");
	process_snapshot_cleanup(&snapshot);
	ASSERT_EQ(process_session_kill(manager, id), 0);
	ASSERT_EQ(process_session_wait(manager, id, nullptr, 2000,
		nullptr, nullptr, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_KILLED);
	EXPECT_EQ(snapshot.signal_number, SIGKILL);
	process_snapshot_cleanup(&snapshot);
}

TEST_F(ProcessTest, RepeatedStopAfterExitPreservesTheCollectedResult)
{
	process_spawn_options options{};
	options.command = "printf verified-exit; exit 7";
	options.yield_time_ms = 2000;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	ASSERT_EQ(snapshot.exit_code, 7);
	for (int attempt = 0; attempt < 3; attempt++) {
		EXPECT_EQ(process_session_interrupt(manager, id), 0);
		EXPECT_EQ(process_session_kill(manager, id), 0);
	}
	process_snapshot final MORPH_CLEANUP(process_snapshot_cleanup) = {};
	process_cursor start{};
	ASSERT_EQ(process_session_poll(manager, id, &start, 0, nullptr, nullptr, &final), 0);
	EXPECT_EQ(final.state, PROCESS_EXITED);
	EXPECT_EQ(final.exit_code, 7);
	EXPECT_STREQ(final.stdout_text, "verified-exit");
	EXPECT_EQ(final.duration_ms, snapshot.duration_ms);
	EXPECT_EQ(process_session_write(manager, id, "x", 1), -EPIPE);
	EXPECT_EQ(process_session_kill(manager, "missing-session"), -ENOENT);
}

TEST_F(ProcessTest, PrivateScratchEnvironmentRetainsWritesWithoutGlobalTemporaryAccess)
{
	ProcessScratchDirectory directory;
	ASSERT_FALSE(directory.path.empty());
	std::string scratch = directory.path + "/private";
	ASSERT_TRUE(std::filesystem::create_directory(scratch));
	std::string outside = "/tmp/morph-blocked-" +
		std::filesystem::path(directory.path).filename().string();
	std::string command =
		"test \"$TMPDIR\" = \"$MORPH_AGENT_SCRATCH\" || exit 20; "
		"printf permitted > \"$TMPDIR/allowed\" || exit 21; "
		"if (printf denied > ../workspace-write) 2>/dev/null; then exit 22; fi; "
		"if (printf denied > '" + outside + "') 2>/dev/null; then exit 23; fi; "
		"printf scratch-only";
	char *write_paths[] = {scratch.data()};
	struct sandbox_config sandbox = {};
	sandbox.path_policy_enabled = 1;
	sandbox.read_all = 1;
	sandbox.process_exec = 1;
	sandbox.allow_process_info = 1;
	sandbox.write_paths = write_paths;
	sandbox.write_paths_count = 1;
	sandbox.delete_paths = write_paths;
	sandbox.delete_paths_count = 1;
	process_spawn_options options{};
	options.command = command.c_str();
	options.workdir = scratch.c_str();
	options.temp_dir = scratch.c_str();
	options.sandbox = &sandbox;
	options.yield_time_ms = 3000;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(snapshot.exit_code, 0) << snapshot.stderr_text;
	EXPECT_STREQ(snapshot.stdout_text, "scratch-only");
	EXPECT_TRUE(std::filesystem::exists(scratch + "/allowed"));
	EXPECT_FALSE(std::filesystem::exists(directory.path + "/workspace-write"));
	EXPECT_FALSE(std::filesystem::exists(outside));
}

TEST_F(ProcessTest, TempdirHonoringShellUsesPrivateScratchForLargeHereDocuments)
{
	const char *shell = "/bin/bash";
#ifdef __APPLE__
	shell = access("/opt/homebrew/bin/bash", X_OK) == 0 ? "/opt/homebrew/bin/bash"
		: access("/usr/local/bin/bash", X_OK) == 0 ? "/usr/local/bin/bash" : nullptr;
	if (!shell)
		GTEST_SKIP() << "macOS system shells ignore TMPDIR for here-document files";
#endif
	process_manager_destroy(manager);
	manager = nullptr;
	process_manager_config config{};
	config.shell = shell;
	ASSERT_EQ(process_manager_create(&config, &manager), 0);
	ProcessScratchDirectory directory;
	ASSERT_FALSE(directory.path.empty());
	std::string body(32768, 'x');
	std::string command = "cat <<'MORPH_PRIVATE_EOF' > \"$TMPDIR/document\"\n" + body +
		"\nMORPH_PRIVATE_EOF\n";
	char *paths[] = {directory.path.data()};
	struct sandbox_config sandbox = {};
	sandbox.path_policy_enabled = 1;
	sandbox.read_all = 1;
	sandbox.process_exec = 1;
	sandbox.allow_process_info = 1;
	sandbox.write_paths = paths;
	sandbox.write_paths_count = 1;
	sandbox.delete_paths = paths;
	sandbox.delete_paths_count = 1;
	process_spawn_options options{};
	options.command = command.c_str();
	options.workdir = directory.path.c_str();
	options.temp_dir = directory.path.c_str();
	options.sandbox = &sandbox;
	options.yield_time_ms = 3000;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(snapshot.exit_code, 0) << snapshot.stderr_text;
	ASSERT_TRUE(std::filesystem::exists(directory.path + "/document"));
	EXPECT_EQ(std::filesystem::file_size(directory.path + "/document"), body.size() + 1);
}

#ifdef __APPLE__
TEST(ExecToolTest, LegacyShellHereDocumentFailureExplainsThePrivateScratchAlternative)
{
	ProcessScratchDirectory directory;
	ASSERT_FALSE(directory.path.empty());
	struct config config;
	config_set_defaults(nullptr, &config);
	strcpy(config.exec.shell, "/bin/bash");
	struct tool_context *context = tool_context_create(nullptr,
		directory.path.c_str(), directory.path.c_str());
	ASSERT_NE(context, nullptr);
	std::unique_ptr<struct tool_context, decltype(&tool_context_destroy)>
		context_guard(context, tool_context_destroy);
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	struct tool_registry registry MORPH_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, context, &config.exec), 0);
	std::string command = "cat <<'MORPH_EOF'\n" + std::string(32768, 'x') + "\nMORPH_EOF\n";
	cJSON *arguments MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	ASSERT_NE(arguments, nullptr);
	cJSON_AddStringToObject(arguments, "command", command.c_str());
	cJSON_AddNumberToObject(arguments, "yield_time_ms", 3000);
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = cJSON_PrintUnformatted(arguments);
	ASSERT_NE(json, nullptr);
	struct tool_runtime_context runtime{};
	runtime.agent_id = "/root/probe";
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &runtime;
	ASSERT_EQ(tool_exec(&registry, "exec", json, &result), 0) << result.text.data;
	EXPECT_NE(cJSON_GetNumberValue(
		cJSON_GetObjectItemCaseSensitive(result.data, "exit_code")), 0);
	const char *error = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(result.data, "stderr"));
	ASSERT_NE(error, nullptr);
	EXPECT_NE(strstr(error, "temp file for here document"), nullptr);
	const char *action = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(result.data, "next_action"));
	ASSERT_NE(action, nullptr);
	EXPECT_NE(strstr(action, "$MORPH_AGENT_SCRATCH"), nullptr);
	EXPECT_NE(strstr(action, "printf"), nullptr);
}
#endif

TEST_F(ProcessTest, CancellationStopsTheInitialYieldPromptly)
{
	std::atomic<bool> cancelled{false};
	process_spawn_options options{};
	options.command = "sleep 10";
	options.yield_time_ms = 10000;
	options.cancelled = [](void *opaque) {
		return static_cast<std::atomic<bool> *>(opaque)->load() ? 1 : 0;
	};
	options.cancel_data = &cancelled;
	char id[PROCESS_SESSION_ID_MAX];
	std::thread canceller([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		cancelled = true;
	});
	auto start = std::chrono::steady_clock::now();
	int rc = process_spawn(manager, &options, id, nullptr);
	auto elapsed = std::chrono::steady_clock::now() - start;
	canceller.join();
	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_LT(elapsed, std::chrono::milliseconds(500));
	ASSERT_NE(id[0], '\0');
	EXPECT_EQ(process_session_kill(manager, id), 0);
}

/*
 * Regression test for a use-after-free in process_spawn().
 *
 * session_reserve() grows manager->sessions with realloc(), which frees the
 * old block and may move the array. process_spawn() caches
 * `session = &manager->sessions[i]` and then parks in
 * pthread_cond_timedwait(), which releases manager->mutex. A concurrent
 * process_spawn() that takes the lock can realloc the array underneath the
 * parked caller; when that caller wakes it re-reads `session->state` in the
 * `while (session->state < PROCESS_EXITED)` guard through the stale pointer.
 *
 * That is a genuine use-after-free: the manifest crash was
 * `SIGSEGV / SEGV_MAPERR` at `process_spawn+0x166306`, precisely this guard
 * (`cmpl $0x1,0x2064(%rbx)`), with two threads stopped at that same
 * instruction after pthread_cond_timedwait.
 *
 * Reproducing it needs three things at once:
 *
 *   1. The array at capacity, so the next spawn calls realloc().
 *   2. A caller parked in the wait loop at that moment.
 *   3. The freed block actually unmapped, so the stale read faults instead of
 *      silently returning stale-but-mapped memory. realloc() often grows in
 *      place, and a freed small block usually stays in the heap and remains
 *      readable, which is why the bug is intermittent in production.
 *
 * Point 3 is the reason this test is usually run under
 * MALLOC_PERTURB_ (and often MALLOC_MMAP_THRESHOLD_): glibc then writes a
 * poison pattern over freed memory and more readily returns pages to the
 * kernel. Without it the realloc frequently does not fault and the test can
 * pass on the unfixed tree. The test detects that situation itself and skips
 * rather than reporting a false pass -- see the malloc tuning check below.
 *
 * On a fixed tree the parked caller re-resolves its session (or the array is
 * stable), the guard reads valid memory, and every spawn returns 0.
 */
TEST_F(ProcessTest, ConcurrentSpawnSurvivesSessionArrayGrowth)
{
	/*
	 * The array starts empty and is first sized to 16, so spawning 16
	 * sessions puts session_count at capacity and makes the next spawn the
	 * one that calls realloc().
	 */
	constexpr int kCapacity = 16;
	constexpr int kGrowWhileParked = 64;

	struct process_spawn_options options = {};
	struct process_snapshot snapshot = {};
	char session_id[PROCESS_SESSION_ID_MAX] = {};
	std::atomic<bool> victim_started{false};
	std::atomic<int> victim_rc{-1};
	std::thread victim;
	int rc;

	for (int i = 0; i < kCapacity; i++) {
		options.command = "true";
		options.yield_time_ms = 2000;
		ASSERT_EQ(process_spawn(manager, &options, session_id, nullptr), 0);
	}

	/*
	 * Victim: a command that outlives its yield window by a wide margin,
	 * so it is guaranteed to sit in pthread_cond_timedwait() with the
	 * mutex released while the array is grown underneath it.
	 */
	victim = std::thread([&]() {
		struct process_spawn_options victim_options = {};
		char victim_id[PROCESS_SESSION_ID_MAX] = {};

		victim_options.command = "sleep 2.0";
		victim_options.yield_time_ms = 1500;
		victim_started.store(true, std::memory_order_release);
		victim_rc.store(process_spawn(manager, &victim_options,
					      victim_id, nullptr),
				std::memory_order_release);
	});

	while (!victim_started.load(std::memory_order_acquire))
		std::this_thread::yield();
	/*
	 * Let the victim reach the wait. fork/exec plus taking the manager
	 * lock is well under this; the victim then stays parked for ~1.5s.
	 */
	usleep(250000);

	/*
	 * Grow the array repeatedly while the victim is parked. One realloc may
	 * grow in place and leave the stale pointer readable, so keep going;
	 * the stale pointer is dereferenced on every wakeup, and each further
	 * realloc is another chance to free the block actually in use.
	 */
	for (int i = 0; i < kGrowWhileParked; i++) {
		options.command = "true";
		options.yield_time_ms = 0;
		rc = process_spawn(manager, &options, session_id, nullptr);
		ASSERT_EQ(rc, 0) << "grower spawn failed at iteration " << i;
	}

	victim.join();

	/*
	 * Reaching this point is the real assertion. On the unfixed tree the
	 * victim dereferences freed memory inside process_spawn() and the whole
	 * test binary dies with SIGSEGV before these expectations run, which is
	 * exactly the manifest failure. These checks catch the non-fatal
	 * variants (moved-but-mapped memory yielding a wrong state or a
	 * corrupted session) if the process happens to survive.
	 */
	EXPECT_EQ(victim_rc.load(std::memory_order_acquire), 0);

	/*
	 * Guard against a silent false pass: if glibc kept the freed block
	 * mapped, the stale read succeeds and this test cannot fail. Say so
	 * instead of implying the code is correct.
	 */
	if (!getenv("MALLOC_PERTURB_"))
		GTEST_SKIP() << "run with MALLOC_PERTURB_=165 to make the freed "
				"session array fault reliably; without it a "
				"passing run does not prove the fix is correct";
}

/*
 * The same defect seen from the outside, without hand-placed timing.
 *
 * Many threads spawn concurrently with mixed yield windows: the zero-yield
 * calls run straight through and grow the array, while the longer-yield calls
 * park in the wait loop and re-read their cached session pointer on every
 * wakeup. This is the interleaving the production crash showed -- two threads
 * stopped at the same instruction after pthread_cond_timedwait.
 *
 * Timing here is deliberately unforced; the point is coverage across many
 * schedules rather than one precise interleaving, so this test is a companion
 * to ConcurrentSpawnSurvivesSessionArrayGrowth rather than a replacement. It
 * is most informative under MALLOC_PERTURB_, but is worth running always.
 */
TEST_F(ProcessTest, ManyConcurrentSpawnsAcrossSessionArrayGrowth)
{
	constexpr int kThreads = 12;
	constexpr int kSpawnsPerThread = 4;

	std::atomic<int> failures{0};
	std::vector<std::thread> threads;

	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([&, t]() {
			for (int i = 0; i < kSpawnsPerThread; i++) {
				struct process_spawn_options options = {};
				char session_id[PROCESS_SESSION_ID_MAX] = {};
				int rc;

				/*
				 * Slow callers park and re-read their session;
				 * fast callers complete without parking and grow
				 * the array. The mix is what makes them overlap.
				 */
				if ((t + i) % 2 == 0) {
					options.command = "sleep 0.2";
					options.yield_time_ms = 50;
				} else {
					options.command = "true";
					options.yield_time_ms = 0;
				}
				rc = process_spawn(manager, &options, session_id,
						   nullptr);
				if (rc != 0 || session_id[0] == '\0')
					failures.fetch_add(1,
							   std::memory_order_relaxed);
			}
		});
	}
	for (auto &thread : threads)
		thread.join();

	/* Reaching here proves no spawn died on a stale session pointer. */
	EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
}

}  /* namespace */

TEST(ExecToolTest, DynamicCommandsStillUseApprovalOutsideTrustedScope)
{
	struct tool_registry registry;
	ExecApprovalState approval;
	auto *tctx =
		tool_context_create(NULL, "/tmp/morph-dynamic-work", "/tmp/morph-dynamic-work");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx,
		[](const struct tool_operation *op, void *data) {
			(void)approve_exec_operation(op, data);
			return TOOL_OP_DENY;
		}, &approval);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"CMD=printf; $CMD dynamic\",\"workdir\":\"/\"}", &result), -EACCES);
	EXPECT_EQ(approval.calls, 1);
	EXPECT_NE(std::find(approval.programs.begin(), approval.programs.end(), "shell"),
		approval.programs.end());
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
}

TEST(ExecToolTest, SandboxReceivesProfileAfterMoreThan32SessionGrants)
{
	const char *home = getenv("HOME");
	ASSERT_NE(home, nullptr);
	char temp[PATH_MAX];
	ASSERT_LT(snprintf(temp, sizeof(temp), "%s/.morph_grant_exec_XXXXXX", home),
		(int)sizeof(temp));
	ASSERT_NE(mkdtemp(temp), nullptr);
	std::string base(temp);
	std::string work = base + "/work";
	std::string profile = base + "/profile";
	ASSERT_EQ(file_ensure_dir(work.c_str()), 0);
	ASSERT_EQ(file_ensure_dir(profile.c_str()), 0);
	auto *tctx = tool_context_create(NULL, work.c_str(), work.c_str());
	ASSERT_NE(tctx, nullptr);
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	char principal[TOOL_CONTEXT_CLI_NAME_MAX];
	ASSERT_EQ(tool_context_command_principal("printf okay", principal, sizeof(principal)), 0);
	for (int i = 0; i < 40; i++) {
		std::string dir = base + "/grant-" + std::to_string(i);
		ASSERT_EQ(tool_context_grant_write_access(tctx, principal, dir.c_str(), 1,
			TOOL_OP_SESSION, NULL, 0), 0);
	}
	ASSERT_EQ(tool_context_add_exec_profile_path(tctx, TOOL_PATH_WRITE, profile.c_str()), 0);
	std::string command = "printf okay > '" + profile + "/result'";
	cJSON *args = cJSON_CreateObject();
	cJSON_AddStringToObject(args, "command", command.c_str());
	cJSON_AddStringToObject(args, "workdir", work.c_str());
	cJSON_AddNumberToObject(args, "yield_time_ms", 1000);
	char *json = cJSON_PrintUnformatted(args);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "exec", json, &result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	EXPECT_TRUE(file_exists((profile + "/result").c_str()));
	tool_result_cleanup(&result);
	free(json);
	cJSON_Delete(args);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	std::filesystem::remove_all(temp);
}

TEST(ExecToolTest, SessionApprovalAllowsRepeatedExecution)
{
	struct tool_registry registry;
	auto *tctx = tool_context_create(NULL, "/different-work", "/different-output");
	ASSERT_NE(tctx, nullptr);
	int approvals = 0;
	tool_context_set_operation_approval(tctx,
		[](const struct tool_operation *, void *data) {
			++*static_cast<int *>(data);
			return TOOL_OP_SESSION;
		}, &approvals);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	for (int attempt = 0; attempt < 2; attempt++) {
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&registry, "exec",
			"{\"command\":\"printf approved\",\"workdir\":\"/\"}", &result), 0);
		ASSERT_NE(result.envelope, nullptr);
		EXPECT_EQ(cJSON_GetObjectItem(result.envelope, "error"), nullptr);
		tool_result_cleanup(&result);
	}
	EXPECT_EQ(approvals, 1);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
}

TEST(ExecToolTest, DirectoryPermissionsFollowPathsAcrossToolsAndPrograms)
{
	const char *home = getenv("HOME");
	ASSERT_NE(home, nullptr);
	std::string pattern = std::string(home) + "/.morph_directory_grants_XXXXXX";
	std::vector<char> temp(pattern.begin(), pattern.end());
	temp.push_back('\0');
	ASSERT_NE(mkdtemp(temp.data()), nullptr);
	std::string base(temp.data());
	std::string work = base + "/work";
	std::string target = base + "/target";
	std::string sibling = base + "/target-other";
	std::filesystem::create_directories(work);
	std::filesystem::create_directories(target);
	std::filesystem::create_directories(sibling);
	auto *tctx = tool_context_create(NULL, work.c_str(), work.c_str());
	ASSERT_NE(tctx, nullptr);
	int approvals = 0;
	tool_context_set_operation_approval(tctx,
		[](const struct tool_operation *op, void *data) {
			if (op->tool_name && strcmp(op->tool_name, "request_permissions") == 0) {
				++*static_cast<int *>(data);
				EXPECT_STREQ(op->principal, "*");
				return TOOL_OP_ALLOW;
			}
			return op->kind == TOOL_OP_COMMAND ? TOOL_OP_ALLOW : TOOL_OP_DENY;
		}, &approvals);
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(exec_tool_init(&registry, tctx, nullptr), 0);
	ASSERT_EQ(request_permissions_init(&registry, tctx), 0);
	auto request = [&](const char *capability, const char *scope) {
		std::string json = "{\"scope\":\"" + std::string(scope) +
			"\",\"permissions\":{\"file_system\":{\"" + capability +
			"\":[\"" + target + "\"]}}}";
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&registry, "request_permissions", json.c_str(), &result), 0);
		ASSERT_NE(result.envelope, nullptr);
		EXPECT_EQ(cJSON_GetObjectItem(result.envelope, "error"), nullptr);
		tool_result_cleanup(&result);
	};
	auto execute = [&](const std::string &command) {
		cJSON *args = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "command", command.c_str());
		cJSON_AddStringToObject(args, "workdir", work.c_str());
		cJSON_AddNumberToObject(args, "yield_time_ms", 1000);
		char *json = cJSON_PrintUnformatted(args);
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&registry, "exec", json, &result), 0);
		int status = result.data ?
			(int)cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")) : -1;
		tool_result_cleanup(&result);
		free(json);
		cJSON_Delete(args);
		return status;
	};
	request("write", "session");
	EXPECT_EQ(approvals, 1);
	EXPECT_EQ(execute("cd '" + work + "' && sh -c \"printf first > '" +
		target + "/first'\""), 0);
	EXPECT_NE(execute("cat '" + target + "/first'"), 0);
	request("read", "turn");
	EXPECT_EQ(execute("cp '" + target + "/first' '" + target + "/second'"), 0);
	EXPECT_EQ(execute("cat '" + target + "/first' > '" + target + "/third'"), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
		(target + "/native").c_str(), nullptr, 0), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_DELETE,
		(target + "/first").c_str(), nullptr, 0), -EACCES);
	EXPECT_NE(execute("rm '" + target + "/first'"), 0);
	EXPECT_NE(execute("printf denied > '" + sibling + "/denied'"), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
		(sibling + "/denied").c_str(), nullptr, 0), -EACCES);
	std::filesystem::create_directory_symlink(sibling, target + "/escape");
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
		(target + "/escape/denied").c_str(), nullptr, 0), -EACCES);
	EXPECT_NE(execute("printf denied > '" + target + "/escape/denied'"), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_READ,
		(target + "/first").c_str(), nullptr, 0), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_LIST,
		target.c_str(), nullptr, 0), 0);
	request("delete", "turn");
	EXPECT_EQ(approvals, 3);
	EXPECT_EQ(execute("rm '" + target + "/second'"), 0);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_DELETE,
		(target + "/first").c_str(), nullptr, 0), 0);
	tool_context_clear_turn_grants(tctx);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_DELETE,
		(target + "/first").c_str(), nullptr, 0), -EACCES);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_READ,
		(target + "/first").c_str(), nullptr, 0), -EACCES);
	EXPECT_EQ(execute("printf session > '" + target + "/session'"), 0);
	EXPECT_NE(execute("rm '" + target + "/first'"), 0);
	request("read", "turn");
	request("read", "session");
	EXPECT_EQ(approvals, 5);
	tool_context_clear_turn_grants(tctx);
	EXPECT_EQ(tool_context_authorize_path(tctx, TOOL_PATH_READ,
		(target + "/first").c_str(), nullptr, 0), 0);
	tool_registry_cleanup(&registry);
	tool_context_destroy(tctx);
	std::filesystem::remove_all(base);
}

TEST_F(ProcessTest, ExplicitCursorWaitsAndReplaysWithoutConsumingOutput)
{
	process_spawn_options options{};
	options.command = "sleep 0.1; printf hello; sleep 0.2";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot first{};
	ASSERT_EQ(process_spawn(manager, &options, id, &first), 0);
	process_cursor cursor = first.next_cursor;
	process_snapshot_cleanup(&first);
	process_snapshot next{};
	ASSERT_EQ(process_session_poll(manager, id, &cursor, 2000, nullptr, nullptr, &next), 0);
	EXPECT_STREQ(next.stdout_text, "hello");
	EXPECT_FALSE(std::string(next.stdout_path).empty());
	EXPECT_GT(next.next_cursor.stdout_offset, cursor.stdout_offset);
	process_snapshot replay{};
	ASSERT_EQ(process_session_poll(manager, id, &cursor, 0, nullptr, nullptr, &replay), 0);
	EXPECT_STREQ(replay.stdout_text, next.stdout_text);
	process_snapshot_cleanup(&next);
	process_snapshot_cleanup(&replay);
}

TEST_F(ProcessTest, CancellingWaitLeavesChildRunning)
{
	process_spawn_options options{};
	options.command = "sleep 2";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot{};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	process_cursor cursor = snapshot.next_cursor;
	process_snapshot_cleanup(&snapshot);
	EXPECT_EQ(process_session_poll(
		      manager, id, &cursor, 2000, [](void *) { return 1; }, nullptr, &snapshot),
		  -ECANCELED);
	ASSERT_EQ(process_session_snapshot(manager, id, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_RUNNING);
	process_snapshot_cleanup(&snapshot);
}

TEST_F(ProcessTest, ExitWaitCollectsIntermediateOutputAndFreezesDuration)
{
	process_spawn_options options{};
	options.command = "printf first; sleep .15; printf last; exit 7";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot{};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	process_snapshot_cleanup(&snapshot);
	process_cursor cursor{};
	ASSERT_EQ(process_session_wait(manager, id, &cursor, 2000, nullptr, nullptr, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_EQ(snapshot.exit_code, 7);
	EXPECT_STREQ(snapshot.stdout_text, "firstlast");
	uint64_t duration = snapshot.duration_ms;
	process_snapshot_cleanup(&snapshot);
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	ASSERT_EQ(process_session_status(manager, id, &snapshot), 0);
	EXPECT_EQ(snapshot.duration_ms, duration);
	EXPECT_EQ(snapshot.stdout_text, nullptr);
}

TEST_F(ProcessTest, ExitWaitCancellationDoesNotTerminateTheChild)
{
	process_spawn_options options{};
	options.command = "sleep 2";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot{};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	process_snapshot_cleanup(&snapshot);
	EXPECT_EQ(process_session_wait(manager, id, nullptr, 2000,
		[](void *) { return 1; }, nullptr, &snapshot), -ECANCELED);
	ASSERT_EQ(process_session_status(manager, id, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_RUNNING);
}

TEST(ProcessCompletionTest, CallbackCanReadStateAndFiresOnce)
{
	struct State {
		process_manager *manager = nullptr;
		std::mutex mutex;
		std::condition_variable condition;
		int calls = 0;
		int rc = -1;
		process_state status = PROCESS_STARTING;
	} state;
	process_manager_config config{};
	config.completed_user_data = &state;
	config.completed = [](const char *id, void *opaque) {
		auto *s = static_cast<State *>(opaque);
		process_snapshot snapshot{};
		int rc = process_session_status(s->manager, id, &snapshot);
		std::lock_guard<std::mutex> lock(s->mutex);
		s->rc = rc;
		s->status = snapshot.state;
		s->calls++;
		s->condition.notify_all();
	};
	ASSERT_EQ(process_manager_create(&config, &state.manager), 0);
	process_spawn_options options{};
	options.command = "printf completed";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(state.manager, &options, id, nullptr), 0);
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		EXPECT_TRUE(state.condition.wait_for(lock, std::chrono::seconds(3),
			[&] { return state.calls > 0; }));
		EXPECT_EQ(state.rc, 0);
		EXPECT_EQ(state.status, PROCESS_EXITED);
	}
	process_manager_destroy(state.manager);
	EXPECT_EQ(state.calls, 1);
}

TEST(ExecToolTest, AsyncJobsNotifyOnceKeepOwnershipAndDoNotConsumeToolOutput)
{
	struct Events {
		std::mutex mutex;
		std::condition_variable ready;
		std::vector<std::string> names;
		std::string output;
	} events;
	tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	ASSERT_EQ(exec_tool_set_event_callback(&registry,
		[](const morph_event *event, void *opaque) -> int {
			auto *s = static_cast<Events *>(opaque);
			std::lock_guard<std::mutex> lock(s->mutex);
			s->names.emplace_back(event->name);
			const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(event->data, "stdout"));
			if (text) s->output = text;
			s->ready.notify_all();
			return 0;
		}, &events), 0);
	tool_runtime_context context{};
	context.memory_session_id = 42;
	context.turn_id = "job-turn";
	tool_result result{};
	tool_result_init(&result);
	result.runtime = &context;
	ASSERT_EQ(tool_exec(&registry, "exec",
		R"({"command":"sleep .15; printf complete","description":"编译项目","background":true})",
		&result), 0);
	std::string id = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
	tool_result_cleanup(&result);
	{
		std::unique_lock<std::mutex> lock(events.mutex);
		ASSERT_TRUE(events.ready.wait_for(lock, std::chrono::seconds(3), [&] {
			return events.names.size() == 2;
		}));
		EXPECT_EQ(events.names[0], "process.started");
		EXPECT_EQ(events.names[1], "process.completed");
		EXPECT_EQ(events.output, "complete");
	}
	cJSON *jobs = nullptr;
	ASSERT_EQ(exec_tool_jobs_list(&registry, 43, &jobs), 0);
	EXPECT_EQ(cJSON_GetArraySize(jobs), 0);
	cJSON_Delete(jobs);
	cJSON *detail = nullptr;
	ASSERT_EQ(exec_tool_job_read(&registry, 42, id.c_str(), &detail), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(detail, "description")), "编译项目");
	cJSON_Delete(detail);
	tool_result_init(&result);
	result.runtime = &context;
	std::string args = "{\"action\":\"poll\",\"session_id\":\"" + id + "\",\"wait_ms\":0}";
	ASSERT_EQ(tool_exec(&registry, "background", args.c_str(), &result), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")), "complete");
	tool_result_cleanup(&result);
	context.memory_session_id = 43;
	tool_result_init(&result);
	result.runtime = &context;
	EXPECT_EQ(tool_exec(&registry, "background", args.c_str(), &result), -ENOENT);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	EXPECT_EQ(events.names.size(), 2u);
}

TEST(ExecToolTest, ExplicitNotifyOnlyAndServicesDoNotHoldFinalization)
{
	tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	tool_runtime_context context{};
	context.memory_session_id = 42;
	context.turn_id = "notify-test";
	for (const char *args : {
		R"({"command":"sleep 2","background":true,"notify_only":true})",
		R"({"command":"sleep 2","service":true})",
		R"({"command":"sleep .1; printf verified","background":true})"}) {
		tool_result result{};
		tool_result_init(&result);
		result.runtime = &context;
		ASSERT_EQ(tool_exec(&registry, "exec", args, &result), 0);
		tool_result_cleanup(&result);
	}
	tool_entry *entry = tool_borrow(&registry, "exec");
	ASSERT_NE(entry->background_wait, nullptr);
	cJSON *completed = nullptr;
	ASSERT_EQ(entry->background_wait(entry->user_data, 42, "notify-test", nullptr,
		nullptr, &completed), 0);
	ASSERT_EQ(cJSON_GetArraySize(completed), 1);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		cJSON_GetArrayItem(completed, 0), "stdout")), "verified");
	cJSON_Delete(completed);
	cJSON *jobs = nullptr;
	ASSERT_EQ(exec_tool_jobs_list(&registry, 42, &jobs), 0);
	int running = 0;
	cJSON *job;
	cJSON_ArrayForEach(job, jobs) {
		const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(job, "status"));
		if (state && std::strcmp(state, "running") == 0) running++;
	}
	EXPECT_EQ(running, 2);
	cJSON_Delete(jobs);
	tool_registry_cleanup(&registry);
}

TEST(ExecToolTest, TurnCancellationStopsOnlyOwnedJobsFromThatTurn)
{
	tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	tool_runtime_context context{};
	std::vector<std::string> ids;
	for (int index = 0; index < 5; index++) {
		context.memory_session_id = index == 4 ? 43 : 42;
		context.turn_id = index == 3 ? "previous-turn" : "cancel-turn";
		tool_result result{};
		tool_result_init(&result);
		result.runtime = &context;
		const char *args = index == 1 ?
			R"({"command":"sleep 10","service":true})" : index == 2 ?
			R"({"command":"sleep 10","background":true,"notify_only":true})" :
			R"({"command":"sleep 10","background":true})";
		ASSERT_EQ(tool_exec(&registry, "exec", args, &result), 0);
		ids.emplace_back(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id")));
		tool_result_cleanup(&result);
	}
	tool_entry *entry = tool_borrow(&registry, "exec");
	ASSERT_NE(entry->background_cancel, nullptr);
	entry->background_cancel(entry->user_data, 42, "cancel-turn");
	for (size_t index = 0; index < ids.size(); index++) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		std::string status;
		do {
			cJSON *detail = nullptr;
			ASSERT_EQ(exec_tool_job_read(&registry, index == 4 ? 43 : 42,
				ids[index].c_str(), &detail), 0);
			status = cJSON_GetStringValue(cJSON_GetObjectItem(detail, "status"));
			cJSON_Delete(detail);
			if (index >= 3 || status == "killed") break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		} while (std::chrono::steady_clock::now() < deadline);
		EXPECT_EQ(status, index < 3 ? "killed" : "running");
	}
	tool_registry_cleanup(&registry);
}

TEST_F(ProcessTest, DetailInspectionReadsRetainedWindowWithoutAdvancingCursor)
{
	process_spawn_options options{};
	options.command = "i=0; while [ $i -lt 5000 ]; do printf abcdefghij; i=$((i+1)); done";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot snapshot{};
	process_cursor cursor{};
	ASSERT_EQ(process_session_wait(manager, id, &cursor, 3000, nullptr, nullptr, &snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_TRUE(snapshot.stdout_truncated);
	process_snapshot_cleanup(&snapshot);
	ASSERT_EQ(process_session_inspect(manager, id, 1, &snapshot), 0);
	EXPECT_EQ(std::strlen(snapshot.stdout_text), 50000u);
	EXPECT_FALSE(snapshot.stdout_truncated);
	process_snapshot_cleanup(&snapshot);
	ASSERT_EQ(process_session_poll(manager, id, nullptr, 0, nullptr, nullptr, &snapshot), 0);
	EXPECT_GT(std::strlen(snapshot.stdout_text), 0u);
	process_snapshot_cleanup(&snapshot);
}

TEST(ExecToolTest, ProcessAcceptsModelSelectedWaitsWithinDefaultRange)
{
	tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	tool_result result{};
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "exec", R"({"command":"printf verified"})", &result), 0);
	const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
	ASSERT_NE(session, nullptr);
	std::string id = session;
	tool_result_cleanup(&result);
	for (const char *action : {"wait", "poll"}) {
		for (const char *budget : {"0", "60001", "120000", "600000"}) {
			std::string args = "{\"action\":\"" + std::string(action) +
				"\",\"session_id\":\"" + id + "\",\"cursor\":\"0:0\",\"wait_ms\":" +
				budget + "}";
			tool_result_init(&result);
			ASSERT_EQ(tool_exec(&registry, "background", args.c_str(), &result), 0);
			EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "status")),
				"exited");
			EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")),
				"verified");
			EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "wait_ms")),
				std::strtod(budget, nullptr));
			EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data,
				"max_wait_ms")), 600000);
			tool_result_cleanup(&result);
		}
	}
	struct budget_case { const char *suffix; int expected; };
	const budget_case cases[] = {{"", 30000}, {",\"wait_ms\":0", 0},
		{",\"wait_ms\":600000", 600000}};
	for (const auto &item : cases) {
		std::string args = "{\"action\":\"wait\",\"session_id\":\"" + id + "\"" +
			item.suffix + "}";
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&registry, "background", args.c_str(), &result), 0);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "wait_ms")),
			item.expected);
		tool_result_cleanup(&result);
	}
	tool_registry_cleanup(&registry);
}

TEST(ExecToolTest, ProcessRejectsInvalidWaitValuesBeforeWritingInput)
{
	tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	tool_result result{};
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "exec",
		R"({"command":"IFS= read -r line; printf 'read:%s' \"$line\"","background":true})",
		&result), 0);
	const char *session = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
	ASSERT_NE(session, nullptr);
	std::string id = session;
	tool_result_cleanup(&result);
	for (const char *budget : {"-1", "1.5", "120000.5", "\"120000\"", "null", "true", "600001", "1e100", "1e999"}) {
		std::string args = "{\"action\":\"write\",\"session_id\":\"" + id +
			"\",\"input\":\"bad\\n\",\"wait_ms\":" + budget + "}";
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&registry, "background", args.c_str(), &result), -EINVAL);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
			cJSON_GetObjectItem(result.envelope, "error"), "code")), "invalid_request");
		tool_result_cleanup(&result);
	}
	tool_result_init(&result);
	std::string write = "{\"action\":\"write\",\"session_id\":\"" + id +
		"\",\"input\":\"good\\n\"}";
	ASSERT_EQ(tool_exec(&registry, "background", write.c_str(), &result), 0);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "wait_ms")), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	std::string wait = "{\"action\":\"wait\",\"session_id\":\"" + id +
		"\",\"wait_ms\":2000,\"cursor\":\"0:0\"}";
	ASSERT_EQ(tool_exec(&registry, "background", wait.c_str(), &result), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "stdout")), "read:good");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
}

TEST_F(ProcessTest, RawWaitSupportsLongBudgetsAndRejectsDeadlineOverflow)
{
	process_snapshot snapshot{};
	EXPECT_EQ(process_session_wait(manager, "not-needed", nullptr,
		UINT64_MAX, nullptr, nullptr, &snapshot), -EOVERFLOW);
	process_spawn_options options{};
	options.command = "printf done";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	ASSERT_EQ(process_session_wait(manager, id, nullptr, 1200000,
		nullptr, nullptr, &snapshot), 0);
	EXPECT_EQ(snapshot.state, PROCESS_EXITED);
	EXPECT_STREQ(snapshot.stdout_text, "done");
	process_snapshot_cleanup(&snapshot);
}

TEST(ExecToolTest, ConfiguredWaitRangeControlsSchemaRuntimeAndToolTimeout)
{
	for (int maximum : {1200000, 1000}) {
		struct config cfg;
		config_set_defaults(nullptr, &cfg);
		cfg.exec.max_wait_ms = maximum;
		tool_registry registry{};
		tool_registry_init(nullptr, &registry);
		ASSERT_EQ(exec_tool_init(&registry, nullptr, &cfg.exec), 0);
		auto *entry = tool_borrow(&registry, "background");
		ASSERT_NE(entry, nullptr);
		EXPECT_GE(entry->timeout_seconds * 1000, maximum);
		cJSON *schema = cJSON_Parse(entry->desc.input_schema);
		ASSERT_NE(schema, nullptr);
		cJSON *wait = cJSON_GetObjectItem(cJSON_GetObjectItem(schema, "properties"), "wait_ms");
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(wait, "minimum")), 0);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(wait, "maximum")), maximum);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(wait, "default")),
			std::min(30000, maximum));
		cJSON_Delete(schema);
		tool_result result{};
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&registry, "exec", "{\"command\":\"printf configured\"}", &result), 0);
		std::string id = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
		tool_result_cleanup(&result);
		for (const auto &budget : {std::to_string(maximum), std::to_string(maximum + 1)}) {
			std::string args = "{\"action\":\"wait\",\"session_id\":\"" + id +
				"\",\"wait_ms\":" + budget + "}";
			tool_result_init(&result);
			bool allowed = std::stoi(budget) <= maximum;
			EXPECT_EQ(tool_exec(&registry, "background", args.c_str(), &result),
				allowed ? 0 : -EINVAL);
			if (allowed)
				EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data,
					"wait_ms")), maximum);
			tool_result_cleanup(&result);
		}
		tool_result_init(&result);
		std::string args = "{\"action\":\"wait\",\"session_id\":\"" + id + "\"}";
		ASSERT_EQ(tool_exec(&registry, "background", args.c_str(), &result), 0);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "wait_ms")),
			std::min(30000, maximum));
		tool_result_cleanup(&result);
		tool_registry_cleanup(&registry);
	}
}

TEST(ExecToolTest, RetainedJobsOwnerSurvivesToolUnregistration)
{
	struct tool_registry registry;
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	struct exec_tool_jobs_handle owner MORPH_CLEANUP(exec_tool_jobs_release) = {};
	ASSERT_EQ(exec_tool_jobs_acquire(&registry, &owner), 0);
	ASSERT_NE(owner.jobs, nullptr);
	auto *jobs = owner.jobs;
	EXPECT_EQ(exec_tool_jobs_acquire(&registry, &owner), -EALREADY);
	EXPECT_EQ(owner.jobs, jobs);
	ASSERT_EQ(tool_unregister(&registry, "exec"), 0);
	ASSERT_EQ(tool_unregister(&registry, "background"), 0);
	tool_registry_cleanup(&registry);
	EXPECT_EQ(exec_jobs_active(owner.jobs), 0);
	exec_tool_jobs_release(&owner);
	EXPECT_EQ(owner.jobs, nullptr);
	EXPECT_EQ(owner.tool.binding, nullptr);
}

TEST_F(ProcessTest, SnapshotCopiesRejectOccupiedOwnersBeforeStartingWork)
{
	process_spawn_options options{};
	options.command = "printf source; printf diagnostic >&2";
	options.yield_time_ms = 1000;
	char id[PROCESS_SESSION_ID_MAX];
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_spawn(manager, &options, id, &snapshot), 0);
	ASSERT_STREQ(snapshot.stdout_text, "source");
	ASSERT_STREQ(snapshot.stderr_text, "diagnostic");
	char *saved = snapshot.stdout_text;
	char rejected_id[PROCESS_SESSION_ID_MAX] = "unchanged";
	EXPECT_EQ(process_spawn(manager, &options, rejected_id, &snapshot), -EEXIST);
	EXPECT_STREQ(rejected_id, "unchanged");
	EXPECT_EQ(process_session_poll(manager, id, nullptr, 0, nullptr, nullptr, &snapshot),
		  -EEXIST);
	EXPECT_EQ(process_session_wait(manager, id, nullptr, 0, nullptr, nullptr, &snapshot),
		  -EEXIST);
	EXPECT_EQ(process_session_inspect(manager, id, 1, &snapshot), -EEXIST);
	EXPECT_EQ(process_session_snapshot(manager, id, &snapshot), -EEXIST);
	EXPECT_EQ(process_session_status(manager, id, &snapshot), -EEXIST);
	EXPECT_EQ(snapshot.stdout_text, saved);
	auto *saved_manager = manager;
	EXPECT_EQ(process_manager_create(nullptr, &manager), -EEXIST);
	EXPECT_EQ(manager, saved_manager);
	process_manager_destroy(manager);
	manager = nullptr;
	EXPECT_STREQ(snapshot.stdout_text, "source");
	EXPECT_STREQ(snapshot.stderr_text, "diagnostic");
}

TEST(ExecToolTest, JobsConstructorsAndSnapshotsRejectOccupiedOwners)
{
	struct exec_jobs *jobs = nullptr;
	ASSERT_EQ(exec_jobs_create(&jobs), 0);
	auto *owner = jobs;
	EXPECT_EQ(exec_jobs_create(&jobs), -EALREADY);
	EXPECT_EQ(jobs, owner);
	cJSON *snapshot = cJSON_CreateArray();
	ASSERT_NE(snapshot, nullptr);
	auto *snapshot_owner = snapshot;
	EXPECT_EQ(exec_jobs_list(jobs, 1, &snapshot), -EALREADY);
	EXPECT_EQ(exec_jobs_read(jobs, 1, "missing", &snapshot), -EALREADY);
	EXPECT_EQ(exec_jobs_wait(jobs, 1, "turn", nullptr, nullptr, &snapshot), -EALREADY);
	EXPECT_EQ(snapshot, snapshot_owner);
	cJSON_Delete(snapshot);
	exec_jobs_destroy(jobs);
}

TEST_F(ProcessTest, SpoolPreservesOutputBeforeTheMemoryWindow)
{
	process_spawn_options options{};
	options.command = "printf 'BEGIN\n'; dd if=/dev/zero bs=1024 count=256 2>/dev/null | "
		"tr '\\000' x; printf '\nEND\n'";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	process_cursor cursor{};
	ASSERT_EQ(process_session_wait(manager, id, &cursor, 10000, nullptr, nullptr,
		&snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	ASSERT_NE(snapshot.stdout_path[0], '\0');
	std::string expected = "BEGIN\n" + std::string(256 * 1024, 'x') + "\nEND\n";
	EXPECT_TRUE(snapshot.stdout_truncated);
	EXPECT_GT(snapshot.stdout_retained_from, 0u);
	EXPECT_EQ(snapshot.stdout_total_bytes, expected.size());
	EXPECT_EQ(snapshot.stdout_path_bytes, expected.size());
	EXPECT_EQ(snapshot.stdout_unrecoverable_bytes, 0u);
	EXPECT_FALSE(snapshot.stdout_path_truncated);
	EXPECT_EQ(snapshot.stdout_capture_error, 0);
	char *stored MORPH_CLEANUP(morph_charp_cleanup) =
		file_read_all_copy(snapshot.stdout_path, nullptr);
	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(std::string(stored), expected);
	EXPECT_EQ(snapshot.next_cursor.stdout_offset, expected.size());
	process_snapshot later MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_session_poll(manager, id, &snapshot.next_cursor, 0, nullptr, nullptr,
		&later), 0);
	EXPECT_STREQ(later.stdout_text, "");
	EXPECT_FALSE(later.stdout_truncated);
	EXPECT_EQ(later.stdout_omitted_bytes, 0u);
	std::string path = snapshot.stdout_path;
	process_manager_destroy(manager);
	manager = nullptr;
	EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(ProcessTest, SpoolQuotaReportsTheExactUnrecoverableRange)
{
	process_spawn_options options{};
	options.command = "dd if=/dev/zero bs=1024 count=65664 2>/dev/null | tr '\\000' q";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	process_cursor cursor{};
	ASSERT_EQ(process_session_wait(manager, id, &cursor, 60000, nullptr, nullptr,
		&snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	const size_t total = PROCESS_SPOOL_MAX_BYTES + 128u * 1024u;
	EXPECT_EQ(snapshot.stdout_total_bytes, total);
	EXPECT_EQ(snapshot.stdout_path_bytes, PROCESS_SPOOL_MAX_BYTES);
	EXPECT_EQ(snapshot.stdout_retained_from, total - 64u * 1024u);
	EXPECT_EQ(snapshot.stdout_unrecoverable_bytes, 64u * 1024u);
	EXPECT_TRUE(snapshot.stdout_path_truncated);
	EXPECT_EQ(snapshot.stdout_capture_error, 0);
	ASSERT_NE(snapshot.stdout_path[0], '\0');
	EXPECT_EQ(std::filesystem::file_size(snapshot.stdout_path), PROCESS_SPOOL_MAX_BYTES);
	options.command = "printf after-quota";
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot next MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_session_wait(manager, id, &cursor, 3000, nullptr, nullptr, &next), 0);
	EXPECT_STREQ(next.stdout_text, "after-quota");
	EXPECT_EQ(next.stdout_path_bytes, 0u);
	EXPECT_TRUE(next.stdout_path_truncated);
	EXPECT_EQ(next.stdout_unrecoverable_bytes, 0u);
}

class ProcessSecretEnvironment {
public:
	explicit ProcessSecretEnvironment(const char *value)
	{
		const char *old = getenv("OPENAI_API_KEY");
		present = old != nullptr;
		if (old)
			previous = old;
		setenv("OPENAI_API_KEY", value, 1);
	}

	~ProcessSecretEnvironment()
	{
		if (present)
			setenv("OPENAI_API_KEY", previous.c_str(), 1);
		else
			unsetenv("OPENAI_API_KEY");
	}

private:
	bool present = false;
	std::string previous;
};

TEST_F(ProcessTest, SecretsAreRedactedBeforeChunkPollAndSpoolBoundaries)
{
	ProcessSecretEnvironment environment("morph-secret-boundary-abcdefghijk");
	process_manager_destroy(manager);
	manager = nullptr;
	process_manager_config config{};
	config.max_session_output = 64 * 1024;
	config.max_inline_output = 4096;
	config.shell = "/bin/bash";
	ASSERT_EQ(process_manager_create(&config, &manager), 0);
	setenv("OPENAI_API_KEY", "changed-environment-after-manager-creation", 1);
	process_spawn_options options{};
	options.command = "printf 'prefixmorph-secret-bound'; read -r resume; "
		"printf 'ary-abcdefghijksuffix'";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot first MORPH_CLEANUP(process_snapshot_cleanup) = {};
	process_cursor start{};
	ASSERT_EQ(process_session_poll(manager, id, &start, 2000, nullptr, nullptr, &first), 0);
	ASSERT_EQ(first.state, PROCESS_RUNNING);
	EXPECT_STREQ(first.stdout_text, "prefix");
	EXPECT_EQ(first.next_cursor.stdout_offset, 6u);
	char *prefix MORPH_CLEANUP(morph_charp_cleanup) =
		file_read_all_copy(first.stdout_path, nullptr);
	ASSERT_NE(prefix, nullptr);
	EXPECT_STREQ(prefix, "prefix");
	ASSERT_EQ(process_session_write(manager, id, "continue\n", 9), 0);
	process_snapshot last MORPH_CLEANUP(process_snapshot_cleanup) = {};
	ASSERT_EQ(process_session_wait(manager, id, &first.next_cursor, 3000, nullptr, nullptr,
		&last), 0);
	EXPECT_STREQ(last.stdout_text, "[REDACTED:OPENAI_API_KEY]suffix");
	EXPECT_STREQ(last.stdout_path, first.stdout_path);
	char *stored MORPH_CLEANUP(morph_charp_cleanup) =
		file_read_all_copy(last.stdout_path, nullptr);
	ASSERT_NE(stored, nullptr);
	EXPECT_STREQ(stored, "prefix[REDACTED:OPENAI_API_KEY]suffix");
	EXPECT_EQ(last.next_cursor.stdout_offset, std::strlen(stored));
	EXPECT_EQ(last.stdout_path_bytes, std::strlen(stored));
	EXPECT_EQ(last.stdout_unrecoverable_bytes, 0u);
}

TEST_F(ProcessTest, UnicodePreviewBoundariesPreserveTheCompleteSpool)
{
	process_spawn_options options{};
	options.command = "i=0; while [ \"$i\" -lt 24000 ]; do printf '\\344\\270\\255'; "
		"i=$((i+1)); done";
	options.background = 1;
	char id[PROCESS_SESSION_ID_MAX];
	ASSERT_EQ(process_spawn(manager, &options, id, nullptr), 0);
	process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {};
	process_cursor start{};
	ASSERT_EQ(process_session_wait(manager, id, &start, 10000, nullptr, nullptr, &snapshot), 0);
	ASSERT_EQ(snapshot.state, PROCESS_EXITED);
	ASSERT_NE(snapshot.stdout_text, nullptr);
	EXPECT_EQ(utf8valid(snapshot.stdout_text), nullptr);
	EXPECT_TRUE(snapshot.stdout_truncated);
	EXPECT_FALSE(snapshot.stdout_path_truncated);
	char *stored MORPH_CLEANUP(morph_charp_cleanup) =
		file_read_all_copy(snapshot.stdout_path, nullptr);
	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(utf8valid(stored), nullptr);
	EXPECT_EQ(std::strlen(stored), 72000u);
	EXPECT_EQ(utf8len(stored), 24000u);
	EXPECT_EQ(snapshot.stdout_path_bytes, 72000u);
	EXPECT_EQ(snapshot.stdout_retained_from % 3, 0u);
}
