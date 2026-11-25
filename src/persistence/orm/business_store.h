#ifndef MORPH_ORM_BUSINESS_STORE_H
#define MORPH_ORM_BUSINESS_STORE_H

#include "persistence/orm/database.h"
#include <nb/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

struct store_history_body {
	int64_t id;
	char *content;
	char *payload_json;
};

struct store_history_context {
	char *kind;
	int token_count;
	char *content;
	char *payload_json;
};

struct store_artifact {
	int64_t id;
	char *kind;
	char *path;
	char *tool_name;
};

struct store_refresh_session {
	int64_t session_id;
	char *display_id;
};

struct store_compaction_status {
	char *trigger_kind;
	char *status;
	int input_tokens;
	int output_tokens;
	int error_code;
};

void store_text_records_cleanup(nb_vec *records);
void store_history_bodies_cleanup(nb_vec *records);
void store_history_context_cleanup(nb_vec *records);
void store_artifacts_cleanup(nb_vec *records);
void store_refresh_sessions_cleanup(nb_vec *records);
void store_compaction_status_cleanup(struct store_compaction_status *status);
int plan_store_put(struct db *db, int64_t session_id, const char *state);
int plan_store_copy(struct db *db, int64_t session_id, char **state);
int activation_store_set(struct db *db, int64_t session_id, const char *name, int active);
int activation_store_list_copy(struct db *db, int64_t session_id, nb_vec *names);
int evidence_store_latest_user_copy(struct db *db, int64_t session_id, int64_t *id, char **content);
int request_snapshot_store_put(struct db *db, int64_t session_id, const char *turn_id,
			       const char *provider, const char *model, const char *input_json);
int history_body_store_list_copy(struct db *db, int64_t session_id, const char *turn_id,
				 nb_vec *records);
int history_body_store_update(struct db *db, int64_t id, const char *payload_json, int tokens);
int history_context_store_list_copy(struct db *db, int64_t session_id, nb_vec *records);
int history_user_inputs_store_copy(struct db *db, int64_t session_id, const char *turn_id,
				   nb_vec *records);
int artifact_context_store_list_copy(struct db *db, int64_t session_id, nb_vec *records);
int memory_refresh_store_list_copy(struct db *db, int64_t session_id, nb_vec *records);
int session_title_store_update(struct db *db, int64_t session_id, const char *display_id,
			       const char *old_name, const char *title);
int compaction_status_store_copy(struct db *db, int64_t session_id,
				 struct store_compaction_status *status);
int process_job_store_init(struct db *db);
int process_job_store_put(struct db *db, const char *process_id, int64_t session_id,
			  const char *data_json);
int process_job_store_list_copy(struct db *db, int64_t session_id, nb_vec *records);
int process_job_store_copy(struct db *db, int64_t session_id, const char *process_id,
			   char **data_json);

#ifdef __cplusplus
}
#endif

#endif
