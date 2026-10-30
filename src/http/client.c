#include "client.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <curl/curl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HTTP_SSE_DEFAULT_TIMEOUT_SECONDS 300L
#define HTTP_CONNECT_TIMEOUT_SECONDS 10L

static int curl_debug_cb(CURL *handle, curl_infotype type,
			 char *data, size_t size, void *userp)
{
	(void)handle;
	(void)userp;
	if (type == CURLINFO_HEADER_OUT) {
		log_dbg(userp ? ((const struct http_control *)userp)->logger : NULL, ">> %.*s",
			(int)size, data);
	} else if (type == CURLINFO_HEADER_IN) {
		log_dbg(userp ? ((const struct http_control *)userp)->logger : NULL, "<< %.*s",
			(int)size, data);
	}
	return 0;
}

void http_apply_ca_env(CURL *curl)
{
	const char *ca_file;
	const char *ca_dir;

	if (!curl)
		return;
	ca_file = getenv("CURL_CA_BUNDLE");
	if (!ca_file || !*ca_file)
		ca_file = getenv("SSL_CERT_FILE");
	if (ca_file && *ca_file)
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
	ca_dir = getenv("SSL_CERT_DIR");
	if (ca_dir && *ca_dir)
		curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir);
}

static void curl_apply_common_opts(CURL *curl, char *errbuf, const struct http_control *control)
{
	/* Concurrent requests must not change process-wide signal handlers. */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	if (errbuf) {
		errbuf[0] = '\0';
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	}
	http_apply_ca_env(curl);
	if (getenv("MORPH_DEBUG")) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
		curl_easy_setopt(curl, CURLOPT_DEBUGDATA, control);
	}
}

static void curl_log_error(struct morph_log *logger, const char *what, CURLcode rc,
			   const char *errbuf)
{
	if (errbuf && *errbuf) {
		log_err(logger, "%s failed: curl=%d %s (%s)", what, (int)rc, curl_easy_strerror(rc),
			errbuf);
	} else {
		log_err(logger, "%s failed: curl=%d %s", what, (int)rc, curl_easy_strerror(rc));
	}
}

static int http_cancelled(const struct http_control *control)
{
	if (!control)
		return 0;
	return morph_cancel_token_is_cancelled(control->cancel_token) ||
	       (control->cancel_flag && __atomic_load_n(control->cancel_flag, __ATOMIC_RELAXED)) ||
	       (control->interrupt_check && control->interrupt_check(control->interrupt_user_data));
}

/* Drive transfers in bounded slices, including when a peer sends no bytes.
 * curl_easy_perform's progress callback can otherwise be idle for a second. */
static CURLcode http_perform_cancelable(CURL *curl, CURLM *shared_multi,
				       const struct http_control *control)
{
	CURLM *multi = shared_multi ? shared_multi : curl_multi_init();
	CURLcode result = CURLE_FAILED_INIT;
	int running = 0;

	if (!multi)
		return CURLE_OUT_OF_MEMORY;
	if (curl_multi_add_handle(multi, curl) != CURLM_OK) {
		if (!shared_multi)
			curl_multi_cleanup(multi);
		return result;
	}
	for (;;) {
		if (http_cancelled(control)) {
			result = CURLE_ABORTED_BY_CALLBACK;
			break;
		}
		if (curl_multi_perform(multi, &running) != CURLM_OK)
			break;
		if (!running) {
			int remaining;
			CURLMsg *message;

			while ((message = curl_multi_info_read(multi, &remaining))) {
				if (message->msg == CURLMSG_DONE && message->easy_handle == curl)
					result = message->data.result;
			}
			break;
		}
#if LIBCURL_VERSION_NUM >= 0x074200
		if (curl_multi_poll(multi, NULL, 0, 50, NULL) != CURLM_OK)
			break;
#else
		int ready = 0;
		if (curl_multi_wait(multi, NULL, 0, 25, &ready) != CURLM_OK)
			break;
		if (!ready && http_wait_cancelable(control, 25) != 0) {
			result = CURLE_ABORTED_BY_CALLBACK;
			break;
		}
#endif
	}
	(void)curl_multi_remove_handle(multi, curl);
	if (!shared_multi)
		(void)curl_multi_cleanup(multi);
	return result;
}

