#include "permission.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct permission_policy permission_policy_default(void)
{
	return (struct permission_policy){PERMISSION_WORKSPACE,
		PERMISSION_ON_REQUEST, PERMISSION_USER};
}

struct permission_access_state permission_effective_access(
	const struct permission_policy *policy)
{
	struct permission_access_state access = {0};

	if (permission_policy_validate(policy) == 0) {
		access.workspace_write = policy->access != PERMISSION_READ_ONLY;
		access.host = policy->access == PERMISSION_FULL;
		access.network = access.host;
	}
	return access;
}

int permission_policy_validate(const struct permission_policy *policy)
{
	if (!policy || policy->access < PERMISSION_READ_ONLY ||
	    policy->access > PERMISSION_FULL ||
	    policy->approval < PERMISSION_ON_REQUEST ||
	    policy->approval > PERMISSION_NEVER ||
	    policy->reviewer < PERMISSION_USER || policy->reviewer > PERMISSION_MODEL)
		return -EINVAL;
	return 0;
}

const char *permission_access_name(enum permission_access access)
{
	switch (access) {
	case PERMISSION_READ_ONLY: return "read_only";
	case PERMISSION_WORKSPACE: return "workspace";
	case PERMISSION_FULL: return "full";
	}
	return "invalid";
}

const char *permission_approval_name(enum permission_approval approval)
{
	switch (approval) {
	case PERMISSION_ON_REQUEST: return "on_request";
	case PERMISSION_NEVER: return "never";
	}
	return "invalid";
}

const char *permission_reviewer_name(enum permission_reviewer reviewer)
{
	switch (reviewer) {
	case PERMISSION_USER: return "user";
	case PERMISSION_MODEL: return "model";
	}
	return "invalid";
}

int permission_policy_parse(const char *access, const char *approval,
	const char *reviewer, struct permission_policy *out)
{
	if (!access || !approval || !reviewer || !out)
		return -EINVAL;
	for (int a = PERMISSION_READ_ONLY; a <= PERMISSION_FULL; a++) {
		for (int p = PERMISSION_ON_REQUEST; p <= PERMISSION_NEVER; p++) {
			for (int r = PERMISSION_USER; r <= PERMISSION_MODEL; r++) {
				struct permission_policy candidate = {
					(enum permission_access)a,
					(enum permission_approval)p,
					(enum permission_reviewer)r};
				if (!strcmp(access, permission_access_name(candidate.access)) &&
				    !strcmp(approval,
					permission_approval_name(candidate.approval)) &&
				    !strcmp(reviewer,
					permission_reviewer_name(candidate.reviewer))) {
					*out = candidate;
					return 0;
				}
			}
		}
	}
	return -EINVAL;
}

int permission_context_init(struct permission_context *context)
{
	int rc;

	if (!context)
		return -EINVAL;
	memset(context, 0, sizeof(*context));
	context->policy = permission_policy_default();
	rc = pthread_mutex_init(&context->review_lock, NULL);
	return rc ? -rc : 0;
}

void permission_context_cleanup(struct permission_context *context)
{
	if (context)
		pthread_mutex_destroy(&context->review_lock);
}

enum permission_requirement permission_check(const struct permission_policy *policy,
	const struct tool_operation *operation, int within_access)
{
	if (permission_policy_validate(policy) != 0 || !operation || operation->forbidden)
		return PERMISSION_DENY;
	if (operation->require_user)
		return policy->approval == PERMISSION_NEVER ?
			PERMISSION_DENY : PERMISSION_REQUIRE_USER;
	if (!operation->require_review &&
	    (within_access || policy->access == PERMISSION_FULL))
		return PERMISSION_ALLOW;
	return policy->approval == PERMISSION_NEVER ? PERMISSION_DENY : PERMISSION_REVIEW;
}

int permission_authorize(struct permission_context *context,
	const struct tool_operation *operation, enum permission_requirement requirement,
	enum tool_operation_verdict *verdict)
{
	struct permission_review review = {.verdict = TOOL_OP_DENY};
	int automatic;
	int rc = 0;

	if (!context || !operation || !verdict ||
	    permission_policy_validate(&context->policy) != 0)
		return -EINVAL;
	*verdict = TOOL_OP_DENY;
	if (operation->review_out)
		*operation->review_out = review;
	if (requirement == PERMISSION_DENY) {
		if (operation->review_out)
			snprintf(operation->review_out->reason,
				sizeof(operation->review_out->reason),
				"The permission policy forbids this operation.");
		return -EACCES;
	}
	if (requirement == PERMISSION_ALLOW) {
		*verdict = TOOL_OP_ALLOW;
		return 0;
	}
	if ((requirement != PERMISSION_REVIEW && requirement != PERMISSION_REQUIRE_USER) ||
	    context->policy.approval == PERMISSION_NEVER)
		return -EACCES;
	pthread_mutex_lock(&context->review_lock);
	automatic = requirement != PERMISSION_REQUIRE_USER &&
		context->policy.reviewer == PERMISSION_MODEL && context->consecutive_denials < 3;
	if (automatic && context->model_review) {
		rc = context->model_review(operation, &review, context->model_data);
		if (rc == -ECANCELED)
			goto out_unlock;
	} else {
		review.defer = 1;
	}
	if (rc != 0 || review.defer || !automatic) {
		memset(&review, 0, sizeof(review));
		if (!context->user_review) {
			snprintf(review.reason, sizeof(review.reason),
				"Human approval is unavailable.");
			rc = -EPERM;
			goto out_unlock;
		}
		rc = context->user_review(operation, &review, context->user_data);
		automatic = 0;
	}
	if (rc != 0)
		goto out_unlock;
	if (review.defer || review.verdict < TOOL_OP_DENY || review.verdict > TOOL_OP_ALWAYS) {
		rc = -EPROTO;
		goto out_unlock;
	}
	if (automatic && review.verdict > TOOL_OP_ALLOW) {
		rc = -EPROTO;
		goto out_unlock;
	}
	if (review.verdict == TOOL_OP_DENY) {
		if (automatic)
			context->consecutive_denials++;
		rc = -EACCES;
	} else {
		context->consecutive_denials = 0;
		*verdict = review.verdict;
	}
out_unlock:
	review.reason[sizeof(review.reason) - 1] = '\0';
	if (operation->review_out)
		*operation->review_out = review;
	pthread_mutex_unlock(&context->review_lock);
	return rc;
}
