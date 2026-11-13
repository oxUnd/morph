#include <gtest/gtest.h>
#include "nb/time.h"
#include <stdlib.h>
#include "nb/err.h"
#include "nb/cleanup.h"
#include "agent/tool.h"
#include "agent/guardrail.h"
#include "config/config.h"
#include "mcp/mcp_internal.h"
#include <fcntl.h>
#include "db/database.h"
#include "db/scheduled_task.h"
#include "nb/process.h"
#include "nb/time.h"
#include "nb/file.h"
#include "nb/cancel.h"
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
	return tool_result_error_copy(out, "invalid_arguments", "Rejected input");
}

static int conflicting_tool(const char *, tool_result *out, void *)
{
	tool_result_success_text_copy(out, "must not be successful");
	return NB_ERR_PERMISSION;
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
	ASSERT_EQ(tool_register_borrow(&reg, &rejected), 0);
	ASSERT_EQ(tool_register_borrow(&reg, &conflict), 0);
	ASSERT_EQ(tool_register_borrow(&reg, &malformed), 0);
	EXPECT_EQ(tool_exec(&reg, "reject", "{}", &out), NB_ERR_INVAL);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(out.envelope, "ok")));
	EXPECT_EQ(tool_exec(&reg, "conflict", "{}", &out), NB_ERR_PERMISSION);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(out.envelope, "ok")));
	EXPECT_EQ(tool_exec(&reg, "malformed", "{}", &out), NB_ERR_PROTOCOL);
	EXPECT_EQ(tool_exec(&reg, "missing", "{}", &out), NB_ERR_NOT_FOUND);
	ASSERT_NE(out.envelope, nullptr);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out.envelope, "ok")));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(out.envelope, "error"), "code")), "tool_not_found");
	EXPECT_EQ(tool_disable(&reg, "reject"), 0);
	EXPECT_EQ(tool_exec(&reg, "reject", "{}", &out), NB_ERR_NOT_PERMITTED);
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
	return tool_result_success_text_copy(out, "old binding");
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
	ASSERT_EQ(tool_register_take(&reg, &spec), 0);
	std::thread worker([&] {
		tool_result out{};
		tool_result_init(&out);
		EXPECT_EQ(tool_exec(&reg, "versioned", "{}", &out), 0);
		EXPECT_NE(std::string(out.text.ptr).find("old binding"), std::string::npos);
		tool_result_cleanup(&out);
	});
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		state.cond.wait(lock, [&] { return state.entered; });
	}
	EXPECT_EQ(tool_unregister(&reg, "versioned"), 0);
	EXPECT_EQ(state.destroyed.load(), 0);
	auto replacement = stability_spec("versioned", rejected_tool);
	EXPECT_EQ(tool_register_borrow(&reg, &replacement), 0);
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
	ASSERT_EQ(tool_register_take(&parent, &spec), 0);
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
		scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
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
	scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
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
	return NB_ERR_IO;
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
	scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
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
	notification *out = nullptr;
	EXPECT_LT(scheduled_task_run_due_collect_with_runner(&database, 2, 10, successful_task,
							     &calls, &out, &count),
		  0);
	EXPECT_EQ(out, nullptr);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(calls, 1);
	scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
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
		  NB_ERR_STALE);
	scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
	ASSERT_EQ(scheduled_task_get(&database, id, &task), 0);
	EXPECT_STREQ(task.status, "cancelled");
	scheduled_task_cleanup(&task);
}

TEST(Stability, ChildIoDrainsStderrWhileSendingLargeInput)
{
	const char *argv[] = {"/bin/sh", "-c",
			      "dd if=/dev/zero bs=4096 count=64 1>&2 2>/dev/null; "
			      "cat >/dev/null; printf done",
			      nullptr};
	nb_process_options spawn{};
	spawn.argv = argv;
	spawn.new_group = true;
	spawn.close_fds = true;
	nb_process *child NB_CLEANUP(nb_process_cleanup) = nullptr;
	ASSERT_EQ(nb_process_spawn(&spawn, &child, nullptr), NB_OK);
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf err NB_CLEANUP(nb_buf_cleanup) = {};
	std::string request(256 * 1024, 'x');
	bool truncated = false;
	nb_process_exchange_options exchange{};
	exchange.timeout_ms = 5000;
	exchange.max_output = 1024;
	exchange.stderr_truncated = &truncated;
	int status = -1;
	EXPECT_EQ(nb_process_exchange(child, request.data(), request.size(), &exchange, &out, &err,
				      &status, nullptr),
		  NB_OK);
	EXPECT_EQ(status, 0);
	EXPECT_STREQ(nb_buf_cstr(&out), "done");
	EXPECT_EQ(nb_buf_len(&err), 1024u);
	EXPECT_TRUE(truncated);
}

