#include "exec/command_analyzer.h"
#include "exec/process.h"
#include "agent/tools/exec_tool.h"
#include "agent/tool.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

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

TEST(CommandAnalyzerTest, SplitsShellOperatorsAndPreservesQuotes)
{
	struct command_analysis analysis = {};

	ASSERT_EQ(command_analyze(
		"echo \"a && b\" && git status | tee out ; git push",
		&analysis), 0);
	ASSERT_EQ(analysis.count, 4u);
	ASSERT_STREQ(analysis.segments[0].argv[0], "echo");
	ASSERT_STREQ(analysis.segments[0].argv[1], "a && b");
	ASSERT_STREQ(analysis.segments[1].argv[0], "git");
	ASSERT_STREQ(analysis.segments[1].argv[1], "status");
	ASSERT_STREQ(analysis.segments[2].argv[0], "tee");
	ASSERT_STREQ(analysis.segments[3].argv[1], "push");
	EXPECT_FALSE(analysis.complex);
	command_analysis_cleanup(&analysis);
}

TEST(CommandAnalyzerTest, MarksUnsupportedShellSyntaxComplex)
{
	struct command_analysis analysis = {};

	ASSERT_EQ(command_analyze("printf '%s' \"$(date)\"", &analysis), 0);
	EXPECT_TRUE(analysis.complex);
	command_analysis_cleanup(&analysis);
}

TEST(ExecToolTest, RegistersExecAndProcessTools)
{
	struct tool_registry registry;

	tool_registry_init(&registry);
	ASSERT_EQ(exec_tool_init(&registry, nullptr, nullptr), 0);
	EXPECT_NE(tool_lookup(&registry, "exec"), nullptr);
	EXPECT_NE(tool_lookup(&registry, "process"), nullptr);
	tool_registry_cleanup(&registry);
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
