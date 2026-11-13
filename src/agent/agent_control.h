#ifndef MORPH_AGENT_CONTROL_H
#define MORPH_AGENT_CONTROL_H

#include "config/config.h"
#include "agent/context.h"
#include "agent/tool_runtime.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

#ifdef __cplusplus
extern "C" {
#endif

struct agent_control;
struct react_context;
struct model;
struct plan_registry;

typedef int (*agent_plan_initializer_fn)(struct tool_registry *tools, struct plan_registry *plans,
					 struct model *model);
/* Install before starting turns; runtime owns concrete tool initialization. */
int agent_control_set_plan_initializer(struct agent_control *control,
				       agent_plan_initializer_fn initialize);

/* Runtime-owned per-child resources outlive ReAct and private tool bindings.
 * Initializers may retain initialized state on failure for safe later cleanup. */
struct agent_child_state {
	void *data;
	void (*destroy)(void *data);
};
typedef int (*agent_child_initializer_fn)(struct tool_registry *tools, struct plan_registry *plans,
					  struct react_context *react, struct model *model,
					  const struct tool_runtime_context *runtime,
					  struct agent_child_state *state, void *user_data);
int agent_control_set_child_initializer(struct agent_control *control,
					agent_child_initializer_fn initialize, void *user_data);
void agent_control_set_interactions_blocked(struct agent_control *control, int blocked);

enum agent_status { AGENT_PENDING, AGENT_RUNNING, AGENT_COMPLETED, AGENT_FAILED, AGENT_CANCELLED };

/* Independent snapshot owners; free with agent_control_free_list(). */
struct agent_info {
	char id[256];
	char agent_name[256];
	char mode[16];
	char *description;
	char *activity;
	char *result;
	enum agent_status status;
	int error_code;
	int iteration_count;
	int64_t parent_session_id;
	int64_t child_session_id;
	int64_t started_at_ms;
	int64_t ended_at_ms;
};

void agent_control_patch_begin(struct agent_control *control);
void agent_control_patch_end(struct agent_control *control);
int agent_control_write_check(struct agent_control *control, const char *id, const char *path);
int agent_control_shell_readonly(struct agent_control *control, const char *id);

struct agent_shell_scope {
	char *scratch;
	nb_vec paths;
	char **write_paths;
	int write_path_count;
};
void agent_shell_scope_cleanup(struct agent_shell_scope *scope);
int agent_control_shell_scope_copy(struct agent_control *control, const char *id,
				   const char *scratch_base, struct agent_shell_scope *out);

struct agent_budget {
	int64_t used;
	int64_t limit;
	int64_t request_tokens;
	int64_t input_tokens;
	int64_t cached_tokens;
	int64_t output_tokens;
	int64_t reserved;
	int reminder_level;
	int exhausted;
	int persistence_error;
};
void agent_control_begin_turn(struct agent_control *control, struct react_context *root);
void agent_control_end_turn(struct agent_control *control);
int agent_control_budget_copy(struct agent_control *control, struct agent_budget *out);
int agent_control_budget_check(struct agent_control *control, int64_t estimate);
int agent_control_budget_check_for(struct agent_control *control, const char *id, int64_t estimate);
int agent_control_budget_reserve(struct agent_control *control, const char *id,
				 int64_t input_estimate, int64_t output_maximum,
				 int64_t *reservation);
/* Fit a positive output maximum to the remaining allocation. Both outputs are
 * changed only on success; refusal does not exhaust or cancel the tree. */
int agent_control_budget_reserve_request(struct agent_control *control, const char *id,
					 int64_t input_estimate, int *output_maximum,
					 int64_t *reservation);
void agent_control_budget_release(struct agent_control *control, int64_t reservation);
void agent_control_budget_release_for(struct agent_control *control, const char *id,
				      int64_t reservation);

struct agent_control *agent_control_create(struct tool_registry *tools, struct model *model,
					   struct tokenizer *tokenizer,
					   const struct compress_config *compress,
					   const struct config_agents *config, struct db *db);
void agent_control_destroy(struct agent_control *control);
void agent_control_cancel(struct agent_control *control);
/* Runtime owner calls only while all agents are idle. */
void agent_control_set_proactive(struct agent_control *control, int proactive);
/* Drain before request-scoped callbacks or runtime bindings are released. */
void agent_control_join(struct agent_control *control);
int agent_control_set_parent_session(struct agent_control *control, int64_t session_id);
int agent_control_set_event_callback(struct agent_control *control, morph_event_cb cb, void *user);
int agent_control_select(struct agent_control *control, const char *id);
int agent_control_list_copy(struct agent_control *control, int64_t session_id,
			    struct agent_info **out, int *count);
int agent_control_read_copy(struct agent_control *control, const char *id, struct agent_info **out);
int agent_control_activity_copy(struct agent_control *control, int64_t session_id,
				struct agent_info **out, int *count);
int agent_control_subscribe(struct agent_control *control, const char *id, char ***events,
			    int *count, int64_t *sequence);
void agent_control_free_list(struct agent_info *items, int count);
int agent_control_events_copy(struct agent_control *control, const char *id, char ***out,
			      int *count);
void agent_control_free_events(char **items, int count);

/* Codex MultiAgentV2 tool operations. Outputs must be empty. */
int agent_control_call(struct agent_control *control, const char *operation, const cJSON *arguments,
		       const struct tool_runtime_context *runtime, cJSON **out);
/* Owner-thread safe point. Drains persisted mailbox into an independent string. */
int agent_control_receive(struct agent_control *control, const char *id, int64_t root_session,
			  char **out, int64_t *cursor);
int agent_control_ack(struct agent_control *control, const char *id, int64_t root_session,
		      int64_t cursor, struct db *db);
int agent_control_interrupted(struct agent_control *control, const char *id);
int agent_control_descendants_active(struct agent_control *control, const char *id);
/* Close a subtree to new work and ask its active descendants for final evidence. */
int agent_control_request_finish(struct agent_control *control, const char *id);
/* Includes the child's inherited monotonic task deadline. */
int agent_control_finish_requested(struct agent_control *control, const char *id);
/* Cancel descendants without interrupting the caller or peer branches. */
int agent_control_interrupt_descendants(struct agent_control *control, const char *id);
int agent_control_active(struct agent_control *control, int64_t session_id);
int agent_control_prompt(struct agent_control *control, const char *id, nb_buf *out);
/* Current assignment state belongs after history and is rebuilt for every request. */
int agent_control_checkpoint(struct agent_control *control, const char *id, nb_buf *out);
/* Explicit coordination-only assignments suspend investigation until evidence arrives. */
int agent_control_tool_allowed(struct agent_control *control, const char *id, const char *tool);
/* New user steering can supply parent work beyond the previous assignment. */
void agent_control_resume_work(struct agent_control *control, const char *id);

#ifdef __cplusplus
}
#endif
#endif
