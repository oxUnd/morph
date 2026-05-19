/* health.c — liveness probe */
#include "handlers.h"
#include <stdio.h>
#include <time.h>

void handle_health(request_t *r) {
	char buf[128];
	snprintf(buf, sizeof(buf),
		 "{\"status\":\"ok\",\"ts\":%lld}",
		 (long long)time(NULL));
	reply_200_json(r, buf);
}