TEST(Stability, ChildIoTimesOutAfterChildClosesStreamsWithoutExiting)
{
	const char *argv[] = {"/bin/sh", "-c", "exec 0<&- 1>&- 2>&-; sleep 5", nullptr};
	nb_process_options spawn{};
	spawn.argv = argv;
	spawn.new_group = true;
	nb_process *child NB_CLEANUP(nb_process_cleanup) = nullptr;
	nb_err error{};
	ASSERT_EQ(nb_process_spawn(&spawn, &child, &error), NB_OK) << error.msg;
	pid_t pid = (pid_t)nb_process_id(child);
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf err NB_CLEANUP(nb_buf_cleanup) = {};
	nb_process_exchange_options exchange{};
	exchange.timeout_ms = 100;
	exchange.max_output = 1024;
	int status = -1;
	EXPECT_EQ(nb_process_exchange(child, "", 0, &exchange, &out, &err, &status, nullptr),
		  NB_ERR_TIMEOUT);
	EXPECT_EQ(waitpid(pid, nullptr, WNOHANG), -1);
	EXPECT_EQ(errno, ECHILD);
}

TEST(Stability, BlockedPipeSendHonorsDeadlineAndRestoresFlags)
{
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	int flags = fcntl(fds[1], F_GETFL);
	ASSERT_GE(flags, 0);
	std::string request(1024 * 1024, 'x');
	EXPECT_EQ(nb_fd_write_until(fds[1], request.data(), request.size(),
				    (nb_time_monotonic_ms() + 50) * 1000000, nullptr, nullptr,
				    nullptr),
		  NB_ERR_TIMEOUT);
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
	EXPECT_EQ(nb_fd_write_until(fds[1], "x", 1, (nb_time_monotonic_ms() + 100) * 1000000,
				    nullptr, nullptr, nullptr),
		  NB_ERR_BROKEN_PIPE);
	close(fds[1]);
}

TEST(Stability, ChildIoMarksBothPipeEndsCloseOnExec)
{
	int fds[2];
	ASSERT_EQ(nb_pipe_open(fds, nullptr), 0);
	EXPECT_NE(fcntl(fds[0], F_GETFD) & FD_CLOEXEC, 0);
	EXPECT_NE(fcntl(fds[1], F_GETFD) & FD_CLOEXEC, 0);
	close(fds[0]);
	close(fds[1]);
}

TEST(Stability, ChildIoDoesNotReportSuccessWhenRequestWasNotSent)
{
	const char *argv[] = {"/bin/sh", "-c", "exec 0<&-; exit 0", nullptr};
	nb_process_options spawn{};
	spawn.argv = argv;
	spawn.new_group = true;
	nb_process *child NB_CLEANUP(nb_process_cleanup) = nullptr;
	ASSERT_EQ(nb_process_spawn(&spawn, &child, nullptr), NB_OK);
	pid_t pid = (pid_t)nb_process_id(child);
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf err NB_CLEANUP(nb_buf_cleanup) = {};
	nb_process_exchange_options exchange{};
	exchange.timeout_ms = 1000;
	exchange.max_output = 1024;
	std::string request(1024 * 1024, 'x');
	int status = -1;
	EXPECT_EQ(nb_process_exchange(child, request.data(), request.size(), &exchange, &out, &err,
				      &status, nullptr),
		  NB_ERR_BROKEN_PIPE);
	EXPECT_EQ(waitpid(pid, nullptr, WNOHANG), -1);
	EXPECT_EQ(errno, ECHILD);
}

TEST(Stability, ReplacementRejectsFailedRegistryInitializationWithoutTakingOwnership)
{
	tool_registry reg{};
	tool_registry_init(nullptr, &reg);
	auto spec = stability_spec("candidate", rejected_tool);
	int destroyed = 0;
	spec.user_data = &destroyed;
	spec.user_data_destroy = [](void *data) { ++*static_cast<int *>(data); };
	reg.init_error = NB_ERR_NOMEM;
	EXPECT_EQ(tool_register_replace_take(&reg, &spec, TOOL_FLAG_DYNAMIC), NB_ERR_NOMEM);
	EXPECT_EQ(tool_borrow(&reg, "candidate"), nullptr);
	tool_registry_cleanup(&reg);
	EXPECT_EQ(destroyed, 0);
}

