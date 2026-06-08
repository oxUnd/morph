#include "client.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <curl/curl.h>
#include <limits.h>
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
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	if (getenv("MORPH_DEBUG")) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
	}
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
	CURLcode rc;

	if (http_initialized)
		return 0;
	rc = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (rc != CURLE_OK) {
		log_err("curl_global_init failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
	}
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
			return 0;
		resp->headers = new_headers;
		resp->headers_cap = new_cap;
	}
	memcpy(resp->headers + resp->headers_len, ptr, total);
	resp->headers_len += total;
	resp->headers[resp->headers_len] = '\0';
	return total;
}

static int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *new_headers;

	if (!headers || !header)
		MORPH_RETURN(-EINVAL);
	new_headers = curl_slist_append(*headers, header);
	if (!new_headers)
		MORPH_RETURN(-ENOMEM);
	*headers = new_headers;
	return 0;
}

static int append_content_type_header(struct curl_slist **headers,
				      const char *content_type)
{
	char ct[256];

	if (!content_type)
		return 0;
	snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
	return append_header(headers, ct);
}

static int append_extra_headers(struct curl_slist **headers,
				const char **extra_headers,
				int extra_header_count)
{
	int rc;

	for (int i = 0; i < extra_header_count; i++) {
		if (extra_headers && extra_headers[i]) {
			rc = append_header(headers, extra_headers[i]);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int do_request(const char *url, const char *method, const char *body,
		      size_t body_len, const char *content_type,
		      const char **extra_headers, int extra_header_count,
		      struct http_response *resp, long timeout)
{
	CURLcode curl_rc;
	struct curl_slist *headers = NULL;
	long status = 0;
	int is_post;
	int rc;
	CURL *curl;

	if (!http_initialized)
		rc = http_init();
	else
		rc = 0;
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);

	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	rc = append_content_type_header(&headers, content_type);
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
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

	is_post = method && strcmp(method, "POST") == 0;
	if (is_post) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				 (curl_off_t)body_len);
	} else if (method && strcmp(method, "GET") != 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
		if (body) {
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
					 (curl_off_t)body_len);
		}
	}

	curl_rc = curl_easy_perform(curl);
	if (curl_rc != CURLE_OK) {
		log_err("http request failed: %s", curl_easy_strerror(curl_rc));
		rc = MORPH_ERR_NETWORK;
		goto out;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

int http_get(const char *url, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "GET", NULL, 0, NULL, NULL, 0, resp, 30L);
}

int http_get_ex(const char *url, const char **extra_headers,
		int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "GET", NULL, 0, NULL, extra_headers,
			  extra_header_count, resp, 30L);
}

int http_post(const char *url, const char *body, size_t body_len,
	      const char *content_type, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type, NULL, 0,
			  resp, 60L);
}

int http_post_ex(const char *url, const char *body, size_t body_len,
		 const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(url, "POST", body, body_len, content_type,
			  extra_headers, extra_header_count, resp, 60L);
}

struct sse_write_data {
	http_callback cb;
	void *user_data;
	int callback_rc;
};

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct sse_write_data *swd = data;
	size_t total;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	total = size * nmemb;
	log_dbg("sse_write_cb: received %zu bytes", total);
	if (http_cancelled())
		return 0;
	if (swd->cb) {
		int rc = swd->cb((const char *)ptr, total, swd->user_data);
		if (rc != 0) {
			swd->callback_rc = rc;
			return 0;
		}
		if (http_cancelled())
			return 0;
	}
	return total;
}

static int do_sse_request(const char *url, const char *body, size_t body_len,
			  const char *content_type,
			  const char **extra_headers, int extra_header_count,
			  long total_timeout, long idle_timeout,
			  http_callback cb, void *user_data)
{
	struct curl_slist *headers = NULL;
	char ct[256];
	struct sse_write_data swd;
	CURLcode curl_rc;
	long status = 0;
	int rc;
	CURL *curl;

	if (!url || !cb)
		return -EINVAL;
	if (!http_initialized)
		rc = http_init();
	else
		rc = 0;
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl)
		MORPH_RETURN(-ENOMEM);
	curl_easy_setopt(curl, CURLOPT_PROXY, "");

	snprintf(ct, sizeof(ct), "Content-Type: %s",
		 content_type ? content_type : "application/json");
	rc = append_header(&headers, ct);
	if (rc != 0)
		goto out;
	rc = append_header(&headers, "Accept: text/event-stream");
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;

	swd.cb = cb;
	swd.user_data = user_data;
	swd.callback_rc = 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_timeout);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if (idle_timeout > 0) {
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_timeout);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	}
	curl_set_debug(curl);
	sse_apply_cancel_opts(curl);

	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);

	curl_rc = curl_easy_perform(curl);

	if (swd.callback_rc != 0) {
		rc = swd.callback_rc;
		goto out;
	}
	if (curl_rc != CURLE_OK) {
		if (http_cancelled())
			rc = -ECANCELED;
		else if (idle_timeout > 0 && curl_rc == CURLE_OPERATION_TIMEDOUT) {
			log_warn("http sse: connection stalled (no data for %lds)",
				 idle_timeout);
			rc = -ETIMEDOUT;
		} else {
			rc = sse_map_curl_error(curl_rc);
		}
		goto out;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	rc = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

int http_post_sse(const char *url, const char *body, size_t body_len,
		   const char *content_type, http_callback cb, void *user_data)
{
	return do_sse_request(url, body, body_len, content_type, NULL, 0,
			      300L, 0L, cb, user_data);
}

int http_post_sse_ex(const char *url, const char *body, size_t body_len,
		     const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data)
{
	return do_sse_request(url, body, body_len, content_type, extra_headers,
			      extra_header_count, 300L, 0L, cb, user_data);
}

int http_post_sse_ex_timeout(const char *url, const char *body, size_t body_len,
			     const char *content_type, const char **extra_headers,
			     int extra_header_count, long timeout_seconds,
			     http_callback cb, void *user_data)
{
	long idle_timeout = timeout_seconds > 0 ? timeout_seconds : 300L;

	return do_sse_request(url, body, body_len, content_type, extra_headers,
			      extra_header_count, 600L, idle_timeout, cb,
			      user_data);
}

void http_response_free(struct http_response *resp)
{
	if (!resp)
		return;
	free(resp->body);
	free(resp->headers);
	memset(resp, 0, sizeof(*resp));
}
