#ifndef MORPH_RUNTIME_TASK_WORKER_H
#define MORPH_RUNTIME_TASK_WORKER_H

struct nb_log;
#include "agent/tools/scheduled_tasks.h"
#include "db/database.h"
#include "runtime/tasks.h"

#include <limits.h>
#include "nb/thread.h"

struct runtime_task_worker {
	struct nb_log *logger;
	nb_thread *thread;
	nb_mutex *lock;
	nb_cond *cond;
	int started;
	int stop;
	char db_path[PATH_MAX];
	scheduled_task_runner_fn runner;
	void *runner_user_data;
	runtime_task_notification_fn notification_cb;
	void *notification_user_data;
	int limit;
};

int runtime_task_worker_start(struct runtime_task_worker *worker,
			      const struct db *db,
			      scheduled_task_runner_fn runner,
			      void *runner_user_data,
			      runtime_task_notification_fn notification_cb,
			      void *notification_user_data,
			      int limit);
void runtime_task_worker_stop(struct runtime_task_worker *worker);
int runtime_task_run_due_collect(struct db *db, int limit,
				 scheduled_task_runner_fn runner,
				 void *runner_user_data,
				 runtime_task_notification_fn notification_cb,
				 void *notification_user_data);

#endif
