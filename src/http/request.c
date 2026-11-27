#include "request.h"

#include <nb/time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int http_cancelled(const struct http_control *control)
{
	if (!control)
		return 0;
	return nb_cancel_requested(control->cancel_token) ||
	       (control->cancel_flag && __atomic_load_n(control->cancel_flag, __ATOMIC_RELAXED)) ||
	       (control->interrupt_check && control->interrupt_check(control->interrupt_user_data));
}

int http_wait_cancelable(const struct http_control *control, unsigned int milliseconds)
{
	const unsigned int slice_ms = 100;
	int64_t deadline = nb_time_monotonic_ms() + milliseconds;
	for (;;) {
		if (http_cancelled(control))
			return NB_ERR_CANCELLED;

		int64_t remaining = deadline - nb_time_monotonic_ms();
		if (remaining <= 0)
			break;

		unsigned int current = remaining > slice_ms ? slice_ms : (unsigned int)remaining;
		struct timespec delay = {.tv_sec = current / 1000,
			.tv_nsec = (long)(current % 1000) * 1000000L};
		while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
			if (http_cancelled(control))
				return NB_ERR_CANCELLED;
		}
	}
	return http_cancelled(control) ? NB_ERR_CANCELLED : 0;
}

void http_request_init(nb_http_request *request, const char *method, const char *url)
{
	nb_http_request_init(request, method, url);
	request->redirect = NB_HTTP_REDIRECT_MANUAL;
	request->max_header_bytes = 0;
	request->max_body_bytes = 0;
	request->timeouts.response_ms = 0;
	request->timeouts.idle_ms = 0;
	request->timeouts.total_ms = strcmp(method ? method : "GET", "GET") == 0 ? 30000 : 60000;
}

int http_client_open(const struct http_control *control, nb_http_client **out)
{
	nb_http_client_options options;
	nb_http_client_options_init(&options);
	options.ca_file = getenv("CURL_CA_BUNDLE");
	if (!options.ca_file || !*options.ca_file)
		options.ca_file = getenv("SSL_CERT_FILE");

	if (options.ca_file && !*options.ca_file)
		options.ca_file = NULL;

	options.ca_dir = getenv("SSL_CERT_DIR");
	if (options.ca_dir && !*options.ca_dir)
		options.ca_dir = NULL;

	options.user_agent = NULL;
	nb_err err = {0};
	int rc = nb_http_client_new(&options, out, &err);

	if (rc != NB_OK)
		nb_log_write(control ? control->logger : NULL, NB_LOG_DEBUG, "HTTP client: %s",
			     err.msg);

	return rc;
}

int http_request_run(const struct http_control *control, nb_http_client *client,
		     const nb_http_request *request, const nb_http_handler *handler,
		     nb_http_call **out)
{
	if (!request || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	if (http_cancelled(control))
		return NB_ERR_CANCELLED;

	nb_http_request configured = *request;
	if (!configured.cancel && control)
		configured.cancel = control->cancel_token;

	nb_err err = {0};
	int rc = nb_http_client_send(client, &configured, handler, out, &err);

	while (rc == NB_OK && !nb_http_call_result(*out)) {
		if (http_cancelled(control))
			rc = nb_http_call_abort(*out, NULL, &err);

		if (rc == NB_OK)
			rc = nb_http_client_poll(client, 50, NULL, &err);
	}

	const nb_http_result *result = nb_http_call_result(*out);
	if (rc == NB_OK && result) {
		rc = result->status;
		err = result->error;
	}

	if (rc != NB_OK)
		nb_log_write(control ? control->logger : NULL, NB_LOG_DEBUG, "HTTP request: %s",
			     err.msg);

	return rc;
}

int http_headers_parse(nb_vec *headers, const char *const *lines, size_t count)
{
	if (!headers || (!lines && count))
		return NB_ERR_INVAL;

	for (size_t i = 0; i < count; i++) {
		if (!lines[i])
			return NB_ERR_INVAL;

		const char *colon = strchr(lines[i], ':');
		if (!colon)
			return NB_ERR_INVAL;

		nb_http_header header = {
			.name = nb_str_from_parts(lines[i], (size_t)(colon - lines[i])),
			.value = nb_str_trim(nb_str_from_cstr(colon + 1)),
		};
		if (!nb_vec_push_copy(headers, &header))
			return NB_ERR_NOMEM;
	}

	return NB_OK;
}