int http_wait_cancelable(const struct http_control *control, unsigned int milliseconds)
{
	const unsigned int slice_ms = 100;
	unsigned int waited = 0;

	while (waited < milliseconds) {
		struct timespec delay;
		unsigned int current = milliseconds - waited;
		if (current > slice_ms)
			current = slice_ms;
		if (http_cancelled(control))
			return -ECANCELED;
		delay.tv_sec = current / 1000;
		delay.tv_nsec = (long)(current % 1000) * 1000000L;
		while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
			if (http_cancelled(control))
				return -ECANCELED;
		}
		waited += current;
	}
	return http_cancelled(control) ? -ECANCELED : 0;
}

static int sse_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
			   curl_off_t ultotal, curl_off_t ulnow)
{
	const struct http_control *control = clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return http_cancelled(control) ? 1 : 0;
}

static void sse_apply_cancel_opts(CURL *curl, const struct http_control *control)
{
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_xferinfo_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, control);
}

static int sse_map_curl_error(const struct http_control *control, CURLcode rc, const char *errbuf)
{
	if (rc == CURLE_OK)
		return 0;
	if (http_cancelled(control) || rc == CURLE_WRITE_ERROR || rc == CURLE_ABORTED_BY_CALLBACK) {
		log_info(control ? control->logger : NULL, "http: request cancelled by user");
		return -ECANCELED;
	}
	curl_log_error(control ? control->logger : NULL, "sse request", rc, errbuf);
	return MORPH_ERR_NETWORK;
}

int http_init(struct morph_log *logger)
{
	CURLcode rc;

	rc = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (rc != CURLE_OK) {
		log_err(logger, "curl_global_init failed: %s", curl_easy_strerror(rc));
		return MORPH_TRACE_ERROR(logger, MORPH_ERR_NETWORK);
	}
	log_info(logger, "http client initialized");
	return 0;
}

void http_cleanup(void)
{
	curl_global_cleanup();
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->body.cap == 0) {
		if (morph_buf_init(&resp->body, 65536) != 0)
			return 0;
	}
	if (morph_buf_append(&resp->body, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_response *resp = data;
	size_t total = size * nmemb;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	if (resp->headers.cap == 0) {
		if (morph_buf_init(&resp->headers, 4096) != 0)
			return 0;
	}
	if (morph_buf_append(&resp->headers, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *new_headers;

	if (!headers || !header)
		return -EINVAL;
	new_headers = curl_slist_append(*headers, header);
	if (!new_headers)
		return -ENOMEM;
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

static int do_request(const struct http_control *control, const char *url, const char *method,
		      const char *body, size_t body_len, const char *content_type,
		      const char **extra_headers, int extra_header_count,
		      struct http_response *resp, long timeout)
{
	CURLcode curl_rc;
	struct curl_slist *headers = NULL;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int is_post;
	int rc;
	CURL *curl;

	rc = http_init(control ? control->logger : NULL);
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl) {
		http_cleanup();
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -ENOMEM);
	}

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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_apply_common_opts(curl, errbuf, control);
    sse_apply_cancel_opts(curl, control);

    is_post = method && strcmp(method, "POST") == 0;
    if (is_post) {
	    curl_easy_setopt(curl, CURLOPT_POST, 1L);
	    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
	    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    } else if (method && strcmp(method, "GET") != 0) {
	    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	    if (body) {
		    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
	    }
    }

	curl_rc = http_perform_cancelable(curl, NULL, control);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled(control)) {
			rc = -ECANCELED;
		} else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			curl_log_error(control ? control->logger : NULL, "http request", curl_rc,
				       errbuf);
			rc = -ETIMEDOUT;
		} else {
			curl_log_error(control ? control->logger : NULL, "http request", curl_rc,
				       errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	http_cleanup();
	return rc;
}

int http_get(const struct http_control *control, const char *url, struct http_response *resp)
{
	if (!url || !resp)
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -EINVAL);
	memset(resp, 0, sizeof(*resp));
	return do_request(control, url, "GET", NULL, 0, NULL, NULL, 0, resp, 30L);
}

int http_get_ex(const struct http_control *control, const char *url, const char **extra_headers,
		int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(control, url, "GET", NULL, 0, NULL, extra_headers, extra_header_count,
			  resp, 30L);
}

int http_post(const struct http_control *control, const char *url, const char *body,
	      size_t body_len, const char *content_type, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(control, url, "POST", body, body_len, content_type, NULL, 0, resp, 60L);
}

int http_post_ex(const struct http_control *control, const char *url, const char *body,
		 size_t body_len, const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp)
{
	if (!url || !resp)
		return -EINVAL;
	memset(resp, 0, sizeof(*resp));
	return do_request(control, url, "POST", body, body_len, content_type, extra_headers,
			  extra_header_count, resp, 60L);
}

int http_post_ex_timeout(const struct http_control *control, const char *url, const char *body,
			 size_t body_len, const char *content_type, const char **extra_headers,
			 int extra_header_count, long timeout_seconds, struct http_response *resp)
{
	if (!url || !resp)
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -EINVAL);
	memset(resp, 0, sizeof(*resp));
	return do_request(control, url, "POST", body, body_len, content_type, extra_headers,
			  extra_header_count, resp, timeout_seconds);
}

