/* events.c — SSE stream
 *
 * GET /api/sessions/:id/events
 *
 * Honours `Last-Event-ID` for resume, sends a 15s heartbeat, hard-caps
 * connection lifetime at 1h to avoid pinning workers indefinitely.
 */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SSE_HEARTBEAT_SEC   15
#define SSE_MAX_LIFETIME    3600

void handle_sse(request_t *r)
{
	const char *sid = path_param(r, "id");
	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) { reply_403(r); return; }

	int64_t last_id = 0;
	if (r->last_event_id && *r->last_event_id) {
		last_id = strtoll(r->last_event_id, NULL, 10);
	}

	sse_write_headers(r);
	/* opening handshake event */
	char hello[128];
	snprintf(hello, sizeof(hello),
		 "{\"resume_from\":%lld,\"ts\":%lld}",
		 (long long)last_id, (long long)time(NULL));
	sse_write_event(r, 0, "ready", hello);
	if (sse_flush(r) != 0) return;

	time_t start = time(NULL);

	while (time(NULL) - start < SSE_MAX_LIFETIME) {
		struct event_record rec = {0};
		int got = events_wait_after(r->store, sid, last_id,
					    SSE_HEARTBEAT_SEC, &rec);
		if (got < 0) break;
		if (got == 1) {
			sse_write_event(r, rec.id, rec.type,
					rec.payload_json ? rec.payload_json : "{}");
			free(rec.payload_json);
			last_id = rec.id;
		} else {
			sse_write_heartbeat(r);
		}
		if (sse_flush(r) != 0) break;
	}
}
