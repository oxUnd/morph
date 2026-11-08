#ifndef SUB_AGENT_H
#define SUB_AGENT_H

struct morph_log;
#ifdef __cplusplus
extern "C" {
#endif

#include "config/config.h"
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/context.h"
#include "db/database.h"
#include "util/array.h"
#include "util/queue.h"
#include <pthread.h>

#define SUB_AGENT_TASK_MAX 8
#define SUB_AGENT_TASK_ID_MAX 32
#define SUB_AGENT_MAX_DEPTH 2

enum sub_agent_task_status {
	SUB_AGENT_PENDING,
	SUB_AGENT_RUNNING,
	SUB_AGENT_COMPLETED,
	SUB_AGENT_FAILED,
	SUB_AGENT_CANCELLED
};

struct sub_agent_entry {
	struct config_sub_agent cfg;
	char *system_prompt;
	struct model *llm;
};

/* Owns both the agent and its independently retained tool registry. */
struct sub_agent_context {
	struct react_context *react;
	struct tool_registry *tools;
};

struct tool_runtime_context;

struct sub_agent_task {
	morph_queue_t link;
	int depth;
	char id[SUB_AGENT_TASK_ID_MAX];
	int64_t parent_session_id;
	int64_t child_session_id;
	int agent_index;
	char mode[16];
	char *task_description;
	enum sub_agent_task_status status;
	char *result;
	int error_code;
	int iteration_count;
	int64_t started_at_ms;
	int64_t ended_at_ms;
	morph_array_t events;
	int events_initialized;
	/* Borrowed from the running context; task mutex protects publication. */
	struct react_context *child_ctx;
	pthread_t thread;
	int thread_started;
	int joined;
	int slot_held;
	pthread_mutex_t mutex;
	int mutex_initialized;
};

struct sub_agent_trace_event {
	char trace_id[36];
	char parent_trace_id[36];
	char agent_name[SUB_AGENT_NAME_MAX];
	int64_t start_ms;
	int64_t end_ms;
	char mode[16];
	int iteration_count;
	int token_usage;
	char *result_preview;
};

struct sub_agent_runtime {
	struct morph_log *logger;
	struct sub_agent_entry entries[SUB_AGENT_MAX];
	int entry_count;
	/* Stable task owners outlive their active execution slots. */
	morph_queue_t tasks;
	int task_count;
	int active_task_count;
	int next_task_id;
	char active_task_id[SUB_AGENT_TASK_ID_MAX];
	struct tool_registry *parent_tools;
	struct model *default_llm;
	struct tokenizer *tokenizer;
	struct compress_config *compress;
	morph_event_cb event_cb;
	void *event_user_data;
	struct db *db;
	int64_t parent_session_id;
	pthread_mutex_t mutex;
	pthread_mutex_t storage_mutex;
	int mutexes_initialized;
	int depth;
	int auto_approve_readonly;
	int auto_approve_media;
	char trace_file[PATH_MAX];
};

struct sub_agent_task_info {
	char id[SUB_AGENT_TASK_ID_MAX];
	char agent_name[SUB_AGENT_NAME_MAX];
	char mode[16];
	char *description;
	char *result;
	enum sub_agent_task_status status;
	int error_code;
	int iteration_count;
	int64_t parent_session_id;
	int64_t child_session_id;
	int64_t started_at_ms;
	int64_t ended_at_ms;
};

struct sub_agent_runtime *
sub_agent_runtime_create(struct tool_registry *parent_tools,
			 struct model *default_llm,
			 struct tokenizer *tokenizer,
			 struct compress_config *compress);

/* Stop submitters and synchronous calls before destruction. Delegate workers
 * are cancelled and joined before their retained task records are released. */
void sub_agent_runtime_destroy(struct sub_agent_runtime *rt);
void sub_agent_runtime_cancel(struct sub_agent_runtime *rt);

int sub_agent_runtime_set_event_callback(struct sub_agent_runtime *rt,
					 morph_event_cb cb, void *user);

int sub_agent_runtime_set_storage(struct sub_agent_runtime *rt,
				  struct db *db);
int sub_agent_runtime_set_parent_session(struct sub_agent_runtime *rt,
					 int64_t session_id);
int sub_agent_runtime_select_task(struct sub_agent_runtime *rt,
				  const char *task_id);
/* Snapshot constructors require empty pointer outputs. Failure leaves them NULL.
 * Returned arrays own every string; pair with the matching free function. */
int sub_agent_runtime_task_list_copy(struct sub_agent_runtime *rt, int64_t parent_session_id,
				     struct sub_agent_task_info **out, int *count);
void sub_agent_runtime_free_task_list(struct sub_agent_task_info *tasks,
				      int count);
int sub_agent_runtime_task_events_copy(struct sub_agent_runtime *rt, const char *task_id,
				       char ***events, int *count);
void sub_agent_runtime_free_events(char **events, int count);

int sub_agent_runtime_load_config(struct sub_agent_runtime *rt,
				  struct config_sub_agents *cfg);

/* Borrows an entry until runtime destruction or config replacement. */
struct sub_agent_entry *sub_agent_borrow(struct sub_agent_runtime *rt, const char *name);

struct tool_registry *sub_agent_tool_registry_create(struct sub_agent_runtime *rt,
						     struct sub_agent_entry *entry);

/* Creates both owners in a zero-initialized output; failure leaves it empty. */
int sub_agent_context_create(struct sub_agent_runtime *rt,
			     const struct tool_runtime_context *runtime,
			     struct sub_agent_entry *entry, const char *task,
			     struct sub_agent_context *out);
void sub_agent_context_cleanup(struct sub_agent_context *context);

/* String outputs are independent malloc owners and must be NULL on entry.
 * Failed calls leave string outputs empty. Delegate permits a NULL output. */
int sub_agent_invoke_sync(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
			  struct sub_agent_entry *entry, const char *task, char **result);

int sub_agent_delegate(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
		       const char *agent_name, const char *task, char **task_id_out);

int sub_agent_fanout(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
		     const char *agent_name, const char **tasks, int task_count,
		     enum sub_agent_merge_strategy merge, char **result);

int sub_agent_check_status(struct sub_agent_runtime *rt,
			   const char *task_id,
			   enum sub_agent_task_status *status_out,
			   char **result_out);

int sub_agent_apply_output_schema(const char *text,
				  const char *schema,
				  struct model *llm,
				  char **result);

void sub_agent_trace_write(struct sub_agent_runtime *rt,
			   struct sub_agent_trace_event *ev);

#ifdef __cplusplus
}
#endif

#endif