int http_post_multipart_ex(const struct http_control *control, const char *url,
			   const struct http_multipart_part *parts, int part_count,
			   const char **extra_headers, int extra_header_count, long timeout_seconds,
			   struct http_response *resp)
{
	struct curl_slist *headers = NULL;
	curl_mime *mime = NULL;
	CURL *curl = NULL;
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc = 0;

	if (!url || !parts || part_count <= 0 || !resp)
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -EINVAL);
	memset(resp, 0, sizeof(*resp));
	rc = http_init(control ? control->logger : NULL);
	if (rc != 0)
		return rc;
	curl = curl_easy_init();
	if (!curl) {
		http_cleanup();
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -ENOMEM);
	}
	mime = curl_mime_init(curl);
	if (!mime) {
		rc = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < part_count; i++) {
		curl_mimepart *part;

		if (!parts[i].name || !parts[i].value) {
			rc = -EINVAL;
			goto out;
		}
		part = curl_mime_addpart(mime);
		if (!part) {
			rc = -ENOMEM;
			goto out;
		}
		if (curl_mime_name(part, parts[i].name) != CURLE_OK) {
			rc = -ENOMEM;
			goto out;
		}
		if (parts[i].kind == HTTP_MULTIPART_FILE) {
			if (curl_mime_filedata(part, parts[i].value) != CURLE_OK) {
				rc = -EINVAL;
				goto out;
			}
		} else if (curl_mime_data(part, parts[i].value,
					 CURL_ZERO_TERMINATED) != CURLE_OK) {
			rc = -ENOMEM;
			goto out;
		}
		if (parts[i].content_type &&
		    curl_mime_type(part, parts[i].content_type) != CURLE_OK) {
			rc = -EINVAL;
			goto out;
		}
	}
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
			 HTTP_CONNECT_TIMEOUT_SECONDS);
	curl_apply_common_opts(curl, errbuf, control);
	sse_apply_cancel_opts(curl, control);

	curl_rc = http_perform_cancelable(curl, NULL, control);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled(control))
			rc = -ECANCELED;
		else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			curl_log_error(control ? control->logger : NULL, "http multipart request",
				       curl_rc, errbuf);
			rc = -ETIMEDOUT;
		} else {
			curl_log_error(control ? control->logger : NULL, "http multipart request",
				       curl_rc, errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	resp->status_code = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	if (mime)
		curl_mime_free(mime);
	if (curl)
		curl_easy_cleanup(curl);
	http_cleanup();
	return rc;
}

struct sse_write_data {
	const struct http_control *control;
	http_callback cb;
	void *user_data;
	int callback_rc;
};

static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct sse_write_data *swd = data;
	const struct http_control *control = swd->control;
	size_t total;

	if (size != 0 && nmemb > SIZE_MAX / size)
		return 0;
	total = size * nmemb;
	log_dbg(control ? control->logger : NULL, "sse_write_cb: received %zu bytes", total);
	if (http_cancelled(control))
		return 0;
	if (swd->cb) {
		int rc = swd->cb((const char *)ptr, total, swd->user_data);
		if (rc != 0) {
			swd->callback_rc = rc;
			return 0;
		}
		if (http_cancelled(control))
			return 0;
	}
	return total;
}

