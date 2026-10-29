#ifndef MORPH_RUNTIME_INTERNAL_H
#define MORPH_RUNTIME_INTERNAL_H

#include "runtime/context_owner.h"
#include "runtime/execute.h"
#include "runtime/runtime.h"
#include "runtime/task_worker.h"

struct runtime_mcp_job {
	struct runtime *runtime;
	struct mcp_client *client;
	pthread_t thread;
	pthread_mutex_t lock;
	struct morph_cancel_token cancel;
	enum runtime_mcp_state state;
	int error_code;
	int started;
	int active;
	int64_t started_ms;
	int64_t elapsed_ms;
};

#define RUNTIME_TITLE_JOB_MAX 4

struct runtime_title_job;
struct runtime_recap_job;

struct runtime {
	struct runtime_context context;
	struct runtime_options options;
	struct scheduled_task_event_sink task_events;
	struct runtime_task_worker task_worker;
	struct morph_sync_worker sync_worker;
	int sync_started;
	struct runtime_mcp_job mcp_jobs[MCP_MAX_SERVERS];
	int mcp_job_count;
	struct runtime_title_job *title_jobs[RUNTIME_TITLE_JOB_MAX];
	struct runtime_recap_job *recap_job;
	uint64_t recap_revision;
	pthread_mutex_t jobs_lock;
	int jobs_lock_ready;
	struct exec_jobs *jobs;
};

int runtime_jobs_init(struct runtime *runtime);
void runtime_jobs_cleanup(struct runtime *runtime);

void runtime_recap_invalidate(struct runtime *runtime);
void runtime_recap_stop(struct runtime *runtime);

/* Start/cancel run under execution_lock; shutdown joins before freeing runtime. */
int runtime_title_start(struct runtime *runtime, const struct runtime_request *request);
void runtime_title_cancel(struct runtime *runtime, int64_t session_id);
void runtime_title_stop(struct runtime *runtime);

int runtime_mcp_background_start(struct runtime *runtime);
void runtime_mcp_background_stop(struct runtime *runtime);
struct runtime_mcp_job *runtime_mcp_job_find(struct runtime *runtime,
	struct mcp_client *client);
int64_t runtime_mcp_now_ms(void);

int runtime_lock_turn(struct runtime_engine *engine,
		      const struct runtime_request *request,
		      struct runtime_result *result);
void runtime_unlock_turn(struct runtime_engine *engine);

int runtime_prepare_turn(void *user_data,
			 const struct runtime_request *request);
void runtime_finish_turn(void *user_data,
			  const struct runtime_request *request,
			  const struct runtime_result *result);
void runtime_record_usage(const struct model_usage *usage, void *user_data);

int runtime_ensure_current_session(struct runtime *runtime);

#endif
