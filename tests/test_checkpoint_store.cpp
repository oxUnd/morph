#include <gtest/gtest.h>

extern "C" {
#include "persistence/checkpoint_store.h"
#include "persistence/credit_store.h"
#include "session.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/file.h"
#include "util/json.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class CheckpointStoreTest : public ::testing::Test
{
      protected:
	struct db database{};
	struct session session{};
	std::string directory;
	std::string workspace;
	std::string database_path;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-checkpoints-XXXXXX";
		char *path = mkdtemp(pattern);
		ASSERT_NE(path, nullptr);
		directory = path;
		workspace = directory + "/work";
		database_path = directory + "/state.sqlite";
		std::filesystem::create_directories(workspace);
		ASSERT_EQ(db_open(nullptr, &database, database_path.c_str()), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "checkpoint-test", "test", &session), 0);
	}

	void TearDown() override
	{
		db_close(&database);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	int64_t Create(const char *prompt = "edit the files")
	{
		int64_t id = 0;
		EXPECT_EQ(checkpoint_store_create(&database, session.id, workspace.c_str(),
						  "turn-test", prompt, &id),
			  0);
		EXPECT_GT(id, 0);
		return id;
	}

	void Write(const std::string &path, const std::string &text)
	{
		std::filesystem::create_directories(
			std::filesystem::path(workspace + "/" + path).parent_path());
		ASSERT_EQ(
			file_write_all((workspace + "/" + path).c_str(), text.data(), text.size()),
			0);
	}

	std::string Read(const std::string &path)
	{
		size_t length = 0;
		char *text MORPH_CLEANUP(morph_charp_cleanup) =
			file_read_all_copy((workspace + "/" + path).c_str(), &length);
		EXPECT_NE(text, nullptr);
		return text ? std::string(text, length) : std::string();
	}

	int64_t Scalar(const std::string &sql)
	{
		sqlite3_stmt *stmt = nullptr;
		EXPECT_EQ(sqlite3_prepare_v2(database.handle, sql.c_str(), -1, &stmt, nullptr),
			  SQLITE_OK);
		if (!stmt)
			return -1;
		int step = sqlite3_step(stmt);
		EXPECT_EQ(step, SQLITE_ROW);
		int64_t value = step == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
		sqlite3_finalize(stmt);
		return value;
	}

	int64_t History(const char *kind, const char *content, int active = 1)
	{
		struct model_history_insert item{};
		item.session_id = session.id;
		item.kind = kind;
		item.role = "user";
		item.content = content;
		item.active = active;
		item.token_count = 7;
		int64_t id = 0;
		EXPECT_EQ(model_history_add(&database, &item, &id), 0);
		return id;
	}
};

TEST_F(CheckpointStoreTest, DirtyPreimageAndModeSurviveDatabaseReopen)
{
	Write("edited.txt", "user's uncommitted text\n");
	ASSERT_EQ(chmod((workspace + "/edited.txt").c_str(), 0600), 0);
	int64_t id = Create("change the existing file");
	ASSERT_EQ(checkpoint_store_capture(&database, id, (workspace + "/edited.txt").c_str()), 0);
	Write("edited.txt", "agent result\n");
	ASSERT_EQ(chmod((workspace + "/edited.txt").c_str(), 0644), 0);
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	db_close(&database);
	ASSERT_EQ(db_open(nullptr, &database, database_path.c_str()), 0);
	ASSERT_EQ(db_init_schema(&database), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_preview_copy(&database, session.id, id, &report), 0);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(report, "files")), 1);
	EXPECT_TRUE(cJSON_IsString(cJSON_GetObjectItem(report, "workspace_root")));
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "code_restored")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "conversation_restored")));
	EXPECT_EQ(Read("edited.txt"), "user's uncommitted text\n");
	struct stat info{};
	ASSERT_EQ(stat((workspace + "/edited.txt").c_str(), &info), 0);
	EXPECT_EQ(info.st_mode & 0777, 0600);
}

