#include "json.h"
#include "error.h"
#include <stdlib.h>

cJSON *morph_json_string_take(char **text)
{
	cJSON *value;

	if (!text || !*text)
		return NULL;
	value = cJSON_CreateString(*text);
	if (!value)
		return NULL;
	free(*text);
	*text = NULL;
	return value;
}

char *morph_json_print_copy(const cJSON *value)
{
	if (!value)
		return NULL;
	return cJSON_PrintUnformatted(value);
}

int morph_json_object_take(cJSON *object, const char *name, cJSON **source)
{
	if (!cJSON_IsObject(object) || !name || !source || !*source || object == *source ||
	    (*source)->prev || (*source)->next)
		return -EINVAL;
	if (!cJSON_AddItemToObject(object, name, *source))
		return -ENOMEM;
	*source = NULL;
	return 0;
}

int morph_json_array_take(cJSON *array, cJSON **source)
{
	if (!cJSON_IsArray(array) || !source || !*source || array == *source || (*source)->prev ||
	    (*source)->next)
		return -EINVAL;
	if (!cJSON_AddItemToArray(array, *source))
		return -ENOMEM;
	*source = NULL;
	return 0;
}
