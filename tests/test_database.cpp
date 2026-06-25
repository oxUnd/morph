#include <gtest/gtest.h>
#include "db/database.h"
#include "util/file.h"
#include <cstdio>
#include <cstring>

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
	int rc = db_open(&db, db_path);
	EXPECT_EQ(rc, 0);
	db_close(&db);
	EXPECT_EQ(file_exists(db_path), 1);
}

TEST_F(DatabaseTest, InitSchema) {
	int rc = db_open(&db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, OpenNull) {
	int rc = db_open(nullptr, "/tmp/test.db");
	EXPECT_NE(rc, 0);
}

TEST_F(DatabaseTest, ExecValid) {
	int rc = db_open(&db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	rc = db_exec(&db, "INSERT INTO sessions(name,model,created_at,updated_at) VALUES('test','gpt-4o',1,1)");
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, ExecInvalid) {
	int rc = db_open(&db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
	rc = db_exec(&db, "INVALID SQL STATEMENT");
	EXPECT_NE(rc, 0);
}

TEST_F(DatabaseTest, WALMode) {
	int rc = db_open(&db, db_path);
	EXPECT_EQ(rc, 0);
	rc = db_init_schema(&db);
	EXPECT_EQ(rc, 0);
}

TEST_F(DatabaseTest, SchemaTables) {
	int rc = db_open(&db, db_path);
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

TEST_F(DatabaseTest, SchemaMigrationBaselineRecorded) {
	int rc = db_open(&db, db_path);
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

TEST_F(DatabaseTest, ForeignKeysEnabled) {
	int rc = db_open(&db, db_path);
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