static size_t sse_header_retry_after(char *ptr, size_t size, size_t count, void *opaque)
{
	struct http_sse_metadata *metadata = opaque;
	size_t len = size * count;
	if (len >= 5 && !strncmp(ptr, "HTTP/", 5))
		metadata->retry_after_seconds = 0;
	if (len > 12 && !strncasecmp(ptr, "Retry-After:", 12)) {
		char value[128];
		size_t n = len - 12;
		char *end;
		long seconds;
		if (n >= sizeof(value))
			n = sizeof(value) - 1;
		memcpy(value, ptr + 12, n);
		value[n] = '\0';
		seconds = strtol(value, &end, 10);
		if (end == value) {
			time_t when = curl_getdate(value, NULL);
			seconds = when > time(NULL) ? (long)(when - time(NULL)) : 0;
		}
		if (seconds > 0)
			metadata->retry_after_seconds = seconds > 86400 ? 86400 : seconds;
	}
	return len;
}

static int do_sse_request(const struct http_control *control, const char *url, const char *body,
			  size_t body_len, const char *content_type, const char **extra_headers,
			  int extra_header_count, long idle_timeout, http_callback cb,
			  void *user_data, struct http_sse_metadata *metadata)
{
	struct http_sse_metadata local_metadata = {0};
	if (!metadata) metadata = &local_metadata;
	metadata->retry_after_seconds = 0;
	struct curl_slist *headers = NULL;
	char ct[256];
	struct sse_write_data swd;
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc;
	CURL *curl;

	if (!url || !cb)
		return -EINVAL;
	rc = http_init(control ? control->logger : NULL);
	if (rc != 0)
		return rc;

	curl = curl_easy_init();
	if (!curl) {
		http_cleanup();
		return MORPH_TRACE_ERROR(control ? control->logger : NULL, -ENOMEM);
	}

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

	swd.control = control;
	swd.cb = cb;
	swd.user_data = user_data;
	swd.callback_rc = 0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	metadata->retry_after_seconds = 0;
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sse_header_retry_after);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, metadata);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &swd);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
			 HTTP_CONNECT_TIMEOUT_SECONDS);
	if (idle_timeout > 0) {
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_timeout);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	}
	curl_apply_common_opts(curl, errbuf, control);
	sse_apply_cancel_opts(curl, control);

	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);

	log_dbg(control ? control->logger : NULL,
		"http sse: request timeout disabled, idle_timeout=%lds", idle_timeout);
	curl_rc = http_perform_cancelable(curl, NULL, control);

	if (swd.callback_rc != 0) {
		rc = swd.callback_rc;
		goto out;
	}
	if (curl_rc != CURLE_OK) {
		if (http_cancelled(control))
			rc = -ECANCELED;
		else if (curl_rc == CURLE_OPERATION_TIMEDOUT) {
			log_warn(control ? control->logger : NULL,
				 "http sse: curl idle timeout "
				 "(idle_timeout=%lds)",
				 idle_timeout);
			rc = -ETIMEDOUT;
		} else {
			rc = sse_map_curl_error(control, curl_rc, errbuf);
		}
		goto out;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	rc = (int)status;

out:
	if (headers)
		curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	http_cleanup();
	return rc;
}

