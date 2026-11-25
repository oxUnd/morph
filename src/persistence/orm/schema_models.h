#ifndef MORPH_ORM_SCHEMA_MODELS_H
#define MORPH_ORM_SCHEMA_MODELS_H

#include "internal.h"

enum schema_table {
	SCHEMA_SCHEMA_MIGRATIONS,
	SCHEMA_SESSIONS,
	SCHEMA_SESSION_SKILL_ACTIVATIONS,
	SCHEMA_MESSAGES,
	SCHEMA_MESSAGE_ATTACHMENTS,
	SCHEMA_REACT_TRACES,
	SCHEMA_EXTS,
	SCHEMA_OUTPUTS,
	SCHEMA_MEMORY_PROFILES,
	SCHEMA_MEMORY_FACTS,
	SCHEMA_MEMORY_EPISODES,
	SCHEMA_MEMORY_PROCEDURES,
	SCHEMA_MEMORY_SCOPES,
	SCHEMA_MEMORY_PREFERENCES,
	SCHEMA_MEMORY_CANDIDATES,
	SCHEMA_MEMORY_JOBS,
	SCHEMA_SUB_AGENT_TASKS,
	SCHEMA_SUB_AGENT_EVENTS,
	SCHEMA_AGENT_THREADS,
	SCHEMA_AGENT_MAILBOX,
	SCHEMA_AGENT_EVENTS,
	SCHEMA_AGENT_BUDGET_STATE,
	SCHEMA_MODEL_HISTORY_ITEMS,
	SCHEMA_HISTORY_COMPACTIONS,
	SCHEMA_HISTORY_SUMMARY_SOURCES,
	SCHEMA_MODEL_REQUEST_SNAPSHOTS,
	SCHEMA_HISTORY_COMPACTION_ATTEMPTS,
	SCHEMA_SESSION_PLANS,
	SCHEMA_PERMISSION_GRANTS,
	SCHEMA_SCHEDULED_TASKS,
	SCHEMA_NOTIFICATIONS,
	SCHEMA_CREDIT_EVENTS,
	SCHEMA_MEMORY_NOTES,
	SCHEMA_MEMORY_SUMMARIES,
	SCHEMA_TABLE_COUNT,
};

const struct norm_model *schema_model(enum schema_table table);
size_t schema_field(const struct norm_model *model, const char *column);
int schema_version_read(struct orm_scope *scope, int64_t version, bool *applied);
int schema_version_record(struct orm_scope *scope, int64_t version, const char *name);
int schema_create_tables(struct orm_scope *scope);
int schema_create_indexes(struct orm_scope *scope);
int schema_create_revision_triggers(struct orm_scope *scope);
int schema_create_credit(struct orm_scope *scope);

#endif
