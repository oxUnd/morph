#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
/* Exercise the real CLI with an isolated database and a local model server. */
#include "sapi/cli/application.h"
#include "http/client.h"
#include "sapi/cli/application_internal.h"
#include <locale.h>

/* Replay the real presenter without a model, for deterministic visual review. */
static int replay_feed(const char *path)
{
	struct cli_application ctx = {0};
	FILE *input = fopen(path, "r");
	char *line = NULL;
	size_t capacity = 0;
	int rc;

	if (!input)
		return -errno;
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.presentation_ready = 1;
	ctx.turn_active = 1;
	(void)getcwd(ctx.workdir, sizeof(ctx.workdir));
	rc = cli_feed_init(&ctx, stdout, STDOUT_FILENO);
	if (rc == 0)
		rc = cli_presentation_init(&ctx);
	while (rc == 0 && getline(&line, &capacity, input) >= 0) {
		struct json *json = json_parse_prefix(line, NULL, NULL);
		const struct json *name = json_member(json, "name");
		struct morph_event event = {0};

		if (!json_is_string_value(name)) {
			json_free(json);
			rc = (MORPH_ERR_PARSE);
			break;
		}
		event.name = json_string_value(name);
		event.type = strncmp(event.name, "tool.", 5) == 0 ?
			MORPH_EVENT_TOOL : MORPH_EVENT_REACT;
		event.data = json_member(json, "data");
		rc = cli_presentation_event(&ctx, &event);
		json_free(json);
	}
	free(line);
	fclose(input);
	cli_feed_render_frame(&ctx, 1);
	fflush(stdout);
	/* Pause before cleanup so a PTY can inspect the active block. */
	if (isatty(STDIN_FILENO))
		(void)getchar();
	cli_presentation_cleanup(&ctx);
	cli_feed_cleanup(&ctx);
	return rc;
}

int main(int argc, char **argv)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	struct cli_application ctx;
	int rc;

	if (argc == 3 && strcmp(argv[1], "--replay-events") == 0) {
		(void)setlocale(LC_ALL, "");
		cli_output_set_color_enabled(_cli_output_context_ptr, 1);
		return replay_feed(argv[2]) == 0 ? 0 : 1;
	}
	if (argc != 4)
		return 2;
	(void)setlocale(LC_ALL, "");
	http_init(NULL);
	rc = cli_application_init(&ctx, NULL, argv[1], argv[3], argv[2], NULL, CLI_PRESENT_INTERACTIVE);
	if (rc == 0) {
		cli_application_run(&ctx);
		cli_application_shutdown(&ctx);
	}
	http_cleanup();
	return rc == 0 ? 0 : 1;
}
