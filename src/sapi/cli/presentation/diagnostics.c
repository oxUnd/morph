#include "sapi/cli/presentation/diagnostics.h"
#include "nb/cleanup.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "config/config.h"
#include "nb/file.h"
#include "errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
cli_diagnostics_config_diagnostic_reason(const struct config_validation_error *item)
{
	const char *reason = item->message;
	size_t path_len;

	if (item->code == CONFIG_VALIDATION_UNKNOWN_KEY)
		return "unknown key; ignored for forward compatibility";
	if (!item->path[0])
		return reason;
	path_len = strlen(item->path);
	if (strncmp(reason, item->path, path_len) != 0)
		return reason;
	reason += path_len;
	while (*reason == ' ' || *reason == ':')
		reason++;
	return reason;
}

void cli_diagnostics_config(struct cli_application *ctx, int is_warning, const char *path,
			    const struct config_validation_error *item)
{
	const char *color =
		cli_output_color_enabled(ctx) ? (is_warning ? ANSI_YELLOW : ANSI_RED) : "";
	const char *bold = cli_output_color_enabled(ctx) ? ANSI_BOLD : "";
	const char *dim = cli_output_color_enabled(ctx) ? ANSI_DIM : "";
	const char *reset = cli_output_color_enabled(ctx) ? ANSI_RESET : "";
	const char *icon = is_warning ? CLI_FEED_WARNING : CLI_FEED_FAILED;
	const char *title = is_warning ? "Configuration warning" : "Invalid configuration";

	fprintf(stderr, "%s%s%s  %s%s\n", color, bold, icon, title, reset);
	fprintf(stderr, "  %sfile%s    %s", dim, reset, path);
	if (item->line > 0) {
		fprintf(stderr, ":%d", item->line);
		if (item->column > 0)
			fprintf(stderr, ":%d", item->column);
	}
	fputc('\n', stderr);
	if (item->path[0])
		fprintf(stderr, "  %skey%s     %s\n", dim, reset, item->path);
	fprintf(stderr, "  %sreason%s  %s\n", dim, reset,
		cli_diagnostics_config_diagnostic_reason(item));
}

struct cli_diagnostics_config_warning_output {
	struct cli_application *ctx;
	const char *path;
};

static void cli_diagnostics_print_config_warning(const struct config_validation_error *warning,
						 void *user_data)
{
	struct cli_diagnostics_config_warning_output *output = user_data;
	struct cli_application *ctx = output->ctx;
	const char *path = output->path;

	if (warning)
		cli_diagnostics_config(ctx, 1, path, warning);
}

void cli_diagnostics_config_error(struct cli_application *ctx, const char *config_path, int rc)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	struct config_validation_error error = {0};
	const char *requested = config_path ? config_path : "~/.morph/config.toml";
	char *expanded = (nb_path_expand(&file_value_1, requested, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_1))
				  : NULL);
	const char *display = expanded ? expanded : requested;
	int validation_rc = expanded ? config_validate_file(expanded, &error) : NB_ERR_INVAL;

	if ((validation_rc == MORPH_ERR_CONFIG || validation_rc == NB_ERR_PARSE) &&
	    error.message[0]) {
		cli_diagnostics_config(ctx, 0, display, &error);
	} else {
		fprintf(stderr, "morph: failed to load configuration %s: %s\n", display,
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
	free(expanded);
}

int cli_diagnostics_preflight(struct cli_application *ctx, const char *config_path)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

	struct config_validation_error error = {0};
	const char *requested = config_path ? config_path : "~/.morph/config.toml";
	char *expanded = (nb_path_expand(&file_value_2, requested, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_2))
				  : NULL);
	int rc;

	if (!expanded)
		return 0;
	if (!nb_file_exists(expanded)) {
		free(expanded);
		return 0;
	}
	struct cli_diagnostics_config_warning_output output = {.ctx = ctx, .path = expanded};

	rc = config_validate_file_with_warnings(expanded, &error,
						cli_diagnostics_print_config_warning, &output);
	if (rc != 0)
		cli_diagnostics_config(ctx, 0, expanded, &error);
	free(expanded);
	return rc;
}
