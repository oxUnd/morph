#include <gtest/gtest.h>
#include "sync/sync.h"
#include "util/file.h"
#include "util/error.h"
#include "blake3.h"
#include "cJSON.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <utime.h>

class SyncTest : public ::testing::Test {
protected:
	char root[PATH_MAX];
	char local[PATH_MAX];
	char remote[PATH_MAX];
	char state[PATH_MAX];
	struct morph_sync_config cfg;

	void SetUp() override {
		snprintf(root, sizeof(root), "/tmp/morph_sync_%d", getpid());
		snprintf(local, sizeof(local), "%s/local", root);
		snprintf(remote, sizeof(remote), "%s/remote", root);
		snprintf(state, sizeof(state), "%s/state", root);
		file_ensure_dir(local);
		file_ensure_dir(remote);
		file_ensure_dir(state);
		memset(&cfg, 0, sizeof(cfg));
		cfg.enabled = 1;
		snprintf(cfg.source_dir, sizeof(cfg.source_dir), "%s", local);
		snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", remote);
		cfg.interval_seconds = 300;
		cfg.retention_days = 30;
		snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "config.toml");
		snprintf(cfg.include[1], sizeof(cfg.include[1]), "%s", "output");
		cfg.include_count = 2;
	}

	void TearDown() override {
		char cmd[PATH_MAX + 32];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
		std::system(cmd);
	}

	int OpenManifest(sqlite3 **db) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/.morph-sync/manifest.db", remote);
		return sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, nullptr);
	}

	int QueryInt(const char *sql) {
		sqlite3 *db = nullptr;
		sqlite3_stmt *stmt = nullptr;
		int value = -1;
		if (OpenManifest(&db) != SQLITE_OK)
			return value;
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK &&
		    sqlite3_step(stmt) == SQLITE_ROW)
			value = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return value;
	}

	void CreateSqlite(const std::string &path, int rows = 1) {
		sqlite3 *db = nullptr;
		ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
		ASSERT_EQ(sqlite3_exec(db,
			"PRAGMA journal_mode=WAL;"
			"CREATE TABLE items(id INTEGER PRIMARY KEY,value TEXT);",
			nullptr, nullptr, nullptr), SQLITE_OK);
		for (int i = 0; i < rows; i++) {
			std::string sql = "INSERT INTO items(value) VALUES('item-" +
				std::to_string(i) + "')";
			ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr,
				nullptr), SQLITE_OK);
		}
		sqlite3_close(db);
	}

	int SqliteCount(const std::string &path) {
		sqlite3 *db = nullptr;
		sqlite3_stmt *stmt = nullptr;
		int count = -1;
		if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY,
			nullptr) != SQLITE_OK)
			return count;
		if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM items", -1,
			&stmt, nullptr) == SQLITE_OK &&
		    sqlite3_step(stmt) == SQLITE_ROW)
			count = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return count;
	}
};

static int test_path_join(char *dst, size_t size, const char *root,
			  const char *rel)
{
	return file_path_join(dst, size, root, rel && rel[0] ? rel : ".");
}

static int test_copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	FILE *out;
	char buf[4096];
	char parent[PATH_MAX];
	char *slash;
	size_t n;
	if (!in)
		return -errno;
	snprintf(parent, sizeof(parent), "%s", dst);
	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		if (parent[0]) {
			int rc = file_ensure_dir(parent);
			if (rc != 0) {
				fclose(in);
				return rc;
			}
		}
	}
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return -errno;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return -EIO;
		}
	}
	fclose(in);
	fclose(out);
	return 0;
}

static int test_backend_stat(void *user_data, const char *rel,
			     struct morph_sync_backend_stat *st)
{
	char path[PATH_MAX];
	struct stat sb;
	const char *root = static_cast<const char *>(user_data);
	memset(st, 0, sizeof(*st));
	if (test_path_join(path, sizeof(path), root, rel) != 0)
		return -EINVAL;
	if (stat(path, &sb) != 0)
		return 0;
	st->exists = 1;
	st->is_dir = S_ISDIR(sb.st_mode);
	st->size = sb.st_size;
	st->mtime = sb.st_mtime;
	if (!st->is_dir) {
		FILE *f = fopen(path, "rb");
		unsigned char buf[4096];
		unsigned char hash[BLAKE3_OUT_LEN];
		blake3_hasher hasher;
		static const char hex[] = "0123456789abcdef";
		size_t n;
		if (f) {
			blake3_hasher_init(&hasher);
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				blake3_hasher_update(&hasher, buf, n);
			fclose(f);
			blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);
			for (size_t i = 0; i < BLAKE3_OUT_LEN; i++) {
				st->hash[i * 2] = hex[hash[i] >> 4];
				st->hash[i * 2 + 1] = hex[hash[i] & 0x0f];
			}
			st->hash[BLAKE3_OUT_LEN * 2] = '\0';
		}
	}
	return 0;
}

