#include <gtest/gtest.h>
#include <stdlib.h>
#include "nb/cleanup.h"
#include "db/database.h"
#include "db/permission_grant.h"
#include "errors.h"
#include "nb/buf.h"
#include "nb/file.h"
#include <cstdio>
#include <cstring>
#include <utility>

class DatabaseTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];
	void SetUp() override {
		memset(&db, 0, sizeof(db));
		snprintf(db_path, sizeof(db_path), "/tmp/ma_test_%d.db", getpid());
		std::remove(db_path);
	}
	void TearDown() override {
		db_close(&db);
		std::remove(db_path);
	}
};

TEST_F(DatabaseTest, OpenClose) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	db_close(&db);
	EXPECT_EQ(nb_file_exists(db_path), 1);
}

TEST_F(DatabaseTest, InitSchema) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, StandaloneCreditInitializationSurvivesFullSchemaInitialization)
{
	sqlite3_stmt *statement = nullptr;

	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_credit_schema(&db), 0);
	ASSERT_EQ(db_exec(&db, "INSERT INTO credit_events(kind,credits,created_at) "
			       "VALUES('test',7,1)"),
		  0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(db_init_credit_schema(&db), 0);
	ASSERT_EQ(sqlite3_prepare_v2(db.handle, "SELECT credits FROM credit_events", -1, &statement,
				     nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(statement, 0), 7);
	EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
	sqlite3_finalize(statement);
}

TEST_F(DatabaseTest, OpenNull) {
	int rc = db_open(NULL, nullptr, "/tmp/test.db");
	EXPECT_NE(rc, 0);
}

TEST_F(DatabaseTest, ExecValid) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	rc = db_exec(&db, "INSERT INTO sessions(name,model,created_at,updated_at) VALUES('test','gpt-4o',1,1)");
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, ExecInvalid) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	rc = db_exec(&db, "INVALID SQL STATEMENT");
	EXPECT_NE(rc, 0);
}

TEST_F(DatabaseTest, WALMode) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, SchemaTables) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db.handle,
		"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
		-1, &stmt, nullptr);
	EXPECT_EQ(rc, SQLITE_OK);
	int tables = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW)
		tables++;
	sqlite3_finalize(stmt);
	EXPECT_GE(tables, 6);
}

TEST_F(DatabaseTest, SubAgentSessionTablesExist) {
	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	for (const char *name : { "sub_agent_tasks", "sub_agent_events" }) {
		sqlite3_stmt *stmt = nullptr;
		ASSERT_EQ(sqlite3_prepare_v2(db.handle,
			"SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
			-1, &stmt, nullptr), SQLITE_OK);
		sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
		EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW);
		sqlite3_finalize(stmt);
	}
}

TEST_F(DatabaseTest, CollaborationAssignmentMigrationPreservesLegacyRows) {
	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(db_exec(&db,
		"INSERT INTO sessions(id,name,created_at,updated_at) VALUES(1,'root',1,1),(2,'child',1,1);"
		"INSERT INTO agent_threads(root_session_id,agent_id,parent_agent_id,session_id,"
		"model,effort,depth,description,status,started_at) "
		"VALUES(1,'/root/reader','/root',2,'test','',1,'legacy scope',2,1);"
		"ALTER TABLE agent_threads DROP COLUMN assignment_json;"), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
		"SELECT description,assignment_json FROM agent_threads", -1, &stmt, nullptr), SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)), "legacy scope");
	EXPECT_EQ(sqlite3_column_type(stmt, 1), SQLITE_NULL);
	sqlite3_finalize(stmt);
}

TEST_F(DatabaseTest, SchemaMigrationBaselineRecorded) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db.handle,
		"SELECT name FROM schema_migrations WHERE version=1",
		-1, &stmt, nullptr);
	EXPECT_EQ(rc, SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ((const char *)sqlite3_column_text(stmt, 0), "baseline");
	sqlite3_finalize(stmt);
}

