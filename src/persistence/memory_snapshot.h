#ifndef MORPH_MEMORY_SNAPSHOT_H
#define MORPH_MEMORY_SNAPSHOT_H

#include "db/database.h"
#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

struct memory_fact_record {
	char *key;
	char *value;
	char *category;
	double importance;
};

struct memory_rule_record {
	char *text;
	int evidence;
};

struct memory_task_record {
	char *summary;
	char *decisions;
	char *artifacts;
	char *tools;
	double importance;
	int64_t created_at;
};

struct memory_change_record {
	char *key;
	char *old_value;
	char *new_value;
	int64_t updated_at;
	int64_t valid_to;
};

struct memory_snapshot_options {
	/* Zero omits a section, negative includes all rows. */
	int facts_limit;
	int tasks_limit;
	int changes_limit;
	int include_user_summary;
	int include_rules;
	int for_reply;
	int facts_by_recency;
};

struct memory_snapshot {
	char *user_summary;
	morph_array_t facts;
	morph_array_t rules;
	morph_array_t tasks;
	morph_array_t changes;
	int task_count;
};

/* Outputs must be zero-initialized and empty. Occupied owners return -EEXIST. */
/* A snapshot owns every row and string until cleanup. No SQLite view escapes. */
int memory_snapshot_read(struct db *db, int64_t session_id,
			 const struct memory_snapshot_options *options,
			 struct memory_snapshot *out);
void memory_snapshot_cleanup(struct memory_snapshot *snapshot);
int memory_store_session_has_rows(struct db *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif
