/* Exercise the real CLI with an isolated database and a local model server. */
#include "sapi/cli/cli.h"
#include "http/client.h"
#include "sapi/cli/internal.h"
#include <locale.h>

extern const char *default_db_path;

/* Replay the real presenter without a model, for deterministic visual review. */
static int replay_feed(const char *path)
{
	struct cli_context ctx = {0};
	FILE *input = fopen(path, "r");
	char *line = NULL;
	size_t capacity = 0;
	int rc;

	if (!input)
		MORPH_RETURN(-errno);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.presentation_ready = 1;
	ctx.turn_active = 1;
	(void)getcwd(ctx.workdir, sizeof(ctx.workdir));
	rc = cli_terminal_init(&ctx, stdout, STDOUT_FILENO);
	if (rc == 0)
		rc = cli_presentation_init(&ctx);
	while (rc == 0 && getline(&line, &capacity, input) >= 0) {
		cJSON *json = cJSON_Parse(line);
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
		struct morph_event event = {0};

		if (!cJSON_IsString(name)) {
			cJSON_Delete(json);
			MORPH_SET_ERR(rc, MORPH_ERR_PARSE);
			break;
		}
		event.name = name->valuestring;
		event.type = strncmp(event.name, "tool.", 5) == 0 ?
			MORPH_EVENT_TOOL : MORPH_EVENT_REACT;
		event.data = cJSON_GetObjectItemCaseSensitive(json, "data");
		rc = cli_presentation_event(&ctx, &event);
		cJSON_Delete(json);
	}
	free(line);
	fclose(input);
	cli_terminal_render_frame(&ctx, 1);
	fflush(stdout);
	/* Pause before cleanup so a PTY can inspect the active block. */
	if (isatty(STDIN_FILENO))
		(void)getchar();
	cli_presentation_cleanup(&ctx);
	cli_terminal_cleanup(&ctx);
	return rc;
}

int main(int argc, char **argv)
{
	struct cli_context ctx;
	int rc;

	if (argc == 3 && strcmp(argv[1], "--replay-events") == 0) {
		(void)setlocale(LC_ALL, "");
		cli_set_color_enabled(1);
		return replay_feed(argv[2]) == 0 ? 0 : 1;
	}
	if (argc != 4)
		return 2;
	(void)setlocale(LC_ALL, "");
	default_db_path = argv[3];
	http_init();
	rc = cli_init(&ctx, argv[1], argv[2], NULL, CLI_PRESENT_INTERACTIVE);
	if (rc == 0) {
		cli_run(&ctx);
		cli_shutdown(&ctx);
	}
	http_cleanup();
	return rc == 0 ? 0 : 1;
}
