#include "probe.h"

#include "config/config.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <stdlib.h>
#include <string.h>

int ext_run_v2(const struct ext_context *context, const char *args_json, char **result_json)
{
	if (!context || context->abi_version != EXT_CONTEXT_ABI_VERSION ||
	    context->struct_size < sizeof(*context) || !context->config || !context->host ||
	    context->host->abi_version != EXT_CONTEXT_ABI_VERSION ||
	    context->host->struct_size < sizeof(*context->host) || !args_json || !result_json)
		return NB_ERR_INVAL;

	if (*result_json)
		return NB_ERR_EXISTS;

	int rc = context->host->check_cancelled(context->host_data);
	if (rc != NB_OK)
		return rc;

	cJSON *input NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(args_json);
	cJSON *output NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!cJSON_IsObject(input))
		return NB_ERR_INVAL;

	if (!output ||
	    !cJSON_AddStringToObject(output, "workdir", context->paths.workdir) ||
	    !cJSON_AddStringToObject(output, "output_dir", context->paths.output_dir) ||
	    !cJSON_AddStringToObject(output, "extension_dir", context->paths.extension_dir) ||
	    !cJSON_AddStringToObject(output, "data_dir", context->paths.data_dir) ||
	    !cJSON_AddStringToObject(output, "temp_dir", context->paths.temp_dir) ||
	    !cJSON_AddStringToObject(output, "user_id", context->call.user_id) ||
	    !cJSON_AddStringToObject(output, "session_id", context->call.session_id) ||
	    !cJSON_AddStringToObject(output, "agent_id", context->call.agent_id) ||
	    !cJSON_AddStringToObject(output, "turn_id", context->call.turn_id) ||
	    !cJSON_AddStringToObject(output, "tool_call_id", context->call.tool_call_id) ||
	    !cJSON_AddStringToObject(output, "model", context->config->models.text.model))
		return NB_ERR_NOMEM;

	context->host->log(context->host_data, NB_LOG_INFO, "Reading extension call context");
	rc = context->host->emit_stream(context->host_data, "status", "Reading extension context");
	if (rc != NB_OK)
		return rc;

	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(output);
	if (!json)
		return NB_ERR_NOMEM;

	*result_json = strdup(json);
	return *result_json ? NB_OK : NB_ERR_NOMEM;
}
