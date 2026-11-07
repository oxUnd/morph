#ifndef MORPH_JSON_INTERNAL_H
#define MORPH_JSON_INTERNAL_H

#include "json.h"
#include "cJSON.h"

struct json {
	cJSON *backend;
	struct json *parent;
	struct json *child;
	struct json *last;
	struct json *next;
	struct json *prev;
};

int json_parse_backend(const char *text, size_t length, int prefix, cJSON **out, size_t *consumed,
		       size_t *error_offset);

#endif
