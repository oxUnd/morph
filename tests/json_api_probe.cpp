/* The public header must remain usable inside a caller's C linkage block. */
extern "C" {
#include "util/json.h"
}

#include <cerrno>
#include <cstdint>
#include <cstring>

#define JSON_PROBE_CHECK(condition)                                                                \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			json_free(root);                                                           \
			return __LINE__;                                                           \
		}                                                                                  \
	} while (0)

extern "C" int json_cpp_api_probe(void)
{
	json *root = json_object();
	json *roots[] = {root, nullptr};
	const char *paths[] = {"once", "unused"};
	char text[] = "Bob";
	int value = 26;
	int root_evaluations = 0;
	int path_evaluations = 0;

	JSON_PROBE_CHECK(root);
	JSON_PROBE_CHECK(json_set(root, "name", text) == 0);
	JSON_PROBE_CHECK(json_set(root, "literal", "Alice") == 0);
	JSON_PROBE_CHECK(json_set(root, "boolean", true) == 0);
	JSON_PROBE_CHECK(json_set(root, "null", nullptr) == 0);
	JSON_PROBE_CHECK(json_set(root, "explicit_null", JSON_NULL) == 0);
	JSON_PROBE_CHECK(json_set(root, "unsigned", std::uint64_t(42)) == 0);
	JSON_PROBE_CHECK(json_set(roots[root_evaluations++], paths[path_evaluations++], value++) ==
			 0);
	JSON_PROBE_CHECK(root_evaluations == 1 && path_evaluations == 1 && value == 27);
	JSON_PROBE_CHECK(json_append(root, "values", value++) == 0 && value == 28);
	JSON_PROBE_CHECK(json_int(root, "values[0]") == 27);
	JSON_PROBE_CHECK(json_is_bool(root, "boolean") && json_bool(root, "boolean"));
	JSON_PROBE_CHECK(json_is_null(root, "null"));
	JSON_PROBE_CHECK(std::strcmp(json_str(root, "name"), "Bob") == 0);
#ifdef JSON_API_REJECT_POINTER
	JSON_PROBE_CHECK(json_set(root, "invalid", &value) == -EINVAL);
#endif
	json_free(root);
	return 0;
}