TEST_F(CheckpointStoreTest, RestoresAddsDeletesEmptyFilesAndOverwrittenMoveDestination)
{
	Write("source", "original source");
	Write("destination", "user's existing destination");
	Write("empty", "");
	int64_t id = Create();
	for (const char *path : {"source", "destination", "empty", "new"})
		ASSERT_EQ(checkpoint_store_capture(&database, id, path), 0);
	ASSERT_EQ(unlink((workspace + "/source").c_str()), 0);
	Write("destination", "moved and changed source");
	ASSERT_EQ(unlink((workspace + "/empty").c_str()), 0);
	Write("new", "created by agent");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_preview_copy(&database, session.id, id, &report), 0);
	ASSERT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(report, "files")), 4);
	cJSON *row = nullptr;
	cJSON_ArrayForEach (row, cJSON_GetObjectItem(report, "files")) {
		ASSERT_TRUE(cJSON_IsBool(cJSON_GetObjectItem(row, "restore_exists")));
		EXPECT_NE(cJSON_GetObjectItem(row, "path")->valuestring[0], '/');
	}
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("source"), "original source");
	EXPECT_EQ(Read("destination"), "user's existing destination");
	EXPECT_TRUE(std::filesystem::exists(workspace + "/empty"));
	EXPECT_EQ(Read("empty"), "");
	EXPECT_FALSE(std::filesystem::exists(workspace + "/new"));
}

TEST_F(CheckpointStoreTest, ExternalModificationRejectsAllCodeWritesButAllowsConversationOnly)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "before", 1), 0);
	Write("a", "a before");
	Write("b", "b before");
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "a"), 0);
	ASSERT_EQ(checkpoint_store_capture(&database, id, "b"), 0);
	Write("a", "a agent");
	Write("b", "b agent");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	Write("a", "user changed this later");
	ASSERT_EQ(message_add(&database, session.id, "assistant", "later", 1), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_preview_copy(&database, session.id, id, &report), 0);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(report, "conflicts")), 1);
	morph_json_cleanup(&report);
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_BOTH,
					   &report),
		  -EAGAIN);
	EXPECT_EQ(Read("b"), "b agent");
	EXPECT_EQ(message_count(&database, session.id), 2);
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id,
					   CHECKPOINT_RESTORE_CONVERSATION, &report),
		  0);
	EXPECT_EQ(message_count(&database, session.id), 1);
	EXPECT_EQ(Read("a"), "user changed this later");
	EXPECT_EQ(Read("b"), "b agent");
}

TEST_F(CheckpointStoreTest, SealDoesNotAbsorbExternalChangesToPreviouslyCapturedPaths)
{
	Write("a", "a original");
	Write("b", "b original");
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "a"), 0);
	Write("a", "a agent");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	Write("a", "a manual");
	ASSERT_EQ(checkpoint_store_capture(&database, id, "b"), 0);
	Write("b", "b agent");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	EXPECT_EQ(checkpoint_store_capture(&database, id, "a"), -EAGAIN);
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
					   &report),
		  -EAGAIN);
	EXPECT_EQ(Read("a"), "a manual");
	EXPECT_EQ(Read("b"), "b agent");
}

TEST_F(CheckpointStoreTest, UnwrittenCapturedPathsArePrunedAfterPartialFailure)
{
	Write("untouched", "original");
	Write("changed", "before");
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "untouched"), 0);
	ASSERT_EQ(checkpoint_store_capture(&database, id, "missing"), 0);
	ASSERT_EQ(checkpoint_store_capture(&database, id, "changed"), 0);
	Write("changed", "first file changed before the patch failed");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	Write("untouched", "manual change after failed patch");
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_preview_copy(&database, session.id, id, &report), 0);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(report, "files")), 1);
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("changed"), "before");
	EXPECT_EQ(Read("untouched"), "manual change after failed patch");
	EXPECT_FALSE(std::filesystem::exists(workspace + "/missing"));
}