TEST(Stability, AtomicFileReplacementPreservesModeAndRejectsSymlinks)
{
	const nb_file_write_options file_write_options_1 = {0, 0600, false, true, false, nullptr};
	const nb_file_write_options file_write_options_2 = {0, 0600, false, true, false, nullptr};
	const nb_file_write_options file_write_options_3 = {0, 0600, false, true, false, nullptr};

	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-atomic-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/data";
	ASSERT_EQ(nb_file_write_all_atomic(path.c_str(), "old", 3, &file_write_options_1, NULL), 0);
	ASSERT_EQ(chmod(path.c_str(), 0640), 0);
	EXPECT_EQ(nb_file_write_all_atomic(path.c_str(), "new", 3, &file_write_options_2, NULL), 0);
	struct stat st{};
	ASSERT_EQ(stat(path.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0640);
	char *text =
		(nb_file_read_all(path.c_str(), &file_value_1, 0, NULL) == NB_OK &&
				 (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				   nb_buf_len(&file_value_1) + 1),
			    nb_charp_steal(&file_copy_1))
			 : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "new");
	free(text);
	std::string link = std::string(directory) + "/link";
	ASSERT_EQ(symlink(path.c_str(), link.c_str()), 0);
	EXPECT_LT(nb_file_write_all_atomic(link.c_str(), "bad", 3, &file_write_options_3, NULL), 0);
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
		_exit(nb_file_write_all(path, bytes, sizeof(bytes), NULL) < 0 ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	unlink(path);
}

TEST(Stability, AtomicFileWriteFailurePreservesCommittedContents)
{
	const nb_file_write_options file_write_options_4 = {0, 0600, false, true, false, nullptr};
	const nb_file_write_options file_write_options_5 = {0, 0600, false, true, false, nullptr};

	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-atomic-failure-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/data";
	ASSERT_EQ(
		nb_file_write_all_atomic(path.c_str(), "committed", 9, &file_write_options_4, NULL),
		0);
	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		signal(SIGXFSZ, SIG_IGN);
		struct rlimit limit{1, 1};
		if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
			_exit(2);
		}
		_exit(nb_file_write_all_atomic(path.c_str(), "replacement", 11,
					       &file_write_options_5, NULL) < 0
			      ? 0
			      : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	char *contents =
		(nb_file_read_all(path.c_str(), &file_value_2, 0, NULL) == NB_OK &&
				 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				   nb_buf_len(&file_value_2) + 1),
			    nb_charp_steal(&file_copy_2))
			 : NULL);
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
		ASSERT_EQ(nb_file_write_all(path, text.data(), text.size(), NULL), 0);
		config_validation_error error{};
		int rc = config_validate_file(path, &error);
		EXPECT_EQ(rc == 0, std::strcmp(policy, "invalid") != 0);
	}
	unlink(path);
}

TEST(Stability, CancellationTokenIsSafeForConcurrentReaders)
{
	nb_cancel token{};
	std::atomic<int> observed{0};
	std::thread first([&] {
		while (!nb_cancel_requested(&token)) {
			std::this_thread::yield();
		}
		observed.fetch_add(1);
	});
	std::thread second([&] {
		while (!nb_cancel_requested(&token)) {
			std::this_thread::yield();
		}
		observed.fetch_add(1);
	});
	nb_cancel_request(&token);
	first.join();
	second.join();
	EXPECT_EQ(observed.load(), 2);
	nb_cancel_reset(&token);
	EXPECT_FALSE(nb_cancel_requested(&token));
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
		  NB_ERR_STALE);
	scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
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
	ASSERT_EQ((nb_buf_init(&client.read_buffer),
		   nb_buf_reserve(&client.read_buffer, MCP_STDIO_MAX_FRAME_BYTES + 1)
			   ? 0
			   : NB_ERR_NOMEM),
		  0);
	std::string frame(MCP_STDIO_MAX_FRAME_BYTES, 'x');
	ASSERT_EQ(
		(nb_buf_append(&client.read_buffer, frame.data(), frame.size()) ? 0 : NB_ERR_NOMEM),
		0);
	ASSERT_EQ(write(output[1], "x", 1), 1);
	mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {};
	request.id = 7;
	request.json = strdup("{}");
	ASSERT_NE(request.json, nullptr);
	mcp_wire_reply reply{};
	EXPECT_EQ(mcp_stdio_exchange(&client, &request, 1000, &reply), NB_ERR_TOO_BIG);
	EXPECT_EQ(reply.json, nullptr);
	EXPECT_EQ(client.stdin_fd, -1);
	EXPECT_EQ(client.stdout_fd, -1);
	EXPECT_EQ(client.read_buffer.ptr, nullptr);
	close(input[0]);
	close(output[1]);
}
