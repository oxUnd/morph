/* health.c — liveness probe */
#include "handlers.h"
#include <stdio.h>
#include <time.h>

void handle_health(request_t *r) {
	char buf[192];
	snprintf(buf, sizeof(buf),
		 "{\"status\":\"ok\",\"setup_required\":%s,\"ts\":%lld}",
		 store_setup_required(r->store) ? "true" : "false",
		 (long long)time(NULL));
	reply_200_json(r, buf);
}
