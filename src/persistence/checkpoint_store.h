#ifndef MORPH_CHECKPOINT_STORE_H
#define MORPH_CHECKPOINT_STORE_H

#include "db/database.h"
#include "cJSON.h"

#define CHECKPOINT_FILE_MAX_BYTES (16U * 1024U * 1024U)
#define CHECKPOINT_DATA_MAX_BYTES (128U * 1024U * 1024U)
#define CHECKPOINT_PLAN_MAX_BYTES (2U * CHECKPOINT_DATA_MAX_BYTES)

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
 * files are limited to 16 MiB; captured before/after data to 128 MiB. Restore
 * plans allow another 128 MiB for current-state compensation copies.
 * Capture/seal pairs use one db connection; interrupted or failed attempts
 * cannot be silently sealed by a later unrelated editing attempt. */
int checkpoint_store_capture(struct db *db, int64_t id, const char *path);
/* Call between capture and seal, before writing. removed_path is the source of
 * a move, or NULL. The caller serializes validation and mutation with other
 * editing attempts; runtime checkpoint begin/finish owns that lock. */
int checkpoint_store_validate_write(struct db *db, int64_t id, const char *path,
				    size_t bytes, const char *removed_path);
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
