#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/guardrail.h"
#include "config/config.h"
#include "mcp/mcp_internal.h"
#include <fcntl.h>
#include "db/database.h"
#include "db/scheduled_task.h"
#include "util/child_io.h"
#include "util/file.h"
#include "util/cancel.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static tool_spec stability_spec(const char *name, tool_exec_fn fn, void *data = nullptr)
{
	tool_spec spec{};
	spec.name = name;
	spec.description = "stability regression";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = fn;
	spec.user_data = data;
	return spec;
}

static int rejected_tool(const char *, tool_result *out, void *)
{
	return tool_result_error(out, "invalid_arguments", "Rejected input");
}

static int conflicting_tool(const char *, tool_result *out, void *)
{
	tool_result_success_text(out, "must not be successful");
	return -EACCES;
}

static int malformed_tool(const char *, tool_result *out, void *)
{
	out->envelope = cJSON_CreateObject();
	return 0;
}

TEST(Stability, ToolBusinessFailureAndConflictsAreNormalized)
{
	tool_registry reg{};
	tool_registry_init(nullptr, &reg);
	tool_result out{};
	tool_result_init(&out);
	auto rejected = stability_spec("reject", rejected_tool);
	auto conflict = stability_spec("conflict", conflicting_tool);
	auto malformed = stability_spec("malformed", malformed_tool);
	ASSERT_EQ(tool_register(&reg, &rejected), 0);
	ASSERT_EQ(tool_register(&reg, &conflict), 0);
	ASSERT_EQ(tool_register(&reg, &malformed), 0);
	EXPECT_EQ(tool_exec(&reg, "reject", "{}", &out), -EINVAL);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(out.envelope, "ok")));
	EXPECT_EQ(tool_exec(&reg, "conflict", "{}", &out), -EACCES);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(out.envelope, "ok")));
	EXPECT_EQ(tool_exec(&reg, "malformed", "{}", &out), MORPH_ERR_PROTOCOL);
	EXPECT_EQ(tool_exec(&reg, "missing", "{}", &out), -ENOENT);
	EXPECT_EQ(out.envelope, nullptr);
	EXPECT_EQ(tool_disable(&reg, "reject"), 0);
	EXPECT_EQ(tool_exec(&reg, "reject", "{}", &out), -EPERM);
	EXPECT_EQ(out.envelope, nullptr);
	tool_result_cleanup(&out);
	tool_registry_cleanup(&reg);
}

struct binding_state {
	std::mutex mutex;
	std::condition_variable cond;
	bool entered = false;
	bool release = false;
	std::atomic<int> destroyed{0};
};

struct binding_payload {
	binding_state *state;
};

static int blocking_binding(const char *, tool_result *out, void *opaque)
{
	auto *payload = static_cast<binding_payload *>(opaque);
	auto *state = payload->state;
	std::unique_lock<std::mutex> lock(state->mutex);
	state->entered = true;
	state->cond.notify_all();
	state->cond.wait(lock, [&] { return state->release; });
	return tool_result_success_text(out, "old binding");
}

static void destroy_binding(void *opaque)
{
	auto *payload = static_cast<binding_payload *>(opaque);
	payload->state->destroyed.fetch_add(1);
	delete payload;
}

TEST(Stability, ExecutingBindingSurvivesUnregisterAndReplacement)
{
	binding_state state;
	tool_registry reg{};
	tool_registry_init(nullptr, &reg);
	auto spec = stability_spec("versioned", blocking_binding, new binding_payload{&state});
	spec.user_data_destroy = destroy_binding;
	ASSERT_EQ(tool_register(&reg, &spec), 0);
	std::thread worker([&] {
		tool_result out{};
		tool_result_init(&out);
		EXPECT_EQ(tool_exec(&reg, "versioned", "{}", &out), 0);
		EXPECT_NE(std::string(out.text.data).find("old binding"), std::string::npos);
		tool_result_cleanup(&out);
	});
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		state.cond.wait(lock, [&] { return state.entered; });
	}
	EXPECT_EQ(tool_unregister(&reg, "versioned"), 0);
	EXPECT_EQ(state.destroyed.load(), 0);
	auto replacement = stability_spec("versioned", rejected_tool);
	EXPECT_EQ(tool_register(&reg, &replacement), 0);
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.release = true;
	}
	state.cond.notify_all();
	worker.join();
	EXPECT_EQ(state.destroyed.load(), 1);
	tool_registry_cleanup(&reg);
}

