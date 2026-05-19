/* fcgi_io.h — FastCGI request/response IO helpers */
#ifndef MORPH_FCGI_IO_H
#define MORPH_FCGI_IO_H

#include <fcgiapp.h>
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

typedef struct request_s {
	FCGX_Request    *fcgx;

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

	struct session_store *store;
} request_t;

int  fcgi_read_body(request_t *r, char **out, size_t *len);
const char *path_param(const request_t *r, const char *key);

void reply_json(request_t *r, int status, const char *body);
void reply_200_json(request_t *r, const char *body);
void reply_201_json(request_t *r, const char *body);
void reply_202_json(request_t *r, const char *body);
void reply_204(request_t *r);
void reply_400(request_t *r, const char *msg);
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
