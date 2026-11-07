#include "sapi/cli/application.h"
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
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	struct cli_application ctx = {0};
	struct cli_application *startup_ctx = &ctx;
	struct cli_options options;
	struct morph_log logger = {0};
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
			options.config_path ? file_expand_path_copy(options.config_path) : NULL;
		char *management_config =
			options.config_path ? (expanded && getcwd(current, sizeof(current))
						       ? file_path_full_alloc(current, expanded)
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
			return cli_management_error(-ENOMEM, "configuration path");
		if (options.workdir && chdir(options.workdir) != 0) {
			free(management_config);
			return cli_management_error(-errno, "working directory");
		}
		int remaining = argc - options.command_index - 1;
		char **arguments = argv + options.command_index + 1;

		rc = strcmp(command, "setup") == 0 ? cli_setup_command(startup_ctx, remaining,
								       arguments, management_config)
		     : strcmp(command, "mcp") == 0
			     ? cli_mcp_manage_run(&logger, remaining, arguments, management_config)
			     : cli_skills_manage_run(&logger, remaining, arguments,
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
	log_dir = file_expand_path_copy("~/.morph/log");
	if (!log_dir)
		return 1;
	rc = file_ensure_dir(log_dir);
	if (rc == 0)
		rc = file_path_join(log_path, sizeof(log_path), log_dir, "agent.log");
	free(log_dir);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to prepare log directory: %s\n", morph_strerror(rc));
		return 1;
	}
	rc = log_init(&logger, log_path, getenv("MORPH_DEBUG") ? LOG_DEBUG : LOG_INFO);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to open log: %s\n", morph_strerror(rc));
		return 1;
	}
	rc = http_init(&logger);
	if (rc != 0) {
		fprintf(stderr, "morph: failed to initialize HTTP: %s\n", morph_strerror(rc));
		log_shutdown(&logger);
		return 1;
	}
	rc = cli_application_init(&ctx, &logger, options.config_path, NULL, options.workdir,
				  options.session_name, presentation_mode);
	cli_output_set_color_enabled(&ctx, !options.no_color);
	if (rc < 0) {
		log_err(&logger, "failed to initialize: %s", morph_strerror(rc));
		if (ctx.startup_error.file[0])
			cli_diagnostics_config(startup_ctx, 0, ctx.startup_error.file,
					       &ctx.startup_error.detail);
		else
			fprintf(stderr, "morph: failed to initialize: %s\n", morph_strerror(rc));
		http_cleanup();
		log_shutdown(&logger);
		return 1;
	}
	ctx.trace_json = options.trace_json;
	ctx.resume_requested = options.resume && !options.resume_ref;
	if (options.resume_ref) {
		struct session selected;

		rc = runtime_session_find_ref(ctx.runtime, options.resume_ref, &selected);
		if (!rc)
			rc = runtime_session_select_existing(ctx.runtime, selected.id, NULL);
		if (rc) {
			fprintf(stderr, "morph: cannot resume session '%s': %s\n",
				options.resume_ref, morph_strerror(rc));
			cli_application_shutdown(&ctx);
			http_cleanup();
			log_shutdown(&logger);
			return 1;
		}
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
				fprintf(stderr, "morph: restore failed: %s\n", morph_strerror(rc));
			}
			rc = cli_application_init(&ctx, &logger, options.config_path, NULL,
						  options.workdir, options.session_name,
						  presentation_mode);
			cli_output_set_color_enabled(&ctx, !options.no_color);
			if (rc < 0 && file_exists(plan.rollback)) {
				(void)morph_sync_rollback_db_replace(&plan);
				restored = 0;
				rc = cli_application_init(&ctx, &logger, options.config_path, NULL,
							  options.workdir, options.session_name,
							  presentation_mode);
				cli_output_set_color_enabled(&ctx, !options.no_color);
			}
			if (rc < 0) {
				fprintf(stderr, "morph: failed to restart: %s\n",
					morph_strerror(rc));
				http_cleanup();
				log_shutdown(&logger);
				return 1;
			}
			if (restored) {
				cli_output_printf(startup_ctx, ANSI_BOLD ANSI_GREEN
						  "● " ANSI_RESET
						  "database restored; Morph restarted\n");
			}
			ctx.trace_json = options.trace_json;
		}
	}
	cli_application_shutdown(&ctx);
	http_cleanup();
	log_shutdown(&logger);
	return 0;
}
