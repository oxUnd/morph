/* stream_pool.h — bounded worker pool for long-lived FastCGI streams */
#ifndef MORPH_FCGI_STREAM_POOL_H
#define MORPH_FCGI_STREAM_POOL_H

#include "fcgi_io.h"

struct morph_log;
struct fcgi_stream_pool;

typedef void (*fcgi_stream_fn)(request_t *r);

struct fcgi_stream_pool *fcgi_stream_pool_create(int threads, int queue_max,
						 struct morph_log *logger);
void fcgi_stream_pool_destroy(struct fcgi_stream_pool *pool);

/* Takes ownership of the FastCGI request on success.  Returns 0, -EAGAIN
 * when the queue is full, or -ESHUTDOWN while stopping. */
int fcgi_stream_pool_submit(struct fcgi_stream_pool *pool, request_t *r,
			    fcgi_stream_fn fn);

void fcgi_stream_pool_status(struct fcgi_stream_pool *pool, int *active,
			     int *queued, int *capacity);

#endif