static int test_backend_list(void *user_data, const char *rel_dir,
			     morph_sync_backend_list_cb cb, void *cb_data)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *ent;
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, rel_dir) != 0)
		return -EINVAL;
	dir = opendir(path);
	if (!dir)
		return errno == ENOENT ? 0 : -EIO;
	while ((ent = readdir(dir)) != nullptr) {
		char child[PATH_MAX];
		struct stat sb;
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		file_path_join(child, sizeof(child), path, ent->d_name);
		if (stat(child, &sb) != 0)
			continue;
		int rc = cb(ent->d_name, S_ISDIR(sb.st_mode), cb_data);
		if (rc != 0) {
			closedir(dir);
			return rc;
		}
	}
	closedir(dir);
	return 0;
}

static int test_backend_copy_from_local(void *user_data, const char *local_path,
					const char *remote_rel, int sqlite_source)
{
	(void)sqlite_source;
	char dst[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(dst, sizeof(dst), root, remote_rel) != 0)
		return -EINVAL;
	return test_copy_file(local_path, dst);
}

static int test_backend_copy_to_local(void *user_data, const char *remote_rel,
				      const char *local_path)
{
	char src[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(src, sizeof(src), root, remote_rel) != 0)
		return -EINVAL;
	return test_copy_file(src, local_path);
}

static int test_backend_delete(void *user_data, const char *remote_rel)
{
	char path[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, remote_rel) != 0)
		return -EINVAL;
	return unlink(path) == 0 || errno == ENOENT ? 0 : -errno;
}

static int test_backend_ensure_dir(void *user_data, const char *remote_rel_dir)
{
	char path[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, remote_rel_dir) != 0)
		return -EINVAL;
	return file_ensure_dir(path);
}

