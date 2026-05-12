#ifndef CLIENT_H
#define CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef int (*http_callback)(const char *data, size_t len, void *user_data);

struct http_response {
	int status_code;
	char *body;
	size_t body_len;
	char *headers;
};

struct http_request {
	const char *url;
	const char *method;
	const char *body;
	size_t body_len;
	const char **headers;
	int header_count;
	int timeout_seconds;
};

int http_init(void);
void http_cleanup(void);
int http_get(const char *url, struct http_response *resp);
int http_post(const char *url, const char *body, size_t body_len,
	      const char *content_type, struct http_response *resp);
int http_post_ex(const char *url, const char *body, size_t body_len,
		 const char *content_type, const char **extra_headers,
		 int extra_header_count, struct http_response *resp);
int http_post_sse(const char *url, const char *body, size_t body_len,
		  const char *content_type, http_callback cb, void *user_data);
int http_post_sse_ex(const char *url, const char *body, size_t body_len,
		     const char *content_type, const char **extra_headers,
		     int extra_header_count, http_callback cb, void *user_data);
void http_response_free(struct http_response *resp);

#ifdef __cplusplus
}
#endif

#endif