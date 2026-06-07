#include "buf.h"
#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int buf_add_size(size_t a, size_t b, size_t *out)
{
	if (a > SIZE_MAX - b)
		MORPH_RETURN(-EOVERFLOW);
	*out = a + b;
	return 0;
}

static int buf_grow(morph_buf_t *b, size_t needed)
{
	size_t new_cap;
	int rc;

	if (b->failed)
		return b->failed;
	if (needed <= b->cap)
		return 0;
	if (needed > MORPH_BUF_MAX_CAP) {
		MORPH_SET_ERR(b->failed, -EFBIG);
		return b->failed;
	}
	new_cap = b->cap ? b->cap : MORPH_BUF_DEFAULT_CAP;
	while (new_cap < needed) {
		size_t next = new_cap + new_cap / 2 + 32;
		if (next <= new_cap) {
			MORPH_SET_ERR(b->failed, -EOVERFLOW);
			return b->failed;
		}
		if (next > MORPH_BUF_MAX_CAP) {
			new_cap = needed;
			break;
		}
		new_cap = next;
	}
	if (new_cap > MORPH_BUF_MAX_CAP) {
		MORPH_SET_ERR(b->failed, -EFBIG);
		return b->failed;
	}

	if (b->heap_alloc) {
		char *new_data = realloc(b->data, new_cap);
		if (!new_data) {
			MORPH_SET_ERR(b->failed, -ENOMEM);
			return b->failed;
		}
		b->data = new_data;
	} else if (b->arena) {
		char *new_data = arena_alloc(b->arena, new_cap);
		if (!new_data) {
			MORPH_SET_ERR(b->failed, -ENOMEM);
			return b->failed;
		}
		memcpy(new_data, b->data, b->len);
		b->data = new_data;
	} else {
		MORPH_SET_ERR(b->failed, -ENOMEM);
		return b->failed;
	}
	b->cap = new_cap;
	rc = buf_add_size(b->len, 1, &needed);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return b->failed;
	}
	if (needed <= b->cap)
		b->data[b->len] = '\0';
	return 0;
}

int morph_buf_init(morph_buf_t *b, size_t init_cap)
{
	if (!b)
		MORPH_RETURN(-EINVAL);
	if (init_cap == 0)
		init_cap = MORPH_BUF_DEFAULT_CAP;
	if (init_cap > MORPH_BUF_MAX_CAP)
		MORPH_RETURN(-EFBIG);
	b->data = malloc(init_cap);
	if (!b->data)
		MORPH_RETURN(-ENOMEM);
	b->len = 0;
	b->cap = init_cap;
	b->heap_alloc = 1;
	b->failed = 0;
	b->arena = NULL;
	b->data[0] = '\0';
	return 0;
}

int morph_buf_init_arena(morph_buf_t *b, struct arena *a, size_t init_cap)
{
	if (!b || !a)
		MORPH_RETURN(-EINVAL);
	if (init_cap == 0)
		init_cap = MORPH_BUF_DEFAULT_CAP;
	if (init_cap > MORPH_BUF_MAX_CAP)
		MORPH_RETURN(-EFBIG);
	b->data = arena_alloc(a, init_cap);
	if (!b->data)
		MORPH_RETURN(-ENOMEM);
	b->len = 0;
	b->cap = init_cap;
	b->heap_alloc = 0;
	b->failed = 0;
	b->arena = a;
	b->data[0] = '\0';
	return 0;
}

void morph_buf_cleanup(morph_buf_t *b)
{
	if (!b)
		return;
	if (b->heap_alloc)
		free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	b->heap_alloc = 0;
	b->failed = 0;
	b->arena = NULL;
}

void morph_buf_reset(morph_buf_t *b)
{
	if (!b)
		return;
	b->len = 0;
	b->failed = 0;
	if (b->data && b->cap > 0)
		b->data[0] = '\0';
}

