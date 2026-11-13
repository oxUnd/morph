#include <gtest/gtest.h>
#include <string.h>
#include "nb/err.h"

extern "C" {
#include "persistence/checkpoint_store.h"
#include "session.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "cJSON.h"
}

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

std::string read_recovery_file(const std::string &path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

struct CommitRefusal {
	std::string first;
	std::string second;
	int rejected = 0;
};

int reject_restored_file_commit(void *opaque)
{
	auto &refusal = *static_cast<CommitRefusal *>(opaque);

	if (read_recovery_file(refusal.first) != "dirty before a\n" ||
	    read_recovery_file(refusal.second) != "dirty before b\n")
		return 0;
	refusal.rejected++;
	return 1;
}

class ScopedCommitRefusal
{
      public:
	ScopedCommitRefusal(sqlite3 *database, CommitRefusal *refusal) : database_(database)
	{
		sqlite3_commit_hook(database_, reject_restored_file_commit, refusal);
	}

	~ScopedCommitRefusal()
	{
		sqlite3_commit_hook(database_, nullptr, nullptr);
	}

	ScopedCommitRefusal(const ScopedCommitRefusal &) = delete;
	ScopedCommitRefusal &operator=(const ScopedCommitRefusal &) = delete;

      private:
	sqlite3 *database_;
};

class CheckpointRecoveryTest : public ::testing::Test
{
      protected:
	struct db database{};
	struct session session{};
	std::string directory;
	std::string workspace;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-checkpoint-recovery-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		workspace = directory + "/workspace";
		ASSERT_TRUE(std::filesystem::create_directory(workspace));
		ASSERT_EQ(db_open(nullptr, &database, (directory + "/state.sqlite").c_str()), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "checkpoint-recovery", "test", &session), 0);
	}

	void TearDown() override
	{
		if (database.handle)
			sqlite3_commit_hook(database.handle, nullptr, nullptr);
		db_close(&database);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	void Write(const char *name, const char *content)
	{
		const std::string path = workspace + "/" + name;
		ASSERT_EQ(nb_file_write_all(path.c_str(), content,
					    std::char_traits<char>::length(content), NULL),
			  0);
	}

	int64_t Scalar(const char *sql)
	{
		sqlite3_stmt *statement = nullptr;
		EXPECT_EQ(sqlite3_prepare_v2(database.handle, sql, -1, &statement, nullptr),
			  SQLITE_OK);
		if (!statement)
			return -1;
		const int rc = sqlite3_step(statement);
		EXPECT_EQ(rc, SQLITE_ROW);
		const int64_t result = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
		sqlite3_finalize(statement);
		return result;
	}

	void AddHistory(const char *content)
	{
		struct model_history_insert item{};
		item.session_id = session.id;
		item.kind = "user_message";
		item.role = "user";
		item.content = content;
		item.token_count = 5;
		item.active = 1;
		ASSERT_EQ(model_history_add(&database, &item, nullptr), 0);
		ASSERT_EQ(message_add(&database, session.id, "user", content, 5), 0);
	}

	int64_t PrepareEditedCheckpoint()
	{
		Write("a.txt", "dirty before a\n");
		Write("b.txt", "dirty before b\n");
		AddHistory("earlier conversation");
		int64_t id = 0;
		EXPECT_EQ(checkpoint_store_create(&database, session.id, workspace.c_str(),
						  "recovery-turn", "edit two files", &id),
			  0);
		EXPECT_GT(id, 0);
		EXPECT_EQ(checkpoint_store_capture(&database, id, "a.txt"), 0);
		EXPECT_EQ(checkpoint_store_capture(&database, id, "b.txt"), 0);
		Write("a.txt", "agent after a\n");
		Write("b.txt", "agent after b\n");
		EXPECT_EQ(checkpoint_store_seal(&database, id), 0);
		AddHistory("later conversation");
		return id;
	}

	void ExpectLaterState()
	{
		EXPECT_EQ(read_recovery_file(workspace + "/a.txt"), "agent after a\n");
		EXPECT_EQ(read_recovery_file(workspace + "/b.txt"), "agent after b\n");
		EXPECT_EQ(message_count(&database, session.id), 2);
		EXPECT_EQ(
			Scalar("SELECT count(*) FROM messages WHERE content='later conversation'"),
			1);
		EXPECT_EQ(Scalar("SELECT count(*) FROM model_history_items WHERE content='later "
				 "conversation'"),
			  1);
		EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoints WHERE code_active=1 AND "
				 "conversation_active=1"),
			  1);
	}

	void ExpectNoRecoveryFiles()
	{
		for (const auto &entry : std::filesystem::recursive_directory_iterator(workspace))
			EXPECT_NE(entry.path().filename().string().find(".morph-rewind-"), 0U)
				<< entry.path();
	}
};

TEST_F(CheckpointRecoveryTest, RefusedCommitCompensatesCodeAndPreservesLaterConversation)
{
	const int64_t id = PrepareEditedCheckpoint();
	ASSERT_GT(id, 0);
	CommitRefusal refusal{workspace + "/a.txt", workspace + "/b.txt"};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	int rc;
	{
		ScopedCommitRefusal hook(database.handle, &refusal);
		rc = checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_BOTH,
					      &report);
	}
	EXPECT_LT(rc, 0);
	EXPECT_EQ(refusal.rejected, 1);
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "compensated")));
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(report, "rollback_incomplete")));
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(report, "database_rollback_failed")));
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(report, "cleanup_incomplete")));
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(report, "code_restored")));
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(report, "conversation_restored")));
	const cJSON *effects = cJSON_GetObjectItem(report, "effects");
	ASSERT_EQ(cJSON_GetArraySize(effects), 2);
	for (int i = 0; i < cJSON_GetArraySize(effects); i++) {
		const cJSON *effect = cJSON_GetArrayItem(effects, i);
		EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "applied")));
		EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "compensated")));
		EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(effect, "recovery_available")));
	}
	ExpectLaterState();
	ExpectNoRecoveryFiles();
}

TEST_F(CheckpointRecoveryTest, OuterTransactionRejectsCaptureAndRestoreWithoutSideEffects)
{
	const int64_t id = PrepareEditedCheckpoint();
	ASSERT_GT(id, 0);
	Write("uncaptured.txt", "user data\n");
	ASSERT_EQ(db_exec(&database, "BEGIN"), 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_EQ(checkpoint_store_capture(&database, id, "uncaptured.txt"), NB_ERR_BUSY);
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, id, CHECKPOINT_RESTORE_BOTH,
					   &report),
		  NB_ERR_BUSY);
	EXPECT_EQ(report, nullptr);
	EXPECT_EQ(sqlite3_get_autocommit(database.handle), 0);
	ExpectLaterState();
	EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoint_files"), 2);
	EXPECT_EQ(read_recovery_file(workspace + "/uncaptured.txt"), "user data\n");
	ASSERT_EQ(db_exec(&database, "ROLLBACK"), 0);
	ExpectLaterState();
	EXPECT_EQ(Scalar("SELECT count(*) FROM code_checkpoint_files"), 2);
	ExpectNoRecoveryFiles();
}

} /* namespace */