TEST_F(DatabaseTest, RetiresOnlyLegacyMemoryMaintenanceReceipts) {
	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(db_exec(&db,
		"INSERT INTO sessions(id,name,model,created_at,updated_at) VALUES(1,'legacy','test',1,1);"
		"INSERT INTO model_history_items(session_id,sequence_no,kind,role,content,active,created_at) "
		"VALUES(1,1,'background_receipt','system',"
		"'[Background receipt] memory_consolidation: memory consolidation queued',1,1),"
		"(1,2,'background_receipt','system','[Background receipt] build: archive created',1,1),"
		"(1,3,'user_message','user','[Background receipt] memory_consolidation: user text',1,1);"
		"DELETE FROM schema_migrations WHERE version=2;"), 0);
	db_close(&db);
	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
		"SELECT sequence_no,active,content FROM model_history_items ORDER BY sequence_no",
		-1, &stmt, nullptr), SQLITE_OK);
	for (int index = 1; index <= 3; index++) {
		ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
		EXPECT_EQ(sqlite3_column_int(stmt, 0), index);
		EXPECT_EQ(sqlite3_column_int(stmt, 1), index == 1 ? 0 : 1);
		EXPECT_GT(sqlite3_column_bytes(stmt, 2), 0);
	}
	EXPECT_EQ(sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
}

static int database_prepare_legacy_errors(struct db *database)
{
	return db_exec(database,
		       "DELETE FROM schema_migrations WHERE version=3;"
		       "ALTER TABLE memory_jobs DROP COLUMN legacy_error_code;"
		       "ALTER TABLE sub_agent_tasks DROP COLUMN legacy_error_code;"
		       "ALTER TABLE agent_threads DROP COLUMN legacy_error_code;"
		       "ALTER TABLE history_compaction_attempts DROP COLUMN legacy_error_code;"
		       "INSERT INTO sessions(id,name,created_at,updated_at) VALUES(1,'legacy',1,1);"
		       "CREATE TABLE migration_error_cases(id INTEGER PRIMARY KEY,code "
		       "INTEGER,expected INTEGER);");
}

static int database_seed_legacy_errors(struct db *database)
{
	return db_exec(
		database,
		"INSERT INTO memory_jobs(id,session_id,generation,payload,error_code) "
		"SELECT id,1,0,'{}',code FROM migration_error_cases;"
		"INSERT INTO sub_agent_tasks(task_id,parent_session_id,agent_name,description,"
		"mode,status,started_at,error_code) "
		"SELECT CAST(id AS TEXT),1,'agent','scope','auto',0,1,code FROM "
		"migration_error_cases;"
		"INSERT INTO agent_threads(root_session_id,agent_id,parent_agent_id,session_id,"
		"model,effort,depth,description,status,started_at,error_code) "
		"SELECT 1,CAST(id AS TEXT),'/root',1,'test','',1,'scope',0,1,code "
		"FROM migration_error_cases;"
		"INSERT INTO history_compaction_attempts(id,session_id,trigger_kind,status,"
		"created_at,error_code) "
		"SELECT id,1,'manual','failed',1,code FROM migration_error_cases;"
		"INSERT INTO sub_agent_events(task_id,event_json,created_at) "
		"VALUES('1','{ \"error_code\": -261, \"nested\": { \"code\": -2 } }',1);");
}

static int database_insert_legacy_error(struct db *database, size_t id, int legacy, int current)
{
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {};

	if (!nb_buf_printf(&sql, "INSERT INTO migration_error_cases VALUES(%zu,%d,%d)", id, legacy,
			   current))
		return nb_buf_status(&sql);
	return db_exec(database, nb_buf_cstr(&sql));
}

static void database_statement_cleanup(sqlite3_stmt **statement)
{
	if (*statement)
		sqlite3_finalize(*statement);
}

static int database_scalar(struct db *database, const char *sql)
{
	sqlite3_stmt *statement NB_CLEANUP(database_statement_cleanup) = nullptr;

	if (sqlite3_prepare_v2(database->handle, sql, -1, &statement, nullptr) != SQLITE_OK)
		return -1;
	if (sqlite3_step(statement) != SQLITE_ROW)
		return -1;
	return sqlite3_column_int(statement, 0);
}

