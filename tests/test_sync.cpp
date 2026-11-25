#include <gtest/gtest.h>
#include <limits.h>
#include "sync/sync.h"
#include <nb/file.h>
#include "errors.h"
#include <nb/cleanup.h>
#include "blake3.h"
#include "cJSON.h"
#include "persistence/orm/test_fixture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <utime.h>

extern "C" {
#include "sync/db_backup.h"
}

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
		nb_file_mkdir(local, true, NULL);
		nb_file_mkdir(remote, true, NULL);
		nb_file_mkdir(state, true, NULL);
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

	int OpenManifest(struct db *database) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/.morph-sync/manifest.db", remote);
		return db_open_plain(nullptr, database, path);
	}

	int QueryInt(const char *sql) {
		struct db database NB_CLEANUP(db_close) = {};
		int64_t value = -1;

		if (OpenManifest(&database) != 0)
			return -1;

		return orm_fixture_integer(&database, sql, &value) == 0 ? (int)value : -1;
	}

	void CreateSqlite(const std::string &path, int rows = 1) {
		struct db database NB_CLEANUP(db_close) = {};

		ASSERT_EQ(db_open_plain(nullptr, &database, path.c_str()), 0);
		ASSERT_EQ(orm_fixture_execute(&database, "PRAGMA journal_mode=WAL;"
			"CREATE TABLE items(id INTEGER PRIMARY KEY,value TEXT);"), 0);
		for (int i = 0; i < rows; i++) {
			std::string sql = "INSERT INTO items(value) VALUES('item-" +
				std::to_string(i) + "')";

			ASSERT_EQ(orm_fixture_execute(&database, sql.c_str()), 0);
		}
	}

	int SqliteCount(const std::string &path) {
		struct db database NB_CLEANUP(db_close) = {};
		int64_t count = -1;

		if (db_open_readonly(nullptr, &database, path.c_str()) != 0)
			return -1;

		return orm_fixture_integer(&database, "SELECT COUNT(*) FROM items", &count) == 0
			? (int)count : -1;
	}

};

static int test_path_join(char *dst, size_t size, const char *root,
			  const char *rel)
{
	nb_buf destination_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int destination_path_status = NB_OK;

	return ((destination_path_status = nb_path_concat(&destination_path_buffer, root,
							  rel && rel[0] ? rel : ".")) != NB_OK
			? destination_path_status
		: (destination_path_status = nb_str_copy_to(nb_buf_str(&destination_path_buffer),
							    dst, size)) == NB_ERR_RANGE
			? NB_ERR_NAME_TOO_LONG
			: destination_path_status);
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
		return nb_status_from_errno(errno, NULL);
	snprintf(parent, sizeof(parent), "%s", dst);
	slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		if (parent[0]) {
			int rc = nb_file_mkdir(parent, true, NULL);
			if (rc != 0) {
				fclose(in);
				return rc;
			}
		}
	}
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return nb_status_from_errno(errno, NULL);
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			fclose(in);
			fclose(out);
			return NB_ERR_IO;
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
		return NB_ERR_INVAL;
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
	nb_buf child_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int child_path_status = NB_OK;

	char path[PATH_MAX];
	DIR *dir;
	struct dirent *ent;
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, rel_dir) != 0)
		return NB_ERR_INVAL;
	dir = opendir(path);
	if (!dir)
		return errno == ENOENT ? 0 : NB_ERR_IO;
	while ((ent = readdir(dir)) != nullptr) {
		char child[PATH_MAX];
		struct stat sb;
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		(void)((child_path_status =
				nb_path_concat(&child_path_buffer, path, ent->d_name)) != NB_OK
			       ? child_path_status
		       : (child_path_status = nb_str_copy_to(nb_buf_str(&child_path_buffer), child,
							     sizeof(child))) == NB_ERR_RANGE
			       ? NB_ERR_NAME_TOO_LONG
			       : child_path_status);
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
		return NB_ERR_INVAL;
	return test_copy_file(local_path, dst);
}

static int test_backend_copy_to_local(void *user_data, const char *remote_rel,
				      const char *local_path)
{
	char src[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(src, sizeof(src), root, remote_rel) != 0)
		return NB_ERR_INVAL;
	return test_copy_file(src, local_path);
}

static int test_backend_delete(void *user_data, const char *remote_rel)
{
	char path[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, remote_rel) != 0)
		return NB_ERR_INVAL;
	return unlink(path) == 0 || errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
}

static int test_backend_ensure_dir(void *user_data, const char *remote_rel_dir)
{
	char path[PATH_MAX];
	const char *root = static_cast<const char *>(user_data);
	if (test_path_join(path, sizeof(path), root, remote_rel_dir) != 0)
		return NB_ERR_INVAL;
	return nb_file_mkdir(path, true, NULL);
}

