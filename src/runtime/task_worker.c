#include "runtime/task_worker.h"
#include "nb/str.h"
#include "nb/utf8.h"

#include "errors.h"
#include "nb/log.h"

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
		return NB_ERR_INVAL;
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
	if (db_open(worker ? worker->logger : NULL, &scheduler_db, worker->db_path) == 0 &&
	    db_init_schema(&scheduler_db) == 0) {
		opened = 1;
	} else {
		nb_log_write(worker ? worker->logger : NULL, NB_LOG_WARN,
			     "runtime: task worker failed to open database");
	}

	while (opened) {
		int rc;

		nb_mutex_lock(worker->lock);
		if (worker->stop) {
			nb_mutex_unlock(worker->lock);
			break;
		}
		nb_mutex_unlock(worker->lock);

		rc = runtime_task_run_due_collect(
			&scheduler_db, worker->limit, worker->runner,
			worker->runner_user_data, worker->notification_cb,
			worker->notification_user_data);
		if (rc < 0)
			nb_log_write(worker ? worker->logger : NULL, NB_LOG_WARN,
				     "runtime: task worker failed: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));

		nb_mutex_lock(worker->lock);
		if (!worker->stop) {
			(void)nb_cond_timedwait(worker->cond, worker->lock, INT64_C(1000000000));
		}
		if (worker->stop) {
			nb_mutex_unlock(worker->lock);
			break;
		}
		nb_mutex_unlock(worker->lock);
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
		return NB_ERR_INVAL;
	if (worker->started)
		return 0;
	memset(worker, 0, sizeof(*worker));
	worker->logger = db->logger;
	(void)nb_str_copy_to(
		nb_str_from_parts(db->path,
				  nb_utf8_clamp_bytes(db->path, sizeof(worker->db_path) - 1)),
		worker->db_path, (sizeof(worker->db_path) - 1) + 1);
	worker->runner = runner;
	worker->runner_user_data = runner_user_data;
	worker->notification_cb = notification_cb;
	worker->notification_user_data = notification_user_data;
	worker->limit = limit > 0 ? limit : 50;
	rc = ((worker->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = ((worker->cond = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0) {
		(nb_mutex_free(worker->lock), worker->lock = NULL);
		return rc;
	}
	rc = nb_thread_create(&worker->thread, runtime_task_worker_main, worker, NULL);
	if (rc != 0) {
		(nb_cond_free(worker->cond), worker->cond = NULL);
		(nb_mutex_free(worker->lock), worker->lock = NULL);
		return rc;
	}
	worker->started = 1;
	nb_log_write(worker ? worker->logger : NULL, NB_LOG_INFO, "runtime: task worker started");
	return 0;
}

void runtime_task_worker_stop(struct runtime_task_worker *worker)
{
	if (!worker || !worker->started)
		return;
	nb_mutex_lock(worker->lock);
	worker->stop = 1;
	nb_cond_signal(worker->cond);
	nb_mutex_unlock(worker->lock);
	nb_thread_join_take(&worker->thread, NULL);
	(nb_cond_free(worker->cond), worker->cond = NULL);
	(nb_mutex_free(worker->lock), worker->lock = NULL);
	worker->started = 0;
	worker->stop = 0;
	nb_log_write(worker ? worker->logger : NULL, NB_LOG_INFO, "runtime: task worker stopped");
}
