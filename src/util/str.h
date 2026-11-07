#ifndef MORPH_STR_H
#define MORPH_STR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <string.h>
#include "error.h"

typedef struct {
	size_t      len;
	const char *data;
} morph_str_t;

#define MORPH_STRLIT(s)    { sizeof(s) - 1, (s) }
#define MORPH_STR_NULL     { 0, NULL }
#define MORPH_STR_EMPTY    { 0, "" }

#define morph_str_set_lit(s, text)  \
	do {                            \
		(s)->len = sizeof(text)-1;  \
		(s)->data = (text);         \
	} while (0)

/* Caller owns each duplicate and releases it with free(). */
char *morph_strndup(const char *data, size_t len);
char *morph_strdup(const char *s);

int  morph_strcmp(morph_str_t a, morph_str_t b);
int  morph_strcasecmp(morph_str_t a, morph_str_t b);
int  morph_strncmp(morph_str_t a, const char *b, size_t n);

/* Copies the view to a NUL-terminated malloc owner; release with free(). */
char *morph_str_cstr_copy(morph_str_t s);

/* Subviews borrow the input bytes and share their lifetime. */
const char *morph_str_chr_borrow(morph_str_t s, char c);
const char *morph_str_rchr_borrow(morph_str_t s, char c);
morph_str_t morph_str_trim_borrow(morph_str_t s);

static inline int morph_str_valid(morph_str_t s)
{
	return s.data != NULL;
}

#ifdef __cplusplus
}
#endif

#endif