TEST_F(SyncTest, CopiesLocalFileToRemote) {
	nb_buf copied_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *copied_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct morph_sync_status st;
	char dst[PATH_MAX];

	ASSERT_EQ(
		nb_file_write_all((std::string(local) + "/config.toml").c_str(), "a=1\n", 4, NULL),
		0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(dst, sizeof(dst), "%s/data/config.toml", remote);
	char *text = (nb_file_read_all(dst, &copied_contents_buffer, 0, NULL) == NB_OK &&
				      (copied_contents_copy = (char *)malloc(
					       nb_buf_len(&copied_contents_buffer) + 1)) != NULL
			      ? (memcpy(copied_contents_copy, nb_buf_cstr(&copied_contents_buffer),
					nb_buf_len(&copied_contents_buffer) + 1),
				 nb_charp_steal(&copied_contents_copy))
			      : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "a=1\n");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, CopiesLocalFileThroughRemoteBackend) {
	nb_buf remote_config_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *remote_config_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "backend", 7,
				    NULL),
		  0);
	int rc = morph_sync_once(&cfg, &st);
	ASSERT_EQ(rc, 0) << st.last_error;
	char *text =
		(nb_file_read_all((std::string(remote) + "/config.toml").c_str(),
				  &remote_config_contents_buffer, 0, NULL) == NB_OK &&
				 (remote_config_contents_copy = (char *)malloc(
					  nb_buf_len(&remote_config_contents_buffer) + 1)) != NULL
			 ? (memcpy(remote_config_contents_copy,
				   nb_buf_cstr(&remote_config_contents_buffer),
				   nb_buf_len(&remote_config_contents_buffer) + 1),
			    nb_charp_steal(&remote_config_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "backend");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, CopiesRemoteBackendFileToLocal) {
	nb_buf local_config_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *local_config_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
	ASSERT_EQ(nb_file_write_all((std::string(remote) + "/config.toml").c_str(),
				    "remote-backend", 14, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text =
		(nb_file_read_all((std::string(local) + "/config.toml").c_str(),
				  &local_config_contents_buffer, 0, NULL) == NB_OK &&
				 (local_config_contents_copy = (char *)malloc(
					  nb_buf_len(&local_config_contents_buffer) + 1)) != NULL
			 ? (memcpy(local_config_contents_copy,
				   nb_buf_cstr(&local_config_contents_buffer),
				   nb_buf_len(&local_config_contents_buffer) + 1),
			    nb_charp_steal(&local_config_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "remote-backend");
	free(text);
	EXPECT_EQ(st.copied, 1);
}

TEST_F(SyncTest, RemoteReadFailureDoesNotBecomeADeletion) {
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
	std::string source = std::string(local) + "/config.toml";
	ASSERT_EQ(nb_file_write_all(source.c_str(), "keep", 4, NULL), 0);
	struct morph_sync_status status{};
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0);
	backend.stat = [](void *, const char *, struct morph_sync_backend_stat *) -> int {
		return NB_ERR_PERMISSION;
	};
	EXPECT_EQ(morph_sync_once(&cfg, &status), NB_ERR_PERMISSION);
	EXPECT_EQ(status.error_code, NB_ERR_PERMISSION);
	EXPECT_EQ(status.deleted, 0);
	EXPECT_TRUE(nb_file_exists(source.c_str()));
}

TEST_F(SyncTest, FailedSnapshotPublishIsNotReportedAsSuccess) {
	struct morph_sync_backend backend = {
		.user_data = remote,
		.stat = test_backend_stat,
		.list = test_backend_list,
		.copy_from_local = [](void *, const char *, const char *, int) -> int {
			return NB_ERR_IO;
		},
		.copy_to_local = test_backend_copy_to_local,
		.delete_file = test_backend_delete,
		.ensure_dir = test_backend_ensure_dir,
	};
	cfg.remote_backend = &backend;
	snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", state);
	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(std::string(local) + "/actual.db");
	struct morph_sync_status status{};
	EXPECT_EQ(morph_sync_once(&cfg, &status), NB_ERR_IO);
	EXPECT_EQ(status.db_snapshots, 0);
	EXPECT_EQ(status.error_code, NB_ERR_IO);
}

TEST_F(SyncTest, BackupPathOverflowPreservesExistingTruncatedPrefix) {
	nb_buf remote_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	nb_buf existing_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *existing_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	const std::string staging_suffix = "/.morph-sync/staging";
	const size_t directory_size = static_cast<size_t>(PATH_MAX) - 3;
	const size_t root_size = directory_size - staging_suffix.size();
	char *canonical_remote NB_CLEANUP(nb_charp_cleanup) =
		(nb_path_expand(&remote_path_buffer, remote, NULL) == NB_OK &&
				 nb_path_canonical_missing(&remote_path_buffer,
							   nb_buf_cstr(&remote_path_buffer),
							   NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&remote_path_buffer))
			 : NULL);
	ASSERT_NE(canonical_remote, nullptr);
	std::string deep_root = canonical_remote;
	while (deep_root.size() + 65 < root_size)
		deep_root += "/" + std::string(64, 'x');
	const size_t remaining = root_size - deep_root.size();
	if (remaining > 1)
		deep_root += "/" + std::string(remaining - 1, 'x');
	else if (remaining == 1)
		deep_root += "x";
	const std::string staging = deep_root + staging_suffix;
	ASSERT_EQ(staging.size(), directory_size);
	ASSERT_EQ(nb_file_mkdir(staging.c_str(), true, NULL), 0);
	/* A truncated random filename retains only its first prefix character. */
	const std::string existing = staging + "/d";
	ASSERT_EQ(nb_file_write_all(existing.c_str(), "keep", 4, NULL), 0);
	ASSERT_LT(deep_root.size(), sizeof(cfg.sync_dir));
	memcpy(cfg.sync_dir, deep_root.c_str(), deep_root.size() + 1);
	struct morph_sync_status status{};
	EXPECT_EQ(sync_db_backup_create_one(&cfg, "data.db", "unused.db", &status, 1),
		  NB_ERR_NAME_TOO_LONG);
	char *text NB_CLEANUP(nb_charp_cleanup) =
		(nb_file_read_all(existing.c_str(), &existing_contents_buffer, 0, NULL) == NB_OK &&
				 (existing_contents_copy = (char *)malloc(
					  nb_buf_len(&existing_contents_buffer) + 1)) != NULL
			 ? (memcpy(existing_contents_copy, nb_buf_cstr(&existing_contents_buffer),
				   nb_buf_len(&existing_contents_buffer) + 1),
			    nb_charp_steal(&existing_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "keep");
	EXPECT_EQ(status.db_snapshots, 0);
}

TEST_F(SyncTest, CopiesRemoteFileToLocal) {
	nb_buf output_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *output_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct morph_sync_status st;
	char dir[PATH_MAX];
	char src[PATH_MAX];

	snprintf(dir, sizeof(dir), "%s/data/output", remote);
	ASSERT_EQ(nb_file_mkdir(dir, true, NULL), 0);
	snprintf(src, sizeof(src), "%s/item.txt", dir);
	ASSERT_EQ(nb_file_write_all(src, "remote", 6, NULL), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text = (nb_file_read_all((std::string(local) + "/output/item.txt").c_str(),
				       &output_contents_buffer, 0, NULL) == NB_OK &&
				      (output_contents_copy = (char *)malloc(
					       nb_buf_len(&output_contents_buffer) + 1)) != NULL
			      ? (memcpy(output_contents_copy, nb_buf_cstr(&output_contents_buffer),
					nb_buf_len(&output_contents_buffer) + 1),
				 nb_charp_steal(&output_contents_copy))
			      : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "remote");
	free(text);
}

TEST_F(SyncTest, KeepsBothCopiesOnConflict) {
	int directory_scan_status = NB_OK;

	struct morph_sync_status st;
	char remote_file[PATH_MAX];

	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "base", 4, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(
		nb_file_write_all((std::string(local) + "/config.toml").c_str(), "local", 5, NULL),
		0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(nb_file_write_all(remote_file, "remote", 6, NULL), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_GE(st.conflicts, 1);
	nb_dir_listing files NB_CLEANUP(nb_dir_listing_destroy) = {};
	int count = 0;
	ASSERT_EQ(((directory_scan_status = nb_dir_scan(
			    local, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
			    &files, NULL)) != NB_OK
			   ? directory_scan_status
		   : files.names.len > INT_MAX ? NB_ERR_RANGE
					       : (count = (int)files.names.len, NB_OK)),
		  0);
	int found = 0;
	for (int i = 0; i < count; i++) {
		if (strncmp(nb_dir_listing_name(&files, (size_t)(i)), "config.toml.conflict", 20) ==
		    0)
			found = 1;
	}
	nb_dir_listing_destroy(&files);
	EXPECT_EQ(found, 1);
}

TEST_F(SyncTest, SoftDeletesIntoTrash) {
	int directory_scan_status = NB_OK;

	struct morph_sync_status st;
	char remote_file[PATH_MAX];
	char trash_dir[PATH_MAX];

	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "base", 4, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(std::remove(remote_file), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_GE(st.recycled, 1);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM tombstones WHERE path='config.toml'"),
		  1);
	EXPECT_EQ(nb_file_exists((std::string(local) + "/config.toml").c_str()), 0);
	snprintf(trash_dir, sizeof(trash_dir), "%s/.morph-sync/trash", remote);
	nb_dir_listing files NB_CLEANUP(nb_dir_listing_destroy) = {};
	int count = 0;
	ASSERT_EQ(((directory_scan_status = nb_dir_scan(trash_dir,
							NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
								NB_DIR_SCAN_FOLLOW_LINKS,
							&files, NULL)) != NB_OK
			   ? directory_scan_status
		   : files.names.len > INT_MAX ? NB_ERR_RANGE
					       : (count = (int)files.names.len, NB_OK)),
		  0);
	EXPECT_GE(count, 1);
	nb_dir_listing_destroy(&files);
}

TEST_F(SyncTest, DoesNotOverwriteLocalDatabaseFromRemote) {
	nb_buf database_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *database_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct morph_sync_status st;
	char data_root[PATH_MAX];
	char remote_db[PATH_MAX];

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "data.db");
	cfg.include_count = 1;
	ASSERT_EQ(nb_file_write_all((std::string(local) + "/data.db").c_str(), "local-db", 8, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(data_root, sizeof(data_root), "%s/data", remote);
	snprintf(remote_db, sizeof(remote_db), "%s/data.db", data_root);
	ASSERT_EQ(nb_file_write_all(remote_db, "remote-db", 9, NULL), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	char *text =
		(nb_file_read_all((std::string(local) + "/data.db").c_str(),
				  &database_contents_buffer, 0, NULL) == NB_OK &&
				 (database_contents_copy = (char *)malloc(
					  nb_buf_len(&database_contents_buffer) + 1)) != NULL
			 ? (memcpy(database_contents_copy, nb_buf_cstr(&database_contents_buffer),
				   nb_buf_len(&database_contents_buffer) + 1),
			    nb_charp_steal(&database_contents_copy))
			 : NULL);
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
	struct db writer NB_CLEANUP(db_close) = {};

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(db_open_plain(nullptr, &writer, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&writer, "INSERT INTO items(value) VALUES('wal-row')"), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0) << st.last_error;
	EXPECT_EQ(st.conflicts, 0);
	EXPECT_EQ(st.db_snapshots, 1);
	EXPECT_GT(st.db_chunks_uploaded, 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(backups[0].path, "actual.db");
	const std::string unrelated = restored + ".restore.tmp." + std::to_string(getpid());
	const char *sentinel = "keep existing staging name";
	ASSERT_EQ(nb_file_write_all(unrelated.c_str(), sentinel, strlen(sentinel), nullptr), NB_OK);
	ASSERT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id,
		restored.c_str()), 0);
	EXPECT_EQ(SqliteCount(restored), 2);
	nb_buf retained NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(nb_file_read_all(unrelated.c_str(), &retained, 0, nullptr), NB_OK);
	EXPECT_TRUE(nb_str_eq_cstr(nb_buf_str(&retained), sentinel));
	EXPECT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id, restored.c_str()),
		  NB_ERR_EXISTS);
	morph_sync_backups_free(backups);
	db_close(&writer);
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

TEST_F(SyncTest, CoreSnapshotIncludesDisplayHistoryFromWalAndIsIdempotent) {
	const std::string database = std::string(local) + "/data.db";
	const std::string ui = std::string(local) + "/ui-history.db";
	const std::string restored = std::string(root) + "/restored.db";
	CreateSqlite(database);
	struct db core NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_open_plain(nullptr, &core, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&core, "CREATE TABLE sessions(id INTEGER PRIMARY KEY, display_id TEXT);"
		"INSERT INTO sessions VALUES(1,'session-a');"
		"CREATE TABLE sync_ui_messages(session_id,turn_id,core_message_id,seq,type,content,"
		"attachments_json,structured_data,agent_ui_ir,hitl_verdict,created_at,updated_at);"
		"INSERT INTO sync_ui_messages VALUES(2,'imported',NULL,0,'thought','preserved',"
		"NULL,NULL,NULL,NULL,1000,1000)"), 0);
	db_close(&core);
	struct db writer NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_open_plain(nullptr, &writer, ui.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&writer, "PRAGMA journal_mode=WAL;"
		"CREATE TABLE ui_session_identity(session_id INTEGER PRIMARY KEY, identity TEXT);"
		"INSERT INTO ui_session_identity VALUES(1,'session-a');"
		"CREATE TABLE ui_messages(session_id,turn_id,core_message_id,seq,type,content,"
		"attachments_json,structured_data,agent_ui_ir,hitl_verdict,created_at,updated_at);"
		"INSERT INTO ui_messages VALUES(1,'turn',10,0,'USER','question',"
		"NULL,NULL,NULL,NULL,1000,1000)"), 0);
	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "data.db");
	cfg.include_count = 1;
	struct morph_sync_status status{};
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0) << status.last_error;
	EXPECT_EQ(status.db_snapshots, 1);
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0) << status.last_error;
	EXPECT_EQ(status.db_snapshots, 0);
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	ASSERT_EQ(morph_sync_backups(&cfg, "data.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id, restored.c_str()), 0);
	morph_sync_backups_free(backups);
	struct db copy NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_open_plain(nullptr, &copy, restored.c_str()), 0);
	char *text NB_CLEANUP(nb_charp_cleanup) = nullptr;
	size_t stored_count = 0;

	ASSERT_EQ(orm_fixture_row_count(&copy,
		"SELECT type,content FROM sync_ui_messages ORDER BY session_id", &stored_count), 0);
	EXPECT_EQ(stored_count, 2u);
	ASSERT_EQ(orm_fixture_text_copy(&copy,
		"SELECT type FROM sync_ui_messages ORDER BY session_id LIMIT 1", &text), 0);
	EXPECT_STREQ(text, "USER");
	free(text);
	text = nullptr;
	ASSERT_EQ(orm_fixture_text_copy(&copy,
		"SELECT content FROM sync_ui_messages ORDER BY session_id LIMIT 1", &text), 0);
	EXPECT_STREQ(text, "question");
	free(text);
	text = nullptr;
	ASSERT_EQ(orm_fixture_text_copy(&copy,
		"SELECT content FROM sync_ui_messages ORDER BY session_id LIMIT 1 OFFSET 1", &text), 0);
	EXPECT_STREQ(text, "preserved");
	db_close(&copy);
	db_close(&writer);
}

TEST_F(SyncTest, ExternalOutputDirectoryUsesPortableOutputNamespace) {
	std::string output = std::string(root) + "/workspace/generated";
	ASSERT_EQ(nb_file_mkdir(output.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all((output + "/picture.png").c_str(), "image", 5, NULL), 0);
	snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", output.c_str());
	struct morph_sync_status status{};
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0) << status.last_error;
	EXPECT_TRUE(nb_file_exists((std::string(remote) + "/data/output/picture.png").c_str()));
	EXPECT_FALSE(nb_file_exists((std::string(local) + "/output/picture.png").c_str()));
	ASSERT_EQ(nb_file_write_all((std::string(remote) + "/data/output/new.png").c_str(), "new",
				    3, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0) << status.last_error;
	EXPECT_TRUE(nb_file_exists((output + "/new.png").c_str()));
	EXPECT_FALSE(nb_file_exists((std::string(local) + "/output/new.png").c_str()));
}

TEST_F(SyncTest, ChangedSqliteCreatesImmutableVersion) {
	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	struct db db NB_CLEANUP(db_close) = {};

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "INSERT INTO items(value) VALUES('new')"), 0);
	db_close(&db);
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
	struct db db NB_CLEANUP(db_close) = {};

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

	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "INSERT INTO items(value) VALUES('new')"), 0);
	db_close(&db);
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
	struct db db NB_CLEANUP(db_close) = {};

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
	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "INSERT INTO items(value) VALUES('new')"), 0);
	db_close(&db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, "actual.db", &backups, &count), 0);
	ASSERT_EQ(count, 2);

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, original_snapshot,
		&plan), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_TRUE(nb_file_exists(plan.staging));
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	EXPECT_TRUE(nb_file_exists(plan.rollback));
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_FALSE(nb_file_exists(plan.journal));

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, original_snapshot,
		&plan), 0);
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	ASSERT_EQ(morph_sync_commit_db_replace(&plan), 0);
	EXPECT_FALSE(nb_file_exists(plan.rollback));
	EXPECT_FALSE(nb_file_exists(plan.journal));
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
	ASSERT_EQ(morph_sync_recover_db_replacements(NULL, local), 0);
	EXPECT_FALSE(nb_file_exists(plan.staging));
	EXPECT_FALSE(nb_file_exists(plan.journal));

	ASSERT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id,
		&plan), 0);
	ASSERT_EQ(morph_sync_apply_db_replace(&plan), 0);
	ASSERT_EQ(morph_sync_recover_db_replacements(NULL, local), 0);
	EXPECT_TRUE(nb_file_exists(plan.journal));
	ASSERT_EQ(morph_sync_finalize_db_replacements(NULL, local), 0);
	EXPECT_FALSE(nb_file_exists(plan.rollback));
	EXPECT_FALSE(nb_file_exists(plan.journal));
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
	struct db db NB_CLEANUP(db_close) = {};

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
	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "PRAGMA wal_autocheckpoint=0;"
		"INSERT INTO items(value) VALUES('live-wal')"), 0);

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
	db_close(&db);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RecoversCrashAfterCurrentDatabaseWasMovedAside) {
	nb_buf journal_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *journal_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
	char *text =
		(nb_file_read_all(plan.journal, &journal_contents_buffer, 0, NULL) == NB_OK &&
				 (journal_contents_copy = (char *)malloc(
					  nb_buf_len(&journal_contents_buffer) + 1)) != NULL
			 ? (memcpy(journal_contents_copy, nb_buf_cstr(&journal_contents_buffer),
				   nb_buf_len(&journal_contents_buffer) + 1),
			    nb_charp_steal(&journal_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "phase",
		cJSON_CreateString("applying")));
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(nb_file_write_all(plan.journal, updated, strlen(updated), NULL), 0);
	free(updated);
	cJSON_Delete(json);

	ASSERT_EQ(morph_sync_recover_db_replacements(NULL, local), 0);
	EXPECT_EQ(SqliteCount(database), 2);
	EXPECT_FALSE(nb_file_exists(plan.rollback));
	EXPECT_FALSE(nb_file_exists(plan.journal));
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
	EXPECT_EQ(morph_sync_apply_db_replace(&plan), NB_ERR_FORMAT);
	EXPECT_EQ(SqliteCount(database), 2);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RejectsTamperedRestorePlanAndSnapshotPathTraversal) {
	nb_buf manifest_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *manifest_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
	EXPECT_EQ(morph_sync_apply_db_replace(&tampered), NB_ERR_FORMAT);
	EXPECT_EQ(SqliteCount(database), 1);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);

	snprintf(manifest, sizeof(manifest),
		 "%s/.morph-sync/db/v1/snapshots/%s.json", remote,
		 backups[0].snapshot_id);
	char *text =
		(nb_file_read_all(manifest, &manifest_contents_buffer, 0, NULL) == NB_OK &&
				 (manifest_contents_copy = (char *)malloc(
					  nb_buf_len(&manifest_contents_buffer) + 1)) != NULL
			 ? (memcpy(manifest_contents_copy, nb_buf_cstr(&manifest_contents_buffer),
				   nb_buf_len(&manifest_contents_buffer) + 1),
			    nb_charp_steal(&manifest_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "path",
		cJSON_CreateString("../outside.db")));
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(nb_file_write_all(manifest, updated, strlen(updated), NULL), 0);
	free(updated);
	cJSON_Delete(json);
	EXPECT_EQ(morph_sync_prepare_db_replace(&cfg, backups[0].snapshot_id, &plan),
		  NB_ERR_FORMAT);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, IgnoresUntrustedRecoveryJournalWithoutTouchingTarget) {
	nb_buf original_journal_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *original_journal_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

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
	original = (nb_file_read_all(plan.journal, &original_journal_contents_buffer, 0, NULL) ==
					    NB_OK &&
				    (original_journal_contents_copy = (char *)malloc(
					     nb_buf_len(&original_journal_contents_buffer) + 1)) !=
					    NULL
			    ? (memcpy(original_journal_contents_copy,
				      nb_buf_cstr(&original_journal_contents_buffer),
				      nb_buf_len(&original_journal_contents_buffer) + 1),
			       nb_charp_steal(&original_journal_contents_copy))
			    : NULL);
	ASSERT_NE(original, nullptr);
	cJSON *json = cJSON_Parse(original);
	ASSERT_NE(json, nullptr);
	ASSERT_TRUE(cJSON_ReplaceItemInObject(json, "target",
		cJSON_CreateString("/tmp/untrusted.db")));
	char *tampered = cJSON_PrintUnformatted(json);
	ASSERT_NE(tampered, nullptr);
	ASSERT_EQ(nb_file_write_all(plan.journal, tampered, strlen(tampered), NULL), 0);
	free(tampered);
	cJSON_Delete(json);
	ASSERT_EQ(morph_sync_recover_db_replacements(NULL, local), 0);
	EXPECT_EQ(SqliteCount(database), 1);
	EXPECT_TRUE(nb_file_exists(plan.journal));
	ASSERT_EQ(nb_file_write_all(plan.journal, original, strlen(original), NULL), 0);
	free(original);
	ASSERT_EQ(morph_sync_rollback_db_replace(&plan), 0);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RejectsCorruptDatabaseChunkWithoutPublishingRestore) {
	int directory_scan_status = NB_OK;
	int object_scan_status = NB_OK;

	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	char object_root[PATH_MAX];
	char prefix_path[PATH_MAX];
	char object_path[PATH_MAX];
	nb_dir_listing prefixes NB_CLEANUP(nb_dir_listing_destroy) = {};
	nb_dir_listing objects NB_CLEANUP(nb_dir_listing_destroy) = {};
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
	ASSERT_EQ(((directory_scan_status = nb_dir_scan(object_root,
							NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
								NB_DIR_SCAN_FOLLOW_LINKS,
							&prefixes, NULL)) != NB_OK
			   ? directory_scan_status
		   : prefixes.names.len > INT_MAX
			   ? NB_ERR_RANGE
			   : (prefix_count = (int)prefixes.names.len, NB_OK)),
		  0);
	ASSERT_GT(prefix_count, 0);
	snprintf(prefix_path, sizeof(prefix_path), "%s/%s", object_root,
		 nb_dir_listing_name(&prefixes, (size_t)(0)));
	ASSERT_EQ(((object_scan_status = nb_dir_scan(prefix_path,
						     NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
							     NB_DIR_SCAN_FOLLOW_LINKS,
						     &objects, NULL)) != NB_OK
			   ? object_scan_status
		   : objects.names.len > INT_MAX ? NB_ERR_RANGE
						 : (object_count = (int)objects.names.len, NB_OK)),
		  0);
	ASSERT_GT(object_count, 0);
	snprintf(object_path, sizeof(object_path), "%s/%s", prefix_path,
		 nb_dir_listing_name(&objects, (size_t)(0)));
	ASSERT_EQ(nb_file_write_all(object_path, "corrupt", 7, NULL), 0);
	EXPECT_EQ(morph_sync_restore_db(&cfg, backups[0].snapshot_id, restored.c_str()),
		  NB_ERR_FORMAT);
	EXPECT_EQ(nb_file_exists(restored.c_str()), 0);
	nb_dir_listing_destroy(&objects);
	nb_dir_listing_destroy(&prefixes);
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
	struct db db NB_CLEANUP(db_close) = {};

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(unlink(device_file.c_str()), 0);
	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "INSERT INTO items(value) VALUES('device-2')"), 0);
	db_close(&db);
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
	EXPECT_EQ(nb_file_exists(conflict.c_str()), 1);
	morph_sync_backups_free(backups);
}

