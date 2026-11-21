#include "sapi/cli/services/checkpoints.h"

#include "sapi/cli/application.h"
#include "sapi/cli/services/sessions.h"
#include <nb/cleanup.h>
#include <nb/err.h>

int cli_checkpoints_list(struct cli_application *ctx, cJSON **out)
{
	if (!ctx)
		return NB_ERR_INVAL;

	return ctx->sessions && cli_sessions_current_id(ctx) > 0
		? cli_sessions_request_current(ctx, "checkpoint.list", NULL, out)
		: runtime_checkpoint_list_copy(ctx->runtime, out);
}

int cli_checkpoints_preview(struct cli_application *ctx, int64_t id, cJSON **out)
{
	if (!ctx)
		return NB_ERR_INVAL;

	if (!ctx->sessions)
		return runtime_checkpoint_preview_copy(ctx->runtime, id, out);

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "checkpoint_id", (double)id))
		return NB_ERR_NOMEM;

	return cli_sessions_request_current(ctx, "checkpoint.preview", params, out);
}

int cli_checkpoints_restore(struct cli_application *ctx, int64_t id,
	enum runtime_rewind_mode mode, cJSON **out)
{
	if (!ctx)
		return NB_ERR_INVAL;

	if (!ctx->sessions)
		return runtime_checkpoint_restore(ctx->runtime, id, mode, out);

	const char *value = mode == RUNTIME_REWIND_CODE ? "code" :
		mode == RUNTIME_REWIND_CONVERSATION ? "conversation" :
		mode == RUNTIME_REWIND_BOTH ? "both" : NULL;
	if (!value)
		return NB_ERR_INVAL;

	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "checkpoint_id", (double)id) ||
	    !cJSON_AddStringToObject(params, "mode", value))
		return NB_ERR_NOMEM;

	return cli_sessions_request_current(ctx, "checkpoint.restore", params, out);
}
