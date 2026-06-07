#include "sse.h"
#include <string.h>

void sse_parser_init(struct sse_parser *p, sse_event_cb cb, void *user_data)
{
	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	p->cb = cb;
	p->user_data = user_data;
	morph_buf_init(&p->buf, 1024);
}

static int process_buffer(struct sse_parser *p)
{
	if (!p->buf.data || p->buf.len == 0)
		return 0;
	char *buf = p->buf.data;
	char *start = buf;
	while (start < buf + p->buf.len) {
		char *nl = memchr(start, '\n', (size_t)(buf + p->buf.len - start));
		if (!nl) {
			size_t remaining = (size_t)(buf + p->buf.len - start);
			if (remaining > 0)
				memmove(p->buf.data, start, remaining);
			p->buf.len = remaining;
			p->buf.data[remaining] = '\0';
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
	p->buf.len = 0;
	if (p->buf.data)
		p->buf.data[0] = '\0';
	return 0;
}

int sse_parser_feed(struct sse_parser *p, const char *data, size_t len)
{
	if (!p || !data || len == 0)
		return 0;
	int rc = morph_buf_append(&p->buf, data, len);
	if (rc != 0)
		return rc;
	return process_buffer(p);
}

void sse_parser_free(struct sse_parser *p)
{
	if (!p)
		return;
	morph_buf_cleanup(&p->buf);
}