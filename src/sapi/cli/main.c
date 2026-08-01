#include "sapi/cli/cli.h"
#include "config/config.h"
#include "skill/skill.h"
#include "util/data.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
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
#define ANSI_RED    "\033[31m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_RESET  "\033[0m"

#define printf cli_printf

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

static const char *config_diagnostic_reason(
	const struct config_validation_error *item)
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

static void print_config_diagnostic(int is_warning, const char *path,
				    const struct config_validation_error *item)
{
	const char *color = cli_color_enabled() ?
		(is_warning ? ANSI_YELLOW : ANSI_RED) : "";
	const char *bold = cli_color_enabled() ? ANSI_BOLD : "";
	const char *dim = cli_color_enabled() ? ANSI_DIM : "";
	const char *reset = cli_color_enabled() ? ANSI_RESET : "";
	const char *icon = is_warning ? "⚠" : "✗";
	const char *title = is_warning ? "Configuration warning" :
		"Invalid configuration";

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
		config_diagnostic_reason(item));
}

static void print_config_warning(
	const struct config_validation_error *warning, void *user_data)
{
	const char *path = user_data;

	if (warning)
		print_config_diagnostic(1, path, warning);
}

static void print_config_error(const char *config_path, int rc)
{
	struct config_validation_error error = {0};
	const char *requested = config_path ? config_path :
		"~/.morph/config.toml";
	char *expanded = file_expand_path(requested);
	const char *display = expanded ? expanded : requested;
	int validation_rc = expanded ? config_validate_file(expanded, &error) :
		-EINVAL;

	if ((validation_rc == MORPH_ERR_CONFIG ||
	     validation_rc == MORPH_ERR_PARSE) && error.message[0]) {
		print_config_diagnostic(0, display, &error);
	} else {
		fprintf(stderr, "morph: failed to load configuration %s: %s\n",
			display, morph_strerror(rc));
	}
	free(expanded);
}

static int preflight_config(const char *config_path)
{
	struct config_validation_error error = {0};
	const char *requested = config_path ? config_path :
		"~/.morph/config.toml";
	char *expanded = file_expand_path(requested);
	int rc;

	if (!expanded)
		return 0;
	if (!file_exists(expanded)) {
		free(expanded);
		return 0;
	}
	rc = config_validate_file_with_warnings(expanded, &error,
		print_config_warning, expanded);
	if (rc != 0)
		print_config_diagnostic(0, expanded, &error);
	free(expanded);
	return rc;
}

static int print_version(const char *config_path)
{
	struct utsname uts;
	int rc = 0;

	uname(&uts);

	struct config cfg;
	config_set_defaults(&cfg);

	char *cfg_path = file_expand_path(config_path ? config_path :
		"~/.morph/config.toml");
	if (cfg_path) {
		rc = config_load(&cfg, cfg_path);
		if (rc != 0) {
			print_config_error(config_path, rc);
			free(cfg_path);
			return rc;
		}
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
	{
		char *builtin_skills = morph_data_find_alloc("skills");

		if (builtin_skills) {
			skill_discover(&skills, builtin_skills);
			free(builtin_skills);
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
	printf("  " ICON_IMAGE " " ANSI_BOLD ANSI_GREEN "Vision"
	       ANSI_RESET "     " ANSI_YELLOW "%s" ANSI_RESET
	       " / %s\n",
	       cfg.models.vision.provider[0] ? cfg.models.vision.provider : "-",
	       cfg.models.vision.model[0] ? cfg.models.vision.model : "-");
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
	       ANSI_RESET "      %d iters, %ds tool timeout\n",
	       cfg.react.max_iterations, cfg.react.tool_timeout_seconds);
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
	return 0;
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	const char *config_path = NULL;
	const char *workdir = NULL;
	const char *one_shot_prompt = NULL;
	int trace_json = 0;
	int show_version = 0;
	int show_help = 0;
	int no_color = 0;
	int events_json = 0;
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
		else if (strcmp(argv[i], "--no-color") == 0)
			no_color = 1;
		else if (strcmp(argv[i], "--events") == 0) {
			const char *mode;

			if (i + 1 >= argc) {
				fprintf(stderr,
					"missing value for --events "
					"(expected json)\n");
				return 2;
			}
			mode = argv[++i];
			if (strcmp(mode, "json") == 0) {
				events_json = 1;
			} else {
				fprintf(stderr,
					"invalid --events mode: %s "
					"(expected json)\n", mode);
				return 2;
			}
		} else if (strncmp(argv[i], "--events=", 9) == 0) {
			const char *mode = argv[i] + 9;
			if (strcmp(mode, "json") == 0) {
				events_json = 1;
			} else {
				fprintf(stderr,
					"invalid --events mode: %s "
					"(expected json)\n", mode);
				return 2;
			}
		}
		else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
			show_version = 1;
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
			show_help = 1;
	}
	enum cli_presentation_mode presentation_mode =
		events_json ? CLI_PRESENT_EVENTS_JSON :
		(one_shot_prompt ? CLI_PRESENT_ONCE_PLAIN :
		 CLI_PRESENT_INTERACTIVE);
	if (one_shot_prompt || events_json)
		no_color = 1;
	cli_set_color_enabled(!no_color);
	if (show_help) {
		printf("Usage: morph [-c config_path] [-w workdir] "
		       "[-p prompt] [-v] [--trace-json] [--no-color] "
		       "[--events json]\n");
		printf("  -p, --prompt  Run once with plain-text progress\n");
		printf("  --events json  Emit raw events as NDJSON\n");
		printf("  --no-color  Disable ANSI color output\n");
		return 0;
	}
	if (preflight_config(config_path) != 0)
		return 1;
	if (show_version) {
		return print_version(config_path) == 0 ? 0 : 1;
	}
	char *log_dir = file_expand_path("~/.morph/log");
	file_ensure_dir(log_dir);
	char log_path[PATH_MAX];
	snprintf(log_path, sizeof(log_path), "%s/agent.log", log_dir);
	free(log_dir);
	log_init(log_path, getenv("MORPH_DEBUG") ? LOG_DEBUG : LOG_INFO);
	http_init();
	struct cli_context ctx;
	int rc = cli_init(&ctx, config_path, workdir, presentation_mode);
	if (rc < 0) {
		log_err("failed to initialize: %s", morph_strerror(rc));
		if (rc == MORPH_ERR_CONFIG || rc == MORPH_ERR_PARSE)
			print_config_error(config_path, rc);
		else
			fprintf(stderr, "morph: failed to initialize: %s\n",
				morph_strerror(rc));
		http_cleanup();
		log_shutdown();
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