TEST_F(CheckpointStoreTest, RewindPreservesEmptyCurrentCheckpointAndSupportsANewBranch)
{
	Write("file", "initial dirty state");
	int64_t first = Create("first");
	ASSERT_EQ(checkpoint_store_capture(&database, first, "file"), 0);
	Write("file", "first change");
	ASSERT_EQ(checkpoint_store_seal(&database, first), 0);
	int64_t second = Create("second");
	ASSERT_EQ(checkpoint_store_capture(&database, second, "file"), 0);
	Write("file", "second change");
	ASSERT_EQ(checkpoint_store_seal(&database, second), 0);
	int64_t empty = Create("undo the previous change and edit again");
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, second, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("file"), "first change");
	EXPECT_EQ(checkpoint_store_capture(&database, second, "file"), -ESTALE);
	ASSERT_EQ(checkpoint_store_capture(&database, empty, "file"), 0);
	Write("file", "new branch");
	ASSERT_EQ(checkpoint_store_seal(&database, empty), 0);
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, first, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("file"), "initial dirty state");
}

TEST_F(CheckpointStoreTest,
       ConversationRestoreRecoversPreCompactionRowsFlagsPlansAndKeepsBillingMemory)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "earlier question", 1), 0);
	int64_t original = History("user_message", "earlier model input");
	ASSERT_EQ(trace_save(&database, session.id, 1, "[]", 0), 0);
	std::string sid = std::to_string(session.id);
	ASSERT_EQ(db_exec(&database,
			  ("INSERT INTO session_plans VALUES(" + sid + ",'old plan')").c_str()),
		  0);
	int64_t id = Create();
	ASSERT_EQ(db_exec(&database,
			  ("UPDATE model_history_items SET active=0 WHERE session_id=" + sid)
				  .c_str()),
		  0);
	int64_t summary = History("compaction_summary", "future summary");
	ASSERT_EQ(db_exec(&database,
			  ("INSERT INTO "
			   "history_compactions(session_id,cutoff_sequence_no,summary_item_id,"
			   "input_tokens,output_tokens,trigger_kind,created_at) VALUES(" +
			   sid + ",1," + std::to_string(summary) + ",7,1,'auto',0)")
				  .c_str()),
		  0);
	ASSERT_EQ(
		db_exec(&database, ("INSERT INTO history_summary_sources VALUES(" +
				    std::to_string(summary) + "," + std::to_string(original) + ")")
					   .c_str()),
		0);
	ASSERT_EQ(message_add(&database, session.id, "assistant", "future answer", 1), 0);
	ASSERT_EQ(trace_save(&database, session.id, 2, "[]", 0), 0);
	ASSERT_EQ(db_exec(&database,
			  ("UPDATE session_plans SET state_json='future plan' WHERE session_id=" +
			   sid)
				  .c_str()),
		  0);
	ASSERT_EQ(db_exec(&database,
			  ("INSERT INTO memory_profiles VALUES(" + sid + ",'remember this',0)")
				  .c_str()),
		  0);
	ASSERT_EQ(session_update_tokens(&database, session.id, 99), 0);
	ASSERT_EQ(credit_store_init_schema(&database), 0);
	struct credit_store_event credit{};
	credit.user_id = "local";
	credit.session_id = "checkpoint-test";
	credit.kind = "llm";
	credit.currency = "USD";
	credit.credits = 42;
	ASSERT_EQ(credit_store_record_event(&database, &credit), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id,
					   CHECKPOINT_RESTORE_CONVERSATION, &report),
		  0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "conversation_restored")));
	EXPECT_EQ(message_count(&database, session.id), 1);
	EXPECT_EQ(Scalar("SELECT count(*) FROM model_history_items"), 1);
	EXPECT_EQ(Scalar("SELECT active FROM model_history_items WHERE id=" +
			 std::to_string(original)),
		  1);
	EXPECT_EQ(Scalar("SELECT count(*) FROM history_compactions"), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM history_summary_sources"), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM react_traces"), 1);
	EXPECT_EQ(Scalar("SELECT count(*) FROM session_plans WHERE state_json='old plan'"), 1);
	EXPECT_EQ(Scalar("SELECT count(*) FROM memory_profiles WHERE profile_text='remember this'"),
		  1);
	EXPECT_EQ(Scalar("SELECT sum(credits) FROM credit_events"), 42);
	EXPECT_EQ(Scalar("SELECT token_used FROM sessions WHERE id=" + sid), 99);
}

