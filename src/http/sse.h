#ifndef SSE_H
#define SSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "util/buf.h"

typedef int (*sse_event_cb)(const char *event, const char *data, void *user_data);

struct sse_parser {
	morph_buf_t buf;
	morph_buf_t data;
	morph_buf_t event;
	morph_buf_t id;
	long retry_ms;
	sse_event_cb cb;
	void *user_data;
};

void sse_parser_init(struct sse_parser *p, sse_event_cb cb, void *user_data);
int sse_parser_feed(struct sse_parser *p, const char *data, size_t len);
void sse_parser_free(struct sse_parser *p);

#ifdef __cplusplus
}
#endif

#endif
