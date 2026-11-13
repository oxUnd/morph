#ifndef MORPH_MEMORY_SEARCH_H
#define MORPH_MEMORY_SEARCH_H

#include "memory_store.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

struct memory_record {
	int64_t id;
	int64_t session_id;
	int64_t updated_at;
	int64_t memory_version;
	char *kind;
	char *key;
	char *value;
	char *source;
	char *details;
	char *session;
	double importance;
	int score;
};

struct memory_search_result {
	nb_vec records;
};

struct memory_search_options {
	const char *query;
	const char *kind;
	int64_t record_id;
	int project;
	int all;
	/* Zero lists all matching records; positive limits select ranked records. */
	int limit;
	int include_unmatched;
	memory_session_visible_fn visible_fn;
	const char *user_id;
	void *visible_data;
};

/* Same-owner project search requires a nonempty matching project binding.
 * The model chooses query words; whitespace separates alternatives.
 * Query terms are bound SQL values. No database extensions are required. */
int memory_search_records(struct db *db, int64_t session_id,
			  const struct memory_search_options *options,
			  struct memory_search_result *out);
void memory_search_result_cleanup(struct memory_search_result *matches);

#ifdef __cplusplus
}
#endif
#endif