TEST_F(CheckpointStoreTest, DatabaseFailureCompensatesCodeAndRollsBackConversation)
{
	Write("file", "before");
	ASSERT_EQ(message_add(&database, session.id, "user", "before", 1), 0);
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "file"), 0);
	Write("file", "after");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	ASSERT_EQ(message_add(&database, session.id, "assistant", "after", 1), 0);
	ASSERT_EQ(db_exec(&database,
			  "CREATE TRIGGER reject_checkpoint_update BEFORE UPDATE OF code_active "
			  "ON code_checkpoints WHEN NEW.code_active=0 BEGIN SELECT "
			  "RAISE(FAIL,'injected'); END"),
		  0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_BOTH,
					   &report),
		  MORPH_ERR_DB);
	ASSERT_NE(report, nullptr);
	EXPECT_EQ(Read("file"), "after");
	EXPECT_EQ(message_count(&database, session.id), 2);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "compensated")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "rollback_incomplete")));
	cJSON *effect = cJSON_GetArrayItem(cJSON_GetObjectItem(report, "effects"), 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "applied")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "compensated")));
}

TEST_F(CheckpointStoreTest, SymlinksHardlinksAndTraversalAreRejectedBeforeMutation)
{
	Write("file", "keep");
	std::filesystem::create_symlink("file", workspace + "/symbolic");
	ASSERT_EQ(link((workspace + "/file").c_str(), (workspace + "/hard").c_str()), 0);
	int64_t id = Create();
	EXPECT_EQ(checkpoint_store_capture(&database, id, "symbolic"), -ELOOP);
	EXPECT_EQ(checkpoint_store_capture(&database, id, "hard"), -ELOOP);
	EXPECT_EQ(checkpoint_store_capture(&database, id, "../outside"), -EACCES);
	EXPECT_EQ(Read("file"), "keep");
}

TEST_F(CheckpointStoreTest, FailedSealDoesNotAdoptManualChangesDuringALaterAttempt)
{
	Write("file", "before");
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "file"), 0);
	ASSERT_EQ(truncate((workspace + "/file").c_str(), 17 * 1024 * 1024), 0);
	EXPECT_EQ(checkpoint_store_seal(&database, id), -EFBIG);
	Write("file", "manual after failed seal");
	EXPECT_EQ(checkpoint_store_capture(&database, id, "other"), -ESTALE);
	int64_t next = Create("new turn after failed seal");
	ASSERT_EQ(checkpoint_store_capture(&database, next, "other"), 0);
	Write("other", "new change");
	ASSERT_EQ(checkpoint_store_seal(&database, next), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
					   &report),
		  -ESTALE);
	EXPECT_EQ(Read("file"), "manual after failed seal");
}

TEST_F(CheckpointStoreTest, LargeCapturedDataRestoresWithBoundedCompensationCopies)
{
	const size_t file_bytes = 15u * 1024u * 1024u;
	std::string contents(file_bytes, 'a');
	for (int i = 0; i < 3; i++)
		Write("large-" + std::to_string(i), contents);
	int64_t id = Create("restore ninety MiB of captured before and after data");
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(checkpoint_store_capture(&database, id,
			("large-" + std::to_string(i)).c_str()), 0);
	contents[0] = 'b';
	for (int i = 0; i < 3; i++)
		Write("large-" + std::to_string(i), contents);
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	EXPECT_EQ(Scalar("SELECT sum(length(before_data)+length(after_data)) FROM "
		"code_checkpoint_files WHERE checkpoint_id=" + std::to_string(id)),
		static_cast<int64_t>(file_bytes * 6));
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_CODE,
		&report), 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "code_restored")));
	for (int i = 0; i < 3; i++) {
		std::string path = workspace + "/large-" + std::to_string(i);
		std::ifstream file(path, std::ios::binary);
		char first = 0;

		file.get(first);
		EXPECT_EQ(first, 'a');
		EXPECT_EQ(std::filesystem::file_size(path), file_bytes);
	}
}