int morph_buf_append(morph_buf_t *b, const char *src, size_t n)
{
	size_t needed;
	int rc;

	if (!b || (!src && n > 0))
		MORPH_RETURN(-EINVAL);
	if (b->failed)
		return b->failed;
	if (n == 0)
		return 0;

	rc = buf_add_size(b->len, n, &needed);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return rc;
	}
	rc = buf_add_size(needed, 1, &needed);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return rc;
	}
	rc = buf_grow(b, needed);
	if (rc != 0)
		return rc;
	memcpy(b->data + b->len, src, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

int morph_buf_puts(morph_buf_t *b, const char *s)
{
	if (!s)
		return 0;
	return morph_buf_append(b, s, strlen(s));
}

int morph_buf_putc(morph_buf_t *b, char c)
{
	return morph_buf_append(b, &c, 1);
}

int morph_buf_vprintf(morph_buf_t *b, const char *fmt, va_list ap)
{
	va_list ap2;
	size_t needed_size;
	size_t needed_total;
	int needed;
	int rc;

	if (!b || !fmt)
		MORPH_RETURN(-EINVAL);
	if (b->failed)
		return b->failed;

	va_copy(ap2, ap);
	needed = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	if (needed < 0)
		MORPH_RETURN(-EINVAL);

	needed_size = (size_t)needed;
	rc = buf_add_size(b->len, needed_size, &needed_total);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return rc;
	}
	rc = buf_add_size(needed_total, 1, &needed_total);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return rc;
	}
	rc = buf_grow(b, needed_total);
	if (rc != 0)
		return rc;
	{
		int written = vsnprintf(b->data + b->len,
				       b->cap - b->len,
				       fmt, ap);
		if (written < 0)
			MORPH_RETURN(-EINVAL);
		b->len += (size_t)written;
		b->data[b->len] = '\0';
	}
	return 0;
}

int morph_buf_printf(morph_buf_t *b, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = morph_buf_vprintf(b, fmt, ap);
	va_end(ap);
	return rc;
}

morph_str_t morph_buf_str(const morph_buf_t *b)
{
	morph_str_t s;

	if (!b || !b->data) {
		s.len = 0;
		s.data = NULL;
		return s;
	}
	s.len = b->len;
	s.data = b->data;
	return s;
}

const char *morph_buf_cstr(const morph_buf_t *b)
{
	if (!b || !b->data)
		return NULL;
	if (b->failed)
		return NULL;
	if (b->len >= b->cap)
		return NULL;
	b->data[b->len] = '\0';
	return b->data;
}

char *morph_buf_detach(morph_buf_t *b)
{
	char *data;

	if (!b)
		return NULL;
	if (b->failed) {
		morph_buf_cleanup(b);
		return NULL;
	}
	if (!b->heap_alloc)
		return NULL;
	if (b->data && b->len < b->cap)
		b->data[b->len] = '\0';
	data = b->data;
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	b->heap_alloc = 0;
	b->arena = NULL;
	return data;
}

int morph_buf_reserve(morph_buf_t *b, size_t extra)
{
	size_t needed;
	int rc;

	if (!b)
		MORPH_RETURN(-EINVAL);
	if (b->failed)
		return b->failed;
	rc = buf_add_size(b->len, extra, &needed);
	if (rc != 0) {
		MORPH_SET_ERR(b->failed, rc);
		return rc;
	}
	return buf_grow(b, needed);
}

int morph_buf_truncate(morph_buf_t *b, size_t new_len)
{
	if (!b)
		MORPH_RETURN(-EINVAL);
	if (b->failed)
		return b->failed;
	if (new_len >= b->len)
		return 0;
	b->len = new_len;
	if (b->data && b->len < b->cap)
		b->data[b->len] = '\0';
	return 0;
}

int morph_buf_append_cb(const char *token, void *user_data)
{
	morph_buf_t *b = (morph_buf_t *)user_data;

	if (!token)
		return 0;
	return morph_buf_puts(b, token);
}
