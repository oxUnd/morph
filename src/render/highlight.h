#ifndef MORPH_HIGHLIGHT_H
#define MORPH_HIGHLIGHT_H

#include <stddef.h>
#include <string.h>

struct sbuf {
	char *buf;
	size_t len;
	size_t cap;
};

static inline void sbuf_init(struct sbuf *s, char *buf, size_t cap)
{
	s->buf = buf;
	s->cap = cap;
	s->len = 0;
}

static inline void sbuf_append_n(struct sbuf *s, const char *src, size_t n)
{
	if (!s->buf) {
		s->len += n;
		return;
	}
	size_t space = s->cap > 0 ? s->cap - 1 - s->len : 0;
	size_t to_write = n < space ? n : space;
	if (to_write > 0) {
		memcpy(s->buf + s->len, src, to_write);
		s->len += to_write;
	}
}

static inline void sbuf_append(struct sbuf *s, const char *str)
{
	sbuf_append_n(s, str, strlen(str));
}

typedef void (*hl_newline_fn)(void *userdata);

void highlight_code(const char *lang, size_t lang_len,
		    const char *code, size_t code_len,
		    struct sbuf *out,
		    hl_newline_fn newline_cb, void *newline_ud);

#endif
