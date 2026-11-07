#include "sapi/cli/commands/options.h"
#include "sapi/cli/presentation/output.h"
#include "event/event.h"
#include "util/error.h"
#include "util/json.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cli_options_emit_option_event(const char *name, const char *phase, const char *command,
					 const char *output)
{
	struct json *data;
	struct json *args;
	struct morph_event event;
	char *json;
	int rc;

	data = json_object();
	args = json_array();
	if (!data || !args) {
		json_free(data);
		json_free(args);
		return -ENOMEM;
	}
	if (!json_add_string(data, "command", command) ||
	    !json_add_string(data, "input", command)) {
		json_free(args);
		json_free(data);
		return -ENOMEM;
	}
	rc = json_object_add(data, "args", args);
	if (rc) {
		json_free(args);
		json_free(data);
		return rc;
	}
	if (output && !json_add_string(data, "output", output)) {
		json_free(data);
		return -ENOMEM;
	}
	event = (struct morph_event){
		.type = MORPH_EVENT_COMMAND,
		.name = name,
		.phase = phase,
		.message = NULL,
		.data = data,
		.turn_id = NULL,
	};
	json = morph_event_to_json_string(&event);
	json_free(data);
	if (!json)
		return -ENOMEM;
	rc = cli_output_write_json(json);
	free(json);
	return rc;
}

int cli_options_emit_result(const char *command, const char *output)
{
	int rc = cli_options_emit_option_event("command.started", "begin", command, NULL);

	if (rc != 0)
		return rc;
	return cli_options_emit_option_event("command.completed", "end", command, output);
}

enum cli_options_option_id {
	CLI_OPTIONS_OPTION_TRACE_JSON = 256,
	CLI_OPTIONS_OPTION_NO_COLOR,
	CLI_OPTIONS_OPTION_EVENTS,
};

static const struct option cli_options_long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"workdir", required_argument, NULL, 'w'},
	{"prompt", required_argument, NULL, 'p'},
	{"session", required_argument, NULL, 's'},
	{"version", no_argument, NULL, 'v'},
	{"help", no_argument, NULL, 'h'},
	{"trace-json", no_argument, NULL, CLI_OPTIONS_OPTION_TRACE_JSON},
	{"no-color", no_argument, NULL, CLI_OPTIONS_OPTION_NO_COLOR},
	{"events", required_argument, NULL, CLI_OPTIONS_OPTION_EVENTS},
	{NULL, 0, NULL, 0},
};

static const char cli_options_usage_text[] =
	"Usage: morph [options]\n"
	"\n"
	"Commands:\n"
	"  resume [NAME|ID]     Resume a session; open the session list without an argument\n"
	"  setup                Choose a role and configure models; save and exit\n"
	"  mcp                  Manage MCP servers (add/list/get/remove/test)\n"
	"  skills (skill)       Install and manage skills from Git or local paths\n"
	"\n"
	"Options:\n"
	"  -c, --config PATH     Use the specified configuration file\n"
	"  -w, --workdir PATH    Use PATH as the working directory\n"
	"  -p, --prompt TEXT     Run once with plain-text progress\n"
	"  -s, --session NAME    Select or create a named session\n"
	"  -v, --version         Show version and runtime information\n"
	"  -h, --help            Show this help text\n"
	"      --trace-json      Include JSON details in trace output\n"
	"      --no-color        Disable ANSI color output\n"
	"      --events json     Emit raw events as NDJSON\n";

static int cli_options_parse_events_mode(struct cli_options *options, const char *mode)
{
	if (strcmp(mode, "json") != 0) {
		fprintf(stderr, "morph: invalid --events mode: %s (expected json)\n", mode);
		return 2;
	}
	options->events_json = 1;
	return 0;
}

int cli_options_parse(int argc, char **argv, struct cli_options *options)
{
	int option;

	memset(options, 0, sizeof(*options));
	opterr = 0;
	optind = 1;
	for (;;) {
		option = getopt_long(argc, argv, "+:c:w:p:s:vh", cli_options_long_options, NULL);
		if (option == -1) {
			if (optind == argc)
				break;
			if (!options->resume && strcmp(argv[optind], "resume") == 0) {
				options->resume = 1;
				optind++;
				continue;
			}
			if (options->resume && !options->resume_ref) {
				options->resume_ref = argv[optind++];
				continue;
			}
			if (!options->resume && (strcmp(argv[optind], "setup") == 0 ||
						 strcmp(argv[optind], "mcp") == 0 ||
						 strcmp(argv[optind], "skills") == 0 ||
						 strcmp(argv[optind], "skill") == 0)) {
				options->command_index = optind;
				return 0;
			}
			fprintf(stderr, "morph: unexpected argument: %s\n", argv[optind]);
			return 2;
		}
		switch (option) {
		case 'c':
			options->config_path = optarg;
			break;
		case 'w':
			options->workdir = optarg;
			break;
		case 'p':
			options->one_shot_prompt = optarg;
			break;
		case 's':
			options->session_name = optarg;
			break;
		case 'v':
			options->show_version = 1;
			break;
		case 'h':
			options->show_help = 1;
			break;
		case CLI_OPTIONS_OPTION_TRACE_JSON:
			options->trace_json = 1;
			break;
		case CLI_OPTIONS_OPTION_NO_COLOR:
			options->no_color = 1;
			break;
		case CLI_OPTIONS_OPTION_EVENTS:
			if (cli_options_parse_events_mode(options, optarg) != 0)
				return 2;
			break;
		case ':':
			fprintf(stderr, "morph: option requires an argument: %s\n",
				argv[optind - 1]);
			return 2;
		case '?':
		default:
			fprintf(stderr, "morph: unknown option: %s\n", argv[optind - 1]);
			return 2;
		}
	}
	if (options->resume && (options->one_shot_prompt || options->session_name ||
				options->events_json || options->show_version)) {
		fprintf(stderr, "morph: resume cannot be combined with --prompt, --session, "
				"--events or --version\n");
		return 2;
	}
	return 0;
}

enum cli_presentation_mode cli_options_presentation_mode(const struct cli_options *options)
{
	if (options->events_json)
		return CLI_PRESENT_EVENTS_JSON;
	if (options->one_shot_prompt)
		return CLI_PRESENT_ONCE_PLAIN;
	return CLI_PRESENT_INTERACTIVE;
}

int cli_options_print_usage(const struct cli_options *options)
{
	if (options->events_json)
		return cli_options_emit_result("--help", cli_options_usage_text) == 0 ? 0 : 1;
	fputs(cli_options_usage_text, stdout);
	return 0;
}
