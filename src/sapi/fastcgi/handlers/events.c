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
#include "../stream_pool.h"
#include "util/signals.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SSE_HEARTBEAT_SEC   15
#define SSE_MAX_LIFETIME    3600

static void sse_serve(request_t *r)
{
	const char *sid = path_param(r, "id");
	int64_t last_id = 0;
	char *end = NULL;

	if (!sid)
		return;
	if (r->last_event_id && *r->last_event_id) {
		long long parsed = strtoll(r->last_event_id, &end, 10);

		if (end && *end == '\0' && parsed >= 0)
			last_id = (int64_t)parsed;
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

		if (r->shutdown && morph_signal_flag_get(r->shutdown))
			break;
		int got = events_wait_after(r->store, sid, last_id,
					    SSE_HEARTBEAT_SEC, &rec);
		if (got < 0) break;
		if (r->shutdown && morph_signal_flag_get(r->shutdown)) {
			free(rec.payload_json);
			break;
		}
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

void handle_sse(request_t *r)
{
	const char *sid = path_param(r, "id");
	int rc;

	if (!sid) { reply_400(r, "missing id"); return; }
	if (!store_session_owned_by(r->store, sid, r->user_id)) {
		reply_403(r);
		return;
	}
	if (!r->streams) {
		reply_500(r, "stream pool unavailable");
		return;
	}
	rc = fcgi_stream_pool_submit(r->streams, r, sse_serve);
	if (rc == 0) {
		r->handed_off = 1;
		return;
	}
	if (rc == -EAGAIN) {
		reply_json(r, 503, "{\"error\":\"stream_queue_full\"}");
		return;
	}
	if (rc == -ESHUTDOWN) {
		reply_json(r, 503, "{\"error\":\"shutting_down\"}");
		return;
	}
	reply_500(r, "stream unavailable");
}
