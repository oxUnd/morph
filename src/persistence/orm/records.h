#ifndef MORPH_ORM_RECORDS_H
#define MORPH_ORM_RECORDS_H

#include "persistence/orm/internal.h"
#include <stdlib.h>
#include <string.h>

struct orm_integer {
	int64_t value;
};

struct orm_text {
	nb_str value;
	bool missing;
};

extern const struct norm_model orm_integer_model;
extern const struct norm_model orm_text_model;

static inline struct norm_value orm_string(const char *text)
{
	return text ? norm_text(nb_str_from_cstr(text)) : norm_null();
}

static inline char *orm_text_copy(nb_str text)
{
	char *copy = malloc(text.len + 1);

	if (!copy)
		return NULL;

	if (text.len)
		memcpy(copy, text.ptr, text.len);

	copy[text.len] = '\0';
	return copy;
}

#endif
