#ifndef MORPH_ORM_SYNC_MANIFEST_H
#define MORPH_ORM_SYNC_MANIFEST_H

#include "persistence/orm/database.h"
#include "sync/sync.h"
#include <stddef.h>

struct sync_file {
	char path[PATH_MAX];
	char full[PATH_MAX];
	char hash[MORPH_SYNC_HASH_LEN];
	int exists;
	int64_t size;
	int64_t mtime;
};

struct sync_pair {
	char path[PATH_MAX];
	struct sync_file local;
	struct sync_file remote;
};

struct manifest_entry {
	int found;
	int local_exists;
	int remote_exists;
	int tombstone;
	int64_t local_size;
	int64_t remote_size;
	int64_t local_mtime;
	int64_t remote_mtime;
	char local_hash[MORPH_SYNC_HASH_LEN];
	char remote_hash[MORPH_SYNC_HASH_LEN];
};

struct runtime_sync_conflict {
	int64_t id;
	char path[PATH_MAX];
	int64_t created_at;
};

int manifest_conflicts_copy(const char *path, struct runtime_sync_conflict **out, int *count);

struct manifest_trash_record {
	int64_t id;
	char *path;
	char *trash_path;
};
struct manifest_trash_records {
	struct manifest_trash_record *items;
	size_t count;
};

int manifest_open(struct db **out, const char *sync_dir);
void manifest_close(struct db **owner);
int manifest_get(struct db *db, const char *path, struct manifest_entry *out);
int manifest_put(struct db *db, const struct sync_pair *pair, int tombstone);
int manifest_conflict(struct db *db, const struct sync_pair *pair);
int manifest_resolve_db_conflicts(struct db *db, const char *path);
int manifest_trash(struct db *db, const char *path, const char *trash_path);
int manifest_tombstone(struct db *db, const char *path);
int manifest_clear_tombstone(struct db *db, const char *path);
int manifest_delete_trash(struct db *db, int64_t id);
int manifest_trash_count(struct db *db);
int manifest_unresolved_count(struct db *db);
int manifest_trash_expired_copy(struct db *db, int64_t cutoff, struct manifest_trash_records *out);
int manifest_trash_get_copy(struct db *db, int64_t id, struct manifest_trash_record *out);
void manifest_trash_record_cleanup(struct manifest_trash_record *record);
void manifest_trash_records_cleanup(struct manifest_trash_records *records);

#endif
