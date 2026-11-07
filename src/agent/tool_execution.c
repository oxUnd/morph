#include "tool_execution.h"

#include "permission/context.h"
#include "tool_runtime.h"
#include "util/error.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/** \brief Map a structured business failure to the existing error domain. */
static int tool_error_code(const cJSON *envelope)
{
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(envelope, "error");
	const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));

	if (code != NULL) {
		if (strcmp(code, "invalid_arguments") == 0) {
			return -EINVAL;
		}
		if (strcmp(code, "not_found") == 0) {
			return -ENOENT;
		}
		if (strcmp(code, "permission_denied") == 0) {
			return -EACCES;
		}
		if (strcmp(code, "cancelled") == 0) {
			return -ECANCELED;
		}
		if (strcmp(code, "timeout") == 0) {
			return -ETIMEDOUT;
		}
	}
	return MORPH_ERR_PROCESSING;
}

/**
 * \brief Execute a tool and normalize its business outcome.
 * \param[in] reg: Borrowed registry; execution retains data across concurrent mutation.
 * \param[in] name: Borrowed tool name.
 * \param[in] args_json: Borrowed arguments until return.
 * \param[in,out] result: Initialized caller-owned result, cleared before execution.
 * \return Zero only for business success, otherwise a negative error.
 */
int tool_exec(struct tool_registry *reg, const char *name, const char *args_json,
	      struct tool_result *result)
{
	struct tool_handle handle = {0};
	const cJSON *ok;
	int rc;
	int result_rc;

	if (result == NULL) {
		return -EINVAL;
	}
	tool_result_clear(result);
	if (reg == NULL || name == NULL) {
		return -EINVAL;
	}
	rc = tool_registry_acquire(reg, name, &handle);
	if (rc != 0)
		return rc;
	if (handle.disabled) {
		rc = -EPERM;
	} else if (handle.exec == NULL) {
		rc = -ENOSYS;
	} else {
		rc = 0;
	}
	if (rc != 0) {
		tool_handle_release(&handle);
		return rc;
	}
	if (handle.permission_context &&
	    (!(handle.flags & (TOOL_FLAG_READONLY | TOOL_FLAG_INTERNAL_APPROVAL)) ||
	     (result->runtime && result->runtime->permission_require_user))) {
		struct permission_review review = {0};
		struct tool_operation operation = {
		    .review_out = &review,
		    .kind = TOOL_OP_TOOL_CALL,
		    .tool_name = name,
		    .principal = name,
		    .action = args_json,
		    .details_json = args_json,
		    .require_user = result->runtime ? result->runtime->permission_require_user : 0,
		};
		rc = tool_context_check_operation(handle.permission_context, &operation);
		if (rc == -EACCES || rc == -EPERM)
			(void)tool_result_error_format(
				result, "permission_denied",
				"%s Do not retry through another tool or route to "
				"bypass this decision.",
				review.reason[0] ? review.reason : "Permission was not granted.");
	} else {
		rc = 0;
	}
	if (rc == 0) {
		const struct tool_runtime_context *saved_runtime = result->runtime;
		struct tool_runtime_context execution = {0};

		if (saved_runtime)
			execution = *saved_runtime;
		execution.permission_context = handle.permission_context;
		if (handle.permission_context)
			result->runtime = &execution;
		rc = handle.exec(args_json, result, handle.user_data);
		result->runtime = saved_runtime;
	}
	tool_handle_release(&handle);
	if (rc > 0) {
		rc = MORPH_ERR_PROTOCOL;
	}
	if (result->envelope != NULL) {
		ok = cJSON_GetObjectItemCaseSensitive(result->envelope, "ok");
		if (!cJSON_IsObject(result->envelope) || !cJSON_IsBool(ok)) {
			if (rc == 0) {
				rc = MORPH_ERR_PROTOCOL;
			}
			tool_result_clear(result);
		} else if (rc == 0 && cJSON_IsFalse(ok)) {
			rc = tool_error_code(result->envelope);
		} else if (rc < 0 && cJSON_IsTrue(ok)) {
			const cJSON *payload =
			    cJSON_GetObjectItemCaseSensitive(result->envelope, "data");
			const char *diagnostic =
			    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "text"));
			char *owned = diagnostic != NULL ? strdup(diagnostic) : NULL;
			if (diagnostic != NULL && owned == NULL) {
				tool_result_clear(result);
				return -ENOMEM;
			}
			result_rc = tool_result_error_copy(
				result, "tool_failed", owned != NULL ? owned : morph_strerror(rc));
			free(owned);
			if (result_rc != 0) {
				return result_rc;
			}
		}
	}
	if (rc < 0 && result->envelope == NULL) {
		result_rc = tool_result_error_copy(result,
						   rc == -EINVAL   ? "invalid_arguments"
						   : rc == -EACCES ? "permission_denied"
						   : rc == -EPERM  ? "approval_unavailable"
								   : "tool_failed",
						   morph_strerror(rc));
		if (result_rc != 0) {
			return result_rc;
		}
	}
	result_rc = tool_result_finalize(result);
	return result_rc != 0 ? result_rc : rc;
}
