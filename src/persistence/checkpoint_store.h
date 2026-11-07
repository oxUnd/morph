#ifndef MORPH_CHECKPOINT_STORE_H
#define MORPH_CHECKPOINT_STORE_H

#include "db/database.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

enum checkpoint_restore_mode {
	CHECKPOINT_RESTORE_CODE = 1,
	CHECKPOINT_RESTORE_CONVERSATION = 2,
	CHECKPOINT_RESTORE_BOTH = 3,
};

/* Public operations require autocommit mode; caller-owned transactions return
 * -EBUSY before filesystem access or mutation. Each call holds the DB mutex. */

int checkpoint_store_create(struct db *db, int64_t session_id, const char *workdir,
			    const char *turn_id, const char *prompt, int64_t *id);
/* Capture precedes mutation. The first capture preserves the preimage; seal
 * records afterimages after every editing attempt, including partial failure.
 * Paths must remain inside the checkpoint workdir; links are rejected. Regular
 * files are limited to 16 MiB; captured data and restore plans to 128 MiB.
 * Capture/seal pairs use one db connection; interrupted or failed attempts
 * cannot be silently sealed by a later unrelated editing attempt. */
int checkpoint_store_capture(struct db *db, int64_t id, const char *path);
int checkpoint_store_seal(struct db *db, int64_t id);

/* JSON output slots start NULL; occupied slots return -EEXIST unchanged.
 * Caller owns every returned JSON value and releases it with cJSON_Delete().
 * Restore may return an owned report on failure, including exact conflicts or
 * compensation failures. Code restores verify all affected paths before writes. */
int checkpoint_store_list_copy(struct db *db, int64_t session_id, cJSON **out);
/* Metadata only: never reads workspace files. */
int checkpoint_store_info_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out);
int checkpoint_store_preview_copy(struct db *db, int64_t session_id, int64_t id, cJSON **out);
int checkpoint_store_restore(struct db *db, int64_t session_id, int64_t id,
			     enum checkpoint_restore_mode mode, cJSON **out);

#ifdef __cplusplus
}
#endif

#endif