TEST_F(CheckpointStoreTest, ValidatesProspectiveAggregateAndRejectsAnOversizedSeal)
{
	const size_t file_bytes = 10u * 1024u * 1024u;
	std::string contents(file_bytes, 'a');
	for (int i = 0; i < 6; i++)
		Write("large-" + std::to_string(i), contents);
	int64_t id = Create("validate aggregate growth before writing");
	for (int i = 0; i < 6; i++)
		ASSERT_EQ(checkpoint_store_capture(&database, id,
			("large-" + std::to_string(i)).c_str()), 0);
	ASSERT_EQ(checkpoint_store_capture(&database, id, "moved"), 0);
	ASSERT_EQ(checkpoint_store_validate_write(&database, id, "large-0",
		CHECKPOINT_FILE_MAX_BYTES, nullptr), 0);
	contents.resize(CHECKPOINT_FILE_MAX_BYTES, 'a');
	Write("large-0", contents);
	EXPECT_EQ(checkpoint_store_validate_write(&database, id, "large-1",
		13u * 1024u * 1024u, nullptr), -EFBIG);
	EXPECT_EQ(std::filesystem::file_size(workspace + "/large-1"), file_bytes);
	EXPECT_EQ(checkpoint_store_validate_write(&database, id, "moved", file_bytes,
		"large-1"), 0);
	EXPECT_EQ(checkpoint_store_validate_write(&database, id, "uncaptured", 1, nullptr),
		-ESTALE);
	contents.resize(13u * 1024u * 1024u);
	Write("large-1", contents);
	contents.resize(file_bytes);
	contents[0] = 'b';
	for (int i = 2; i < 6; i++)
		Write("large-" + std::to_string(i), contents);
	EXPECT_GT(60u * 1024u * 1024u +
		std::filesystem::file_size(workspace + "/large-0") +
		std::filesystem::file_size(workspace + "/large-1") + file_bytes * 4,
		CHECKPOINT_DATA_MAX_BYTES);
	EXPECT_EQ(checkpoint_store_seal(&database, id), -EFBIG);
	EXPECT_EQ(Scalar("SELECT code_active FROM code_checkpoints WHERE id=" +
		std::to_string(id)), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoint_files WHERE checkpoint_id=" +
		std::to_string(id) + " AND sealed=1"), 0);
	EXPECT_EQ(checkpoint_store_validate_write(&database, id, "large-0", 1, nullptr), -ESTALE);
}

TEST_F(CheckpointStoreTest, RetentionExpirySessionIsolationAndDeletionCleanSnapshotRows)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "kept message", 1), 0);
	int64_t first = Create();
	for (int i = 0; i < 100; i++)
		ASSERT_GT(Create(), first);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_list_copy(&database, session.id, &report), 0);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(report, "checkpoints")), 100);
	morph_json_cleanup(&report);
	EXPECT_EQ(checkpoint_store_info_copy(&database, session.id, first, &report), -ENOENT);
	int64_t expired = Scalar("SELECT min(id) FROM code_checkpoints");
	ASSERT_EQ(db_exec(&database, ("UPDATE code_checkpoints SET created_at=0 WHERE id=" +
				      std::to_string(expired))
					     .c_str()),
		  0);
	EXPECT_EQ(checkpoint_store_info_copy(&database, session.id, expired, &report), -ENOENT);
	int64_t latest = Scalar("SELECT max(id) FROM code_checkpoints");
	struct session other{};
	ASSERT_EQ(session_create(&database, "other", "test", &other), 0);
	EXPECT_EQ(checkpoint_store_info_copy(&database, other.id, latest, &report), -ENOENT);
	ASSERT_GT(Scalar("SELECT count(*) FROM checkpoint_rows_messages"), 0);
	ASSERT_EQ(session_delete(&database, session.id), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoints"), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM checkpoint_rows_messages"), 0);
	EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoint_files"), 0);
}

