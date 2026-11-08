#ifndef AGENT_PATCH_H
#define AGENT_PATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "util/array.h"
#include "util/buf.h"
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

typedef int (*patch_authorize_fn)(const char *path, int deletion, void *user_data);
typedef int (*patch_validate_write_fn)(const char *path, size_t bytes,
				      const char *removed_path, void *user_data);

struct patch_write_policy {
	size_t max_file_bytes;
	patch_validate_write_fn validate;
	void *user_data;
};
/* Result owners must be zero-initialized; failures leave them empty. */
int patch_apply_authorized(const char *workdir, const char *input,
	struct patch_result *result, char *error, size_t error_size,
	patch_authorize_fn authorize, void *user_data);
/* Reject an oversized postimage before writing that file. Earlier hunks can
 * already have applied, as with any other patch failure. Zero is unlimited. */
int patch_apply_authorized_with_limit(const char *workdir, const char *input,
	struct patch_result *result, char *error, size_t error_size,
	patch_authorize_fn authorize, void *user_data, size_t max_file_bytes);
int patch_apply_authorized_with_policy(const char *workdir, const char *input,
	struct patch_result *result, char *error, size_t error_size,
	patch_authorize_fn authorize, void *user_data, const struct patch_write_policy *policy);
int patch_apply(const char *workdir, const char *input,
		struct patch_result *result, char *error, size_t error_size);
/*
 * Render the patch as a unified diff (Codex-style) without touching the
 * filesystem. Target files are read to locate the hunks so the emitted
 * line numbers are the real line numbers from those files. Returns a
 * negative error when the patch cannot be previewed; callers may then fall
 * back to showing the raw patch text.
 */
int patch_preview(const char *workdir, const char *input, morph_buf_t *out,
		  char *error, size_t error_size);
void patch_result_cleanup(struct patch_result *result);
const char *patch_action_name(enum patch_action action);

#ifdef __cplusplus
}
#endif

#endif
