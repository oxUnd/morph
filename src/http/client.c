#include "client.h"
#include "sse.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int http_initialized = 0;
static __thread volatile sig_atomic_t *http_cancel_flag = NULL;

static int curl_debug_cb(CURL *handle, curl_infotype type,
			 char *data, size_t size, void *userp)
{
	(void)handle;
	(void)userp;
	if (type == CURLINFO_HEADER_OUT) {
		log_dbg(">> %.*s", (int)size, data);
	} else if (type == CURLINFO_HEADER_IN) {
		log_dbg("<< %.*s", (int)size, data);
	}
	return 0;
}

static void curl_set_debug(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

void http_set_cancel_flag(volatile sig_atomic_t *flag)
{
	http_cancel_flag = flag;
}

static int http_cancelled(void)
{
	return http_cancel_flag && *http_cancel_flag;
}

static int sse_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
			   curl_off_t ultotal, curl_off_t ulnow)
{
	(void)clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return http_cancelled() ? 1 : 0;
}

static void sse_apply_cancel_opts(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
}

static int sse_map_curl_error(CURLcode rc)
{
	if (rc == CURLE_OK)
		return 0;
	if (http_cancelled() ||
	    rc == CURLE_WRITE_ERROR ||
	    rc == CURLE_ABORTED_BY_CALLBACK) {
		log_info("http: request cancelled by user");
		return -ECANCELED;
	}
	log_err("sse request failed: %s", curl_easy_strerror(rc));
	return MORPH_ERR_NETWORK;
}

int http_init(void)
{
	if (http_initialized)
		return 0;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	http_initialized = 1;
	log_info("http client initialized");
	return 0;
}

void http_cleanup(void)
{
	if (http_initialized) {
		curl_global_cleanup();
		http_initialized = 0;
	}
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;
	size_t needed;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->body_len > SIZE_MAX - total - 1)
		return 0;
	needed = resp->body_len + total + 1;
	if (needed > resp->body_cap) {
		size_t new_cap = resp->body_cap ? resp->body_cap * 2 : 65536;
		while (new_cap < needed) {
			if (new_cap > SIZE_MAX / 2) {
				new_cap = needed;
				break;
			}
			new_cap *= 2;
		}
		char *new_body = realloc(resp->body, new_cap);
		if (!new_body)
			return 0;
		resp->body = new_body;
		resp->body_cap = new_cap;
	}
	memcpy(resp->body + resp->body_len, ptr, total);
	resp->body_len += total;
	resp->body[resp->body_len] = '\0';
	return total;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;
	size_t needed;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->headers_len > SIZE_MAX - total - 1)
		return 0;
	needed = resp->headers_len + total + 1;
	if (needed > resp->headers_cap) {
		size_t new_cap = resp->headers_cap ? resp->headers_cap * 2 : 4096;
		while (new_cap < needed) {
			if (new_cap > SIZE_MAX / 2) {
				new_cap = needed;
				break;
			}
			new_cap *= 2;
		}
		char *new_headers = realloc(resp->headers, new_cap);
		if (!new_headers)
			return total;
		resp->headers = new_headers;
		resp->headers_cap = new_cap;
	}
	memcpy(resp->headers + resp->headers_len, ptr, total);
	resp->headers_len += total;
	resp->headers[resp->headers_len] = '\0';
	return total;
}

static int do_request(const char *url, const char *method, const char *body,
		      size_t body_len, const char *content_type,
		      struct http_response *resp, int timeout)
{
	if (!http_initialized)
		http_init();
	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	struct curl_slist *headers = NULL;
	if (content_type) {
		char ct[256];
		snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
		headers = curl_slist_append(headers, ct);
	}
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
	curl_set_debug(curl);
	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}
	CURLcode rc = curl_easy_perform(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		log_err("http request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
	}
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;
	curl_easy_cleanup(curl);
	return 0;
}

int http_get(const char *url, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "GET", NULL, 0, NULL, resp, 30);
}

int http_get_ex(const char *url, const char **extra_headers,
		int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	if (!http_initialized)
		http_init();
	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	struct curl_slist *headers = NULL;
	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i])
			headers = curl_slist_append(headers, extra_headers[i]);
	}
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_set_debug(curl);
	CURLcode rc = curl_easy_perform(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		log_err("http request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
	}
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;
	curl_easy_cleanup(curl);
	return 0;
}

int http_post(const char *url, const char *body, size_t body_len,
	      const char *content_type, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type, resp, 60);
}