TEST(Stability, ChildRegistryRetainsParentOwnedBindings)
{
	binding_state state;
	state.release = true;
	tool_registry parent{}, child{};
	tool_registry_init(nullptr, &parent);
	tool_registry_init(nullptr, &child);
	auto spec = stability_spec("shared", blocking_binding, new binding_payload{&state});
	spec.user_data_destroy = destroy_binding;
	ASSERT_EQ(tool_register(&parent, &spec), 0);
	ASSERT_EQ(tool_registry_copy(&child, &parent), 0);
	tool_registry_cleanup(&parent);
	EXPECT_EQ(state.destroyed.load(), 0);
	tool_result out{};
	tool_result_init(&out);
	EXPECT_EQ(tool_exec(&child, "shared", "{}", &out), 0);
	tool_result_cleanup(&out);
	tool_registry_cleanup(&child);
	EXPECT_EQ(state.destroyed.load(), 1);
}

class StableTask : public ::testing::Test
{
      protected:
	struct db database{};
	void SetUp() override
	{
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
	}
	void TearDown() override
	{
		db_close(&database);
	}
	int64_t create()
	{
		scheduled_task_input input{};
		input.title = "stability";
		input.kind = "agent";
		input.action_type = "agent_run";
		input.trigger_type = "once";
		input.next_run_at = 1;
		input.payload_json = "{}";
		scheduled_task task{};
		EXPECT_EQ(scheduled_task_create(&database, &input, &task), 0);
		int64_t id = task.id;
		scheduled_task_cleanup(&task);
		return id;
	}
};

