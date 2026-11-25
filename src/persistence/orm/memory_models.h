#ifndef MORPH_ORM_MEMORY_MODELS_H
#define MORPH_ORM_MEMORY_MODELS_H

#include "internal.h"

struct fact_row {
	int64_t id;
	int64_t session_id;
	nb_str key_name;
	nb_str value_text;
	nb_str source_text;
	bool source_text_missing;
	double confidence;
	bool confidence_missing;
	nb_str category;
	bool category_missing;
	double importance;
	bool importance_missing;
	int is_current;
	int64_t valid_from;
	int64_t valid_to;
	bool valid_to_missing;
	int64_t superseded_by;
	bool superseded_by_missing;
	int64_t created_at;
	int64_t updated_at;
};
enum fact_field {
	FACT_ID,
	FACT_SESSION_ID,
	FACT_KEY_NAME,
	FACT_VALUE_TEXT,
	FACT_SOURCE_TEXT,
	FACT_CONFIDENCE,
	FACT_CATEGORY,
	FACT_IMPORTANCE,
	FACT_IS_CURRENT,
	FACT_VALID_FROM,
	FACT_VALID_TO,
	FACT_SUPERSEDED_BY,
	FACT_CREATED_AT,
	FACT_UPDATED_AT,
};
extern const struct norm_model fact_model;

struct memory_scope_row {
	int64_t session_id;
	nb_str owner;
	nb_str project;
	int32_t migrated;
	int64_t generation;
	int64_t content_revision;
};

enum memory_scope_field {
	SCOPE_SESSION_ID,
	SCOPE_OWNER,
	SCOPE_PROJECT,
	SCOPE_MIGRATED,
	SCOPE_GENERATION,
	SCOPE_CONTENT_REVISION,
};
extern const struct norm_model memory_scope_model;

struct memory_candidate_row {
	int64_t id;
	int64_t session_id;
	nb_str kind;
	nb_str key;
	nb_str value;
	nb_str source;
	int64_t created_at;
};

enum memory_candidate_field {
	CANDIDATE_ID,
	CANDIDATE_SESSION_ID,
	CANDIDATE_KIND,
	CANDIDATE_KEY,
	CANDIDATE_VALUE,
	CANDIDATE_SOURCE,
	CANDIDATE_CREATED_AT,
};
extern const struct norm_model memory_candidate_model;

struct memory_episode_row {
	int64_t id;
	int64_t session_id;
	nb_str task_type;
	bool task_type_missing;
	nb_str summary_text;
	nb_str outcome_text;
	bool outcome_text_missing;
	int32_t success;
	nb_str entities;
	bool entities_missing;
	nb_str key_decisions;
	bool key_decisions_missing;
	nb_str artifacts;
	bool artifacts_missing;
	nb_str tools_used;
	bool tools_used_missing;
	double importance;
	bool importance_missing;
	int64_t created_at;
	int64_t job_id;
	bool job_id_missing;
};

enum memory_episode_field {
	EPISODE_ID,
	EPISODE_SESSION_ID,
	EPISODE_TASK_TYPE,
	EPISODE_SUMMARY_TEXT,
	EPISODE_OUTCOME_TEXT,
	EPISODE_SUCCESS,
	EPISODE_ENTITIES,
	EPISODE_KEY_DECISIONS,
	EPISODE_ARTIFACTS,
	EPISODE_TOOLS_USED,
	EPISODE_IMPORTANCE,
	EPISODE_CREATED_AT,
	EPISODE_JOB_ID,
};
extern const struct norm_model memory_episode_model;

struct memory_procedure_row {
	int64_t id;
	int64_t session_id;
	nb_str rule_text;
	nb_str trigger_text;
	bool trigger_text_missing;
	int32_t evidence_count;
	int64_t updated_at;
};

enum memory_procedure_field {
	PROCEDURE_ID,
	PROCEDURE_SESSION_ID,
	PROCEDURE_RULE_TEXT,
	PROCEDURE_TRIGGER_TEXT,
	PROCEDURE_EVIDENCE_COUNT,
	PROCEDURE_UPDATED_AT,
};
extern const struct norm_model memory_procedure_model;

struct memory_note_row {
	int64_t id;
	int64_t session_id;
	nb_str title;
	nb_str text;
	nb_str source;
	nb_str event_token;
	nb_str scope;
	int64_t created_at;
	int64_t updated_at;
};

enum memory_note_field {
	NOTE_ID,
	NOTE_SESSION_ID,
	NOTE_TITLE,
	NOTE_TEXT,
	NOTE_SOURCE,
	NOTE_EVENT_TOKEN,
	NOTE_SCOPE,
	NOTE_CREATED_AT,
	NOTE_UPDATED_AT,
};
extern const struct norm_model memory_note_model;

struct memory_summary_row {
	int64_t session_id;
	int64_t memory_version;
	int64_t content_revision;
	nb_str title;
	nb_str text;
	nb_str sources;
	int64_t updated_at;
};

enum memory_summary_field {
	SUMMARY_SESSION_ID,
	SUMMARY_MEMORY_VERSION,
	SUMMARY_CONTENT_REVISION,
	SUMMARY_TITLE,
	SUMMARY_TEXT,
	SUMMARY_SOURCES,
	SUMMARY_UPDATED_AT,
};
extern const struct norm_model memory_summary_model;

struct memory_profile_row {
	int64_t session_id;
	nb_str profile_text;
	int64_t updated_at;
};

enum memory_profile_field {
	PROFILE_SESSION_ID,
	PROFILE_PROFILE_TEXT,
	PROFILE_UPDATED_AT,
};
extern const struct norm_model memory_profile_model;

struct memory_preference_row {
	int64_t id;
	nb_str owner;
	nb_str scope;
	nb_str target;
	nb_str key;
	nb_str value;
	bool value_missing;
	nb_str source;
	nb_str origin;
	int64_t source_order;
	int64_t session_id;
	nb_str event_token;
	int64_t created_at;
};

enum memory_preference_field {
	PREFERENCE_ID,
	PREFERENCE_OWNER,
	PREFERENCE_SCOPE,
	PREFERENCE_TARGET,
	PREFERENCE_KEY,
	PREFERENCE_VALUE,
	PREFERENCE_SOURCE,
	PREFERENCE_ORIGIN,
	PREFERENCE_SOURCE_ORDER,
	PREFERENCE_SESSION_ID,
	PREFERENCE_EVENT_TOKEN,
	PREFERENCE_CREATED_AT,
};
extern const struct norm_model memory_preference_model;

struct memory_job_row {
	int64_t id;
	int64_t session_id;
	int64_t generation;
	nb_str payload;
	bool payload_missing;
	nb_str state;
	int64_t worker_pid;
	int attempts;
	int error_code;
	nb_str stage;
	int64_t updated_at;
	int64_t content_revision;
};

enum memory_job_field {
	JOB_ID,
	JOB_SESSION_ID,
	JOB_GENERATION,
	JOB_PAYLOAD,
	JOB_STATE,
	JOB_WORKER_PID,
	JOB_ATTEMPTS,
	JOB_ERROR_CODE,
	JOB_STAGE,
	JOB_UPDATED_AT,
	JOB_CONTENT_REVISION,
};
extern const struct norm_model job_model;

#endif