int http_post_ex(const char *url, const char *body, size_t body_len,
		 const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	if (!http_initialized)
		http_init();
	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	struct curl_slist *headers = NULL;
	if (content_type) {
		char ct[256];
		snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
		headers = curl_slist_append(headers, ct);
	}
	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i])
			headers = curl_slist_append(headers, extra_headers[i]);
	}
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_set_debug(curl);
	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}
	CURLcode rc = curl_easy_perform(curl);
	if (headers)
		curl_slist_free_all(headers);
	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		log_err("http_post_ex request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
	}
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;
	curl_easy_cleanup(curl);
	return 0;
}

struct sse_write_data {
	http_callback cb;
	void *user_data;
	struct sse_parser parser;
};

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct sse_write_data *swd = data;
	size_t total = size * nmemb;
	log_dbg("sse_write_cb: received %zu bytes", total);
	if (http_cancelled())
		return 0;
	sse_parser_feed(&swd->parser, (const char *)ptr, total);
	if (swd->cb) {
		int rc = swd->cb((const char *)ptr, total, swd->user_data);
		if (rc != 0 || http_cancelled())
			return 0;
	}
	return total;
}

int http_post_sse(const char *url, const char *body, size_t body_len,
		   const char *content_type, http_callback cb, void *user_data)
{
	if (!url || !cb)
		return -EINVAL;
	if (!http_initialized)
		http_init();

	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");

	struct curl_slist *headers = NULL;
	char ct[256];
	if (content_type) {
		snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
	} else {
		snprintf(ct, sizeof(ct), "Content-Type: application/json");
	}
	headers = curl_slist_append(headers, ct);
	headers = curl_slist_append(headers, "Accept: text/event-stream");

	struct sse_write_data swd;
	swd.cb = cb;
	swd.user_data = user_data;
	sse_parser_init(&swd.parser, NULL, NULL);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_set_debug(curl);
	sse_apply_cancel_opts(curl);

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		return sse_map_curl_error(rc);
	}

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return (int)status;
}

int http_post_sse_ex(const char *url, const char *body, size_t body_len,
		     const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data)
{
	if (!url || !cb)
		return -EINVAL;
	if (!http_initialized)
		http_init();

	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");

	struct curl_slist *headers = NULL;
	char ct[256];
	if (content_type) {
		snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
	} else {
		snprintf(ct, sizeof(ct), "Content-Type: application/json");
	}
	headers = curl_slist_append(headers, ct);
	headers = curl_slist_append(headers, "Accept: text/event-stream");

	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i])
			headers = curl_slist_append(headers, extra_headers[i]);
	}

	struct sse_write_data swd;
	swd.cb = cb;
	swd.user_data = user_data;
	sse_parser_init(&swd.parser, NULL, NULL);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_set_debug(curl);
	sse_apply_cancel_opts(curl);

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		return sse_map_curl_error(rc);
	}

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return (int)status;
}

int http_post_sse_ex_timeout(const char *url, const char *body, size_t body_len,
			     const char *content_type, const char **extra_headers,
			     int extra_header_count, long timeout_seconds,
			     http_callback cb, void *user_data)
{
	if (!url || !cb)
		return -EINVAL;
	if (!http_initialized)
		http_init();
	CURL *curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");

	struct curl_slist *headers = NULL;
	char ct[256];
	if (content_type) {
		snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
	} else {
		snprintf(ct, sizeof(ct), "Content-Type: application/json");
	}
	headers = curl_slist_append(headers, ct);
	headers = curl_slist_append(headers, "Accept: text/event-stream");

	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i])
			headers = curl_slist_append(headers, extra_headers[i]);
	}

	struct sse_write_data swd;
	swd.cb = cb;
	swd.user_data = user_data;
	sse_parser_init(&swd.parser, NULL, NULL);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout_seconds > 0 ? timeout_seconds : 300L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_set_debug(curl);
	sse_apply_cancel_opts(curl);

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		if (http_cancelled())
			return -ECANCELED;
		if (rc == CURLE_OPERATION_TIMEDOUT) {
			log_warn("http_post_sse_ex_timeout: connection stalled (no data for %lds)", timeout_seconds);
			return -ETIMEDOUT;
		}
		return sse_map_curl_error(rc);
	}

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return (int)status;
}

void http_response_free(struct http_response *resp)
{
	if (!resp)
		return;
	free(resp->body);
	free(resp->headers);
	memset(resp, 0, sizeof(*resp));
}