int http_post_sse(const struct http_control *control, const char *url, const char *body,
		  size_t body_len, const char *content_type, http_callback cb, void *user_data)
{
	return do_sse_request(control, url, body, body_len, content_type, NULL, 0,
			      HTTP_SSE_DEFAULT_TIMEOUT_SECONDS, cb, user_data, NULL);
}

int http_post_sse_ex(const struct http_control *control, const char *url, const char *body,
		     size_t body_len, const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data)
{
	return do_sse_request(control, url, body, body_len, content_type, extra_headers,
			      extra_header_count, HTTP_SSE_DEFAULT_TIMEOUT_SECONDS, cb, user_data,
			      NULL);
}

int http_post_sse_ex_timeout(const struct http_control *control, const char *url, const char *body,
			     size_t body_len, const char *content_type, const char **extra_headers,
			     int extra_header_count, long timeout_seconds, http_callback cb,
			     void *user_data)
{
	long idle_timeout = timeout_seconds > 0 ? timeout_seconds :
		HTTP_SSE_DEFAULT_TIMEOUT_SECONDS;

	return do_sse_request(control, url, body, body_len, content_type, extra_headers,
			      extra_header_count, idle_timeout, cb, user_data, NULL);
}

int http_post_sse_ex_timeout_metadata(const struct http_control *control, const char *url,
				      const char *body, size_t body_len, const char *content_type,
				      const char **extra_headers, int extra_header_count,
				      long timeout_seconds, http_callback cb, void *user_data,
				      struct http_sse_metadata *metadata)
{
	long idle_timeout = timeout_seconds > 0 ? timeout_seconds : HTTP_SSE_DEFAULT_TIMEOUT_SECONDS;
	return do_sse_request(control, url, body, body_len, content_type, extra_headers,
			      extra_header_count, idle_timeout, cb, user_data, metadata);
}

void http_response_free(struct http_response *resp)
{
	if (!resp)
		return;
	morph_buf_cleanup(&resp->body);
	morph_buf_cleanup(&resp->headers);
	memset(resp, 0, sizeof(*resp));
}

/* ---- HTTP Session ---- */

