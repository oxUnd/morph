#ifndef MORPH_MEMORY_SNAPSHOT_H
#define MORPH_MEMORY_SNAPSHOT_H

#include "db/database.h"
#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

struct arena;

struct memory_fact_record {
	const char *key;
	const char *value;
	const char *category;
	double importance;
};

struct memory_rule_record {
	const char *text;
	int evidence;
};

struct memory_episode_record {
	const char *summary;
	const char *decisions;
	const char *artifacts;
	const char *tools;
	double importance;
	int64_t created_at;
};

struct memory_change_record {
	const char *key;
	const char *old_value;
	const char *new_value;
	int64_t updated_at;
	int64_t valid_to;
};

struct memory_snapshot_options {
	/* Zero omits a section, negative includes all rows. */
	int facts_limit;
	int episodes_limit;
	int changes_limit;
	int include_profile;
	int include_rules;
	int context_facts;
	int facts_by_recency;
};

struct memory_snapshot {
	struct arena *arena;
	const char *profile;
	morph_array_t facts;
	morph_array_t rules;
	morph_array_t episodes;
	morph_array_t changes;
	int episode_total;
};

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