TEST(Stability, LegacyScheduledTaskMigrationPreservesExistingRows)
{
	db database{};
	ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
	ASSERT_EQ(
		db_exec(&database,
			"CREATE TABLE scheduled_tasks (id INTEGER PRIMARY KEY, title TEXT NOT NULL,"
			"kind TEXT NOT NULL,status TEXT NOT NULL,trigger_type TEXT NOT NULL,"
			"next_run_at INTEGER NOT NULL,interval_seconds INTEGER NOT NULL DEFAULT 0,"
			"timeout_at INTEGER NOT NULL DEFAULT 0,attempts INTEGER NOT NULL DEFAULT 0,"
			"max_attempts INTEGER NOT NULL DEFAULT 0,action_type TEXT NOT NULL,"
			"payload_json TEXT,policy_json TEXT,notify_json TEXT,last_error TEXT,"
			"created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
			"INSERT INTO scheduled_tasks(id,title,kind,status,trigger_type,next_run_at,"
			"action_type,created_at,updated_at) "
			"VALUES(1,'legacy','reminder','pending','once',1,'reminder',1,1);"),
		0);
	ASSERT_EQ(db_init_schema(&database), 0);
	ASSERT_EQ(db_init_schema(&database), 0);
	scheduled_task task{};
	ASSERT_EQ(scheduled_task_get(&database, 1, &task), 0);
	EXPECT_STREQ(task.title, "legacy");
	EXPECT_STREQ(task.status, "pending");
	EXPECT_STREQ(task.run_token, "");
	scheduled_task_cleanup(&task);
	int ran = 0;
	EXPECT_EQ(scheduled_task_run_due(&database, 2, 10, &ran), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(scheduled_task_get(&database, 1, &task), 0);
	EXPECT_STREQ(task.status, "completed");
	EXPECT_NE(task.run_token[0], '\0');
	scheduled_task_cleanup(&task);
	db_close(&database);
}

static int successful_task(const scheduled_task *, scheduled_task_action_result *, void *opaque)
{
	++*static_cast<int *>(opaque);
	return 0;
}

static int failing_observer(const morph_event *, void *)
{
	return -EIO;
}

TEST_F(StableTask, ObserverFailureDoesNotUndoOrRepeatSuccessfulExecution)
{
	int64_t id = create();
	int calls = 0, ran = 0;
	scheduled_task_event_sink sink{failing_observer, nullptr};
	EXPECT_EQ(scheduled_task_run_due_with_runner_events(&database, 2, 10, successful_task,
							    &calls, &ran, &sink),
		  0);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(ran, 1);
	scheduled_task task{};
	ASSERT_EQ(scheduled_task_get(&database, id, &task), 0);
	EXPECT_STREQ(task.status, "completed");
	scheduled_task_cleanup(&task);
	EXPECT_EQ(scheduled_task_run_due_with_runner_events(&database, 3, 10, successful_task,
							    &calls, &ran, &sink),
		  0);
	EXPECT_EQ(calls, 1);
}

struct transaction_trace {
	std::atomic<int> active{0};
	std::atomic<int> foreign_reads{0};
};

static int trace_task_transaction(unsigned type, void *opaque, void *statement, void *)
{
	if (type != SQLITE_TRACE_STMT) {
		return 0;
	}
	auto *trace = static_cast<transaction_trace *>(opaque);
	const char *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
	if (std::strstr(sql, "SAVEPOINT task_result")) {
		trace->active.store(1);
	} else if (std::strstr(sql, "RELEASE task_result")) {
		trace->active.store(0);
	} else if (std::strstr(sql, "SELECT 987654321") && trace->active.load()) {
		trace->foreign_reads.fetch_add(1);
	}
	return 0;
}

TEST_F(StableTask, OtherConnectionUsersCannotEnterTheResultTransaction)
{
	for (int i = 0; i < 100; ++i) {
		create();
	}
	transaction_trace trace;
	ASSERT_EQ(sqlite3_trace_v2(database.handle, SQLITE_TRACE_STMT, trace_task_transaction,
				   &trace),
		  SQLITE_OK);
	std::atomic<bool> running{true};
	std::atomic<int> queries{0};
	std::thread reader([&] {
		while (running.load()) {
			EXPECT_EQ(db_exec(&database, "SELECT 987654321"), 0);
			queries.fetch_add(1);
			std::this_thread::yield();
		}
	});
	while (queries.load() == 0) {
		std::this_thread::yield();
	}
	int calls = 0, ran = 0;
	EXPECT_EQ(scheduled_task_run_due_with_runner(&database, 2, 100, successful_task, &calls,
						     &ran),
		  0);
	running.store(false);
	reader.join();
	EXPECT_EQ(sqlite3_trace_v2(database.handle, 0, nullptr, nullptr), SQLITE_OK);
	EXPECT_EQ(calls, 100);
	EXPECT_EQ(ran, 100);
	EXPECT_EQ(trace.foreign_reads.load(), 0);
}

TEST_F(StableTask, NotificationFailureRollsBackStateAndCleansInitializedOutput)
{
	int64_t id = create();
	ASSERT_EQ(db_exec(&database, "CREATE TRIGGER reject_notification BEFORE INSERT "
				     "ON notifications BEGIN SELECT RAISE(FAIL,'injected'); END"),
		  0);
	int calls = 0;
	int count = 123;
	notification *out = reinterpret_cast<notification *>(1);
	EXPECT_LT(scheduled_task_run_due_collect_with_runner(&database, 2, 10, successful_task,
							     &calls, &out, &count),
		  0);
	EXPECT_EQ(out, nullptr);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(calls, 1);
	scheduled_task task{};
	ASSERT_EQ(scheduled_task_get(&database, id, &task), 0);
	EXPECT_STREQ(task.status, "running");
	EXPECT_EQ(task.attempts, 0);
	scheduled_task_cleanup(&task);
}

static int cancel_running_task(const scheduled_task *task, scheduled_task_action_result *,
			       void *opaque)
{
	return scheduled_task_cancel(static_cast<db *>(opaque), task->id);
}

TEST_F(StableTask, CancelledRunCannotOverwriteCancellation)
{
	int64_t id = create();
	int ran = 0;
	EXPECT_EQ(scheduled_task_run_due_with_runner(&database, 2, 10, cancel_running_task,
						     &database, &ran),
		  -ESTALE);
	scheduled_task task{};
	ASSERT_EQ(scheduled_task_get(&database, id, &task), 0);
	EXPECT_STREQ(task.status, "cancelled");
	scheduled_task_cleanup(&task);
}

TEST(Stability, ChildIoDrainsStderrWhileSendingLargeInput)
{
	int input[2], output[2], errors[2];
	ASSERT_EQ(pipe(input), 0);
	ASSERT_EQ(pipe(output), 0);
	ASSERT_EQ(pipe(errors), 0);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(input[1]);
		close(output[0]);
		close(errors[0]);
		char bytes[4096]{};
		for (int i = 0; i < 64; i++) {
			if (write(errors[1], bytes, sizeof(bytes)) <= 0) {
				_exit(3);
			}
		}
		while (read(input[0], bytes, sizeof(bytes)) > 0) {}
		(void)write(output[1], "done", 4);
		_exit(0);
	}
	close(input[0]);
	close(output[1]);
	close(errors[1]);
	morph_buf_t out{}, err{};
	ASSERT_EQ(morph_buf_init(&out, 128), 0);
	ASSERT_EQ(morph_buf_init(&err, 128), 0);
	std::string request(256 * 1024, 'x');
	int status = 0;
	EXPECT_EQ(morph_child_exchange(pid, input[1], output[0], errors[0], request.data(),
				       request.size(), 5000, 1024, nullptr, nullptr, &out, &err,
				       &status),
		  0);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	EXPECT_STREQ(morph_buf_cstr(&out), "done");
	EXPECT_NE(std::string(err.data + 1024).find("truncated"), std::string::npos);
	close(output[0]);
	close(errors[0]);
	morph_buf_cleanup(&out);
	morph_buf_cleanup(&err);
}

