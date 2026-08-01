#ifndef MORPH_SYNC_DB_BACKUP_H
#define MORPH_SYNC_DB_BACKUP_H

#include "sync/sync.h"

int sync_db_is_sqlite(const char *path);
int sync_db_backup_create(const struct morph_sync_config *cfg,
			  const char *rel, const char *path,
			  struct morph_sync_status *status);
int sync_db_backup_cleanup(const struct morph_sync_config *cfg);

#endif
