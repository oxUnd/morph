#include "sse.h"
#include <stdlib.h>
#include <string.h>

void sse_parser_init(struct sse_parser *p, sse_event_cb cb, void *user_data)
{
	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	p->cb = cb;
	p->user_data = user_data;
}

static int process_buffer(struct sse_parser *p)
{
	if (!p->buf || p->buf_len == 0)
		return 0;
	char *buf = p->buf;
	char *start = buf;
	while (start < buf + p->buf_len) {
		char *nl = memchr(start, '\n', (size_t)(buf + p->buf_len - start));
		if (!nl) {
			size_t remaining = (size_t)(buf + p->buf_len - start);
			if (remaining > 0)
				memmove(p->buf, start, remaining);
			p->buf_len = remaining;
			p->buf[remaining] = '\0';
			return 0;
		}
		size_t line_len = (size_t)(nl - start);
		*nl = '\0';
		if (line_len > 0) {
			int rc = 0;
			if (strncmp(start, "data: ", 6) == 0) {
				if (p->cb)
					rc = p->cb("data", start + 6, p->user_data);
			} else if (strncmp(start, "event: ", 7) == 0) {
				if (p->cb)
					rc = p->cb("event", start + 7, p->user_data);
			} else if (strncmp(start, "id: ", 4) == 0) {
				/* skip id lines */
			} else if (strncmp(start, "retry: ", 7) == 0) {
				/* skip retry lines */
			}
			if (rc != 0)
				return rc;
		}
		start = nl + 1;
	}
	p->buf_len = 0;
	if (p->buf)
		p->buf[0] = '\0';
	return 0;
}

int sse_parser_feed(struct sse_parser *p, const char *data, size_t len)
{
	if (!p || !data || len == 0)
		return 0;
	size_t needed = p->buf_len + len + 1;
	if (needed > p->buf_cap) {
		size_t new_cap = needed * 2;
		char *new_buf = realloc(p->buf, new_cap);
		if (!new_buf)
			return 0;
		p->buf = new_buf;
		p->buf_cap = new_cap;
	}
	memcpy(p->buf + p->buf_len, data, len);
	p->buf_len += len;
	p->buf[p->buf_len] = '\0';
	return process_buffer(p);
}

void sse_parser_free(struct sse_parser *p)
{
	if (!p)
		return;
	free(p->buf);
	p->buf = NULL;
	p->buf_len = 0;
	p->buf_cap = 0;
}