TEST(Stability, ChildIoTimesOutAfterChildClosesStreamsWithoutExiting)
{
	int input[2], output[2];
	ASSERT_EQ(pipe(input), 0);
	ASSERT_EQ(pipe(output), 0);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(input[1]);
		close(output[0]);
		close(output[1]);
		for (;;) {
			pause();
		}
	}
	close(input[0]);
	close(output[1]);
	morph_buf_t out{}, err{};
	ASSERT_EQ(morph_buf_init(&out, 16), 0);
	ASSERT_EQ(morph_buf_init(&err, 16), 0);
	int status = 0;
	EXPECT_EQ(morph_child_exchange(pid, input[1], output[0], -1, "", 0, 100, 1024, nullptr,
				       nullptr, &out, &err, &status),
		  -ETIMEDOUT);
	EXPECT_EQ(waitpid(pid, nullptr, WNOHANG), -1);
	EXPECT_EQ(errno, ECHILD);
	close(output[0]);
	morph_buf_cleanup(&out);
	morph_buf_cleanup(&err);
}

TEST(Stability, BlockedPipeSendHonorsDeadlineAndRestoresFlags)
{
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	int flags = fcntl(fds[1], F_GETFL);
	ASSERT_GE(flags, 0);
	std::string request(1024 * 1024, 'x');
	EXPECT_EQ(morph_io_write(fds[1], request.data(), request.size(), morph_io_now_ms() + 50,
				 nullptr, nullptr),
		  -ETIMEDOUT);
	/* Darwin may add internal pipe flags after a large write. Compare the
	 * mutable status flags rather than kernel-maintained bookkeeping. */
	constexpr int mutable_flags = O_NONBLOCK | O_APPEND | O_ASYNC;
	EXPECT_EQ(fcntl(fds[1], F_GETFL) & mutable_flags, flags & mutable_flags);
	close(fds[0]);
	close(fds[1]);
}

TEST(Stability, ClosedPipeSendDoesNotDeliverSigpipe)
{
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	close(fds[0]);
	EXPECT_EQ(morph_io_write(fds[1], "x", 1, morph_io_now_ms() + 100, nullptr, nullptr),
		  -EPIPE);
	close(fds[1]);
}

TEST(Stability, ChildIoMarksBothPipeEndsCloseOnExec)
{
	int fds[2];
	ASSERT_EQ(morph_io_pipe(fds), 0);
	EXPECT_NE(fcntl(fds[0], F_GETFD) & FD_CLOEXEC, 0);
	EXPECT_NE(fcntl(fds[1], F_GETFD) & FD_CLOEXEC, 0);
	close(fds[0]);
	close(fds[1]);
}

TEST(Stability, ChildIoDoesNotReportSuccessWhenRequestWasNotSent)
{
	int input[2], output[2];
	ASSERT_EQ(morph_io_pipe(input), 0);
	ASSERT_EQ(morph_io_pipe(output), 0);
	/* Keep a reader open and fill stdin so POLLOUT cannot hide the race.
	 * EOF/reap must not cause the unsent request to disappear from completion. */
	ASSERT_EQ(fcntl(input[1], F_SETFL, O_NONBLOCK), 0);
	char bytes[4096]{};
	while (write(input[1], bytes, sizeof(bytes)) > 0) {}
	ASSERT_EQ(errno, EAGAIN);
	ASSERT_EQ(fcntl(input[1], F_SETFL, 0), 0);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(input[1]);
		close(input[0]);
		close(output[0]);
		close(output[1]);
		_exit(0);
	}
	close(output[1]);
	std::string request(1024 * 1024, 'x');
	morph_buf_t out{}, err{};
	ASSERT_EQ(morph_buf_init(&out, 16), 0);
	ASSERT_EQ(morph_buf_init(&err, 16), 0);
	int status = 0;
	EXPECT_EQ(morph_child_exchange(pid, input[1], output[0], -1, request.data(), request.size(),
				       1000, 1024, nullptr, nullptr, &out, &err, &status),
		  -EPIPE);
	EXPECT_EQ(waitpid(pid, nullptr, WNOHANG), -1);
	EXPECT_EQ(errno, ECHILD);
	close(input[0]);
	close(output[0]);
	morph_buf_cleanup(&out);
	morph_buf_cleanup(&err);
}

