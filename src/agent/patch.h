#ifndef AGENT_PATCH_H
#define AGENT_PATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "util/array.h"
#include <limits.h>

enum patch_action {
	PATCH_ACTION_ADD,
	PATCH_ACTION_UPDATE,
	PATCH_ACTION_DELETE,
};

struct patch_change {
	char path[PATH_MAX];
	enum patch_action action;
	int added;
	int removed;
};

struct patch_result {
	morph_array_t changes;
};

int patch_apply(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size);
void patch_result_cleanup(struct patch_result *result);
const char *patch_action_name(enum patch_action action);

#ifdef __cplusplus
}
#endif

#endif
