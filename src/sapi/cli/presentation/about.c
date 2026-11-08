#include "sapi/cli/presentation/about.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/diagnostics.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "config/config.h"
#include "skill/skill.h"
#include "util/data.h"
#include "util/file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#define CLI_ABOUT_ICON_VERSION "\uea66"
#define CLI_ABOUT_ICON_OS "\uea7a"
#define CLI_ABOUT_ICON_ARCH "\uf2db"
#define CLI_ABOUT_ICON_LLM "\uf0e7"
#define CLI_ABOUT_ICON_IMAGE "\ueada"
#define CLI_ABOUT_ICON_VIDEO "\uead9"
#define CLI_ABOUT_ICON_REACT "\ueb37"
#define CLI_ABOUT_ICON_MEMORY "\uf5dc"
#define CLI_ABOUT_ICON_CONTEXT "\ueaba"
#define CLI_ABOUT_ICON_TOOLS "\ueb6d"
#define CLI_ABOUT_ICON_SKILLS "\uebcf"
#define CLI_ABOUT_ICON_EXTS "\ueae6"
#define CLI_ABOUT_ICON_MCP "\ueb01"
#define CLI_ABOUT_ICON_CONFIG "\ueaf8"

int cli_about_print(struct cli_application *ctx, const char *config_path)
{
	struct utsname uts;
	int rc = 0;

	uname(&uts);

	struct config cfg;
	config_set_defaults(ctx ? ctx->logger : NULL, &cfg);

	char *cfg_path = file_expand_path_copy(config_path ? config_path : "~/.morph/config.toml");
	if (cfg_path) {
		rc = config_load(ctx ? ctx->logger : NULL, &cfg, cfg_path);
		if (rc != 0) {
			cli_diagnostics_config_error(ctx, config_path, rc);
			free(cfg_path);
			return rc;
		}
	}

	struct skill_registry skills;
	skill_registry_init(ctx ? ctx->logger : NULL, &skills);

	(void)skill_discover_configured(&skills, ".", cfg.skill.dir);

	{
		char *builtin_skills = morph_data_find_alloc("skills");

		if (builtin_skills) {
			skill_discover(&skills, builtin_skills);
			free(builtin_skills);
		}
	}

	int ext_count = 0;
	char **ext_dirs = NULL;
	char *exts_path = file_expand_path_copy(cfg.ext.dir[0] ? cfg.ext.dir : "~/.morph/exts");
	if (exts_path) {
		file_list_dirs_copy(exts_path, &ext_dirs, &ext_count);
		free(exts_path);
	}

	int builtin_tools = 12;
	int has_skill_tool = (skills.count > 0);
	builtin_tools += 2;
	if (has_skill_tool)
		builtin_tools++;

	int mcp_stdio = 0, mcp_http = 0;
	for (int i = 0; i < cfg.mcp.server_count; i++) {
		if (strcmp(cfg.mcp.servers[i].transport, "http") == 0)
			mcp_http++;
		else
			mcp_stdio++;
	}

	char *user = getenv("USER");
	if (!user)
		user = "unknown";

	cli_output_printf(ctx, "\n");
	cli_output_printf(ctx,
			  "" ANSI_BOLD ANSI_CYAN "  __  __    ____    _____    _____    _   _  \n"
			  " |  \\/  |  / __ \\  |  __ \\  |  __ \\  | | | | \n"
			  " | \\  / | | |  | | | |__) | | |__) | | |_| | \n"
			  " | |\\/| | | |__| | |  _  /  |  ___/  |  _  | \n"
			  " |_|  |_|  \\____/  |_| \\_\\  |_|      |_| |_| " ANSI_RESET "\n");
	cli_output_printf(ctx,
			  "           " ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET "@" ANSI_BOLD ANSI_CYAN
			  "%s" ANSI_RESET "\n",
			  user, uts.nodename);
	cli_output_printf(ctx, "           " ANSI_DIM
			       "──────────────────────────────────────" ANSI_RESET "\n");
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_VERSION " " ANSI_BOLD ANSI_GREEN "Version" ANSI_RESET
			  "     %s\n",
			  MORPH_VERSION);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_OS " " ANSI_BOLD ANSI_GREEN "OS" ANSI_RESET
			  "         %s %s\n",
			  uts.sysname, uts.release);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_ARCH " " ANSI_BOLD ANSI_GREEN "Arch" ANSI_RESET
			  "       %s\n",
			  uts.machine);
	cli_output_printf(ctx, "\n");
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_LLM " " ANSI_BOLD ANSI_GREEN "LLM" ANSI_RESET
			  "        " ANSI_YELLOW "%s" ANSI_RESET " / %s\n",
			  cfg.models.text.provider, cfg.models.text.model);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_IMAGE " " ANSI_BOLD ANSI_GREEN "Vision" ANSI_RESET
			  "     " ANSI_YELLOW "%s" ANSI_RESET " / %s\n",
			  cfg.models.vision.provider[0] ? cfg.models.vision.provider : "-",
			  cfg.models.vision.model[0] ? cfg.models.vision.model : "-");
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_IMAGE " " ANSI_BOLD ANSI_GREEN "Image" ANSI_RESET
			  "      " ANSI_YELLOW "%s" ANSI_RESET " / %s\n",
			  cfg.models.image.provider, cfg.models.image.model);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_VIDEO " " ANSI_BOLD ANSI_GREEN "Video" ANSI_RESET
			  "      " ANSI_YELLOW "%s" ANSI_RESET " / %s\n",
			  cfg.models.video.provider,
			  cfg.models.video.model[0] ? cfg.models.video.model : "-");
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_REACT " " ANSI_BOLD ANSI_GREEN "React" ANSI_RESET
			  "      %d iters, %ds tool timeout\n",
			  cfg.react.max_iterations, cfg.react.tool_timeout_seconds);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_MEMORY " " ANSI_BOLD ANSI_GREEN "Memory" ANSI_RESET
			  "     " ANSI_YELLOW "%s" ANSI_RESET " (%d facts, %d past tasks)\n",
			  cfg.memory.enabled ? "on" : "off", cfg.memory.max_facts,
			  cfg.memory.max_past_tasks);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_CONTEXT " " ANSI_BOLD ANSI_GREEN "Context" ANSI_RESET
			  "    %.2f / %.2f compress\n",
			  cfg.context.summarize_threshold_ratio, cfg.context.compress_target_ratio);
	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_TOOLS " " ANSI_BOLD ANSI_GREEN "Tools" ANSI_RESET
			  "      %d built-in, " ANSI_YELLOW "exec on" ANSI_RESET "\n",
			  builtin_tools);

	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_SKILLS " " ANSI_BOLD ANSI_GREEN "Skills" ANSI_RESET
			  "     %d discovered",
			  skills.count);
	if (skills.count > 0) {
		cli_output_printf(ctx, " " ANSI_DIM "(");
		for (int i = 0; i < skills.count && i < 5; i++) {
			if (i > 0)
				cli_output_printf(ctx, ", ");
			cli_output_printf(ctx, "%s", skill_entry_borrow_at(&skills,
			(size_t)i)->fm.name);
		}
		if (skills.count > 5)
			cli_output_printf(ctx, ", …");
		cli_output_printf(ctx, ")" ANSI_RESET);
	}
	cli_output_printf(ctx, "\n");

	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_EXTS " " ANSI_BOLD ANSI_GREEN "Exts" ANSI_RESET
			  "       %d",
			  ext_count);
	if (ext_count > 0) {
		cli_output_printf(ctx, " " ANSI_DIM "(");
		for (int i = 0; i < ext_count && i < 5; i++) {
			if (i > 0)
				cli_output_printf(ctx, ", ");
			cli_output_printf(ctx, "%s", ext_dirs[i]);
		}
		if (ext_count > 5)
			cli_output_printf(ctx, ", …");
		cli_output_printf(ctx, ")" ANSI_RESET);
	}
	cli_output_printf(ctx, "\n");

	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_MCP " " ANSI_BOLD ANSI_GREEN "MCP" ANSI_RESET
			  "        %d server%s",
			  cfg.mcp.server_count, cfg.mcp.server_count == 1 ? "" : "s");
	if (cfg.mcp.server_count > 0)
		cli_output_printf(ctx, " " ANSI_DIM "(stdio:%d, http:%d)" ANSI_RESET, mcp_stdio,
				  mcp_http);
	cli_output_printf(ctx, "\n");

	cli_output_printf(ctx,
			  "  " CLI_ABOUT_ICON_CONFIG " " ANSI_BOLD ANSI_GREEN "Config" ANSI_RESET
			  "     " ANSI_DIM "%s" ANSI_RESET "\n",
			  cfg_path ? cfg_path : "none");
	cli_output_printf(ctx, "\n");

	if (ext_dirs)
		file_free_list(ext_dirs, ext_count);
	skill_registry_cleanup(&skills);
	free(cfg_path);
	return 0;
}
