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

struct morph_sync_config {
	int enabled;
	char source_dir[PATH_MAX];
	char sync_dir[PATH_MAX];
	int interval_seconds;
	int retention_days;
	char include[MORPH_SYNC_INCLUDE_MAX][MORPH_SYNC_INCLUDE_LEN_MAX];
	int include_count;
};

struct morph_sync_status {
	int running;
	int copied;
	int deleted;
	int conflicts;
	int recycled;
	int error_code;
	int64_t last_run_at;
	char last_error[MORPH_SYNC_STATUS_TEXT_MAX];
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

#ifdef __cplusplus
}
#endif

#endif
