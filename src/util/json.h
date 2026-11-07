#ifndef MORPH_JSON_H
#define MORPH_JSON_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caller owns the serialized text; release it with cJSON_free(). */
char *morph_json_print_copy(const cJSON *value);
/* Success consumes and clears a malloc-owned string; failure preserves it. */
cJSON *morph_json_string_take(char **text);
/* Sources are independent owners. Failure preserves both owners. */
int morph_json_object_take(cJSON *object, const char *name, cJSON **source);
int morph_json_array_take(cJSON *array, cJSON **source);

static inline void morph_json_cleanup(cJSON **owner)
{
	if (!owner)
		return;
	cJSON_Delete(*owner);
	*owner = NULL;
}

static inline void morph_json_text_cleanup(char **owner)
{
	if (!owner)
		return;
	if (*owner)
		cJSON_free(*owner);
	*owner = NULL;
}

#ifdef __cplusplus
}
#endif
#endif