TEST_F(SyncTest, RetentionKeepsLatestSnapshotAndCollectsOldChunks) {
	int directory_scan_status = NB_OK;
	int object_scan_status = NB_OK;
	int remaining_prefix_scan_status = NB_OK;
	int remaining_object_scan_status = NB_OK;

	nb_buf manifest_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *manifest_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct morph_sync_status st;
	struct morph_sync_backup *backups = nullptr;
	char manifest[PATH_MAX];
	char object_root[PATH_MAX];
	nb_dir_listing prefixes NB_CLEANUP(nb_dir_listing_destroy) = {};
	int prefix_count = 0;
	int count = 0;
	std::string database = std::string(local) + "/actual.db";
	struct db db NB_CLEANUP(db_close) = {};

	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "actual.db");
	cfg.include_count = 1;
	cfg.retention_days = 1;
	CreateSqlite(database);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(db_open_plain(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "INSERT INTO items(value) VALUES('new')"), 0);
	db_close(&db);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	ASSERT_EQ(count, 2);
	snprintf(manifest, sizeof(manifest),
		 "%s/.morph-sync/db/v1/snapshots/%s.json", remote,
		 backups[1].snapshot_id);
	char *text =
		(nb_file_read_all(manifest, &manifest_contents_buffer, 0, NULL) == NB_OK &&
				 (manifest_contents_copy = (char *)malloc(
					  nb_buf_len(&manifest_contents_buffer) + 1)) != NULL
			 ? (memcpy(manifest_contents_copy, nb_buf_cstr(&manifest_contents_buffer),
				   nb_buf_len(&manifest_contents_buffer) + 1),
			    nb_charp_steal(&manifest_contents_copy))
			 : NULL);
	ASSERT_NE(text, nullptr);
	cJSON *json = cJSON_Parse(text);
	free(text);
	ASSERT_NE(json, nullptr);
	cJSON_SetNumberValue(cJSON_GetObjectItem(json, "created_at"), 1);
	char *updated = cJSON_PrintUnformatted(json);
	ASSERT_NE(updated, nullptr);
	ASSERT_EQ(nb_file_write_all(manifest, updated, strlen(updated), NULL), 0);
	free(updated);
	cJSON_Delete(json);
	morph_sync_backups_free(backups);
	backups = nullptr;

	snprintf(object_root, sizeof(object_root), "%s/.morph-sync/db/v1/objects",
		 remote);
	ASSERT_EQ(((directory_scan_status = nb_dir_scan(object_root,
							NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
								NB_DIR_SCAN_FOLLOW_LINKS,
							&prefixes, NULL)) != NB_OK
			   ? directory_scan_status
		   : prefixes.names.len > INT_MAX
			   ? NB_ERR_RANGE
			   : (prefix_count = (int)prefixes.names.len, NB_OK)),
		  0);
	for (int i = 0; i < prefix_count; i++) {
		char directory[PATH_MAX];
		nb_dir_listing objects NB_CLEANUP(nb_dir_listing_destroy) = {};
		int object_count = 0;
		snprintf(directory, sizeof(directory), "%s/%s", object_root,
			 nb_dir_listing_name(&prefixes, (size_t)(i)));
		ASSERT_EQ(((object_scan_status = nb_dir_scan(directory,
							     NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
								     NB_DIR_SCAN_FOLLOW_LINKS,
							     &objects, NULL)) != NB_OK
				   ? object_scan_status
			   : objects.names.len > INT_MAX
				   ? NB_ERR_RANGE
				   : (object_count = (int)objects.names.len, NB_OK)),
			  0);
		for (int j = 0; j < object_count; j++) {
			char object[PATH_MAX];
			struct utimbuf old_time = { 1, 1 };
			snprintf(object, sizeof(object), "%s/%s", directory,
				 nb_dir_listing_name(&objects, (size_t)(j)));
			ASSERT_EQ(utime(object, &old_time), 0);
		}
		nb_dir_listing_destroy(&objects);
	}
	nb_dir_listing_destroy(&prefixes);

	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_EQ(morph_sync_backups(&cfg, nullptr, &backups, &count), 0);
	EXPECT_EQ(count, 1);
	morph_sync_backups_free(backups);
	nb_dir_listing_destroy(&prefixes);
	prefix_count = 0;
	int remaining_objects = 0;
	ASSERT_EQ(
		((remaining_prefix_scan_status = nb_dir_scan(object_root,
							     NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
								     NB_DIR_SCAN_FOLLOW_LINKS,
							     &prefixes, NULL)) != NB_OK
			 ? remaining_prefix_scan_status
		 : prefixes.names.len > INT_MAX ? NB_ERR_RANGE
						: (prefix_count = (int)prefixes.names.len, NB_OK)),
		0);
	for (int i = 0; i < prefix_count; i++) {
		char directory[PATH_MAX];
		nb_dir_listing objects NB_CLEANUP(nb_dir_listing_destroy) = {};
		int object_count = 0;
		snprintf(directory, sizeof(directory), "%s/%s", object_root,
			 nb_dir_listing_name(&prefixes, (size_t)(i)));
		ASSERT_EQ(((remaining_object_scan_status = nb_dir_scan(
				    directory,
				    NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
				    &objects, NULL)) != NB_OK
				   ? remaining_object_scan_status
			   : objects.names.len > INT_MAX
				   ? NB_ERR_RANGE
				   : (object_count = (int)objects.names.len, NB_OK)),
			  0);
		remaining_objects += object_count;
		nb_dir_listing_destroy(&objects);
	}
	nb_dir_listing_destroy(&prefixes);
	EXPECT_EQ(remaining_objects, 1);
}