TEST(Stability, ReplacementRejectsFailedRegistryInitializationWithoutTakingOwnership)
{
	tool_registry reg{};
	tool_registry_init(nullptr, &reg);
	auto spec = stability_spec("candidate", rejected_tool);
	int destroyed = 0;
	spec.user_data = &destroyed;
	spec.user_data_destroy = [](void *data) { ++*static_cast<int *>(data); };
	reg.init_error = -ENOMEM;
	EXPECT_EQ(tool_register_replace(&reg, &spec, TOOL_FLAG_DYNAMIC), -ENOMEM);
	EXPECT_EQ(tool_lookup(&reg, "candidate"), nullptr);
	tool_registry_cleanup(&reg);
	EXPECT_EQ(destroyed, 0);
}

TEST(Stability, AtomicFileReplacementPreservesModeAndRejectsSymlinks)
{
	char directory[] = "/tmp/morph-atomic-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/data";
	ASSERT_EQ(file_write_atomic(path.c_str(), "old", 3), 0);
	ASSERT_EQ(chmod(path.c_str(), 0640), 0);
	EXPECT_EQ(file_write_atomic(path.c_str(), "new", 3), 0);
	struct stat st{};
	ASSERT_EQ(stat(path.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0640);
	char *text = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "new");
	free(text);
	std::string link = std::string(directory) + "/link";
	ASSERT_EQ(symlink(path.c_str(), link.c_str()), 0);
	EXPECT_LT(file_write_atomic(link.c_str(), "bad", 3), 0);
	unlink(link.c_str());
	unlink(path.c_str());
	rmdir(directory);
}

TEST(Stability, BufferedFileCloseFailureIsReported)
{
	char path[] = "/tmp/morph-close-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	close(fd);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		signal(SIGXFSZ, SIG_IGN);
		struct rlimit limit{1, 1};
		if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
			_exit(2);
		}
		char bytes[100]{};
		_exit(file_write_all(path, bytes, sizeof(bytes)) < 0 ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	unlink(path);
}

TEST(Stability, AtomicFileWriteFailurePreservesCommittedContents)
{
	char directory[] = "/tmp/morph-atomic-failure-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/data";
	ASSERT_EQ(file_write_atomic(path.c_str(), "committed", 9), 0);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		signal(SIGXFSZ, SIG_IGN);
		struct rlimit limit{1, 1};
		if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
			_exit(2);
		}
		_exit(file_write_atomic(path.c_str(), "replacement", 11) < 0 ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	char *contents = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "committed");
	free(contents);
	unlink(path.c_str());
	/* A leaked temporary file would prevent removing the directory. */
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(Stability, GuardrailCheckFailureHasExplicitPolicy)
{
	guardrail_config config{};
	config.enabled = 1;
	ASSERT_EQ(guardrail_rule_register(&config, "unavailable", GUARDRAIL_HOOK_OUTPUT,
					  GUARDRAIL_RULE_EXT, nullptr, "test",
					  "/nonexistent/morph-check", ""),
		  0);
	guardrail_eval_ctx input{};
	input.proposed_answer = "answer";
	auto result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &input);
	EXPECT_EQ(result.verdict, GUARDRAIL_PASS);
	EXPECT_LT(result.check_error, 0);
	config.rules[0].block_on_error = 1;
	result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &input);
	EXPECT_EQ(result.verdict, GUARDRAIL_FAIL);
	EXPECT_LT(result.check_error, 0);
}

TEST(Stability, GuardrailErrorPolicyIsValidatedByConfigSchema)
{
	char path[] = "/tmp/morph-guardrail-config-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	close(fd);
	for (const char *policy : {"continue", "block", "invalid"}) {
		std::string text =
			"[[react.guardrail_ext_rules]]\nname = \"checker\"\non_error = \"";
		text += policy;
		text += "\"\n";
		ASSERT_EQ(file_write_all(path, text.data(), text.size()), 0);
		config_validation_error error{};
		int rc = config_validate_file(path, &error);
		EXPECT_EQ(rc == 0, std::strcmp(policy, "invalid") != 0);
	}
	unlink(path);
}

