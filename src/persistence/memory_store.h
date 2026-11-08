#ifndef MEMORY_STORE_H
#define MEMORY_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include <stdint.h>

enum memory_clear_scope {
	MEMORY_CLEAR_ALL = 0,
	MEMORY_CLEAR_FACTS,
	MEMORY_CLEAR_TASKS,
	MEMORY_CLEAR_TIPS,
};

enum memory_query_type {
	MEMORY_QUERY_ALL = 0,
	MEMORY_QUERY_USER_SUMMARY,
	MEMORY_QUERY_FACTS,
	MEMORY_QUERY_TIPS,
	MEMORY_QUERY_TASKS,
	MEMORY_QUERY_CHANGES,
	MEMORY_QUERY_PREFERENCES,
	MEMORY_QUERY_UNAPPLIED_RULES,
	MEMORY_QUERY_NOTES,
	MEMORY_QUERY_SUMMARY,
	MEMORY_QUERY_LEARNED_FACTS,
	MEMORY_QUERY_UNAPPLIED_SETTINGS,
};

typedef int (*memory_session_visible_fn)(const char *display_id, const char *user_id,
					 void *user_data);

struct memory_query_options {
	enum memory_query_type type;
	int scope_all;
	int max_past_tasks;
	const char *user_id;
	memory_session_visible_fn visible_fn;
	void *visible_user_data;
	/* Borrowed query text for ranked project/session search. */
	const char *query;
	int64_t record_id;
	int max_results;
	int scope_project;
};

int memory_store_clear_all(struct db *db, int64_t session_id);
int memory_store_clear_facts(struct db *db, int64_t session_id);
int memory_store_clear_tasks(struct db *db, int64_t session_id);
int memory_store_clear_tips(struct db *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif
