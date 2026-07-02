#ifndef AGENT_TURN_H
#define AGENT_TURN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/memory.h"
#include "agent/react.h"
#include "db/database.h"
#include <stdint.h>

#define AGENT_TURN_LOAD_HISTORY		(1u << 0)
#define AGENT_TURN_BUILD_MEMORY_CONTEXT	(1u << 1)
#define AGENT_TURN_SAVE_TRACE		(1u << 2)
#define AGENT_TURN_SAVE_MESSAGES	(1u << 3)
#define AGENT_TURN_UPDATE_TOKENS	(1u << 4)
#define AGENT_TURN_CONSOLIDATE_MEMORY	(1u << 5)
#define AGENT_TURN_ASYNC_MEMORY		(1u << 6)
#define AGENT_TURN_SAVE_EMPTY_USER	(1u << 7)
#define AGENT_TURN_SAVE_EMPTY_ASSISTANT	(1u << 8)

#define AGENT_TURN_DEFAULT_FLAGS \
	(AGENT_TURN_LOAD_HISTORY | \
	 AGENT_TURN_BUILD_MEMORY_CONTEXT | \
	 AGENT_TURN_SAVE_TRACE | \
	 AGENT_TURN_SAVE_MESSAGES | \
	 AGENT_TURN_UPDATE_TOKENS | \
	 AGENT_TURN_CONSOLIDATE_MEMORY | \
	 AGENT_TURN_ASYNC_MEMORY)

typedef char *(*agent_turn_render_fn)(const char *text, void *user_data);

typedef int (*agent_turn_background_cb)(void *user_data,
					const char *name,
					const char *phase,
					const char *message,
					const char *task,
					int count,
					int error_code);

struct agent_session_runtime {
	struct db *db;
	int64_t session_id;
	struct react_context *react;
	const struct memory_options *memory_options;
	agent_turn_render_fn render_assistant;
	void *render_user_data;
	agent_turn_background_cb background_cb;
	void *background_user_data;
	unsigned flags;
};

struct agent_turn_input {
	const char *model_input;
	const char *stored_user_input;
	const char *turn_id;
};

struct agent_turn {
	struct agent_session_runtime runtime;
	struct agent_turn_input input;
	int begun;
	int finished;
};

struct agent_turn_result {
	int history_loaded;
	int memory_context_built;
	int trace_saved;
	int user_saved;
	int assistant_saved;
	int user_tokens;
	int assistant_tokens;
	int memory_queued;
	int memory_ran_inline;
	int memory_rc;
};

int agent_session_load_history(const struct agent_session_runtime *runtime);
int agent_turn_begin(struct agent_turn *turn,
		     const struct agent_session_runtime *runtime,
		     const struct agent_turn_input *input);
int agent_turn_finish(struct agent_turn *turn,
		      struct agent_turn_result *result);

#ifdef __cplusplus
}
#endif

#endif
