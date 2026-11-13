/* stream_pool.h — bounded worker pool for long-lived FastCGI streams */
#ifndef MORPH_FCGI_STREAM_POOL_H
#define MORPH_FCGI_STREAM_POOL_H

#include "fcgi_io.h"

struct nb_log;
struct fcgi_stream_pool;

typedef void (*fcgi_stream_fn)(request_t *r);

struct fcgi_stream_pool *fcgi_stream_pool_create(int threads, int queue_max, struct nb_log *logger);
void fcgi_stream_pool_destroy(struct fcgi_stream_pool *pool);

/* Success takes the wrapper's FCGX_Request, clears wrapper->fcgx, marks it
 * handed_off, and clears *owner. The wrapper storage itself remains borrowed.
 * Failure preserves the wrapper and its request. */
int fcgi_stream_pool_submit_take(struct fcgi_stream_pool *pool, request_t **owner,
				 fcgi_stream_fn fn);

void fcgi_stream_pool_status(struct fcgi_stream_pool *pool, int *active,
			     int *queued, int *capacity);

#endif
