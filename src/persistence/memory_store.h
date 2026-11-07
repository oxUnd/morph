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
	MEMORY_CLEAR_EPISODES,
	MEMORY_CLEAR_PROCEDURES,
};

enum memory_query_type {
	MEMORY_QUERY_ALL = 0,
	MEMORY_QUERY_PROFILE,
	MEMORY_QUERY_FACTS,
	MEMORY_QUERY_PROCEDURES,
	MEMORY_QUERY_EPISODES,
	MEMORY_QUERY_CHANGES,
};

typedef int (*memory_session_visible_fn)(const char *display_id, const char *user_id,
					 void *user_data);

struct memory_query_options {
	enum memory_query_type type;
	int scope_all;
	int max_episodes;
	const char *user_id;
	memory_session_visible_fn visible_fn;
	void *visible_user_data;
};

int memory_store_clear_all(struct db *db, int64_t session_id);
int memory_store_clear_facts(struct db *db, int64_t session_id);
int memory_store_clear_episodes(struct db *db, int64_t session_id);
int memory_store_clear_procedures(struct db *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif
