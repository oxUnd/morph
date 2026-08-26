/* health.c — liveness probe */
#include "handlers.h"
#include "../agent_bridge.h"
#include <stdio.h>
#include <time.h>

void handle_health(request_t *r)
{
	struct fcgi_bridge_pool_status pool;
	char buf[512];

	fcgi_bridge_pool_status(&pool);
	snprintf(buf, sizeof(buf),
		 "{\"status\":\"ok\",\"setup_required\":%s,\"ts\":%lld,"
		 "\"runtime_pool\":{\"workers\":%d,\"busy\":%d,"
		 "\"starting\":%d,\"min\":%d,\"max\":%d,"
		 "\"waiting\":%d,\"queue_max\":%d,\"idle_seconds\":%d}}",
		 store_setup_required(r->store) ? "true" : "false",
		 (long long)time(NULL), pool.workers, pool.busy_workers,
		 pool.starting_workers, pool.min_workers, pool.max_workers,
		 pool.waiting_turns, pool.queue_max, pool.idle_seconds);
	reply_200_json(r, buf);
}
