#include "permission.h"
#include "nb/err.h"

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
		return NB_ERR_INVAL;
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
		return NB_ERR_INVAL;
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
	return NB_ERR_INVAL;
}

int permission_context_init(struct permission_context *context)
{
	int rc;

	if (!context)
		return NB_ERR_INVAL;
	memset(context, 0, sizeof(*context));
	context->policy = permission_policy_default();
	rc = ((context->review_lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	return rc ? rc : 0;
}

void permission_context_cleanup(struct permission_context *context)
{
	if (context)
		(nb_mutex_free(context->review_lock), context->review_lock = NULL);
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

static int permission_review_operation(struct permission_context *context,
	const struct tool_operation *operation, enum permission_requirement requirement,
	enum tool_operation_verdict *verdict, struct permission_review *review)
{
	int automatic;
	int rc = 0;

	automatic = requirement != PERMISSION_REQUIRE_USER &&
		context->policy.reviewer == PERMISSION_MODEL && context->consecutive_denials < 3;
	if (automatic && context->model_review) {
		rc = context->model_review(operation, review, context->model_data);
		if (rc == NB_ERR_CANCELLED)
			return rc;
	} else {
		review->defer = 1;
	}
	if (rc != 0 || review->defer || !automatic) {
		memset(review, 0, sizeof(*review));
		if (!context->user_review) {
			snprintf(review->reason, sizeof(review->reason),
				"Human approval is unavailable.");
			rc = NB_ERR_NOT_PERMITTED;
			return rc;
		}
		rc = context->user_review(operation, review, context->user_data);
		automatic = 0;
	}
	if (rc != 0)
		return rc;
	if (review->defer || review->verdict < TOOL_OP_DENY || review->verdict > TOOL_OP_ALWAYS) {
		rc = NB_ERR_PROTOCOL;
		return rc;
	}
	if (automatic && review->verdict > TOOL_OP_ALLOW) {
		rc = NB_ERR_PROTOCOL;
		return rc;
	}
	if (review->verdict == TOOL_OP_DENY) {
		if (automatic)
			context->consecutive_denials++;
		rc = NB_ERR_PERMISSION;
	} else {
		context->consecutive_denials = 0;
		*verdict = review->verdict;
	}
	return rc;
}

int permission_authorize(struct permission_context *context,
	const struct tool_operation *operation, enum permission_requirement requirement,
	enum tool_operation_verdict *verdict)
{
	struct permission_review review = {.verdict = TOOL_OP_DENY};
	int rc = 0;

	if (!context || !operation || !verdict ||
	    permission_policy_validate(&context->policy) != 0)
		return NB_ERR_INVAL;
	*verdict = TOOL_OP_DENY;
	if (operation->review_out)
		*operation->review_out = review;
	if (requirement == PERMISSION_DENY) {
		if (operation->review_out)
			snprintf(operation->review_out->reason,
				sizeof(operation->review_out->reason),
				"The permission policy forbids this operation.");
		return NB_ERR_PERMISSION;
	}
	if (requirement == PERMISSION_ALLOW) {
		*verdict = TOOL_OP_ALLOW;
		return 0;
	}
	if ((requirement != PERMISSION_REVIEW && requirement != PERMISSION_REQUIRE_USER) ||
	    context->policy.approval == PERMISSION_NEVER)
		return NB_ERR_PERMISSION;
	nb_mutex_lock(context->review_lock);
	rc = permission_review_operation(context, operation, requirement, verdict, &review);
	review.reason[sizeof(review.reason) - 1] = '\0';
	if (operation->review_out)
		*operation->review_out = review;
	nb_mutex_unlock(context->review_lock);
	return rc;
}
