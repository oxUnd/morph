#include "json.h"
#include <stdlib.h>

cJSON *morph_json_take_string(char *text)
{
	cJSON *value;

	if (!text)
		return NULL;
	value = cJSON_CreateStringReference(text);
	if (!value) {
		free(text);
		return NULL;
	}
	value->type &= ~cJSON_IsReference;
	return value;
}

char *morph_json_print(struct arena *arena, const cJSON *value)
{
	struct arena_cleanup *cleanup;
	char *text;

	if (!arena || !value)
		return NULL;
	text = cJSON_PrintUnformatted(value);
	if (!text)
		return NULL;
	cleanup = arena_cleanup_add(arena, 0);
	if (!cleanup) {
		cJSON_free(text);
		return NULL;
	}
	cleanup->handler = cJSON_free;
	cleanup->data = text;
	return text;
}
