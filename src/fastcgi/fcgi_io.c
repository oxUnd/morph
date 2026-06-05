#define _GNU_SOURCE
#include "fcgi_io.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fcgi_read_body(request_t *r, char **out, size_t *len) {
	*out = NULL;
	*len = 0;
	if (r->content_length <= 0) {
		*out = calloc(1, 1);
		return *out ? 0 : -ENOMEM;
	}
	if (r->content_length > FCGI_MAX_BODY_BYTES) MORPH_RETURN(-EFBIG);

	char *buf = malloc((size_t)r->content_length + 1);
	if (!buf) MORPH_RETURN(-ENOMEM);

	int got = FCGX_GetStr(buf, r->content_length, r->fcgx->in);
	if (got != r->content_length) { free(buf); MORPH_RETURN(-EIO); }
	buf[got] = '\0';
	*out = buf;
	*len = (size_t)got;
	return 0;
}

const char *path_param(const request_t *r, const char *key) {
	for (int i = 0; i < r->n_params; i++)
		if (strcmp(r->params[i].key, key) == 0)
			return r->params[i].val;
	return NULL;
}

static const char *status_text(int code) {
	switch (code) {
	case 200: return "200 OK";
	case 201: return "201 Created";
	case 202: return "202 Accepted";
	case 204: return "204 No Content";
	case 400: return "400 Bad Request";
	case 401: return "401 Unauthorized";
	case 403: return "403 Forbidden";
	case 404: return "404 Not Found";
	case 409: return "409 Conflict";
	case 429: return "429 Too Many Requests";
	case 500: return "500 Internal Server Error";
	default:  return "500 Internal Server Error";
	}
}

void reply_json(request_t *r, int status, const char *body) {
	size_t blen = body ? strlen(body) : 0;
	FCGX_FPrintF(r->fcgx->out,
		"Status: %s\r\n"
		"Content-Type: application/json; charset=utf-8\r\n"
		"Content-Length: %lu\r\n"
		"Cache-Control: no-store\r\n"
		"\r\n",
		status_text(status), (unsigned long)blen);
	if (blen) FCGX_PutStr(body, (int)blen, r->fcgx->out);
}

void reply_200_json(request_t *r, const char *body) { reply_json(r, 200, body); }
void reply_201_json(request_t *r, const char *body) { reply_json(r, 201, body); }
void reply_202_json(request_t *r, const char *body) { reply_json(r, 202, body); }
void reply_204(request_t *r) {
	FCGX_FPrintF(r->fcgx->out, "Status: 204 No Content\r\n\r\n");
}
void reply_400(request_t *r, const char *msg) {
	char buf[256];
	snprintf(buf, sizeof(buf), "{\"error\":\"bad_request\",\"message\":\"%s\"}",
		 msg ? msg : "");
	reply_json(r, 400, buf);
}
void reply_401(request_t *r) { reply_json(r, 401, "{\"error\":\"unauthorized\"}"); }
void reply_403(request_t *r) { reply_json(r, 403, "{\"error\":\"forbidden\"}"); }
void reply_404(request_t *r) { reply_json(r, 404, "{\"error\":\"not_found\"}"); }
void reply_500(request_t *r, const char *msg) {
	char buf[256];
	snprintf(buf, sizeof(buf), "{\"error\":\"internal\",\"message\":\"%s\"}",
		 msg ? msg : "");
	reply_json(r, 500, buf);
}

void sse_write_headers(request_t *r) {
	FCGX_FPrintF(r->fcgx->out,
		"Status: 200 OK\r\n"
		"Content-Type: text/event-stream; charset=utf-8\r\n"
		"Cache-Control: no-cache, no-transform\r\n"
		"Connection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n"
		"\r\n");
	FCGX_FFlush(r->fcgx->out);
}

void sse_write_event(request_t *r, int64_t id, const char *type,
		     const char *json_payload) {
	FCGX_FPrintF(r->fcgx->out,
		"id: %lld\nevent: %s\ndata: %s\n\n",
		(long long)id,
		type ? type : "message",
		json_payload ? json_payload : "{}");
}

void sse_write_heartbeat(request_t *r) {
	FCGX_FPrintF(r->fcgx->out, ": hb %lld\n\n", (long long)time(NULL));
}

int sse_flush(request_t *r) {
	if (FCGX_FFlush(r->fcgx->out) < 0) MORPH_RETURN(-EIO);
	if (FCGX_GetError(r->fcgx->out)  != 0) MORPH_RETURN(-EIO);
	return 0;
}