static void database_expect_migrated_errors(struct db *database)
{
	const char *const tables[] = {
		"memory_jobs",
		"sub_agent_tasks",
		"agent_threads",
		"history_compaction_attempts",
	};
	for (const char *table : tables) {
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {};

		ASSERT_TRUE(nb_buf_printf(
			&sql,
			"SELECT count(*) FROM %s AS actual JOIN migration_error_cases AS expected "
			"ON actual.rowid=expected.id WHERE actual.error_code<>expected.expected "
			"OR actual.legacy_error_code IS NOT expected.code",
			table));
		EXPECT_EQ(database_scalar(database, nb_buf_cstr(&sql)), 0) << table;
		nb_buf_clear(&sql);
		ASSERT_TRUE(nb_buf_printf(&sql, "SELECT count(*) FROM %s", table));
		EXPECT_EQ(database_scalar(database, nb_buf_cstr(&sql)),
			  database_scalar(database, "SELECT count(*) FROM migration_error_cases"))
			<< table;
	}
	EXPECT_EQ(database_scalar(database, "SELECT count(*) FROM schema_migrations WHERE "
					    "version=3 AND name='nbase_error_codes'"),
		  1);
	EXPECT_EQ(database_scalar(database,
				  "SELECT count(*) FROM sub_agent_events WHERE event_json="
				  "'{ \"error_code\": -261, \"nested\": { \"code\": -2 } }'"),
		  1);
}

TEST_F(DatabaseTest, LegacyErrorCodesMigrateOnceWithExactAuditValues)
{
	const std::pair<int, int> cases[] = {
		{-257, MORPH_ERR_NOT_CONFIGURED},
		{-258, MORPH_ERR_NOT_INITIALIZED},
		{-259, MORPH_ERR_API},
		{-260, MORPH_ERR_NETWORK},
		{-261, NB_ERR_PARSE},
		{-262, NB_ERR_PROTOCOL},
		{-263, MORPH_ERR_DB},
		{-264, NB_ERR_FORMAT},
		{-265, MORPH_ERR_PROCESSING},
		{-266, MORPH_ERR_SANDBOX},
		{-267, MORPH_ERR_LOAD},
		{-268, MORPH_ERR_LLM},
		{-269, MORPH_ERR_REACT_MAX_ITERATIONS},
		{-270, MORPH_ERR_CONFIG},
		{-271, MORPH_ERR_CONTEXT_LIMIT},
		{-272, MORPH_ERR_TOKEN_BUDGET},
		{-273, MORPH_ERR_AGENT_BUDGET},
		{-274, MORPH_ERR_REQUEST_BUDGET},
		{-1, NB_ERR_NOT_PERMITTED},
		{-2, NB_ERR_NOT_FOUND},
		{-4, NB_ERR_INTERRUPTED},
		{-5, NB_ERR_IO},
		{-7, NB_ERR_TOO_BIG},
		{-9, NB_ERR_BAD_HANDLE},
		{-10, NB_ERR_NO_CHILD},
		{-12, NB_ERR_NOMEM},
		{-13, NB_ERR_PERMISSION},
		{-16, NB_ERR_BUSY},
		{-17, NB_ERR_EXISTS},
		{-18, NB_ERR_CROSS_DEVICE},
		{-20, NB_ERR_NOT_DIR},
		{-21, NB_ERR_IS_DIR},
		{-22, NB_ERR_INVAL},
		{-24, NB_ERR_RESOURCE},
		{-25, NB_ERR_NOT_TERMINAL},
		{-27, NB_ERR_TOO_BIG},
		{-28, NB_ERR_NO_SPACE},
		{-32, NB_ERR_BROKEN_PIPE},
		{-34, NB_ERR_RANGE},
		{-11, NB_ERR_OS},
		{-35, NB_ERR_OS},
		{-60, NB_ERR_OS},
		{-89, NB_ERR_OS},
		{-125, NB_ERR_OS},
		{-65535, NB_ERR_OS},
		{INT_MIN, NB_ERR_OS},
		{0, 0},
		{42, 42},
	};
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(database_prepare_legacy_errors(&db), 0);
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		ASSERT_EQ(database_insert_legacy_error(&db, index + 1, cases[index].first,
						       cases[index].second),
			  0);
	}
	ASSERT_EQ(database_seed_legacy_errors(&db), 0);
	db_close(&db);
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	database_expect_migrated_errors(&db);
	db_close(&db);
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	database_expect_migrated_errors(&db);
}

