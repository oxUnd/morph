#ifndef MORPH_ORM_SYNC_MANIFEST_MODELS_H
#define MORPH_ORM_SYNC_MANIFEST_MODELS_H

#include "persistence/orm/internal.h"

struct orm_sync_conflicts_row {
	int64_t id;
	nb_str path;
	nb_str local_hash;
	bool missing_local_hash;
	nb_str remote_hash;
	bool missing_remote_hash;
	int64_t created_at;
	int64_t resolved_at;
};
extern const struct norm_model orm_sync_conflicts;

struct orm_sync_entries_row {
	nb_str path;
	nb_str local_hash;
	bool missing_local_hash;
	nb_str remote_hash;
	bool missing_remote_hash;
	int64_t local_exists;
	int64_t remote_exists;
	int64_t local_size;
	int64_t remote_size;
	int64_t local_mtime;
	int64_t remote_mtime;
	int64_t tombstone;
	nb_str last_device;
	bool missing_last_device;
	int64_t updated_at;
};
extern const struct norm_model orm_sync_entries;

struct orm_sync_tombstones_row {
	nb_str path;
	int64_t deleted_at;
	nb_str device_id;
	bool missing_device_id;
};
extern const struct norm_model orm_sync_tombstones;

struct orm_sync_trash_row {
	int64_t id;
	nb_str path;
	nb_str trash_path;
	int64_t created_at;
};
extern const struct norm_model orm_sync_trash;

#endif
