#ifndef MORPH_RUNTIME_SYNC_H
#define MORPH_RUNTIME_SYNC_H

#include "config.h"
#include "sync/sync.h"

struct runtime;

int runtime_sync_config_from_config(const struct config_sync *config,
				    const char *source_dir,
				    const char *default_sync_dir,
				    const struct morph_sync_backend *backend,
				    struct morph_sync_config *out);
int runtime_sync_start_instance(struct runtime *runtime,
				const char *default_sync_dir,
				const struct morph_sync_backend *backend);
void runtime_sync_stop_instance(struct runtime *runtime);
int runtime_sync_status_instance(struct runtime *runtime,
				 struct morph_sync_status *status);
int runtime_sync_now_instance(struct runtime *runtime,
			      const char *default_sync_dir,
			      const struct morph_sync_backend *backend,
			      struct morph_sync_status *status);
int runtime_sync_running(const struct runtime *runtime);
int runtime_sync_config_instance(struct runtime *runtime,
				 const char *default_sync_dir,
				 const struct morph_sync_backend *backend,
				 struct morph_sync_config *out);

struct runtime_sync_conflict {
	int64_t id;
	char path[PATH_MAX];
	int64_t created_at;
};

int runtime_sync_conflicts(struct runtime *runtime,
			   struct runtime_sync_conflict **out, int *count);
void runtime_sync_conflicts_free(struct runtime_sync_conflict *items);
int runtime_sync_restore(struct runtime *runtime, int64_t trash_id);

#endif
