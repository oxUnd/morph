#include "sapi/cli/pages/model.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/components/selector.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/services/sessions.h"
#include "sapi/cli/services/event_queue.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <nothing/tui/terminal/text.h>

static int cli_model_publish_mode(struct cli_application *ctx, void *opaque)
{
	int ultra = *(const int *)opaque;
	cli_composer_model_set_ultra(&ctx->composer, ultra, cli_output_color_enabled(ctx));
	ctx->input_status.updated = 0;
	CMD_OK(ctx, "model mode: %s", ultra ? "ultra" : "normal");
	return 0;
}

int cli_model_set_mode(struct cli_application *ctx, int ultra)
{
	int rc;
	if (ctx->sessions) {
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		const char *scope = cli_sessions_current_id(ctx) > 0
			? "session_and_defaults" : "defaults";
		if (!params || !cJSON_AddBoolToObject(params, "ultra", ultra) ||
		    !cJSON_AddStringToObject(params, "scope", scope))
			return NB_ERR_NOMEM;

		rc = cli_sessions_request_current(ctx, "model.update", params, NULL);
	} else
		rc = runtime_model_set_ultra(ctx->runtime, ultra);
	if (!rc) {
		return ctx->events ? cli_event_queue_call_owner(ctx, cli_model_publish_mode,
			&ultra) : cli_model_publish_mode(ctx, &ultra);
	} else {
		CMD_ERROR(ctx, "model mode change failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}

	return rc;
}

int cli_model_get_mode(struct cli_application *ctx, int *ultra)
{
	if (!ctx || !ultra)
		return NB_ERR_INVAL;

	if (!ctx->sessions)
		return runtime_model_get_ultra(ctx->runtime, ultra);

	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;
	if (cli_sessions_current_id(ctx) > 0)
		rc = cli_sessions_request_current(ctx, "model.get", NULL, &result);
	else {
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!params || !cJSON_AddStringToObject(params, "scope", "defaults"))
			return NB_ERR_NOMEM;

		rc = cli_sessions_request(ctx, "model.get", params, &result);
	}
	if (!rc)
		*ultra = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "ultra"));

	return rc;
}

static void cli_model_apply(struct cli_application *ctx, size_t selection)
{
	(void)cli_model_set_mode(ctx, selection != 0);
}

int cli_model_show(struct cli_application *ctx)
{
	static const struct cli_selector_choice choices[] = {
		{"Normal", "Delegate only when explicitly requested."},
		{"Ultra", "Proactively collaborate with parallel agents."},
	};
	int current;
	int rc = cli_model_get_mode(ctx, &current);
	if (rc)
		return rc;

	struct session session = {0};
	(void)runtime_session_current(ctx->runtime, &session);
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(session.model, strlen(session.model), NT_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	char subtitle[256] = "  ";
	nt_terminal_copy_ellipsized_display_width(subtitle + 2, sizeof(subtitle) - 2, safe, 48, 0);
	return cli_selector_show(ctx, "  Select Model Mode", subtitle, NULL, choices, 2, current,
				 cli_model_apply);
}
