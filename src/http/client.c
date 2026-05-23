#include "client.h"
#include "sse.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

static int http_initialized = 0;

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
	char *new_body = realloc(resp->body, resp->body_len + total + 1);
	if (!new_body)
		return 0;
	resp->body = new_body;
	memcpy(resp->body + resp->body_len, ptr, total);
	resp->body_len += total;
	resp->body[resp->body_len] = '\0';
	return total;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;
	size_t old_len = resp->headers ? strlen(resp->headers) : 0;
	char *new_headers = realloc(resp->headers, old_len + total + 1);
	if (!new_headers)
		return total;
	resp->headers = new_headers;
	memcpy(resp->headers + old_len, ptr, total);
	resp->headers[old_len + total] = '\0';
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
	sse_parser_feed(&swd->parser, (const char *)ptr, total);
	if (swd->cb)
		swd->cb((const char *)ptr, total, swd->user_data);
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

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		log_err("sse request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
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

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		log_err("sse request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
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

	if (body && body_len > 0) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
	}

	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	sse_parser_free(&swd.parser);

	if (rc != CURLE_OK) {
		curl_easy_cleanup(curl);
		if (rc == CURLE_OPERATION_TIMEDOUT) {
			log_warn("http_post_sse_ex_timeout: connection stalled (no data for %lds)", timeout_seconds);
			return -ETIMEDOUT;
		}
		log_err("sse request failed: %s", curl_easy_strerror(rc));
		MORPH_RETURN(MORPH_ERR_NETWORK);
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