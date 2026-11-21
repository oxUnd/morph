#include "sapi/cli/services/processes.h"

#include "sapi/cli/application.h"
#include "sapi/cli/services/sessions.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>

static int cli_processes_request(struct cli_application *ctx, const char *method,
	const char *id, cJSON **out)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!params || (id && !cJSON_AddStringToObject(params, "process_id", id)))
		return NB_ERR_NOMEM;

	return cli_sessions_request_current(ctx, method, params, out);
}

int cli_process_list(struct cli_application *ctx, cJSON **out)
{
	if (!ctx)
		return NB_ERR_INVAL;

	return ctx->sessions && cli_sessions_current_id(ctx) > 0 ?
		cli_processes_request(ctx, "process.list", NULL, out) :
		runtime_process_list(ctx->runtime, out);
}

int cli_process_read(struct cli_application *ctx, const char *id, cJSON **out)
{
	if (!ctx)
		return NB_ERR_INVAL;

	return ctx->sessions && cli_sessions_current_id(ctx) > 0 ?
		cli_processes_request(ctx, "process.read", id, out) :
		runtime_process_read(ctx->runtime, id, out);
}

int cli_process_stop(struct cli_application *ctx, const char *id)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ctx)
		return NB_ERR_INVAL;

	return ctx->sessions && cli_sessions_current_id(ctx) > 0 ?
		cli_processes_request(ctx, "process.stop", id, &result) :
		runtime_process_stop(ctx->runtime, id);
}