TEST_F(DatabaseTest, NewDatabaseNeverReinterpretsNativeErrorCodes)
{
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(db_exec(&db, "INSERT INTO sessions(id,name,created_at,updated_at) "
			       "VALUES(1,'native',1,1);"),
		  0);
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_TRUE(
		nb_buf_printf(&sql,
			      "INSERT INTO memory_jobs(session_id,generation,payload,error_code) "
			      "VALUES(1,0,'{}',%d)",
			      NB_ERR_CANCELLED));
	ASSERT_EQ(db_exec(&db, nb_buf_cstr(&sql)), 0);
	db_close(&db);
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	EXPECT_EQ(database_scalar(&db, "SELECT error_code FROM memory_jobs"), NB_ERR_CANCELLED);
	EXPECT_EQ(database_scalar(
			  &db, "SELECT count(*) FROM memory_jobs WHERE legacy_error_code IS NULL"),
		  1);
}

TEST_F(DatabaseTest, LegacyErrorMigrationRollsBackSchemaAndRowsOnFailure)
{
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(database_prepare_legacy_errors(&db), 0);
	ASSERT_EQ(database_insert_legacy_error(&db, 1, -261, NB_ERR_PARSE), 0);
	ASSERT_EQ(database_seed_legacy_errors(&db), 0);
	ASSERT_EQ(db_exec(&db,
			  "CREATE TRIGGER reject_error_migration BEFORE UPDATE OF error_code ON "
			  "agent_threads "
			  "BEGIN SELECT RAISE(ABORT,'injected error migration failure'); END;"),
		  0);
	EXPECT_EQ(db_init_schema(&db), MORPH_ERR_DB);
	const char *const tables[] = {
		"memory_jobs",
		"sub_agent_tasks",
		"agent_threads",
		"history_compaction_attempts",
	};
	for (const char *table : tables) {
		EXPECT_NE(sqlite3_table_column_metadata(db.handle, nullptr, table,
							"legacy_error_code", nullptr, nullptr,
							nullptr, nullptr, nullptr),
			  SQLITE_OK)
			<< table;
		nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_TRUE(nb_buf_printf(&sql, "SELECT error_code FROM %s", table));
		EXPECT_EQ(database_scalar(&db, nb_buf_cstr(&sql)), -261) << table;
	}
	EXPECT_EQ(database_scalar(&db, "SELECT count(*) FROM schema_migrations WHERE version=3"),
		  0);
	ASSERT_EQ(db_exec(&db, "BEGIN; ROLLBACK; DROP TRIGGER reject_error_migration;"), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	database_expect_migrated_errors(&db);
}

TEST_F(DatabaseTest, LegacyErrorMigrationCommitsVersionOnlyAfterAllUpdatesSucceed)
{
	ASSERT_EQ(db_open(nullptr, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(database_prepare_legacy_errors(&db), 0);
	ASSERT_EQ(database_insert_legacy_error(&db, 1, -261, NB_ERR_PARSE), 0);
	ASSERT_EQ(database_seed_legacy_errors(&db), 0);
	ASSERT_EQ(db_exec(&db,
			  "CREATE TRIGGER reject_error_version BEFORE INSERT ON schema_migrations "
			  "WHEN NEW.version=3 BEGIN SELECT RAISE(ABORT,'injected version "
			  "failure'); END;"),
		  0);
	EXPECT_EQ(db_init_schema(&db), MORPH_ERR_DB);
	EXPECT_EQ(database_scalar(&db, "SELECT error_code FROM memory_jobs"), -261);
	EXPECT_EQ(database_scalar(&db, "SELECT error_code FROM history_compaction_attempts"), -261);
	EXPECT_EQ(database_scalar(&db, "SELECT count(*) FROM schema_migrations WHERE version=3"),
		  0);
	ASSERT_EQ(db_exec(&db, "DROP TRIGGER reject_error_version; SAVEPOINT caller;"), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	database_expect_migrated_errors(&db);
	ASSERT_EQ(db_exec(&db, "ROLLBACK TO caller; RELEASE caller;"), 0);
	EXPECT_EQ(database_scalar(&db, "SELECT error_code FROM memory_jobs"), -261);
	EXPECT_EQ(database_scalar(&db, "SELECT count(*) FROM schema_migrations WHERE version=3"),
		  0);
	ASSERT_EQ(db_init_schema(&db), 0);
	database_expect_migrated_errors(&db);
}

TEST_F(DatabaseTest, MessagesHaveTurnIdColumn) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db.handle, "PRAGMA table_info(messages)", -1,
				&stmt, nullptr);
	EXPECT_EQ(rc, SQLITE_OK);
	int found = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 1);
		if (name && strcmp(name, "turn_id") == 0) {
			found = 1;
			break;
		}
	}
	sqlite3_finalize(stmt);
	EXPECT_EQ(found, 1);
}

