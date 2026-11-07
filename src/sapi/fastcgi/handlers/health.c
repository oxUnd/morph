/* health.c — liveness probe */
#include "handlers.h"
#include "sapi/fastcgi/storage/users.h"
#include "../agent_bridge.h"
#include <stdio.h>
#include <time.h>

void handle_health(request_t *r)
{
	struct fcgi_bridge_pool_status pool;
	unsigned long long total = 0, active = 0, ok = 0, client = 0;
	unsigned long long server_errors = 0;
	char buf[512];

	fcgi_bridge_pool_status(r->bridge, &pool);
	if (r->metrics) {
		total = __atomic_load_n(&r->metrics->requests_total, __ATOMIC_RELAXED);
		active = __atomic_load_n(&r->metrics->active, __ATOMIC_RELAXED);
		ok = __atomic_load_n(&r->metrics->responses_2xx, __ATOMIC_RELAXED);
		client = __atomic_load_n(&r->metrics->responses_4xx, __ATOMIC_RELAXED);
		server_errors = __atomic_load_n(&r->metrics->responses_5xx, __ATOMIC_RELAXED);
	}
	snprintf(buf, sizeof(buf),
		 "{\"status\":\"ok\",\"setup_required\":%s,\"ts\":%lld,"
		 "\"runtime_pool\":{\"workers\":%d,\"busy\":%d,"
		 "\"starting\":%d,\"min\":%d,\"max\":%d,"
		 "\"waiting\":%d,\"queue_max\":%d,\"idle_seconds\":%d},"
		 "\"metrics\":{\"requests_total\":%llu,\"active\":%llu,"
		 "\"2xx\":%llu,\"4xx\":%llu,\"5xx\":%llu}}",
		 store_setup_required(r->store) ? "true" : "false", (long long)time(NULL),
		 pool.workers, pool.busy_workers, pool.starting_workers, pool.min_workers,
		 pool.max_workers, pool.waiting_turns, pool.queue_max, pool.idle_seconds, total,
		 active, ok, client, server_errors);
	reply_json(r, 200, buf);
}
