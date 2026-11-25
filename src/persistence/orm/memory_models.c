#include "memory_models.h"

static const struct norm_field fact_fields[] = {
	NORM_FIELD(struct fact_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct fact_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct fact_row, key_name, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct fact_row, value_text, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct fact_row, source_text, source_text_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct fact_row, confidence, confidence_missing, NORM_TYPE_F64, 0),
	NORM_NULLABLE_FIELD(struct fact_row, category, category_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct fact_row, importance, importance_missing, NORM_TYPE_F64, 0),
	NORM_FIELD(struct fact_row, is_current, NORM_TYPE_I32, 0),
	NORM_FIELD(struct fact_row, valid_from, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct fact_row, valid_to, valid_to_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct fact_row, superseded_by, superseded_by_missing, NORM_TYPE_I64,
			    0),
	NORM_FIELD(struct fact_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct fact_row, updated_at, NORM_TYPE_I64, 0),
};
const struct norm_model fact_model = NORM_MODEL(struct fact_row, "memory_facts", fact_fields);

static const struct norm_field scope_fields[] = {
	NORM_FIELD(struct memory_scope_row, session_id, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct memory_scope_row, owner, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_scope_row, project, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_scope_row, migrated, NORM_TYPE_I32, 0),
	NORM_FIELD(struct memory_scope_row, generation, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_scope_row, content_revision, NORM_TYPE_I64, 0),
};
const struct norm_model memory_scope_model =
	NORM_MODEL(struct memory_scope_row, "memory_scopes", scope_fields);

static const struct norm_field candidate_fields[] = {
	NORM_FIELD(struct memory_candidate_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_candidate_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_candidate_row, kind, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_candidate_row, key, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_candidate_row, value, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_candidate_row, source, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_candidate_row, created_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_candidate_model =
	NORM_MODEL(struct memory_candidate_row, "memory_candidates", candidate_fields);

static const struct norm_field episode_fields[] = {
	NORM_FIELD(struct memory_episode_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_episode_row, session_id, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, task_type, task_type_missing, NORM_TYPE_TEXT,
			    0),
	NORM_FIELD(struct memory_episode_row, summary_text, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, outcome_text, outcome_text_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_episode_row, success, NORM_TYPE_I32, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, entities, entities_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, key_decisions, key_decisions_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, artifacts, artifacts_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, tools_used, tools_used_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, importance, importance_missing,
			    NORM_TYPE_F64, 0),
	NORM_FIELD(struct memory_episode_row, created_at, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct memory_episode_row, job_id, job_id_missing, NORM_TYPE_I64, 0),
};
const struct norm_model memory_episode_model =
	NORM_MODEL(struct memory_episode_row, "memory_episodes", episode_fields);

static const struct norm_field procedure_fields[] = {
	NORM_FIELD(struct memory_procedure_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_procedure_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_procedure_row, rule_text, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct memory_procedure_row, trigger_text, trigger_text_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_procedure_row, evidence_count, NORM_TYPE_I32, 0),
	NORM_FIELD(struct memory_procedure_row, updated_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_procedure_model =
	NORM_MODEL(struct memory_procedure_row, "memory_procedures", procedure_fields);

static const struct norm_field note_fields[] = {
	NORM_FIELD(struct memory_note_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_note_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_note_row, title, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_note_row, text, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_note_row, source, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_note_row, event_token, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_note_row, scope, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_note_row, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_note_row, updated_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_note_model =
	NORM_MODEL(struct memory_note_row, "memory_notes", note_fields);

static const struct norm_field summary_fields[] = {
	NORM_FIELD(struct memory_summary_row, session_id, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct memory_summary_row, memory_version, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_summary_row, content_revision, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_summary_row, title, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_summary_row, text, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_summary_row, sources, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_summary_row, updated_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_summary_model =
	NORM_MODEL(struct memory_summary_row, "memory_summaries", summary_fields);

static const struct norm_field profile_fields[] = {
	NORM_FIELD(struct memory_profile_row, session_id, NORM_TYPE_I64, NORM_FIELD_PRIMARY_KEY),
	NORM_FIELD(struct memory_profile_row, profile_text, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_profile_row, updated_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_profile_model =
	NORM_MODEL(struct memory_profile_row, "memory_profiles", profile_fields);

static const struct norm_field preference_fields[] = {
	NORM_FIELD(struct memory_preference_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_preference_row, owner, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, scope, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, target, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, key, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct memory_preference_row, value, value_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, source, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, origin, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, source_order, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_preference_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_preference_row, event_token, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_preference_row, created_at, NORM_TYPE_I64, 0),
};
const struct norm_model memory_preference_model =
	NORM_MODEL(struct memory_preference_row, "memory_preferences", preference_fields);

static const struct norm_field job_fields[] = {
	NORM_FIELD(struct memory_job_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct memory_job_row, session_id, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_job_row, generation, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct memory_job_row, payload, payload_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_job_row, state, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_job_row, worker_pid, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_job_row, attempts, NORM_TYPE_I32, 0),
	NORM_FIELD(struct memory_job_row, error_code, NORM_TYPE_I32, 0),
	NORM_FIELD(struct memory_job_row, stage, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct memory_job_row, updated_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct memory_job_row, content_revision, NORM_TYPE_I64, 0),
};
const struct norm_model job_model = NORM_MODEL(struct memory_job_row, "memory_jobs", job_fields);
