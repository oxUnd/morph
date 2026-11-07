#ifndef MORPH_BUF_H
#define MORPH_BUF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "error.h"
#include "str.h"

#define MORPH_BUF_DEFAULT_CAP  4096
#define MORPH_BUF_MAX_CAP      (SIZE_MAX / 2)

typedef struct {
	char          *data;
	size_t         len;
	size_t         cap;
	int            failed;
} morph_buf_t;

int  morph_buf_init(morph_buf_t *b, size_t init_cap);
void morph_buf_cleanup(morph_buf_t *b);
/* Replace destination, transfer storage, and reset source. Cannot allocate. */
void morph_buf_move(morph_buf_t *destination, morph_buf_t *source);
void morph_buf_reset(morph_buf_t *b);

int  morph_buf_append(morph_buf_t *b, const char *src, size_t n);
int  morph_buf_puts(morph_buf_t *b, const char *s);
int  morph_buf_putc(morph_buf_t *b, char c);
int  morph_buf_printf(morph_buf_t *b, const char *fmt, ...);
int  morph_buf_vprintf(morph_buf_t *b, const char *fmt, va_list ap);

/* Borrowed storage: append/reserve may reallocate; reset/truncate may change it.
 * No view survives cleanup, detach, or move. */
morph_str_t morph_buf_str_borrow(const morph_buf_t *b);
const char *morph_buf_cstr_borrow(const morph_buf_t *b);
/* Success transfers the buffer and clears b; failure preserves b. */
char       *morph_buf_detach(morph_buf_t *b);

int  morph_buf_reserve(morph_buf_t *b, size_t extra);
int  morph_buf_truncate(morph_buf_t *b, size_t new_len);

int  morph_buf_append_cb(const char *token, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
