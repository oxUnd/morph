#include <gtest/gtest.h>
#include "sync/sync.h"
#include "util/file.h"
#include "blake3.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>

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
