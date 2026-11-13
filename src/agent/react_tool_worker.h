#ifndef MORPH_REACT_TOOL_WORKER_H
#define MORPH_REACT_TOOL_WORKER_H

#include "react.h"

#include "nb/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private worker state. ReAct owns the batch until every worker is joined;
 * retirement closes borrowed request callbacks before control can return. */
struct async_tool_call {
	struct tool_registry *tools;
	struct react_context *react;
	struct tool_runtime_context runtime;
	struct async_tool_call *retired_next;
	nb_thread *thread;
	int exited;
	int64_t started_ms;
	int callbacks_closed;
	int callbacks_active;
	char *tool_name;
	char *tool_args;
	char *tool_call_id;
	char *provider_tool_call_id;
	char *result;
	cJSON *data;
	cJSON *ui;
	cJSON *meta;
	struct tool_artifact_list artifacts;
	int rc;
	react_output_cb output_cb;
	void *output_user_data;
	nb_atomic_int completed;
	nb_atomic_int cancelled;
	nb_atomic_int start_released;
	struct nb_cancel cancel_token;
	nb_mutex *mutex;
	nb_cond *cond;
};

void react_tool_worker_retire(struct react_context *ctx, struct async_tool_call *call,
			      nb_thread *thread);
struct async_tool_call *react_tool_worker_create(struct tool_registry *tools,
						 struct react_context *react, const char *tool_name,
						 const char *tool_args, const char *tool_call_id,
						 const char *provider_tool_call_id,
						 react_output_cb output_cb, void *output_user_data);
void react_tool_worker_destroy(struct async_tool_call *call);
int react_tool_worker_join(struct react_context *ctx, nb_thread *thread, nb_atomic_int *cancelled,
			   struct async_tool_call *call, int timeout_seconds);
/* Start returns a negative pthread error. Release follows the running event. */
int react_tool_worker_start(struct async_tool_call *call, nb_thread **thread, int require_user);
void react_tool_worker_release(struct async_tool_call *call);
void react_tool_worker_cancel(struct async_tool_call *call);
/* After joining, collect transfers heap results/JSON to the caller.
 * NULL optional outputs remain worker-owned. Destroy frees uncollected payloads;
 * artifacts are inline value data. */
void react_tool_worker_collect(struct async_tool_call *call, int *rc, char **result,
			       struct tool_artifact_list *artifacts, cJSON **data, cJSON **ui,
			       cJSON **meta);

#ifdef __cplusplus
}
#endif

#endif
