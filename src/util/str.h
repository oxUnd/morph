#ifndef MORPH_STR_H
#define MORPH_STR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <string.h>
#include "error.h"

struct arena;

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

morph_str_t morph_strndup(struct arena *a, const char *data, size_t len);
morph_str_t morph_strdup(struct arena *a, const char *s);

int  morph_strcmp(morph_str_t a, morph_str_t b);
int  morph_strcasecmp(morph_str_t a, morph_str_t b);
int  morph_strncmp(morph_str_t a, const char *b, size_t n);

const char *morph_str_to_c(struct arena *a, morph_str_t s);

const char *morph_str_chr(morph_str_t s, char c);
const char *morph_str_rchr(morph_str_t s, char c);
morph_str_t morph_str_trim(morph_str_t s);

static inline int morph_str_valid(morph_str_t s)
{
	return s.data != NULL;
}

#ifdef __cplusplus
}
#endif

#endif
