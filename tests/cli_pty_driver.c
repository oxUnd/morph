#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/agents_browser.h"
#include "util/cleanup.h"
#include "util/terminal_size.h"
/* Exercise the real CLI with an isolated database and a local model server. */
#include "sapi/cli/application.h"
#include "http/client.h"
#ifdef MORPH_PTY_RECAP_CLOCK
#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/services/recap.h"
#include <readline/readline.h>
#endif
#include "sapi/cli/application_internal.h"
#include <locale.h>
#ifdef MORPH_PTY_RECAP_CLOCK

#define MORPH_PTY_RECAP_KEY "\033[99;9~"

/* Age scheduling state on the UI owner. The host's next timer event starts
 * the real recap, after this Readline callback and its redisplay have ended. */
static int advance_recap(struct cli_application *ctx, int count, int key)
{
	(void)count;
	(void)key;
	if (ctx->turn_active)
		return 0;
	int64_t now = cli_recap_now();
	ctx->recap.next_poll = 0;
	cli_recap_tick(ctx, now);
	if (!ctx->recap.unfocused_since || ctx->recap.pending)
		return 0;
	ctx->recap.last_finished = now - CLI_RECAP_DELAY_MS - 1;
	ctx->recap.unfocused_since = now - CLI_RECAP_DELAY_MS - 1;
	ctx->recap.next_poll = 0;
	struct morph_tui_application *app = cli_application_tui_borrow(ctx);
	if (app)
		morph_tui_application_schedule(app, 0);
	return 0;
}
#endif

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
		cJSON *json = cJSON_Parse(line);
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
		struct morph_event event = {0};

		if (!cJSON_IsString(name)) {
			cJSON_Delete(json);
			rc = (MORPH_ERR_PARSE);
			break;
		}
		event.name = name->valuestring;
		event.type =
			strncmp(event.name, "tool.", 5) == 0 ? MORPH_EVENT_TOOL : MORPH_EVENT_REACT;
		event.data = cJSON_GetObjectItemCaseSensitive(json, "data");
		rc = cli_presentation_event(&ctx, &event);
		cJSON_Delete(json);
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

static int preview_agents_browser(void)
{
	struct agent_info agents[3] = {0};
	strcpy(agents[0].id, "/root/reader");
	agents[0].status = AGENT_RUNNING;
	agents[0].description = "Inspect the CLI components and preserve terminal ownership.";
	agents[0].activity = "Reading presentation and widget sources";
	agents[0].started_at_ms = 1000;
	strcpy(agents[1].id, "/root/execution");
	agents[1].status = AGENT_COMPLETED;
	agents[1].description = "Review the process execution boundary.";
	agents[1].result = "Verified session isolation and cancellation.";
	agents[1].started_at_ms = 1000;
	agents[1].ended_at_ms = 18000;
	strcpy(agents[2].id, "/root/reviewer");
	agents[2].status = AGENT_PENDING;
	agents[2].description = "Check the final UI and regressions.";
	struct cli_agents_browser_data data = {
		.agents = agents,
		.count = 3,
		.detail = &agents[0],
		.project = "~/Work/AI/morph",
		.model = "configured text model",
		.root_running = 1,
		.now_ms = 43000,
	};
	struct cli_agents_browser_state state = {0};
	state.selection.index = 1;
	morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int columns = 100;
	int rows = 24;
	morph_terminal_dimensions(STDOUT_FILENO, &columns, &rows);
	int rc = morph_buf_init(&output, BUFSIZ);
	if (rc == 0)
		rc = cli_agents_browser_append(&output, &state, &data, columns - 1, rows, 1);
	if (rc == 0) {
		cli_output_printf(NULL, "%s", morph_buf_cstr_borrow(&output));
		fflush(stdout);
		if (isatty(STDIN_FILENO))
			(void)getchar();
	}
	return rc;
}

int main(int argc, char **argv)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	struct cli_application ctx;
	int rc;
	if (argc == 2 && strcmp(argv[1], "--agents-browser-fixture") == 0) {
		(void)setlocale(LC_ALL, "");
		return preview_agents_browser() == 0 ? 0 : 1;
	}

	if (argc == 3 && strcmp(argv[1], "--replay-events") == 0) {
		(void)setlocale(LC_ALL, "");
		cli_output_set_color_enabled(_cli_output_context_ptr, 1);
		return replay_feed(argv[2]) == 0 ? 0 : 1;
	}
	if (argc != 4)
		return 2;
	(void)setlocale(LC_ALL, "");
	http_init(NULL);
	rc = cli_application_init(&ctx, NULL, argv[1], argv[3], argv[2], NULL,
				  CLI_PRESENT_INTERACTIVE);
	if (rc == 0) {
		const char *advance = getenv("MORPH_PTY_ADVANCE_RECAP");
		int fast_recap = advance && strcmp(advance, "1") == 0;
#ifdef MORPH_PTY_RECAP_CLOCK
		if (fast_recap) {
			rl_initialize();
			int (*callback)(int, int) = cli_callback_key(&ctx, advance_recap);
			if (!callback || rl_bind_keyseq(MORPH_PTY_RECAP_KEY, callback) != 0)
				rc = -EIO;
		}
#else
		if (fast_recap)
			rc = -ENOTSUP;
#endif
		if (rc == 0)
			cli_application_run(&ctx);
#ifdef MORPH_PTY_RECAP_CLOCK
		if (fast_recap)
			(void)rl_bind_keyseq(MORPH_PTY_RECAP_KEY, NULL);
#endif
		cli_application_shutdown(&ctx);
	}
	http_cleanup();
	return rc == 0 ? 0 : 1;
}
