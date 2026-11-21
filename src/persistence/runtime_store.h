#ifndef MORPH_RUNTIME_STORE_H
#define MORPH_RUNTIME_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct db;

struct runtime_store_instance {
	char *epoch;
	int64_t process_id;
	int missing;
	int closed;
};

struct runtime_store_instances {
	struct runtime_store_instance *items;
	size_t count;
};

struct runtime_store_session_settings {
	int64_t session_id;
	int has_permission;
	int access;
	int approval;
	int reviewer;
	int has_ultra;
	int ultra;
};

struct runtime_store_turn_input {
	const char *turn_id;
	int64_t session_id;
	const char *owner_epoch;
	const char *user_id;
	const char *idempotency_key;
	const char *input;
	const char *stored_input;
	const char *status;
	const char *final_text;
	int execution_rc;
	int persistence_rc;
};

struct runtime_store_turn_record {
	char *turn_id;
	int64_t session_id;
	char *status;
	char *final_text;
	int execution_rc;
	int persistence_rc;
};

int runtime_store_init(struct db *db);
/* Candidate ownership rows only. The caller decides whether an instance is
 * alive and whether its unfinished turns should be recovered. */
int runtime_store_pending_instances_copy(struct db *db, struct runtime_store_instances *out);
void runtime_store_instances_cleanup(struct runtime_store_instances *instances);
int runtime_store_instance_register(struct db *db, const char *epoch, int64_t process_id);
int runtime_store_instance_close(struct db *db, const char *epoch);
int runtime_store_recover_epoch(struct db *db, const char *epoch, int execution_rc);

/* Settings are stored as scalar values; domain enum validation belongs to the
 * runtime. Missing settings return NB_ERR_NO_DATA. */
int runtime_store_session_settings_get(struct db *db, int64_t session_id,
	struct runtime_store_session_settings *out);
int runtime_store_session_settings_put(struct db *db,
	const struct runtime_store_session_settings *settings);

/* Input strings are borrowed through the call. Returned records own their
 * strings and require an empty output; failures leave outputs empty. A NULL or
 * empty turn ID selects the latest turn in the named session. */
int runtime_store_turn_put(struct db *db, const struct runtime_store_turn_input *turn);
int runtime_store_turn_copy(struct db *db, int64_t session_id, const char *turn_id,
	struct runtime_store_turn_record *out);
int runtime_store_turn_by_key_copy(struct db *db, int64_t session_id, const char *user_id,
	const char *key, struct runtime_store_turn_record *out);
void runtime_store_turn_record_cleanup(struct runtime_store_turn_record *record);

#ifdef __cplusplus
}
#endif

#endif
