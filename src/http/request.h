#ifndef MORPH_HTTP_REQUEST_H
#define MORPH_HTTP_REQUEST_H

#include <nb/http.h>
#include <nb/log.h>
#include <nb/thread.h>
#include <nb/cleanup.h>
#include <nb/vec.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

struct http_control {
	struct nb_log *logger;
	const nb_atomic_int *cancel_flag;
	const nb_cancel *cancel_token;
	int (*interrupt_check)(void *);
	void *interrupt_user_data;
};

/* Application defaults: manual redirects, unbounded buffered responses,
 * 10s connect and 30s GET / 60s POST total. Streaming callers set idle instead. */
void http_request_init(nb_http_request *request, const char *method, const char *url);
/* Applies Morph's certificate environment precedence to an owned native client. */
int http_client_open(const struct http_control *control, nb_http_client **out);
/* Drives a native call while observing application cancellation and steering.
 * The caller owns out even on transfer failure and frees it before client. */
int http_request_run(const struct http_control *control, nb_http_client *client,
		     const nb_http_request *request, const nb_http_handler *handler,
		     nb_http_call **out);
/* Converts configured header lines into borrowed native header views. */
int http_headers_parse(nb_vec *headers, const char *const *lines, size_t count);
int http_wait_cancelable(const struct http_control *control, unsigned int milliseconds);

static inline int http_timeout_ms(long seconds)
{
	return seconds < 0 ? -1 : seconds > INT_MAX / 1000 ? INT_MAX : (int)seconds * 1000;
}

static inline void http_client_cleanup(nb_http_client **owner)
{
	nb_http_client_free(*owner);
	*owner = NULL;
}

static inline void http_call_cleanup(nb_http_call **owner)
{
	nb_http_call_free(*owner);
	*owner = NULL;
}

#ifdef __cplusplus
}
#endif

#endif
