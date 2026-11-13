#define _GNU_SOURCE
#include "fcgi_io.h"
#include "errors.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fcgi_read_body(request_t *r, char **out, size_t *len)
{
	if (!r || !out || !len)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (r->content_length <= 0) {
		char *empty = calloc(1, 1);
		if (!empty)
			return NB_ERR_NOMEM;
		*out = empty;
		*len = 0;
		return 0;
	}
	if (r->content_length > FCGI_MAX_BODY_BYTES)
		return NB_ERR_TOO_BIG;
	if (!r->fcgx || !r->fcgx->in)
		return NB_ERR_INVAL;

	char *buf = malloc((size_t)r->content_length + 1);
	if (!buf)
		return NB_ERR_NOMEM;

	size_t offset = 0;
	while (offset < (size_t)r->content_length) {
		int got = FCGX_GetStr(buf + offset,
				     (int)((size_t)r->content_length - offset),
				     r->fcgx->in);
		if (got <= 0) {
			free(buf);
			return NB_ERR_IO;
		}
		offset += (size_t)got;
	}
	buf[offset] = '\0';
	*out = buf;
	*len = offset;
	return 0;
}

const char *path_param_borrow(const request_t *r, const char *key)
{
	for (int i = 0; i < r->n_params; i++)
		if (strcmp(r->params[i].key, key) == 0)
			return r->params[i].val;
	return NULL;
}

int64_t query_i64(const request_t *r, const char *key, int64_t def)
{
	const char *p;
	size_t klen;

	if (!r || !key || !r->query)
		return def;
	klen = strlen(key);
	p = r->query;
	while (*p) {
		const char *end = strchr(p, '&');
		size_t seg = end ? (size_t)(end - p) : strlen(p);
		if (seg > klen && p[klen] == '=' &&
		    strncmp(p, key, klen) == 0) {
			char buf[32];
			size_t n = seg - klen - 1;
			char *num_end = NULL;
			long long value;

			if (n >= sizeof(buf))
				n = sizeof(buf) - 1;
			memcpy(buf, p + klen + 1, n);
			buf[n] = '\0';
			errno = 0;
			value = strtoll(buf, &num_end, 10);
			if (errno != 0 || num_end == buf || *num_end != '\0' ||
			    value < 0)
				return def;
			return (int64_t)value;
		}
		if (!end)
			break;
		p = end + 1;
	}
	return def;
}

static const char *status_text(int code)
{
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
	case 413: return "413 Payload Too Large";
	case 429: return "429 Too Many Requests";
	case 500: return "500 Internal Server Error";
	case 503: return "503 Service Unavailable";
	default:  return "500 Internal Server Error";
	}
}

void fcgi_record_status(request_t *r, int status)
{
	r->status = status;
	if (!r->metrics)
		return;
	if (status >= 500)
		__atomic_add_fetch(&r->metrics->responses_5xx, 1,
				   __ATOMIC_RELAXED);
	else if (status >= 400)
		__atomic_add_fetch(&r->metrics->responses_4xx, 1,
				   __ATOMIC_RELAXED);
	else if (status >= 200 && status < 300)
		__atomic_add_fetch(&r->metrics->responses_2xx, 1,
				   __ATOMIC_RELAXED);
}

void reply_json(request_t *r, int status, const char *body)
{
	size_t blen = body ? strlen(body) : 0;
	const char *rid = r->request_id[0] ? r->request_id : "-";

	fcgi_record_status(r, status);
	FCGX_FPrintF(r->fcgx->out,
		"Status: %s\r\n"
		"Content-Type: application/json; charset=utf-8\r\n"
		"Content-Length: %lu\r\n"
		"Cache-Control: no-store\r\n"
		"X-Request-Id: %s\r\n"
		"\r\n",
		status_text(status), (unsigned long)blen, rid);
	if (blen) FCGX_PutStr(body, (int)blen, r->fcgx->out);
}

void reply_200_json(request_t *r, const char *body)
{
	reply_json(r, 200, body);
}

void reply_201_json(request_t *r, const char *body)
{
	reply_json(r, 201, body);
}

void reply_202_json(request_t *r, const char *body)
{
	reply_json(r, 202, body);
}

void reply_204(request_t *r)
{
	const char *rid = r->request_id[0] ? r->request_id : "-";

	fcgi_record_status(r, 204);
	FCGX_FPrintF(r->fcgx->out,
		"Status: 204 No Content\r\n"
		"X-Request-Id: %s\r\n"
		"\r\n", rid);
}

void reply_400(request_t *r, const char *msg)
{
	cJSON *obj = cJSON_CreateObject();
	char *json = NULL;

	if (obj) {
		cJSON_AddStringToObject(obj, "error", "bad_request");
		cJSON_AddStringToObject(obj, "message", msg ? msg : "");
		json = cJSON_PrintUnformatted(obj);
		cJSON_Delete(obj);
	}
	reply_json(r, 400, json ? json : "{\"error\":\"bad_request\"}");
	free(json);
}

void reply_401(request_t *r)
{
	reply_json(r, 401, "{\"error\":\"unauthorized\"}");
}

void reply_403(request_t *r)
{
	reply_json(r, 403, "{\"error\":\"forbidden\"}");
}

void reply_404(request_t *r)
{
	reply_json(r, 404, "{\"error\":\"not_found\"}");
}

void reply_500(request_t *r, const char *msg)
{
	cJSON *obj = cJSON_CreateObject();
	char *json = NULL;

	if (obj) {
		cJSON_AddStringToObject(obj, "error", "internal");
		cJSON_AddStringToObject(obj, "message", msg ? msg : "");
		json = cJSON_PrintUnformatted(obj);
		cJSON_Delete(obj);
	}
	reply_json(r, 500, json ? json : "{\"error\":\"internal\"}");
	free(json);
}

void reply_body_error(request_t *r, int rc)
{
	if (rc == NB_ERR_TOO_BIG)
		reply_json(r, 413,
			   "{\"error\":\"payload_too_large\","
			   "\"max_bytes\":4194304}");
	else if (rc == NB_ERR_NOMEM)
		reply_500(r, "out of memory");
	else
		reply_400(r, "invalid request body");
}

void sse_write_headers(request_t *r)
{
	const char *rid = r->request_id[0] ? r->request_id : "-";

	fcgi_record_status(r, 200);
	FCGX_FPrintF(r->fcgx->out,
		"Status: 200 OK\r\n"
		"Content-Type: text/event-stream; charset=utf-8\r\n"
		"Cache-Control: no-cache, no-transform\r\n"
		"Connection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n"
		"X-Request-Id: %s\r\n"
		"\r\n", rid);
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

void sse_write_heartbeat(request_t *r)
{
	FCGX_FPrintF(r->fcgx->out, ": hb %lld\n\n", (long long)time(NULL));
}

int sse_flush(request_t *r)
{
	if (FCGX_FFlush(r->fcgx->out) < 0)
		return NB_ERR_IO;
	if (FCGX_GetError(r->fcgx->out) != 0)
		return NB_ERR_IO;
	return 0;
}
