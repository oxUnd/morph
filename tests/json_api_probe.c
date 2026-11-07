#include "util/json.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define JSON_PROBE_CHECK(condition)                                                                \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			json_free(root);                                                           \
			return __LINE__;                                                           \
		}                                                                                  \
	} while (0)

int json_c_api_probe(void)
{
	json *root = json_object();
	json *roots[2] = {root, NULL};
	const char *paths[2] = {"once", "unused"};
	char text[] = "Bob";
	const char *constant = "Alice";
	bool flag = true;
	int value = 26;
	int root_evaluations = 0;
	int path_evaluations = 0;

	JSON_PROBE_CHECK(root);
	JSON_PROBE_CHECK(json_set(root, "name", text) == 0);
	JSON_PROBE_CHECK(json_set(root, "constant", constant) == 0);
	JSON_PROBE_CHECK(json_set(root, "literal", "literal") == 0);
	JSON_PROBE_CHECK(json_set(root, "signed", (signed char)-7) == 0);
	JSON_PROBE_CHECK(json_set(root, "unsigned", (unsigned long long)42) == 0);
	JSON_PROBE_CHECK(json_set(root, "u64", UINT64_C(26)) == 0);
	JSON_PROBE_CHECK(json_set(root, "float", 0.25f) == 0);
	JSON_PROBE_CHECK(json_set(root, "double", 0.125) == 0);
	JSON_PROBE_CHECK(json_set(root, "boolean", flag) == 0);
	JSON_PROBE_CHECK(json_set(root, "boolean_literal", JSON_BOOL(true)) == 0);
	JSON_PROBE_CHECK(json_set(root, "null", JSON_NULL) == 0);
	JSON_PROBE_CHECK(json_set(roots[root_evaluations++], paths[path_evaluations++], value++) ==
			 0);
	JSON_PROBE_CHECK(root_evaluations == 1 && path_evaluations == 1 && value == 27);
	JSON_PROBE_CHECK(json_int(root, "once") == 26);
	JSON_PROBE_CHECK(json_append(root, "values", value++) == 0 && value == 28);
	JSON_PROBE_CHECK(json_int(root, "values[0]") == 27);
	JSON_PROBE_CHECK(json_is_bool(root, "boolean"));
	JSON_PROBE_CHECK(json_bool(root, "boolean_literal"));
	JSON_PROBE_CHECK(json_is_null(root, "null"));
	JSON_PROBE_CHECK(strcmp(json_str(root, "name"), "Bob") == 0);
	JSON_PROBE_CHECK(json_set(root, "integer_literal_true", true) == 0);
	JSON_PROBE_CHECK(json_is_number(root, "integer_literal_true"));
#ifdef JSON_API_REJECT_POINTER
	JSON_PROBE_CHECK(json_set(root, "invalid", &value) == -EINVAL);
#endif
	json_free(root);
	return 0;
}
