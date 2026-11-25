#include <gtest/gtest.h>

extern "C" {
#include "persistence/orm/agent_store.h"
#include "persistence/orm/database.h"
#include "persistence/orm/session.h"
#include "persistence/orm/session_import.h"
#include "persistence/orm/test_fixture.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/log.h>
}

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

static void postgresql_import_message_cleanup(struct message **owner)
{
	message_free_list(*owner);
	*owner = nullptr;
}

static void postgresql_import_log(void *context, enum nb_log_level level,
				 const char *text, size_t length)
{
	if (level < NB_LOG_ERROR &&
	    std::string_view(text, length).find("ORM operation failed") == std::string_view::npos)
		return;

	static_cast<std::string *>(context)->append(text, length);
}

class PostgresqlImportTest : public ::testing::Test {
protected:
	struct db target{};
	struct db source{};
	struct session incoming{};
	struct nb_log *logger = nullptr;
	std::string logs;
	std::string directory;
	std::string prefix;
	std::string path;
	std::vector<int64_t> retained;

	void SetUp() override
	{
		ASSERT_NE(getenv("MORPH_TEST_POSTGRESQL_URL"), nullptr);
		char pattern[] = "/tmp/morph-postgresql-import-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		prefix = std::filesystem::path(directory).filename().string();
		path = directory + "/source.db";
		struct db_options options{};
		options.backend = DB_BACKEND_POSTGRESQL;
		options.url_env = "MORPH_TEST_POSTGRESQL_URL";
		options.tls_mode = DB_TLS_DISABLE;
		options.query_timeout_ms = 30000;
		logger = nb_log_new(NB_LOG_DEBUG, postgresql_import_log, &logs);
		ASSERT_NE(logger, nullptr);
		ASSERT_EQ(db_open_options(logger, &target, &options), NB_OK);
		ASSERT_EQ(db_init_schema(&target), NB_OK);
		struct session anchor{};
		ASSERT_EQ(session_create(&target, (prefix + "-anchor").c_str(), "test", &anchor),
			  NB_OK);
		retained.push_back(anchor.id);
		ASSERT_EQ(message_add(&target, anchor.id, "user", "existing target row", 1), NB_OK);
		ASSERT_EQ(db_open(logger, &source, path.c_str()), NB_OK);
		ASSERT_EQ(db_init_schema(&source), NB_OK);
		ASSERT_EQ(session_create(&source, (prefix + "-root").c_str(), "import-model",
					 &incoming),
			  NB_OK);
		ASSERT_EQ(message_add(&source, incoming.id, "user", "imported question", 3), NB_OK);
		ASSERT_EQ(message_add(&source, incoming.id, "assistant", "imported answer", 3),
			  NB_OK);
	}

	void TearDown() override
	{
		db_close(&source);
		for (int64_t id : retained)
			EXPECT_EQ(session_delete(&target, id), NB_OK);

		db_close(&target);
		nb_log_free(logger);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	struct session Find(const std::string &name)
	{
		struct session session{};

		EXPECT_EQ(session_get_by_name(&target, name.c_str(), &session), NB_OK);
		if (session.id)
			retained.push_back(session.id);

		return session;
	}

	int64_t Integer(const std::string &sql)
	{
		int64_t value = 0;

		EXPECT_EQ(orm_fixture_integer(&target, sql.c_str(), &value), NB_OK);
		return value;
	}
};

TEST_F(PostgresqlImportTest, RepeatedAndChangedImportsKeepReceiptsAndIdentitySequences)
{
	struct runtime_session_import_result result{};

	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 0);
	auto imported = Find(prefix + "-root");
	ASSERT_GT(imported.id, 0);
	int count = 0;
	struct message *messages NB_CLEANUP(postgresql_import_message_cleanup) =
		message_list(&target, imported.id, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(messages, nullptr);
	EXPECT_STREQ(messages->content, "imported question");
	ASSERT_NE(messages->next, nullptr);
	EXPECT_STREQ(messages->next->content, "imported answer");
	int64_t last_imported_id = messages->next->id;
	ASSERT_EQ(message_add(&target, imported.id, "assistant", "new target reply", 2), NB_OK);
	postgresql_import_message_cleanup(&messages);
	messages = message_list(&target, imported.id, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(messages->next->next, nullptr);
	EXPECT_GT(messages->next->next->id, last_imported_id);

	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 1);
	ASSERT_EQ(message_add(&source, incoming.id, "assistant", "changed source reply", 2), NB_OK);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 0);
	auto changed = Find(prefix + "-root [sync 1]");
	EXPECT_NE(changed.id, imported.id);
	postgresql_import_message_cleanup(&messages);
	messages = message_list(&target, changed.id, &count);
	EXPECT_EQ(count, 3);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 1);
}

