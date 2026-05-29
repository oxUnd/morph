#ifndef MEMORY_H
#define MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include "agent/react.h"

struct model;

struct memory_options {
	int enabled;
	int hot_path_enabled;
	int cold_path_enabled;
	int llm_extract_enabled;
	int max_facts;
	int max_episodes;
	int max_procedures;
	int max_context_chars;
};

enum memory_clear_scope {
	MEMORY_CLEAR_ALL = 0,
	MEMORY_CLEAR_FACTS,
	MEMORY_CLEAR_EPISODES,
	MEMORY_CLEAR_PROCEDURES,
};

/* Inject the LLM model used for structured extraction / consolidation.
 * Pass NULL to disable LLM-based memory and fall back to anchor heuristics
 * only. Safe to call multiple times. */
void memory_set_llm(struct model *llm);

char *memory_build_context(struct db *db, int64_t session_id,
			   const char *query,
			   const struct memory_options *opts);

char *memory_render_session(struct db *db, int64_t session_id,
			    int max_episodes);

int memory_consolidate_turn(struct db *db, int64_t session_id,
			    const char *user_input,
			    const char *assistant_output,
			    const struct react_step *steps,
			    int success,
			    const struct memory_options *opts);

int memory_clear(struct db *db, int64_t session_id,
		 enum memory_clear_scope scope);

#ifdef __cplusplus
}
#endif

#endif
