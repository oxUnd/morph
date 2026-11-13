#ifndef MORPH_PERMISSION_H
#define MORPH_PERMISSION_H

#include "operation.h"
#include "nb/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

enum permission_access {
	PERMISSION_READ_ONLY,
	PERMISSION_WORKSPACE,
	PERMISSION_FULL,
};

enum permission_approval {
	PERMISSION_ON_REQUEST,
	PERMISSION_NEVER,
};

enum permission_reviewer {
	PERMISSION_USER,
	PERMISSION_MODEL,
};

enum permission_requirement {
	PERMISSION_ALLOW,
	PERMISSION_REVIEW,
	PERMISSION_REQUIRE_USER,
	PERMISSION_DENY,
};

struct permission_policy {
	enum permission_access access;
	enum permission_approval approval;
	enum permission_reviewer reviewer;
};

struct permission_access_state {
	int workspace_write;
	int network;
	int host;
};

struct permission_access_state permission_effective_access(
	const struct permission_policy *policy);

struct permission_review {
	enum tool_operation_verdict verdict;
	char reason[1024];
	int defer;
};

typedef int (*permission_review_fn)(const struct tool_operation *operation,
	struct permission_review *result, void *user_data);

struct permission_context {
	struct permission_policy policy;
	permission_review_fn user_review;
	void *user_data;
	permission_review_fn model_review;
	void *model_data;
	nb_mutex *review_lock;
	unsigned consecutive_denials;
};

struct permission_policy permission_policy_default(void);
int permission_policy_validate(const struct permission_policy *policy);
int permission_policy_parse(const char *access, const char *approval,
	const char *reviewer, struct permission_policy *out);
/* Name strings are immutable process-lifetime labels and must not be freed. */
const char *permission_access_name(enum permission_access access);
const char *permission_approval_name(enum permission_approval approval);
const char *permission_reviewer_name(enum permission_reviewer reviewer);
int permission_context_init(struct permission_context *context);
void permission_context_cleanup(struct permission_context *context);
enum permission_requirement permission_check(const struct permission_policy *policy,
	const struct tool_operation *operation, int within_access);
int permission_authorize(struct permission_context *context,
	const struct tool_operation *operation, enum permission_requirement requirement,
	enum tool_operation_verdict *verdict);

#ifdef __cplusplus
}
#endif
#endif
