#include "exec/process.h"
#include "agent/tools/exec_tool.h"
#include "agent/tool.h"
#include "agent/tool_context.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

TEST(ExecToolTest, ReportsUnavailableApprovalWithoutClaimingItIsPending)
{
	char work_template[] = "/tmp/morph_exec_unavailable_XXXXXX";
	char *workdir = mkdtemp(work_template);
	struct tool_registry registry;
	struct tool_result result;

	ASSERT_NE(workdir, nullptr);
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_registry_init(&registry);
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
	struct tool_context *tctx = tool_context_create(workdir, workdir);
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(
		tctx, approve_exec_operation, &approval);
	tool_registry_init(&registry);
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
