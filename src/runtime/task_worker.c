#include "runtime/task_worker.h"

#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>
#include <time.h>

int runtime_task_run_due_collect(struct db *db, int limit,
				 scheduled_task_runner_fn runner,
				 void *runner_user_data,
				 runtime_task_notification_fn notification_cb,
				 void *notification_user_data)
{
	struct notification *notifications = NULL;
	int count = 0;
	int rc;
	int64_t now;

	if (!db || !runner)
		MORPH_RETURN(-EINVAL);
	if (limit <= 0)
		limit = 20;
	now = (int64_t)time(NULL);
	rc = scheduled_task_recover_stale_running(db, now, 15 * 60, NULL);
	if (rc != 0)
		return rc;
	rc = scheduled_task_run_due_collect_with_runner(
		db, now, limit, runner, runner_user_data,
		&notifications, &count);
	if (rc != 0)
		return rc;
	if (notification_cb) {
		for (int i = 0; i < count; i++)
			notification_cb(&notifications[i],
					notification_user_data);
	}
	notification_free_list(notifications, count);
	return count;
}

static void *runtime_task_worker_main(void *arg)
{
	struct runtime_task_worker *worker = arg;
	struct db scheduler_db;
	int opened = 0;

	memset(&scheduler_db, 0, sizeof(scheduler_db));
	if (!worker || !worker->db_path[0])
		return NULL;
	if (db_open(&scheduler_db, worker->db_path) == 0 &&
	    db_init_schema(&scheduler_db) == 0) {
		opened = 1;
	} else {
		log_warn("runtime: task worker failed to open database");
	}

	while (opened) {
		int rc;

		pthread_mutex_lock(&worker->lock);
		if (worker->stop) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		pthread_mutex_unlock(&worker->lock);

		rc = runtime_task_run_due_collect(
			&scheduler_db, worker->limit, worker->runner,
			worker->runner_user_data, worker->notification_cb,
			worker->notification_user_data);
		if (rc < 0)
			log_warn("runtime: task worker failed: %s",
				 morph_strerror(rc));

		pthread_mutex_lock(&worker->lock);
		if (!worker->stop) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			(void)pthread_cond_timedwait(&worker->cond,
						     &worker->lock, &ts);
		}
		if (worker->stop) {
			pthread_mutex_unlock(&worker->lock);
			break;
		}
		pthread_mutex_unlock(&worker->lock);
	}

	if (opened)
		db_close(&scheduler_db);
	return NULL;
}

int runtime_task_worker_start(struct runtime_task_worker *worker,
			      const struct db *db,
			      scheduled_task_runner_fn runner,
			      void *runner_user_data,
			      runtime_task_notification_fn notification_cb,
			      void *notification_user_data,
			      int limit)
{
	int rc;

	if (!worker || !db || !db->path[0] || !runner)
		MORPH_RETURN(-EINVAL);
	if (worker->started)
		return 0;
	memset(worker, 0, sizeof(*worker));
	strncpy(worker->db_path, db->path, sizeof(worker->db_path) - 1);
	worker->runner = runner;
	worker->runner_user_data = runner_user_data;
	worker->notification_cb = notification_cb;
	worker->notification_user_data = notification_user_data;
	worker->limit = limit > 0 ? limit : 50;
	rc = pthread_mutex_init(&worker->lock, NULL);
	if (rc != 0)
		return -rc;
	rc = pthread_cond_init(&worker->cond, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&worker->lock);
		return -rc;
	}
	rc = pthread_create(&worker->thread, NULL,
			    runtime_task_worker_main, worker);
	if (rc != 0) {
		pthread_cond_destroy(&worker->cond);
		pthread_mutex_destroy(&worker->lock);
		return -rc;
	}
	worker->started = 1;
	log_info("runtime: task worker started");
	return 0;
}

void runtime_task_worker_stop(struct runtime_task_worker *worker)
{
	if (!worker || !worker->started)
		return;
	pthread_mutex_lock(&worker->lock);
	worker->stop = 1;
	pthread_cond_signal(&worker->cond);
	pthread_mutex_unlock(&worker->lock);
	pthread_join(worker->thread, NULL);
	pthread_cond_destroy(&worker->cond);
	pthread_mutex_destroy(&worker->lock);
	worker->started = 0;
	worker->stop = 0;
	log_info("runtime: task worker stopped");
}