TEST_F(SyncTest, CopiesLocalFileToRemote) {
	struct morph_sync_status st;
	char dst[PATH_MAX];

	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "a=1\n", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(dst, sizeof(dst), "%s/data/config.toml", remote);
	char *text = file_read_all(dst, nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "a=1\n");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, CopiesLocalFileThroughRemoteBackend) {
	struct morph_sync_status st;
	struct morph_sync_backend backend = {
		.user_data = remote,
		.stat = test_backend_stat,
		.list = test_backend_list,
		.copy_from_local = test_backend_copy_from_local,
		.copy_to_local = test_backend_copy_to_local,
		.delete_file = test_backend_delete,
		.ensure_dir = test_backend_ensure_dir,
	};

	cfg.remote_backend = &backend;
	snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", state);
	cfg.include_count = 1;
	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "backend", 7), 0);
	int rc = morph_sync_once(&cfg, &st);
	ASSERT_EQ(rc, 0) << st.last_error;
	char *text = file_read_all((std::string(remote) + "/config.toml").c_str(),
				   nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "backend");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, CopiesRemoteBackendFileToLocal) {
	struct morph_sync_status st;
	struct morph_sync_backend backend = {
		.user_data = remote,
		.stat = test_backend_stat,
		.list = test_backend_list,
		.copy_from_local = test_backend_copy_from_local,
		.copy_to_local = test_backend_copy_to_local,
		.delete_file = test_backend_delete,
		.ensure_dir = test_backend_ensure_dir,
	};

	cfg.remote_backend = &backend;
	snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", state);
	ASSERT_EQ(file_write_all((std::string(remote) + "/config.toml").c_str(),
				 "remote-backend", 14), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text = file_read_all((std::string(local) + "/config.toml").c_str(),
				   nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "remote-backend");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, CopiesRemoteFileToLocal) {
	struct morph_sync_status st;
	char dir[PATH_MAX];
	char src[PATH_MAX];

	snprintf(dir, sizeof(dir), "%s/data/output", remote);
	ASSERT_EQ(file_ensure_dir(dir), 0);
	snprintf(src, sizeof(src), "%s/item.txt", dir);
	ASSERT_EQ(file_write_all(src, "remote", 6), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text = file_read_all((std::string(local) + "/output/item.txt").c_str(),
				   nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "remote");
	free(text);
}

TEST_F(SyncTest, KeepsBothCopiesOnConflict) {
	struct morph_sync_status st;
	char remote_file[PATH_MAX];

	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "base", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "local", 5), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(file_write_all(remote_file, "remote", 6), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_GE(st.conflicts, 1);
	char **files = nullptr;
	int count = 0;
	ASSERT_EQ(file_list_files(local, &files, &count), 0);
	int found = 0;
	for (int i = 0; i < count; i++) {
		if (strncmp(files[i], "config.toml.conflict", 20) == 0)
			found = 1;
	}
	file_free_list(files, count);
	EXPECT_EQ(found, 1);
}

TEST_F(SyncTest, SoftDeletesIntoTrash) {
	struct morph_sync_status st;
	char remote_file[PATH_MAX];
	char trash_dir[PATH_MAX];

	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "base", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(std::remove(remote_file), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_GE(st.recycled, 1);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM tombstones WHERE path='config.toml'"),
		  1);
	EXPECT_EQ(file_exists((std::string(local) + "/config.toml").c_str()), 0);
	snprintf(trash_dir, sizeof(trash_dir), "%s/.morph-sync/trash", remote);
	char **files = nullptr;
	int count = 0;
	ASSERT_EQ(file_list_files(trash_dir, &files, &count), 0);
	EXPECT_GE(count, 1);
	file_free_list(files, count);
}

TEST_F(SyncTest, DoesNotOverwriteLocalDatabaseFromRemote) {
	struct morph_sync_status st;
	char data_root[PATH_MAX];
	char remote_db[PATH_MAX];

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "data.db");
	cfg.include_count = 1;
	ASSERT_EQ(file_write_all((std::string(local) + "/data.db").c_str(),
				 "local-db", 8), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(data_root, sizeof(data_root), "%s/data", remote);
	snprintf(remote_db, sizeof(remote_db), "%s/data.db", data_root);
	ASSERT_EQ(file_write_all(remote_db, "remote-db", 9), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text = file_read_all((std::string(local) + "/data.db").c_str(),
				   nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "local-db");
	free(text);
	EXPECT_GE(st.conflicts, 1);
}

TEST_F(SyncTest, BacksUpAndRestoresLiveSqliteDatabase) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string restored = std::string(root) + "/restored.db";
	sqlite3 *writer = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(sqlite3_open(database.c_str(), &writer), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(writer,
		"INSERT INTO items(value) VALUES('wal-row')", nullptr, nullptr,
		nullptr), SQLITE_OK);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0) << st.last_error;
	EXPECT_EQ(st.conflicts, 0);
	EXPECT_EQ(st.db_snapshots, 1);
	EXPECT_GT(st.db_chunks_uploaded, 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(backups[0].path, "actual.db");
	ASSERT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		restored.c_str()), 0);
	EXPECT_EQ(SqliteCount(restored), 2);
	EXPECT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		restored.c_str()), -EEXIST);
	morph_sync_backups_free(backups);
	sqlite3_close(writer);
}