TEST_F(DatabaseTest, ForeignKeysEnabled) {
	int rc = db_open(NULL, &db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db.handle, "PRAGMA foreign_keys", -1, &stmt, nullptr);
	EXPECT_EQ(rc, SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
	sqlite3_finalize(stmt);
}

TEST_F(DatabaseTest, LegacySessionsGainWorkdirColumn)
{
	sqlite3_stmt *stmt = nullptr;

	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_exec(&db,
			  "CREATE TABLE sessions ("
			  "id INTEGER PRIMARY KEY AUTOINCREMENT,"
			  "display_id TEXT UNIQUE,"
			  "name TEXT UNIQUE NOT NULL,"
			  "model TEXT,"
			  "created_at INTEGER NOT NULL,"
			  "updated_at INTEGER NOT NULL,"
			  "token_used INTEGER DEFAULT 0);"
			  "INSERT INTO sessions(display_id,name,model,created_at,updated_at) "
			  "VALUES('1234abcd','legacy','test',1,1);"),
		  0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
				     "SELECT workdir FROM sessions WHERE name='legacy'", -1,
				     &stmt, nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)), "");
	sqlite3_finalize(stmt);
}

static int count_permission_grant(const struct permission_grant *grant,
				  void *user_data)
{
	int *count = static_cast<int *>(user_data);

	EXPECT_GT(grant->id, 0);
	EXPECT_STREQ(grant->subject, "lark-cli");
	EXPECT_STREQ(grant->resource_kind, "write_path");
	EXPECT_GT(grant->created_at, 0);
	(*count)++;
	return 0;
}

TEST_F(DatabaseTest, PermissionGrantPersistsAndDeduplicates)
{
	struct permission_grant grant = {};
	int count = 0;

	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	snprintf(grant.subject, sizeof(grant.subject), "%s", "lark-cli");
	snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s",
		 "write_path");
	snprintf(grant.resource, sizeof(grant.resource), "%s",
		 "/tmp/lark-state");
	snprintf(grant.project_root, sizeof(grant.project_root), "%s",
		 "/tmp/project");
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	ASSERT_EQ(permission_grant_each(&db, "/tmp/project",
					count_permission_grant, &count), 0);
	EXPECT_EQ(count, 1);
}

TEST_F(DatabaseTest, PermissionGrantCanBeRevokedAndCleared)
{
	struct permission_grant grant = {};
	int count = 0;
	int deleted = 0;

	ASSERT_EQ(db_open(NULL, &db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	snprintf(grant.subject, sizeof(grant.subject), "%s", "lark-cli");
	snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s",
		 "write_path");
	snprintf(grant.resource, sizeof(grant.resource), "%s",
		 "/tmp/lark-state");
	snprintf(grant.project_root, sizeof(grant.project_root), "%s",
		 "/tmp/project");
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	snprintf(grant.resource, sizeof(grant.resource), "%s",
		 "/tmp/lark-cache");
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	ASSERT_EQ(permission_grant_delete_subject(
		&db, "/tmp/project", "lark-cli", &deleted), 0);
	EXPECT_EQ(deleted, 2);
	ASSERT_EQ(permission_grant_each(&db, "/tmp/project",
					count_permission_grant, &count), 0);
	EXPECT_EQ(count, 0);

	snprintf(grant.subject, sizeof(grant.subject), "%s", "git");
	snprintf(grant.resource, sizeof(grant.resource), "%s", "git");
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	ASSERT_EQ(permission_grant_clear_project(
		&db, "/tmp/project", &deleted), 0);
	EXPECT_EQ(deleted, 1);
}
