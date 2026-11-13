/* stream_pool.c — bounded worker pool for long-lived FastCGI streams */
#include "stream_pool.h"

#include "errors.h"

#include <errno.h>
#include "nb/thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct stream_job {
	struct stream_job *next;
	FCGX_Request *req;
	request_t r;
	fcgi_stream_fn fn;
};

struct fcgi_stream_pool {
	nb_mutex *lock;
	nb_cond *changed;
	nb_thread **threads;
	int thread_count;
	struct stream_job *head;
	struct stream_job *tail;
	int queued;
	int queue_max;
	int active;
	int stopping;
};

static struct stream_job *stream_job_pop(struct fcgi_stream_pool *pool)
{
	struct stream_job *job = pool->head;

	if (!job)
		return NULL;
	pool->head = job->next;
	if (!pool->head)
		pool->tail = NULL;
	pool->queued--;
	return job;
}

static void stream_job_finish(struct stream_job *job, int status)
{
	if (status != 0) {
		char body[96];
		const char *error = status == NB_ERR_AGAIN ? "stream_queue_full" : "shutting_down";

		snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
		reply_json(&job->r, 503, body);
	}
	FCGX_Finish_r(job->req);
	free(job->req);
	free(job);
}

static void *stream_worker_main(void *arg)
{
	struct fcgi_stream_pool *pool = arg;

	for (;;) {
		struct stream_job *job;
		int stopping;

		nb_mutex_lock(pool->lock);
		while (!pool->head && !pool->stopping)
			nb_cond_wait(pool->changed, pool->lock);
		if (!pool->head && pool->stopping) {
			nb_mutex_unlock(pool->lock);
			break;
		}
		job = stream_job_pop(pool);
		stopping = pool->stopping;
		pool->active++;
		nb_mutex_unlock(pool->lock);

		if (stopping)
			stream_job_finish(job, NB_ERR_CLOSED);
		else {
			job->fn(&job->r);
			FCGX_Finish_r(job->req);
			free(job->req);
			free(job);
		}

		nb_mutex_lock(pool->lock);
		pool->active--;
		nb_cond_broadcast(pool->changed);
		nb_mutex_unlock(pool->lock);
	}
	return NULL;
}

struct fcgi_stream_pool *fcgi_stream_pool_create(int threads, int queue_max, struct nb_log *logger)
{
	struct fcgi_stream_pool *pool;
	int started = 0;
	int error;

	(void)logger;
	if (threads < 1)
		threads = 1;
	if (queue_max < 1)
		queue_max = 1;
	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return NULL;
	error = ((pool->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (error) {
		free(pool);
		return NULL;
	}
	error = ((pool->changed = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM);
	if (error) {
		(nb_mutex_free(pool->lock), pool->lock = NULL);
		free(pool);
		return NULL;
	}
	pool->threads = calloc((size_t)threads, sizeof(*pool->threads));
	if (!pool->threads) {
		(nb_cond_free(pool->changed), pool->changed = NULL);
		(nb_mutex_free(pool->lock), pool->lock = NULL);
		free(pool);
		return NULL;
	}
	pool->thread_count = threads;
	pool->queue_max = queue_max;
	for (; started < threads; started++) {
		error = nb_thread_create(&pool->threads[started], stream_worker_main, pool, NULL);
		if (error)
			break;
	}
	if (started == 0) {
		free(pool->threads);
		(nb_cond_free(pool->changed), pool->changed = NULL);
		(nb_mutex_free(pool->lock), pool->lock = NULL);
		free(pool);
		return NULL;
	}
	pool->thread_count = started;
	return pool;
}

int fcgi_stream_pool_submit_take(struct fcgi_stream_pool *pool, request_t **owner,
				 fcgi_stream_fn fn)
{
	struct stream_job *job;
	request_t *r;

	if (!pool || !owner || !*owner || !(*owner)->fcgx || !fn)
		return NB_ERR_INVAL;
	r = *owner;
	job = calloc(1, sizeof(*job));
	if (!job)
		return NB_ERR_NOMEM;
	job->req = r->fcgx;
	job->r = *r;
	job->r.fcgx = job->req;
	job->fn = fn;

	nb_mutex_lock(pool->lock);
	if (pool->stopping) {
		nb_mutex_unlock(pool->lock);
		free(job);
		return NB_ERR_CLOSED;
	}
	if (pool->queued >= pool->queue_max) {
		nb_mutex_unlock(pool->lock);
		free(job);
		return NB_ERR_AGAIN;
	}
	if (pool->tail)
		pool->tail->next = job;
	else
		pool->head = job;
	pool->tail = job;
	pool->queued++;
	r->fcgx = NULL;
	r->method = NULL;
	r->path = NULL;
	r->query = NULL;
	r->auth_hdr = NULL;
	r->trust_user = NULL;
	r->last_event_id = NULL;
	r->content_type = NULL;
	r->handed_off = 1;
	*owner = NULL;
	nb_cond_signal(pool->changed);
	nb_mutex_unlock(pool->lock);
	return 0;
}

void fcgi_stream_pool_destroy(struct fcgi_stream_pool *pool)
{
	if (!pool)
		return;
	nb_mutex_lock(pool->lock);
	pool->stopping = 1;
	nb_cond_broadcast(pool->changed);
	nb_mutex_unlock(pool->lock);
	for (int i = 0; i < pool->thread_count; i++)
		nb_thread_join_take(&pool->threads[i], NULL);
	free(pool->threads);
	(nb_cond_free(pool->changed), pool->changed = NULL);
	(nb_mutex_free(pool->lock), pool->lock = NULL);
	free(pool);
}

void fcgi_stream_pool_status(struct fcgi_stream_pool *pool, int *active,
			     int *queued, int *capacity)
{
	if (!pool)
		return;
	nb_mutex_lock(pool->lock);
	if (active)
		*active = pool->active;
	if (queued)
		*queued = pool->queued;
	if (capacity)
		*capacity = pool->queue_max;
	nb_mutex_unlock(pool->lock);
}
