#ifndef MORPH_EXEC_JOBS_H
#define MORPH_EXEC_JOBS_H

#include "event/event.h"
#include "exec/process.h"

struct exec_jobs;

int exec_jobs_create(struct exec_jobs **out);
void exec_jobs_destroy(struct exec_jobs *jobs);
void exec_jobs_bind(struct exec_jobs *jobs, struct process_manager *manager, morph_event_cb cb,
		    void *user_data);
void exec_jobs_complete(const char *session_id, void *user_data);
int exec_jobs_add(struct exec_jobs *jobs, const char *id, const char *command,
		  const char *description, const char *workdir, int64_t owner, const char *turn_id,
		  int service, int notify_only);
int exec_jobs_list(struct exec_jobs *jobs, int64_t owner, cJSON **out);
int exec_jobs_read_retained(struct exec_jobs *jobs, int64_t owner, const char *id, cJSON **out);
int exec_jobs_read(struct exec_jobs *jobs, int64_t owner, const char *id, cJSON **out);

int exec_jobs_stop(struct exec_jobs *jobs, int64_t owner, const char *id);
void exec_jobs_cancel_turn(struct exec_jobs *jobs, int64_t owner, const char *turn_id);
int exec_jobs_authorize(struct exec_jobs *jobs, const char *id, int64_t owner);
void exec_jobs_observed(struct exec_jobs *jobs, const char *id);
int exec_jobs_wait(struct exec_jobs *jobs, int64_t owner, const char *turn_id,
		   int (*cancelled)(void *), void *cancel_data, cJSON **out);

#endif
