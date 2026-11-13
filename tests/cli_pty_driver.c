#include "sapi/cli/presentation/output.h"
#include "nb/err.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/agents_browser.h"
#include "nb/cleanup.h"
#include <morph/tui/terminal/size.h>
/* Exercise the real CLI with an isolated database and a local model server. */
#include "sapi/cli/application.h"
#include "http/client.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include <locale.h>
/* Age scheduling state on the UI owner. The host's next timer event starts
 * the real recap, after this owner callback has returned. */
static int advance_recap(void *opaque)
{
	struct cli_application *ctx = opaque;
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

static void advance_recap_signal(int number, void *opaque)
{
	if (number == SIGUSR2)
		(void)cli_event_queue_call_owner(opaque, advance_recap, opaque);
}

/* Replay the real presenter without a model, for deterministic visual review. */
static int replay_feed(const char *path)
{
	struct cli_application ctx = {0};
	FILE *input = fopen(path, "r");
	char *line = NULL;
	size_t capacity = 0;
	int rc;

	if (!input)
		return nb_status_from_errno(errno, NULL);
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
			rc = (NB_ERR_PARSE);
			break;
		}
		event.name = name->valuestring;
		event.type =
			strncmp(event.name, "tool.", 5) == 0 ? MORPH_EVENT_TOOL : MORPH_EVENT_REACT;
		if (!strncmp(event.name, "mcp.", 4))
			event.type = MORPH_EVENT_MCP;
		else if (!strncmp(event.name, "task.", 5))
			event.type = MORPH_EVENT_TASK;
		else if (!strncmp(event.name, "hook.", 5) || !strncmp(event.name, "plan.", 5) ||
			 !strncmp(event.name, "agent.", 6) || !strncmp(event.name, "process.", 8) ||
			 !strncmp(event.name, "background.", 11) || !strncmp(event.name, "history.", 8))
			event.type = MORPH_EVENT_BACKGROUND;
		const cJSON *phase = cJSON_GetObjectItemCaseSensitive(json, "phase");
		const cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
		const cJSON *turn_active = cJSON_GetObjectItemCaseSensitive(json, "turn_active");
		if (cJSON_IsBool(turn_active))
			ctx.turn_active = cJSON_IsTrue(turn_active);
		event.phase = cJSON_GetStringValue(phase);
		event.message = cJSON_GetStringValue(message);
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
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	int columns = 100;
	int rows = 24;
	morph_terminal_dimensions(STDOUT_FILENO, &columns, &rows);
	int rc = (nb_buf_init(&output), nb_buf_reserve(&output, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_agents_browser_append(&output, &state, &data, columns - 1, rows, 1);
	if (rc == 0) {
		cli_output_printf(NULL, "%s", nb_buf_cstr(&output));
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
	struct cli_application ctx = {0};
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
	nb_signal_listener *recap_clock NB_CLEANUP(nb_signal_listener_cleanup) = NULL;
	const char *advance = getenv("MORPH_PTY_ADVANCE_RECAP");
	if (advance && strcmp(advance, "1") == 0) {
		const int signals[] = {SIGUSR2};
		rc = nb_signal_listener_start(&recap_clock, signals, 1, advance_recap_signal, &ctx,
					      NULL);
		if (rc != NB_OK)
			return 1;
	}
	http_init(NULL);
	rc = cli_application_init(&ctx, NULL, argv[1], argv[3], argv[2], NULL, NULL,
				  CLI_PRESENT_INTERACTIVE);
	if (rc == 0) {
		cli_application_run(&ctx);
		(void)cli_event_queue_stop_owner_calls(&ctx);
		(void)nb_signal_listener_stop(&recap_clock, NULL);
		cli_application_shutdown(&ctx);
	}

	http_cleanup();
	return rc == 0 ? 0 : 1;
}