TEST_F(SyncTest, RestoreTrashClearsTombstone) {
	nb_buf config_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *config_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct morph_sync_status st;
	char remote_file[PATH_MAX];

	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "base", 4, NULL),
		  0);
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
	char *text = (nb_file_read_all((std::string(local) + "/config.toml").c_str(),
				       &config_contents_buffer, 0, NULL) == NB_OK &&
				      (config_contents_copy = (char *)malloc(
					       nb_buf_len(&config_contents_buffer) + 1)) != NULL
			      ? (memcpy(config_contents_copy, nb_buf_cstr(&config_contents_buffer),
					nb_buf_len(&config_contents_buffer) + 1),
				 nb_charp_steal(&config_contents_copy))
			      : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "base");
	free(text);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(nb_file_exists((std::string(local) + "/config.toml").c_str()), 1);
}

TEST_F(SyncTest, CleansExpiredTrash) {
	struct morph_sync_status st;
	struct db db NB_CLEANUP(db_close) = {};
	char remote_file[PATH_MAX];
	char trash_path[PATH_MAX];

	cfg.retention_days = 1;
	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "base", 4, NULL),
		  0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	snprintf(remote_file, sizeof(remote_file), "%s/data/config.toml", remote);
	ASSERT_EQ(std::remove(remote_file), 0);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	ASSERT_GT(QueryInt("SELECT COUNT(*) FROM trash"), 0);
	ASSERT_EQ(OpenManifest(&db), 0);
	ASSERT_EQ(orm_fixture_execute(&db, "UPDATE trash SET created_at=strftime('%s','now')-172800"), 0);
	char *stored_path NB_CLEANUP(nb_charp_cleanup) = nullptr;

	ASSERT_EQ(orm_fixture_text_copy(&db,
		"SELECT trash_path FROM trash LIMIT 1", &stored_path), 0);
	ASSERT_EQ(nb_str_copy_to(nb_str_from_cstr(stored_path), trash_path, sizeof(trash_path)), 0);
	db_close(&db);
	ASSERT_EQ(nb_file_exists(trash_path), 1);
	ASSERT_EQ(morph_sync_once(&cfg, &st), 0);
	EXPECT_EQ(QueryInt("SELECT COUNT(*) FROM trash"), 0);
	EXPECT_EQ(nb_file_exists(trash_path), 0);
}

TEST_F(SyncTest, ManifestStoresSizeAndMtime) {
	struct morph_sync_status st;

	ASSERT_EQ(nb_file_write_all((std::string(local) + "/config.toml").c_str(), "abcd", 4, NULL),
		  0);
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
