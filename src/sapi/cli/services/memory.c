#include "sapi/cli/services/memory.h"

#include "sapi/cli/application.h"
#include "sapi/cli/services/sessions.h"
#include "runtime/services.h"
#include <nb/cleanup.h>
#include <nb/err.h>

#include <stdlib.h>
#include <string.h>

int cli_memory_request(struct cli_application *ctx, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!ctx || !method || !out || *out)
		return NB_ERR_INVAL;

	if (ctx->sessions && cli_sessions_current_id(ctx) > 0)
		return cli_sessions_request_current(ctx, method, params, out);

	const char *query = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "query"));
	if (!strcmp(method, "memory.report"))
		return runtime_memory_report_copy(ctx->runtime, query, out);

	char *text NB_CLEANUP(nb_charp_cleanup) = NULL;
	int rc = 0;
	if (!strcmp(method, "memory.read"))
		text = runtime_memory_render_current_copy(ctx->runtime, 0);
	else if (!strcmp(method, "memory.jobs"))
		text = runtime_memory_background_render_copy(ctx->runtime);
	else if (!strcmp(method, "preference.read"))
		text = runtime_preferences_render_copy(ctx->runtime,
			cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "history")));
	else if (!strcmp(method, "preference.update"))
		rc = runtime_preference_set(ctx->runtime,
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "scope")),
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "key")),
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "value")));
	else if (!strcmp(method, "memory.retry"))
		rc = runtime_memory_retry_current(ctx->runtime);
	else if (!strcmp(method, "memory.clear"))
		rc = runtime_memory_clear_current(ctx->runtime, (enum memory_clear_scope)
			cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(params, "scope")));
	else
		return NB_ERR_UNSUPPORTED;

	if (rc != 0)
		return rc;

	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!result || (text && !cJSON_AddStringToObject(result, "text", text)))
		return NB_ERR_NOMEM;

	*out = cJSON_StealOwner(&result);
	return 0;
}

char *cli_memory_text_copy(struct cli_application *ctx, const char *method,
	const cJSON *params)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = cli_memory_request(ctx, method, params, &result);
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "text"));
	return rc == 0 && text ? strdup(text) : NULL;
}