TEST_F(CheckpointStoreTest, FilesystemFailureAfterTheFirstWriteCompensatesWithoutFollowingLinks)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "before", 1), 0);
	Write("a", "a before");
	Write("z/b", "b before");
	int64_t id = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, id, "a"), 0);
	ASSERT_EQ(checkpoint_store_capture(&database, id, "z/b"), 0);
	Write("a", "a after");
	Write("z/b", "b after");
	ASSERT_EQ(checkpoint_store_seal(&database, id), 0);
	ASSERT_EQ(message_add(&database, session.id, "assistant", "after", 1), 0);
	std::filesystem::create_directories(directory + "/outside");
	std::ofstream(directory + "/outside/b") << "outside sentinel";
	struct Fault {
		std::string root;
		std::string outside;
		int calls = 0;
	} fault{workspace, directory + "/outside"};
	ASSERT_EQ(sqlite3_create_function_v2(
			  database.handle, "checkpoint_parent_fault", 0, SQLITE_UTF8, &fault,
			  [](sqlite3_context *context, int, sqlite3_value **) {
				  auto *state = static_cast<Fault *>(sqlite3_user_data(context));
				  if (state->calls++ == 0) {
					  std::error_code error;
					  std::filesystem::rename(state->root + "/z",
								  state->root + "/z-saved", error);
					  if (!error)
						  std::filesystem::create_directory_symlink(
							  state->outside, state->root + "/z",
							  error);
					  if (error) {
						  sqlite3_result_error(
							  context, "fault injection failed", -1);
						  return;
					  }
				  }
				  sqlite3_result_int(context, 0);
			  },
			  nullptr, nullptr, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(db_exec(&database,
			  "CREATE TRIGGER checkpoint_filesystem_fault BEFORE DELETE ON messages "
			  "BEGIN SELECT checkpoint_parent_fault(); END"),
		  0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	EXPECT_NE(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_BOTH,
					   &report),
		  0);
	ASSERT_NE(report, nullptr);
	EXPECT_EQ(Read("a"), "a after");
	EXPECT_EQ(message_count(&database, session.id), 2);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "compensated")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "rollback_incomplete")));
	std::ifstream outside(directory + "/outside/b");
	std::string text;
	std::getline(outside, text);
	EXPECT_EQ(text, "outside sentinel");
	std::filesystem::remove(workspace + "/z");
	std::filesystem::rename(workspace + "/z-saved", workspace + "/z");
	EXPECT_EQ(Read("z/b"), "b after");
}

TEST_F(CheckpointStoreTest, ANewTurnPreservesManualEditsAndInvalidatesUnsafeOlderCodeHistory)
{
	Write("file", "initial");
	int64_t old = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, old, "file"), 0);
	Write("file", "agent first");
	ASSERT_EQ(checkpoint_store_seal(&database, old), 0);
	Write("file", "manual edit between turns");
	int64_t newer = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, newer, "file"), 0);
	Write("file", "agent next");
	ASSERT_EQ(checkpoint_store_seal(&database, newer), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, old, CHECKPOINT_RESTORE_CODE,
					   &report),
		  -ESTALE);
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, newer, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("file"), "manual edit between turns");
}

TEST_F(CheckpointStoreTest, BothRestoreDoesNotInvalidateCodeHistoryInADifferentWorkspace)
{
	Write("file", "root a before");
	int64_t root_a = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, root_a, "file"), 0);
	Write("file", "root a after");
	ASSERT_EQ(checkpoint_store_seal(&database, root_a), 0);
	std::string original_workspace = workspace;
	workspace = directory + "/another-workspace";
	Write("file", "root b before");
	int64_t root_b = Create();
	ASSERT_EQ(checkpoint_store_capture(&database, root_b, "file"), 0);
	Write("file", "root b after");
	ASSERT_EQ(checkpoint_store_seal(&database, root_b), 0);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, root_a, CHECKPOINT_RESTORE_BOTH,
					   &report),
		  0);
	EXPECT_EQ(Read("file"), "root b after");
	EXPECT_EQ(Scalar("SELECT code_active FROM code_checkpoints WHERE id=" +
			 std::to_string(root_b)),
		  1);
	morph_json_cleanup(&report);
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, root_b, CHECKPOINT_RESTORE_CODE,
					   &report),
		  0);
	EXPECT_EQ(Read("file"), "root b before");
	workspace = original_workspace;
	EXPECT_EQ(Read("file"), "root a before");
}
