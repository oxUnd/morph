/* stream_pool.c — bounded worker pool for long-lived FastCGI streams */
#include "stream_pool.h"

#include "util/error.h"

#include <errno.h>
#include <pthread.h>
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
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_t *threads;
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

		snprintf(body, sizeof(body),
			 "{\"error\":\"%s\"}",
			 status == -EAGAIN ? "stream_queue_full" : "shutting_down");
		reply_json(&job->r, status == -EAGAIN ? 503 : 503, body);
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

		pthread_mutex_lock(&pool->lock);
		while (!pool->head && !pool->stopping)
			pthread_cond_wait(&pool->changed, &pool->lock);
		if (!pool->head && pool->stopping) {
			pthread_mutex_unlock(&pool->lock);
			break;
		}
		job = stream_job_pop(pool);
		stopping = pool->stopping;
		pool->active++;
		pthread_mutex_unlock(&pool->lock);

		if (stopping)
			stream_job_finish(job, -ESHUTDOWN);
		else {
			job->fn(&job->r);
			FCGX_Finish_r(job->req);
			free(job->req);
			free(job);
		}

		pthread_mutex_lock(&pool->lock);
		pool->active--;
		pthread_cond_broadcast(&pool->changed);
		pthread_mutex_unlock(&pool->lock);
	}
	return NULL;
}

struct fcgi_stream_pool *fcgi_stream_pool_create(int threads, int queue_max,
						 struct morph_log *logger)
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
	error = pthread_mutex_init(&pool->lock, NULL);
	if (error) {
		free(pool);
		return NULL;
	}
	error = pthread_cond_init(&pool->changed, NULL);
	if (error) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
		return NULL;
	}
	pool->threads = calloc((size_t)threads, sizeof(*pool->threads));
	if (!pool->threads) {
		pthread_cond_destroy(&pool->changed);
		pthread_mutex_destroy(&pool->lock);
		free(pool);
		return NULL;
	}
	pool->thread_count = threads;
	pool->queue_max = queue_max;
	for (; started < threads; started++) {
		error = pthread_create(&pool->threads[started], NULL,
				       stream_worker_main, pool);
		if (error)
			break;
	}
	if (started == 0) {
		free(pool->threads);
		pthread_cond_destroy(&pool->changed);
		pthread_mutex_destroy(&pool->lock);
		free(pool);
		return NULL;
	}
	pool->thread_count = started;
	return pool;
}

int fcgi_stream_pool_submit(struct fcgi_stream_pool *pool, request_t *r,
			    fcgi_stream_fn fn)
{
	struct stream_job *job;

	if (!pool || !r || !r->fcgx || !fn)
		return -EINVAL;
	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->req = r->fcgx;
	job->r = *r;
	job->r.fcgx = job->req;
	job->fn = fn;

	pthread_mutex_lock(&pool->lock);
	if (pool->stopping) {
		pthread_mutex_unlock(&pool->lock);
		free(job);
		return -ESHUTDOWN;
	}
	if (pool->queued >= pool->queue_max) {
		pthread_mutex_unlock(&pool->lock);
		free(job);
		return -EAGAIN;
	}
	if (pool->tail)
		pool->tail->next = job;
	else
		pool->head = job;
	pool->tail = job;
	pool->queued++;
	pthread_cond_signal(&pool->changed);
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

void fcgi_stream_pool_destroy(struct fcgi_stream_pool *pool)
{
	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->stopping = 1;
	pthread_cond_broadcast(&pool->changed);
	pthread_mutex_unlock(&pool->lock);
	for (int i = 0; i < pool->thread_count; i++)
		pthread_join(pool->threads[i], NULL);
	free(pool->threads);
	pthread_cond_destroy(&pool->changed);
	pthread_mutex_destroy(&pool->lock);
	free(pool);
}

void fcgi_stream_pool_status(struct fcgi_stream_pool *pool, int *active,
			     int *queued, int *capacity)
{
	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	if (active)
		*active = pool->active;
	if (queued)
		*queued = pool->queued;
	if (capacity)
		*capacity = pool->queue_max;
	pthread_mutex_unlock(&pool->lock);
}
