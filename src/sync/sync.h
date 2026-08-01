#ifndef MORPH_SYNC_H
#define MORPH_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stdint.h>
#include <pthread.h>

#define MORPH_SYNC_INCLUDE_MAX 16
#define MORPH_SYNC_INCLUDE_LEN_MAX PATH_MAX
#define MORPH_SYNC_HASH_LEN 65
#define MORPH_SYNC_STATUS_TEXT_MAX 256
#define MORPH_SYNC_SNAPSHOT_ID_MAX 256
#define MORPH_SYNC_DEVICE_ID_MAX 64

struct morph_sync_backend_stat {
	int exists;
	int is_dir;
	int64_t size;
	int64_t mtime;
	char hash[MORPH_SYNC_HASH_LEN];
};

typedef int (*morph_sync_backend_list_cb)(const char *name, int is_dir,
					  void *user_data);

struct morph_sync_backend {
	void *user_data;
	int (*stat)(void *user_data, const char *rel,
		    struct morph_sync_backend_stat *st);
	int (*list)(void *user_data, const char *rel_dir,
		    morph_sync_backend_list_cb cb, void *cb_data);
	int (*copy_from_local)(void *user_data, const char *local_path,
			       const char *remote_rel, int sqlite_source);
	int (*copy_to_local)(void *user_data, const char *remote_rel,
			     const char *local_path);
	int (*delete_file)(void *user_data, const char *remote_rel);
	int (*ensure_dir)(void *user_data, const char *remote_rel_dir);
};

struct morph_sync_config {
	int enabled;
	char source_dir[PATH_MAX];
	char sync_dir[PATH_MAX];
	int interval_seconds;
	int retention_days;
	char include[MORPH_SYNC_INCLUDE_MAX][MORPH_SYNC_INCLUDE_LEN_MAX];
	int include_count;
	const struct morph_sync_backend *remote_backend;
};

struct morph_sync_status {
	int running;
	int copied;
	int deleted;
	int conflicts;
	int recycled;
	int db_snapshots;
	int db_chunks_uploaded;
	int db_chunks_reused;
	int64_t db_bytes_uploaded;
	int error_code;
	int64_t last_run_at;
	char last_error[MORPH_SYNC_STATUS_TEXT_MAX];
};

struct morph_sync_backup {
	char snapshot_id[MORPH_SYNC_SNAPSHOT_ID_MAX];
	char path[PATH_MAX];
	char device_id[MORPH_SYNC_DEVICE_ID_MAX];
	char hash[MORPH_SYNC_HASH_LEN];
	int64_t created_at;
	int64_t size;
};

struct morph_sync_worker {
	struct morph_sync_config cfg;
	struct morph_sync_status status;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int started;
	int stop;
};

int morph_sync_once(const struct morph_sync_config *cfg,
		    struct morph_sync_status *status);
int morph_sync_start(struct morph_sync_worker *worker,
		     const struct morph_sync_config *cfg);
void morph_sync_stop(struct morph_sync_worker *worker);
int morph_sync_worker_status(struct morph_sync_worker *worker,
			     struct morph_sync_status *status);
int morph_sync_restore_trash(const struct morph_sync_config *cfg,
			     int64_t trash_id);
int morph_sync_backups(const struct morph_sync_config *cfg,
			const char *path, struct morph_sync_backup **out,
			int *count);
void morph_sync_backups_free(struct morph_sync_backup *items);
int morph_sync_restore_db(const struct morph_sync_config *cfg,
			  const char *snapshot_id, const char *destination);

#ifdef __cplusplus
}
#endif

#endif
