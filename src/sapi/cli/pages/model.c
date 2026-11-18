#include "sapi/cli/pages/model.h"
#include <nb/err.h>
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/components/selector.h"
#include "sapi/cli/models/composer.h"
#include <nb/cleanup.h>
#include <nothing/tui/terminal/text.h>

int cli_model_set_mode(struct cli_application *ctx, int ultra)
{
	int rc = runtime_model_set_ultra(ctx->runtime, ultra);
	if (!rc) {
		cli_composer_model_set_ultra(&ctx->composer, ultra, cli_output_color_enabled(ctx));
		ctx->input_status.updated = 0;
		CMD_OK(ctx, "model mode: %s", ultra ? "ultra" : "normal");
	} else {
		CMD_ERROR(ctx, "model mode change failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
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
	int rc = runtime_model_get_ultra(ctx->runtime, &current);
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