TEST(Stability, CancellationTokenIsSafeForConcurrentReaders)
{
	morph_cancel_token token{};
	std::atomic<int> observed{0};
	std::thread first([&] {
		while (!morph_cancel_token_is_cancelled(&token)) {
			std::this_thread::yield();
		}
		observed.fetch_add(1);
	});
	std::thread second([&] {
		while (!morph_cancel_token_is_cancelled(&token)) {
			std::this_thread::yield();
		}
		observed.fetch_add(1);
	});
	morph_cancel_token_cancel(&token);
	first.join();
	second.join();
	EXPECT_EQ(observed.load(), 2);
	morph_cancel_token_reset(&token);
	EXPECT_FALSE(morph_cancel_token_is_cancelled(&token));
}

TEST(Stability, ConcurrentJsonParsingKeepsErrorCursorsThreadLocal)
{
	std::atomic<int> stage{0};
	std::thread failed([&] {
		cJSON *value = cJSON_Parse("{invalid");
		EXPECT_EQ(value, nullptr);
		cJSON_Delete(value);
		const char *error = cJSON_GetErrorPtr();
		EXPECT_NE(error, nullptr);
		stage.store(1);
		while (stage.load() != 2) {
			std::this_thread::yield();
		}
		EXPECT_EQ(cJSON_GetErrorPtr(), error);
	});
	std::thread successful([&] {
		while (stage.load() != 1) {
			std::this_thread::yield();
		}
		cJSON *value = cJSON_Parse("{}");
		EXPECT_NE(value, nullptr);
		EXPECT_EQ(cJSON_GetErrorPtr(), nullptr);
		cJSON_Delete(value);
		stage.store(2);
	});
	failed.join();
	successful.join();
}

static int replace_running_generation(const scheduled_task *task, scheduled_task_action_result *,
				      void *opaque)
{
	auto *database = static_cast<db *>(opaque);
	int rc = scheduled_task_update_run(database, task->id, "waiting", 1, 0, 0, nullptr);
	if (rc != 0) {
		return rc;
	}
	int calls = 0, ran = 0;
	rc = scheduled_task_run_due_with_runner(database, 2, 10, successful_task, &calls, &ran);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(ran, 1);
	return rc;
}

TEST_F(StableTask, ObsoleteRunCannotOverwriteANewerCompletedGeneration)
{
	int64_t id = create();
	int ran = 0;
	EXPECT_EQ(scheduled_task_run_due_with_runner(&database, 2, 10, replace_running_generation,
						     &database, &ran),
		  -ESTALE);
	scheduled_task task{};
	ASSERT_EQ(scheduled_task_get(&database, id, &task), 0);
	EXPECT_STREQ(task.status, "completed");
	EXPECT_EQ(task.attempts, 1);
	EXPECT_NE(task.run_token[0], '\0');
	scheduled_task_cleanup(&task);
	notification *notifications = nullptr;
	int count = 0;
	ASSERT_EQ(notification_list_unread(&database, 10, &notifications, &count), 0);
	EXPECT_EQ(count, 1);
	notification_free_list(notifications, count);
}

TEST(Stability, OversizedMcpFrameDisconnectsInsteadOfGrowingWithoutBound)
{
	int input[2], output[2];
	ASSERT_EQ(pipe(input), 0);
	ASSERT_EQ(pipe(output), 0);
	mcp_client client{};
	client.stdin_fd = input[1];
	client.stdout_fd = output[0];
	client.server_pid = -1;
	ASSERT_EQ(morph_buf_init(&client.read_buffer, MCP_STDIO_MAX_FRAME_BYTES + 1), 0);
	std::string frame(MCP_STDIO_MAX_FRAME_BYTES, 'x');
	ASSERT_EQ(morph_buf_append(&client.read_buffer, frame.data(), frame.size()), 0);
	ASSERT_EQ(write(output[1], "x", 1), 1);
	mcp_wire_request request{};
	request.id = 7;
	request.json = "{}";
	mcp_wire_reply reply{};
	EXPECT_EQ(mcp_stdio_exchange(&client, &request, 1000, &reply), -EFBIG);
	EXPECT_EQ(reply.json, nullptr);
	EXPECT_EQ(client.stdin_fd, -1);
	EXPECT_EQ(client.stdout_fd, -1);
	EXPECT_EQ(client.read_buffer.data, nullptr);
	close(input[0]);
	close(output[1]);
}
