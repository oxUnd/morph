/* fcgi_io.h — FastCGI request/response IO helpers */
#ifndef MORPH_FCGI_IO_H
#define MORPH_FCGI_IO_H

#include <fcgiapp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "session_store.h"

#define FCGI_MAX_BODY_BYTES   (4 * 1024 * 1024)
#define FCGI_MAX_PATH_PARAMS  4

typedef struct {
	char key[32];
	char val[128];
} fcgi_path_param_t;

struct fcgi_bridge;
struct fcgi_auth;
struct fcgi_stream_pool;
struct morph_log;

struct fcgi_metrics {
	unsigned long long requests_total;
	unsigned long long responses_2xx;
	unsigned long long responses_4xx;
	unsigned long long responses_5xx;
	unsigned long long active;
};

typedef struct request_s {
	struct fcgi_bridge *bridge;
	struct fcgi_auth *auth;
	FCGX_Request    *fcgx;
	struct fcgi_stream_pool *streams;
	struct fcgi_metrics     *metrics;
	struct morph_log        *logger;
	volatile sig_atomic_t   *shutdown;
	int               handed_off;
	int               status;
	long long         started_ms;
	char              request_id[64];

	const char      *method;
	const char      *path;
	const char      *query;
	const char      *auth_hdr;
	const char      *trust_user;
	const char      *last_event_id;
	const char      *content_type;
	int              content_length;

	fcgi_path_param_t params[FCGI_MAX_PATH_PARAMS];
	int               n_params;

	char             user_id[64];
	char             username[64];
	char             role[24];

	struct session_store *store;
} request_t;

int  fcgi_read_body(request_t *r, char **out, size_t *len);
const char *path_param(const request_t *r, const char *key);
int64_t query_i64(const request_t *r, const char *key, int64_t def);
void fcgi_record_status(request_t *r, int status);

void reply_json(request_t *r, int status, const char *body);
void reply_200_json(request_t *r, const char *body);
void reply_201_json(request_t *r, const char *body);
void reply_202_json(request_t *r, const char *body);
void reply_204(request_t *r);
void reply_400(request_t *r, const char *msg);
void reply_body_error(request_t *r, int rc);
void reply_401(request_t *r);
void reply_403(request_t *r);
void reply_404(request_t *r);
void reply_500(request_t *r, const char *msg);

void sse_write_headers(request_t *r);
void sse_write_event(request_t *r, int64_t id, const char *type,
		     const char *json_payload);
void sse_write_heartbeat(request_t *r);
int  sse_flush(request_t *r);

#endif
