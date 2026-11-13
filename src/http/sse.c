#include "sse.h"
#include "nb/err.h"
#include <limits.h>
#include <string.h>

static int sse_line_field(const char *line, size_t line_len,
			  const char **field, size_t *field_len,
			  const char **value, size_t *value_len)
{
	const char *colon;

	if (!line || !field || !field_len || !value || !value_len)
		return NB_ERR_INVAL;

	colon = memchr(line, ':', line_len);
	*field = line;
	if (colon) {
		*field_len = (size_t)(colon - line);
		*value = colon + 1;
		*value_len = line_len - *field_len - 1;
		if (*value_len > 0 && **value == ' ') {
			(*value)++;
			(*value_len)--;
		}
	} else {
		*field_len = line_len;
		*value = line + line_len;
		*value_len = 0;
	}
	return 0;
}

static int sse_field_eq(const char *field, size_t field_len,
			const char *name, size_t name_len)
{
	return field_len == name_len && strncmp(field, name, name_len) == 0;
}

static int sse_parse_retry(const char *value, size_t value_len, long *out)
{
	long retry = 0;

	if (!value || !out || value_len == 0)
		return 0;

	for (size_t i = 0; i < value_len; i++) {
		long digit;

		if (value[i] < '0' || value[i] > '9')
			return 0;
		digit = (long)(value[i] - '0');
		if (retry > (LONG_MAX - digit) / 10)
			return 0;
		retry = retry * 10 + digit;
	}
	*out = retry;
	return 0;
}

static void sse_reset_event(struct sse_parser *p)
{
	nb_buf_clear(&p->data);
	nb_buf_clear(&p->event);
}

static int sse_dispatch(struct sse_parser *p)
{
	const char *event_name;
	int rc;

	if (p->data.len == 0) {
		sse_reset_event(p);
		return 0;
	}

	if (p->data.ptr[p->data.len - 1] == '\n')
		p->data.ptr[--p->data.len] = '\0';

	event_name = p->event.len > 0 ? p->event.ptr : "message";
	rc = 0;
	if (p->cb)
		rc = p->cb(event_name, p->data.ptr, p->user_data);
	sse_reset_event(p);
	return rc;
}

void sse_parser_init(struct sse_parser *p, sse_event_cb cb, void *user_data)
{
	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	p->cb = cb;
	p->user_data = user_data;
	(nb_buf_init(&p->buf), nb_buf_reserve(&p->buf, 1024));
	(nb_buf_init(&p->data), nb_buf_reserve(&p->data, 1024));
	(nb_buf_init(&p->event), nb_buf_reserve(&p->event, 128));
	(nb_buf_init(&p->id), nb_buf_reserve(&p->id, 128));
	p->retry_ms = -1;
}

static int process_buffer(struct sse_parser *p)
{
	nb_str line;
	while (nb_buf_next_line(&p->buf, &line)) {
		if (line.len == 0) {
			int rc = sse_dispatch(p);
			if (rc)
				return rc;
			continue;
		}
		if (line.ptr[0] == ':')
			continue;
		const char *field;
		const char *value;
		size_t field_len;
		size_t value_len;
		int rc = sse_line_field(line.ptr, line.len, &field, &field_len, &value, &value_len);
		if (rc)
			return rc;
		if (sse_field_eq(field, field_len, "data", 4)) {
			if (!nb_buf_append(&p->data, value, value_len) ||
			    !nb_buf_append_char(&p->data, '\n'))
				return NB_ERR_NOMEM;
		} else if (sse_field_eq(field, field_len, "event", 5)) {
			nb_buf_clear(&p->event);
			if (!nb_buf_append(&p->event, value, value_len))
				return NB_ERR_NOMEM;
		} else if (sse_field_eq(field, field_len, "id", 2)) {
			if (!memchr(value, '\0', value_len)) {
				nb_buf_clear(&p->id);
				if (!nb_buf_append(&p->id, value, value_len))
					return NB_ERR_NOMEM;
			}
		} else if (sse_field_eq(field, field_len, "retry", 5)) {
			rc = sse_parse_retry(value, value_len, &p->retry_ms);
			if (rc)
				return rc;
		}
	}
	return 0;
}

int sse_parser_feed(struct sse_parser *p, const char *data, size_t len)
{
	if (!p || !data || len == 0)
		return 0;
	int rc = (nb_buf_append(&p->buf, data, len) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	return process_buffer(p);
}

void sse_parser_free(struct sse_parser *p)
{
	if (!p)
		return;
	nb_buf_cleanup(&p->buf);
	nb_buf_cleanup(&p->data);
	nb_buf_cleanup(&p->event);
	nb_buf_cleanup(&p->id);
}
