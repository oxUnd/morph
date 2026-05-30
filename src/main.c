#include "cli.h"
#include "config.h"
#include "skill/skill.h"
#include "util/log.h"
#include "util/file.h"
#include "http/client.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_RESET  "\033[0m"

#define ICON_VERSION "\uea66"
#define ICON_OS      "\uea7a"
#define ICON_ARCH    "\uf2db"
#define ICON_LLM     "\uf0e7"
#define ICON_IMAGE   "\ueada"
#define ICON_VIDEO   "\uead9"
#define ICON_REACT   "\ueb37"
#define ICON_MEMORY  "\uf5dc"
#define ICON_CONTEXT "\ueaba"
#define ICON_TOOLS   "\ueb6d"
#define ICON_SKILLS  "\uebcf"
#define ICON_EXTS    "\ueae6"
#define ICON_MCP     "\ueb01"
#define ICON_CONFIG  "\ueaf8"

static void print_version(const char *config_path)
{
	struct utsname uts;
	uname(&uts);

	struct config cfg;
	config_set_defaults(&cfg);

	char *cfg_path = NULL;
	if (config_path) {
		cfg_path = strdup(config_path);
	} else {
		cfg_path = file_expand_path("~/.morph/config.toml");
	}
	if (cfg_path) {
		config_load(&cfg, cfg_path);
	}

	struct skill_registry skills;
	skill_registry_init(&skills);

	if (cfg.skill.dir[0]) {
		char *sdir = file_expand_path(cfg.skill.dir);
		if (sdir) {
			if (file_exists(sdir))
				skill_discover(&skills, sdir);
			free(sdir);
		}
	} else {
		char *morph_skills = file_expand_path("~/.morph/skills");
		if (morph_skills) {
			if (!file_exists(morph_skills))
				file_ensure_dir(morph_skills);
			skill_discover(&skills, morph_skills);
			free(morph_skills);
		}
		char *agents_skills = file_expand_path("~/.agents/skills");
		if (agents_skills) {
			if (!file_exists(agents_skills))
				file_ensure_dir(agents_skills);
			skill_discover(&skills, agents_skills);
			free(agents_skills);
		}
	}

	int ext_count = 0;
	char **ext_dirs = NULL;
	char *exts_path = file_expand_path(cfg.ext.dir[0] ? cfg.ext.dir : "~/.morph/exts");
	if (exts_path) {
		file_list_dirs(exts_path, &ext_dirs, &ext_count);
	}

	int builtin_tools = 12;
	int has_bash = cfg.react.bash_exec_enabled;
	int has_skill_tool = (skills.count > 0);
	if (has_bash)
		builtin_tools++;
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

	printf("\n");
	printf("" ANSI_BOLD ANSI_CYAN
	       "  __  __    ____    _____    _____    _   _  \n"
	       " |  \\/  |  / __ \\  |  __ \\  |  __ \\  | | | | \n"
	       " | \\  / | | |  | | | |__) | | |__) | | |_| | \n"
	       " | |\\/| | | |__| | |  _  /  |  ___/  |  _  | \n"
	       " |_|  |_|  \\____/  |_| \\_\\  |_|      |_| |_| "
	       ANSI_RESET "\n");
	printf("           "
	       ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET "@"
	       ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET "\n",
	       user, uts.nodename);
	printf("           " ANSI_DIM "──────────────────────────────────────"
	       ANSI_RESET "\n");
	printf("  " ICON_VERSION " " ANSI_BOLD ANSI_GREEN "Version"
	       ANSI_RESET "     %s\n", MORPH_VERSION);
	printf("  " ICON_OS " " ANSI_BOLD ANSI_GREEN "OS"
	       ANSI_RESET "         %s %s\n", uts.sysname, uts.release);
	printf("  " ICON_ARCH " " ANSI_BOLD ANSI_GREEN "Arch"
	       ANSI_RESET "       %s\n", uts.machine);
	printf("\n");
	printf("  " ICON_LLM " " ANSI_BOLD ANSI_GREEN "LLM"
	       ANSI_RESET "        " ANSI_YELLOW "%s" ANSI_RESET
	       " / %s\n",
	       cfg.models.text.provider, cfg.models.text.model);
	printf("  " ICON_IMAGE " " ANSI_BOLD ANSI_GREEN "Image"
	       ANSI_RESET "      " ANSI_YELLOW "%s" ANSI_RESET
	       " / %s\n",
	       cfg.models.image.provider, cfg.models.image.model);
	printf("  " ICON_VIDEO " " ANSI_BOLD ANSI_GREEN "Video"
	       ANSI_RESET "      " ANSI_YELLOW "%s" ANSI_RESET
	       " / %s\n",
	       cfg.models.video.provider,
	       cfg.models.video.model[0] ? cfg.models.video.model : "-");
	printf("  " ICON_REACT " " ANSI_BOLD ANSI_GREEN "React"
	       ANSI_RESET "      %d iters, %ds timeout\n",
	       cfg.react.max_iterations, cfg.react.step_timeout_seconds);
	printf("  " ICON_MEMORY " " ANSI_BOLD ANSI_GREEN "Memory"
	       ANSI_RESET "     " ANSI_YELLOW "%s" ANSI_RESET
	       " (%d facts, %d episodes)\n",
	       cfg.memory.enabled ? "on" : "off",
	       cfg.memory.max_facts, cfg.memory.max_episodes);
	printf("  " ICON_CONTEXT " " ANSI_BOLD ANSI_GREEN "Context"
	       ANSI_RESET "    %.2f / %.2f compress\n",
	       cfg.context.summarize_threshold_ratio,
	       cfg.context.compress_target_ratio);
	printf("  " ICON_TOOLS " " ANSI_BOLD ANSI_GREEN "Tools"
	       ANSI_RESET "      %d built-in, "
	       ANSI_YELLOW "bash %s" ANSI_RESET "\n",
	       builtin_tools, has_bash ? "on" : "off");

	printf("  " ICON_SKILLS " " ANSI_BOLD ANSI_GREEN "Skills"
	       ANSI_RESET "     %d discovered",
	       skills.count);
	if (skills.count > 0) {
		printf(" " ANSI_DIM "(");
		for (int i = 0; i < skills.count && i < 5; i++) {
			if (i > 0)
				printf(", ");
			printf("%s", skills.entries[i].fm.name);
		}
		if (skills.count > 5)
			printf(", …");
		printf(")" ANSI_RESET);
	}
	printf("\n");

	printf("  " ICON_EXTS " " ANSI_BOLD ANSI_GREEN "Exts"
	       ANSI_RESET "       %d", ext_count);
	if (ext_count > 0) {
		printf(" " ANSI_DIM "(");
		for (int i = 0; i < ext_count && i < 5; i++) {
			if (i > 0)
				printf(", ");
			printf("%s", ext_dirs[i]);
		}
		if (ext_count > 5)
			printf(", …");
		printf(")" ANSI_RESET);
	}
	printf("\n");

	printf("  " ICON_MCP " " ANSI_BOLD ANSI_GREEN "MCP"
	       ANSI_RESET "        %d server%s",
	       cfg.mcp.server_count,
	       cfg.mcp.server_count == 1 ? "" : "s");
	if (cfg.mcp.server_count > 0)
		printf(" " ANSI_DIM "(stdio:%d, http:%d)" ANSI_RESET,
		       mcp_stdio, mcp_http);
	printf("\n");

	printf("  " ICON_CONFIG " " ANSI_BOLD ANSI_GREEN "Config"
	       ANSI_RESET "     " ANSI_DIM "%s" ANSI_RESET "\n",
	       cfg_path ? cfg_path : "none");
	printf("\n");

	if (ext_dirs)
		file_free_list(ext_dirs, ext_count);
	skill_registry_cleanup(&skills);
	free(cfg_path);
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	const char *config_path = NULL;
	const char *workdir = NULL;
	const char *one_shot_prompt = NULL;
	int trace_json = 0;
	int show_version = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			config_path = argv[++i];
		else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			config_path = argv[++i];
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			workdir = argv[++i];
		else if (strcmp(argv[i], "--workdir") == 0 && i + 1 < argc)
			workdir = argv[++i];
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			one_shot_prompt = argv[++i];
		else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc)
			one_shot_prompt = argv[++i];
		else if (strcmp(argv[i], "--trace-json") == 0)
			trace_json = 1;
		else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
			show_version = 1;
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: morph [-c config_path] [-w workdir] [-p prompt] [-v] [--trace-json]\n");
			return 0;
		}
	}
	if (show_version) {
		print_version(config_path);
		return 0;
	}
	char *log_dir = file_expand_path("~/.morph/log");
	file_ensure_dir(log_dir);
	char log_path[512];
	snprintf(log_path, sizeof(log_path), "%s/agent.log", log_dir);
	free(log_dir);
	log_init(log_path, getenv("MORPH_DEBUG") ? LOG_DEBUG : LOG_INFO);
	http_init();
	struct cli_context ctx;
	int rc = cli_init(&ctx, config_path, workdir);
	if (rc < 0) {
		log_err("failed to initialize: %d", rc);
		return 1;
	}
	ctx.trace_json = trace_json;
	if (one_shot_prompt) {
		cli_run_once(&ctx, one_shot_prompt);
	} else {
		cli_run(&ctx);
	}
	cli_shutdown(&ctx);
	http_cleanup();
	log_shutdown();
	return 0;
}