TEST_F(PostgresqlImportTest, LegacyErrorsAndAgentRelationshipsSurviveChangedRootImports)
{
	struct session child{};

	ASSERT_EQ(session_create(&source, (prefix + "-child").c_str(), "import-model", &child),
		  NB_OK);
	std::string task = prefix + "-task";
	std::string rows =
		"DELETE FROM schema_migrations WHERE version=3;"
		"INSERT INTO sub_agent_tasks(task_id,parent_session_id,child_session_id,agent_name,"
		"description,mode,status,started_at,error_code) VALUES('" + task + "'," +
		std::to_string(incoming.id) + "," + std::to_string(child.id) +
		",'agent','scope','auto',3,1,-261);"
		"INSERT INTO sub_agent_events(task_id,event_json,created_at) VALUES('" + task +
		"','{\"error_code\":-261}',1);"
		"INSERT INTO agent_threads(root_session_id,agent_id,parent_agent_id,session_id,"
		"model,effort,depth,description,status,started_at,error_code) VALUES(" +
		std::to_string(incoming.id) + ",'reader','root'," + std::to_string(child.id) +
		",'test','',1,'scope',1,1,-1);"
		"INSERT INTO history_compaction_attempts(session_id,trigger_kind,status,created_at,"
		"error_code) VALUES(" + std::to_string(incoming.id) + ",'manual','failed',1,-35);"
		"INSERT INTO agent_budget_state(root_session_id,used,input_tokens,cached_tokens,"
		"output_tokens,reminder_level) VALUES(" + std::to_string(incoming.id) +
		",123,200,100,23,1)";
	ASSERT_EQ(orm_fixture_execute(&source, rows.c_str()), NB_OK);
	struct runtime_session_import_result result{};

	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 2);
	auto root = Find(prefix + "-root");
	auto imported_child = Find(prefix + "-child");
	std::string root_id = std::to_string(root.id);
	EXPECT_EQ(Integer("SELECT child_session_id FROM sub_agent_tasks WHERE parent_session_id=" +
			  root_id),
		  imported_child.id);
	EXPECT_EQ(Integer("SELECT session_id FROM agent_threads WHERE root_session_id=" + root_id),
		imported_child.id);
	EXPECT_EQ(Integer("SELECT status FROM agent_threads WHERE root_session_id=" + root_id), 4);
	EXPECT_EQ(Integer("SELECT error_code FROM sub_agent_tasks WHERE parent_session_id=" +
			  root_id),
		  NB_ERR_PARSE);
	EXPECT_EQ(Integer("SELECT legacy_error_code FROM sub_agent_tasks WHERE parent_session_id=" +
			  root_id),
		  -261);
	EXPECT_EQ(Integer("SELECT error_code FROM agent_threads WHERE root_session_id=" + root_id),
		NB_ERR_NOT_PERMITTED);
	EXPECT_EQ(Integer("SELECT error_code FROM history_compaction_attempts WHERE session_id=" +
			  root_id),
		  NB_ERR_OS);
	EXPECT_EQ(Integer("SELECT legacy_error_code FROM history_compaction_attempts WHERE "
			  "session_id=" +
			  root_id),
		  -35);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 2);

	ASSERT_EQ(orm_fixture_execute(&source,
		("UPDATE agent_budget_state SET used=500 WHERE root_session_id=" +
		 std::to_string(incoming.id)).c_str()), NB_OK);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(Integer("SELECT used FROM agent_budget_state WHERE root_session_id=" + root_id),
		  500);
	ASSERT_EQ(message_add(&source, incoming.id, "user", "changed parent", 2), NB_OK);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 1);
	auto changed = Find(prefix + "-root [sync 1]");
	std::string changed_id = std::to_string(changed.id);
	EXPECT_EQ(Integer("SELECT child_session_id FROM sub_agent_tasks WHERE parent_session_id=" +
			  changed_id),
		  imported_child.id);
	EXPECT_EQ(Integer("SELECT count(*) FROM sub_agent_events e JOIN sub_agent_tasks t "
		"ON t.task_id=e.task_id WHERE t.parent_session_id=" + changed_id), 1);
	EXPECT_EQ(
		Integer("SELECT session_id FROM agent_threads WHERE root_session_id=" + changed_id),
		imported_child.id);
}

TEST_F(PostgresqlImportTest, CopyFailureRollsBackSessionsAndReceiptsBeforeRetry)
{
	ASSERT_EQ(orm_fixture_execute(&source,
		"UPDATE messages SET token_count='bad integer' WHERE role='assistant'"), NB_OK);
	struct runtime_session_import_result result{};

	EXPECT_NE(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK);
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 0);
	EXPECT_TRUE(orm_fixture_transaction_idle(&target));
	struct session missing{};

	EXPECT_EQ(session_get_by_name(&target, (prefix + "-root").c_str(), &missing),
		NB_ERR_NOT_FOUND);
	EXPECT_EQ(Integer("SELECT count(*) FROM sync_import_receipts WHERE origin LIKE '" +
		prefix + ":%'"), 0);
	ASSERT_EQ(orm_fixture_execute(&source,
		"UPDATE messages SET token_count=3 WHERE role='assistant'"), NB_OK);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 0);
	EXPECT_GT(Find(prefix + "-root").id, 0);
}

TEST_F(PostgresqlImportTest, DuplicateLogicalOriginsKeepOriginalImportStatistics)
{
	struct session alias{};

	ASSERT_EQ(session_create(&source, (prefix + "-alias").c_str(), "import-model", &alias),
		  NB_OK);
	ASSERT_EQ(message_add(&source, alias.id, "user", "imported question", 3), NB_OK);
	ASSERT_EQ(message_add(&source, alias.id, "assistant", "imported answer", 3), NB_OK);
	std::string origins = "CREATE TABLE sync_session_origins(session_id INTEGER PRIMARY "
			      "KEY,origin TEXT NOT NULL);"
			      "INSERT INTO sync_session_origins VALUES(" +
			      std::to_string(incoming.id) + ",'" + prefix + "-logical-origin'),(" +
			      std::to_string(alias.id) + ",'" + prefix + "-logical-origin')";
	ASSERT_EQ(orm_fixture_execute(&source, origins.c_str()), NB_OK);
	struct runtime_session_import_result result{};

	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 1);
	EXPECT_GT(Find(prefix + "-root").id, 0);
	ASSERT_EQ(orm_session_import_file(&target, path.c_str(), prefix.c_str(), &result), NB_OK)
		<< logs;
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 2);
}