TEST_F(SyncTest, UnchangedSqliteDoesNotCreateAnotherSnapshot) {
	struct morph_sync_status first;
	struct morph_sync_status second;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database, 4);
	ASSERT_EQ(morph_sync_once(&cfg, &first), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &second), 0);
	EXPECT_EQ(first.db_snapshots, 1);
	EXPECT_EQ(second.db_snapshots, 0);
	EXPECT_EQ(second.db_bytes_uploaded, 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	EXPECT_EQ(count, 1);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, ChangedSqliteCreatesImmutableVersion) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "INSERT INTO items(value) VALUES('new')",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(st.db_snapshots, 1);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	EXPECT_EQ(count, 2);
	EXPECT_STRNE(backups[0].snapshot_id, backups[1].snapshot_id);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RevertedSqliteCreatesAnotherImmutableVersion) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string baseline = std::string(root) + "/baseline.db";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		baseline.c_str()), 0);
	morph_sync_backups_free(backups);
	backups = nullptr;

	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "INSERT INTO items(value) VALUES('new')",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(std::remove(database.c_str()), 0);
	ASSERT_EQ(std::rename(baseline.c_str(), database.c_str()), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(st.db_snapshots, 1);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	EXPECT_EQ(count, 3);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, TransactionalDatabaseReplaceCanCommitAndRollback) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	char original_snapshot[MORPH_SYNC_SNAPSHOT_ID_MAX];
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	snprintf(original_snapshot, sizeof(original_snapshot), "%s",
		 backups[0].snapshot_id);
	morph_sync_backups_free(backups);
	backups = nullptr;
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "INSERT INTO items(value) VALUES('new')",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 2);

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, original_snapshot,
		&plan), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_TRUE(file_exists(plan.staging));
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	EXPECT_TRUE(file_exists(plan.rollback));
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_FALSE(file_exists(plan.journal));

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, original_snapshot,
		&plan), 0);
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	ASSERT_EQ(morph_sync_commit_db_replace(&plan), 0);
	EXPECT_FALSE(file_exists(plan.rollback));
	EXPECT_FALSE(file_exists(plan.journal));
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RecoversPreparedAndFinalizesAppliedRestoreJournals) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	char *json = nullptr;
	struct morph_sync_restore_plan decoded;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	json = morph_sync_restore_plan_to_json(&plan);
	ASSERT_NE(json, nullptr);
	ASSERT_EQ(morph_sync_restore_plan_from_json(json, &decoded), 0);
	EXPECT_STREQ(decoded.target, plan.target);
	free(json);
	ASSERT_EQ(morph_sync_recover_db_replacements(local), 0);
	EXPECT_FALSE(file_exists(plan.staging));
	EXPECT_FALSE(file_exists(plan.journal));

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	ASSERT_EQ(morph_sync_recover_db_replacements(local), 0);
	EXPECT_TRUE(file_exists(plan.journal));
	ASSERT_EQ(morph_sync_finalize_db_replacements(local), 0);
	EXPECT_FALSE(file_exists(plan.rollback));
	EXPECT_FALSE(file_exists(plan.journal));
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, PrepareProtectsUncheckpointedLiveWalState) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	char original_snapshot[MORPH_SYNC_SNAPSHOT_ID_MAX];
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string protected_copy = std::string(root) + "/protected.db";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	snprintf(original_snapshot, sizeof(original_snapshot), "%s",
		 backups[0].snapshot_id);
	morph_sync_backups_free(backups);
	backups = nullptr;
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "PRAGMA wal_autocheckpoint=0;"
		"INSERT INTO items(value) VALUES('live-wal')",
		nullptr, nullptr, nullptr), SQLITE_OK);

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, original_snapshot,
		&plan), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 2);
	const char *protected_snapshot = strcmp(backups[0].snapshot_id,
		original_snapshot) == 0 ? backups[1].snapshot_id :
		backups[0].snapshot_id;
	ASSERT_EQ(morph_sync_restore_db(&cfg, protected_snapshot,
		protected_copy.c_str()), 0);
	EXPECT_EQ(SqliteCount(protected_copy), 2);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	sqlite3_close(db);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RecoversCrashAfterCurrentDatabaseWasMovedAside) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database, 2);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	ASSERT_EQ(std::rename(plan.target, plan.rollback), 0);
	char *text = file_read_all(plan.journal, nullptr);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "phase",
		cJSON_CreateString("applying")));
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(file_write_all(plan.journal, updated, strlen(updated)), 0);
	free(updated);
	cJSON_Delete(json);

	ASSERT_EQ(morph_sync_recover_db_replacements(local), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_FALSE(file_exists(plan.rollback));
	EXPECT_FALSE(file_exists(plan.journal));
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RejectsChangedStagingDatabaseBeforeReplacement) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	FILE *file;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database, 2);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	file = fopen(plan.staging, "ab");
	ASSERT_NE(file, nullptr);
	ASSERT_EQ(fwrite("x", 1, 1, file), 1u);
	ASSERT_EQ(fclose(file), 0);
	EXPECT_EQ(morph_sync_apply_db_replace(&plan), MORPH_ERR_FORMAT);
	EXPECT_EQ(SqliteCount(database), 2);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RejectsTamperedRestorePlanAndSnapshotPathTraversal) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	struct morph_sync_restore_plan tampered;
	char manifest[PATH_MAX];
	int count = 0;
	std::string database = std::string(local) + "/actual.db";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	tampered = plan;
	snprintf(tampered.target, sizeof(tampered.target), "%s", "/tmp/x.db");
	EXPECT_EQ(morph_sync_apply_db_replace(&tampered), MORPH_ERR_FORMAT);
	EXPECT_EQ(SqliteCount(database), 1);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);

	snprintf(manifest, sizeof(manifest),
		 "%s/.morph-sync/db/v1/snapshots/%s.json", remote,
		 backups[0].snapshot_id);
	char *text = file_read_all(manifest, nullptr);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "path",
		cJSON_CreateString("../outside.db")));
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(file_write_all(manifest, updated, strlen(updated)), 0);
	free(updated);
	cJSON_Delete(json);
	EXPECT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), MORPH_ERR_FORMAT);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, IgnoresUntrustedRecoveryJournalWithoutTouchingTarget) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_restore_plan plan;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	char *original;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	original = file_read_all(plan.journal, nullptr);
	ASSERT_NE(original, nullptr);
	cJSON *json = cJSON_Parse(original);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "target",
		cJSON_CreateString("/tmp/untrusted.db")));
	char *tampered = cJSON_PrintUnformatted(json);
	ASSERT_NE(tampered, nullptr);
	ASSERT_EQ(file_write_all(plan.journal, tampered, strlen(tampered)), 0);
	free(tampered);
	cJSON_Delete(json);
	ASSERT_EQ(morph_sync_recover_db_replacements(local), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	EXPECT_TRUE(file_exists(plan.journal));
	ASSERT_EQ(file_write_all(plan.journal, original, strlen(original)), 0);
	free(original);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RejectsCorruptDatabaseChunkWithoutPublishingRestore) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	char object_root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char object_path[PATH_MAX];
	char **prefixes = nullptr;
	char **objects = nullptr;
	int prefix_count = 0;
	int object_count = 0;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string restored = std::string(root) + "/corrupt-restore.db";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	ASSERT_EQ(count, 1);
	snprintf(object_root, sizeof(object_root), "%s/.morph-sync/db/v1/objects",
		 remote);
	ASSERT_EQ(file_list_dirs(object_root, &prefixes, &prefix_count), 0);
	ASSERT_GT(prefix_count, 0);
	snprintf(prefix_path, sizeof(prefix_path), "%s/%s", object_root,
		 prefixes[0]);
	ASSERT_EQ(file_list_files(prefix_path, &objects, &object_count), 0);
	ASSERT_GT(object_count, 0);
	snprintf(object_path, sizeof(object_path), "%s/%s", prefix_path,
		 objects[0]);
	ASSERT_EQ(file_write_all(object_path, "corrupt", 7), 0);
	EXPECT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		restored.c_str()), MORPH_ERR_FORMAT);
	EXPECT_EQ(file_exists(restored.c_str()), 0);
	file_free_list(objects, object_count);
	file_free_list(prefixes, prefix_count);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, BacksUpSqliteThroughRemoteBackend) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	struct morph_sync_backend backend = {
		.user_data = remote,
		.stat = test_backend_stat,
		.list = test_backend_list,
		.copy_from_local = test_backend_copy_from_local,
		.copy_to_local = test_backend_copy_to_local,
		.delete_file = test_backend_delete,
		.ensure_dir = test_backend_ensure_dir,
	};
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string restored = std::string(root) + "/backend-restored.db";

	cfg.remote_backend = &backend;
	snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", state);
	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database, 3);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0) << st.last_error;
	EXPECT_EQ(st.db_snapshots, 1);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		restored.c_str()), 0);
	EXPECT_EQ(SqliteCount(restored), 3);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, DifferentDevicesPublishIndependentSqliteVersions) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string device_file = std::string(local) + "/.morph-sync/device-id";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(unlink(device_file.c_str()), 0);
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "INSERT INTO items(value) VALUES('device-2')",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(st.conflicts, 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 2);
	EXPECT_STRNE(backups[0].device_id, backups[1].device_id);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, ImportsLegacySqliteConflictCopiesAsBackupVersions) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	std::string conflict = database + ".conflict.legacy";

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	CreateSqlite(conflict, 2);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(st.conflicts, 0);
	EXPECT_EQ(st.db_snapshots, 2);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	EXPECT_EQ(count, 2);
	EXPECT_EQ(file_exists(conflict.c_str()), 1);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RetentionKeepsLatestSnapshotAndCollectsOldChunks) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	char manifest[PATH_MAX];
	char object_root[PATH_MAX];
	char **prefixes = nullptr;
	int prefix_count = 0;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	sqlite3 *db = nullptr;

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	cfg.retention_days = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db, "INSERT INTO items(value) VALUES('new')",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	ASSERT_EQ(count, 2);
	snprintf(manifest, sizeof(manifest),
		 "%s/.morph-sync/db/v1/snapshots/%s.json", remote,
		 backups[1].snapshot_id);
	char *text = file_read_all(manifest, nullptr);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	cJSON_SetNumberValue(cJSON_GetObjectItem(json, "created_at"), 1);
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(file_write_all(manifest, updated, strlen(updated)), 0);
	free(updated);
	cJSON_Delete(json);
	morph_sync_backups_free(backups);
	backups = nullptr;

	snprintf(object_root, sizeof(object_root), "%s/.morph-sync/db/v1/objects",
		 remote);
	ASSERT_EQ(file_list_dirs(object_root, &prefixes, &prefix_count), 0);
	for (int i = 0; i < prefix_count; i++) {
		char directory[PATH_MAX];
		char **objects = nullptr;
		int object_count = 0;
		snprintf(directory, sizeof(directory), "%s/%s", object_root,
			 prefixes[i]);
		ASSERT_EQ(file_list_files(directory, &objects, &object_count), 0);
		for (int j = 0; j < object_count; j++) {
			char object[PATH_MAX];
			struct utimbuf old_time = { 1, 1 };
			snprintf(object, sizeof(object), "%s/%s", directory,
				 objects[j]);
			ASSERT_EQ(utime(object, &old_time), 0);
		}
		file_free_list(objects, object_count);
	}
	file_free_list(prefixes, prefix_count);

	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	EXPECT_EQ(count, 1);
	morph_sync_backups_free(backups);
	prefixes = nullptr;
	prefix_count = 0;
	int remaining_objects = 0;
	ASSERT_EQ(file_list_dirs(object_root, &prefixes, &prefix_count), 0);
	for (int i = 0; i < prefix_count; i++) {
		char directory[PATH_MAX];
		char **objects = nullptr;
		int object_count = 0;
		snprintf(directory, sizeof(directory), "%s/%s", object_root,
			 prefixes[i]);
		ASSERT_EQ(file_list_files(directory, &objects, &object_count), 0);
		remaining_objects += object_count;
		file_free_list(objects, object_count);
	}
	file_free_list(prefixes, prefix_count);
	EXPECT_EQ(remaining_objects, 1);
}

