#include "sapi/cli/application.h"
#include "nb/cleanup.h"
#include "sapi/cli/commands/options.h"
#include "sapi/cli/commands/mcp_manage.h"
#include "sapi/cli/commands/skills_manage.h"
#include "sapi/cli/pages/setup/wizard.h"
#include "sapi/cli/presentation/about.h"
#include "sapi/cli/presentation/diagnostics.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/management.h"
#include "runtime/services.h"
#include "http/client.h"
#include "nb/log.h"
#include "nb/file.h"
#include "errors.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;

	struct cli_application ctx = {0};
	struct cli_application *startup_ctx = &ctx;
	struct cli_options options;
	struct nb_log *logger = NULL;
	enum cli_presentation_mode presentation_mode;
	char log_path[PATH_MAX];
	char *log_dir;
	int rc;

	setlocale(LC_ALL, "");
	rc = cli_options_parse(argc, argv, &options);
	if (rc != 0)
		return rc;
	presentation_mode = cli_options_presentation_mode(&options);
	const char *no_color = getenv("NO_COLOR");
	const char *term = getenv("TERM");

	if (options.one_shot_prompt || options.events_json || !isatty(STDOUT_FILENO) ||
	    (no_color && no_color[0]) || (term && strcmp(term, "dumb") == 0))
		options.no_color = 1;
	cli_output_set_color_enabled(startup_ctx, !options.no_color);
	if (options.show_help)
		return cli_options_print_usage(&options);
	if (options.command_index) {
		char *command = argv[options.command_index];
		char current[PATH_MAX];
		char *expanded =
			options.config_path
				? (nb_path_expand(&file_value_1, options.config_path, NULL) == NB_OK
					   ? strdup(nb_buf_cstr(&file_value_1))
					   : NULL)
				: NULL;
		char *management_config =
			options.config_path
				? (expanded && getcwd(current, sizeof(current))
					   ? (nb_path_join(&file_value_2, current, expanded) ==
							      NB_OK
						      ? strdup(nb_buf_cstr(&file_value_2))
						      : NULL)
					   : NULL)
				: NULL;

		free(expanded);

		if (options.one_shot_prompt || options.session_name || options.events_json ||
		    options.show_version || options.trace_json) {
			fputs("morph: session/output options cannot be used with management "
			      "commands\n",
			      stderr);
			free(management_config);
			return 2;
		}
		if (options.config_path && !management_config)
			return cli_management_error(NB_ERR_NOMEM, "configuration path");
		if (options.workdir && chdir(options.workdir) != 0) {
			free(management_config);
			return cli_management_error(nb_status_from_errno(errno, NULL),
						    "working directory");
		}
		int remaining = argc - options.command_index - 1;
		char **arguments = argv + options.command_index + 1;

		rc = strcmp(command, "setup") == 0 ? cli_setup_command(startup_ctx, remaining,
								       arguments, management_config)
		     : strcmp(command, "mcp") == 0
			     ? cli_mcp_manage_run(logger, remaining, arguments, management_config)
			     : cli_skills_manage_run(logger, remaining, arguments,
						     management_config);
		free(management_config);
		return rc;
	}
	if (!options.show_version) {
		int setup_rc =
			cli_setup_if_missing(startup_ctx, options.config_path,
					     !options.one_shot_prompt && !options.events_json);
		if (setup_rc != 0)
			return setup_rc < 0 ? 1 : 0;
	}
	if (cli_diagnostics_preflight(startup_ctx, options.config_path) != 0)
		return 1;
	if (options.show_version) {
		if (options.events_json)
			return cli_options_emit_result("--version", MORPH_VERSION) == 0 ? 0 : 1;
		return cli_about_print(startup_ctx, options.config_path) == 0 ? 0 : 1;
	}
	log_dir = (nb_path_expand(&file_value_3, "~/.morph/log", NULL) == NB_OK
			   ? strdup(nb_buf_cstr(&file_value_3))
			   : NULL);
	if (!log_dir)
		return 1;
	rc = nb_file_mkdir(log_dir, true, NULL);
	if (rc == 0)
		rc = ((file_status_4 = nb_path_concat(&file_value_4, log_dir, "agent.log")) != NB_OK
			      ? file_status_4
		      : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), log_path,
							sizeof(log_path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_4);
	free(log_dir);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to prepare log directory: %s\n",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return 1;
	}
	logger = nb_log_new_file(getenv("MORPH_DEBUG") ? NB_LOG_DEBUG : NB_LOG_INFO, log_path,
				 5 * 1024 * 1024, 3);
	rc = logger ? 0 : nb_status_from_errno(errno, NULL);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to open log: %s\n",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return 1;
	}
	rc = http_init(logger);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to initialize HTTP: %s\n",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		nb_log_free(logger);
		logger = NULL;
		return 1;
	}
	rc = cli_application_init(&ctx, logger, options.config_path, NULL, options.workdir,
				  options.session_name, options.resume_ref, presentation_mode);
	cli_output_set_color_enabled(&ctx, !options.no_color);
	if (rc < 0) {
		nb_log_write(logger, NB_LOG_ERROR, "failed to initialize: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		if (ctx.startup_error.file[0])
			cli_diagnostics_config(startup_ctx, 0, ctx.startup_error.file,
					       &ctx.startup_error.detail);
		else if (options.resume_ref)
			fprintf(stderr, "morph: cannot resume session '%s': %s\n",
				options.resume_ref,
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		else
			fprintf(stderr, "morph: failed to initialize: %s\n",
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		http_cleanup();
		nb_log_free(logger);
		logger = NULL;
		return 1;
	}
	ctx.trace_json = options.trace_json;
	ctx.resume_requested = options.resume && !options.resume_ref;
	if (options.resume_ref) {
		ctx.session_auto_named = 1;
		ctx.session_history_pending = 1;
	}
	if (options.one_shot_prompt) {
		cli_application_run_once(&ctx, options.one_shot_prompt);
	} else {
		for (;;) {
			struct morph_sync_restore_plan plan;
			int restored;

			cli_application_run(&ctx);
			if (!ctx.pending_db_restore)
				break;
			plan = ctx.db_restore_plan;
			cli_application_shutdown(&ctx);
			rc = morph_sync_apply_db_replace(&plan);
			restored = rc == 0;
			if (rc != 0) {
				fprintf(stderr, "morph: restore failed: %s\n",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   rc));
			}
			rc = cli_application_init(&ctx, logger, options.config_path, NULL,
						  options.workdir, options.session_name,
						  options.resume_ref, presentation_mode);
			cli_output_set_color_enabled(&ctx, !options.no_color);
			if (rc < 0 && nb_file_exists(plan.rollback)) {
				(void)morph_sync_rollback_db_replace(&plan);
				restored = 0;
				rc = cli_application_init(&ctx, logger, options.config_path, NULL,
							  options.workdir, options.session_name,
							  options.resume_ref, presentation_mode);
				cli_output_set_color_enabled(&ctx, !options.no_color);
			}
			if (rc < 0) {
				fprintf(stderr, "morph: failed to restart: %s\n",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   rc));
				http_cleanup();
				nb_log_free(logger);
				logger = NULL;
				return 1;
			}
			if (restored) {
				cli_output_printf(startup_ctx, ANSI_BOLD ANSI_GREEN
						  CLI_FEED_DONE " " ANSI_RESET
						  "database restored; Morph restarted\n");
			}
			ctx.trace_json = options.trace_json;
		}
	}
	cli_application_shutdown(&ctx);
	http_cleanup();
	nb_log_free(logger);
	logger = NULL;
	return 0;
}