static size_t session_write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_session *s = data;
	size_t total = size * nmemb;

	if (morph_buf_append(&s->resp_body, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

static size_t session_header_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
	struct http_session *s = data;
	size_t total = size * nmemb;

	if (morph_buf_append(&s->resp_headers, (const char *)ptr, total) != 0)
		return 0;
	return total;
}

int http_session_init(struct morph_log *logger, struct http_session *s)
{
	if (!s)
		return -EINVAL;
	int rc = http_init(logger);

	if (rc != 0)
		return rc;
	memset(s, 0, sizeof(*s));
	s->logger = logger;
	s->curl = curl_easy_init();
	if (!s->curl) {
		http_cleanup();
		return MORPH_TRACE_ERROR(logger, -ENOMEM);
	}
	/* Keep the connection pool alive across cancellable session requests. */
	s->multi = curl_multi_init();
	if (!s->multi) {
		http_session_cleanup(s);
		return MORPH_TRACE_ERROR(logger, -ENOMEM);
	}
	rc = morph_buf_init(&s->resp_body, 4096);
	if (rc == 0)
		rc = morph_buf_init(&s->resp_headers, 2048);
	if (rc != 0) {
		http_session_cleanup(s);
		return MORPH_TRACE_ERROR(logger, rc);
	}
	s->status_code = 0;
	s->initialized = 1;
	return 0;
}

void http_session_cleanup(struct http_session *s)
{
	if (!s)
		return;
	if (s->multi) {
		curl_multi_cleanup(s->multi);
		s->multi = NULL;
	}
	if (s->curl) {
		curl_easy_cleanup(s->curl);
		s->curl = NULL;
		http_cleanup();
	}
	morph_buf_cleanup(&s->resp_body);
	morph_buf_cleanup(&s->resp_headers);
	s->initialized = 0;
}

void http_session_reset(struct http_session *s)
{
	if (!s)
		return;
	morph_buf_reset(&s->resp_body);
	morph_buf_reset(&s->resp_headers);
	s->status_code = 0;
}

int http_session_post(const struct http_control *control, struct http_session *s, const char *url,
		      const char *body, size_t body_len, const char *content_type,
		      const char **extra_headers, int extra_header_count, long timeout_seconds)
{
	struct curl_slist *headers = NULL;
	struct http_control request_control = control ? *control : (struct http_control){0};
	CURLcode curl_rc;
	char errbuf[CURL_ERROR_SIZE];
	long status = 0;
	int rc;

	if (!s || !s->curl || !url)
		return -EINVAL;

	if (!request_control.logger)
		request_control.logger = s->logger;
	control = &request_control;
	http_session_reset(s);

	rc = append_content_type_header(&headers, content_type);
	if (rc != 0)
		goto out;
	rc = append_extra_headers(&headers, extra_headers, extra_header_count);
	if (rc != 0)
		goto out;
	if (headers)
		curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(s->curl, CURLOPT_URL, url);
	curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
	curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
	curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, session_write_cb);
	curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, session_header_cb);
	curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);
	curl_easy_setopt(s->curl, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 30L);
	curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_apply_common_opts(s->curl, errbuf, control);
	sse_apply_cancel_opts(s->curl, control);

	curl_rc = http_perform_cancelable(s->curl, s->multi, control);
	if (curl_rc != CURLE_OK) {
		if (http_cancelled(control)) {
			rc = -ECANCELED;
		} else {
			curl_log_error(control ? control->logger : NULL, "http session request",
				       curl_rc, errbuf);
			rc = MORPH_ERR_NETWORK;
		}
		goto out;
	}
	curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
	s->status_code = status;
	rc = 0;

out:
	curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, NULL);
	curl_easy_setopt(s->curl, CURLOPT_ERRORBUFFER, NULL);
	curl_easy_setopt(s->curl, CURLOPT_DEBUGDATA, NULL);
	curl_easy_setopt(s->curl, CURLOPT_XFERINFODATA, NULL);
	if (headers)
		curl_slist_free_all(headers);
	return rc;
}

const char *http_session_body(struct http_session *s, size_t *len)
{
	if (!s)
		return NULL;
	if (len)
		*len = s->resp_body.len;
	return morph_buf_cstr(&s->resp_body);
}

long http_session_status(struct http_session *s)
{
	if (!s)
		return 0;
	return s->status_code;
}

char *http_session_header_get(struct http_session *s, const char *name)
{
	if (!s || !name)
		return NULL;
	const char *hdrs = morph_buf_cstr(&s->resp_headers);
	if (!hdrs)
		return NULL;
	size_t name_len = strlen(name);
	const char *p = hdrs;
	while (*p) {
		const char *eol = strstr(p, "\r\n");
		if (!eol)
			eol = p + strlen(p);
		size_t line_len = (size_t)(eol - p);
		if (line_len > name_len + 1) {
			size_t i;
			for (i = 0; i < name_len; i++) {
				char c = (p[i] >= 'A' && p[i] <= 'Z') ? (p[i] + 32) : p[i];
				char n = (name[i] >= 'A' && name[i] <= 'Z') ? (name[i] + 32) : name[i];
				if (c != n)
					break;
			}
			if (i == name_len && p[name_len] == ':') {
				const char *val = p + name_len + 1;
				while (*val == ' ')
					val++;
				size_t val_end = (size_t)(eol - val);
				char *result = malloc(val_end + 1);
				if (!result)
					return NULL;
				memcpy(result, val, val_end);
				result[val_end] = '\0';
				return result;
			}
		}
		p = (*eol == '\0') ? eol : eol + 2;
	}
	return NULL;
}