TEST_F(SyncTest, RestoreTrashClearsTombstone) {
	struct morph_sync_status st;
	char remote_file[PATH_MAX];

	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "base", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(std::remove(remote_file), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	int trash_id = QueryInt("SELECT id FROM trash ORDER BY id DESC LIMIT 1");
	ASSERT_GT(trash_id, 0);
	ASSERT_EQ(morph_sync_restore_trash(&cfg, trash_id), 0);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM trash"), 0);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM tombstones WHERE path='config.toml'"),
		  0);
	EXPECT_EQ(QueryInt("SELECT tombstone FROM entries WHERE path='config.toml'"),
		  0);
	char *text = file_read_all((std::string(local) + "/config.toml").c_str(),
				   nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "base");
	free(text);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(file_exists((std::string(local) + "/config.toml").c_str()), 1);
}

TEST_F(SyncTest, CleansExpiredTrash) {
	struct morph_sync_status st;
	sqlite3 *db = nullptr;
	char remote_file[PATH_MAX];
	char trash_path[PATH_MAX];

	cfg.retention_days = 1;
	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "base", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(std::remove(remote_file), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_GT(QueryInt("SELECT COUNT(*) FROM trash"), 0);
	ASSERT_EQ(OpenManifest(&db), SQLITE_OK);
	ASSERT_EQ(sqlite3_exec(db,
		"UPDATE trash SET created_at=strftime('%s','now')-172800",
		nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db,
		"SELECT trash_path FROM trash LIMIT 1", -1, &stmt, nullptr),
		SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	snprintf(trash_path, sizeof(trash_path), "%s",
		 (const char *)sqlite3_column_text(stmt, 0));
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	ASSERT_EQ(file_exists(trash_path), 1);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM trash"), 0);
	EXPECT_EQ(file_exists(trash_path), 0);
}

TEST_F(SyncTest, ManifestStoresSizeAndMtime) {
	struct morph_sync_status st;

	ASSERT_EQ(file_write_all((std::string(local) + "/config.toml").c_str(),
				 "abcd", 4), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(QueryInt("SELECT local_size FROM entries WHERE path='config.toml'"),
		  4);
	EXPECT_EQ(QueryInt("SELECT remote_size FROM entries WHERE path='config.toml'"),
		  4);
	EXPECT_GT(QueryInt("SELECT local_mtime FROM entries WHERE path='config.toml'"),
		  0);
	EXPECT_GT(QueryInt("SELECT remote_mtime FROM entries WHERE path='config.toml'"),
		  0);
	EXPECT_GT(QueryInt("SELECT length(last_device) FROM entries WHERE path='config.toml'"),
		  0);
}
