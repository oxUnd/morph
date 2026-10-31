#include "sapi/cli/ui/application.h"
#include "sapi/cli/internal.h"

struct cli_screen {
	struct cli_context *ctx;
	struct morph_tui_core_screen_ops ops;
	void *data;
	void (*complete)(void *data, int result);
};

static int cli_screen_event(void *data, struct morph_tui_core_app *app,
			    const struct morph_tui_core_event *event)
{
	struct cli_screen *screen = data;
	int rc = screen->ops.update(screen->data, event);

	if (rc == MORPH_TUI_CORE_SCREEN_CLOSE)
		return morph_tui_core_app_dismiss(app, 0);
	if (rc == MORPH_TUI_CORE_SCREEN_REDRAW)
		morph_tui_core_app_invalidate(app, MORPH_TUI_CORE_INVALIDATE_DISPLAY);
	return rc < 0 ? rc : 1;
}

static int cli_screen_draw(void *data, const struct morph_tui_core_screen *terminal)
{
	struct cli_screen *screen = data;

	return screen->ops.draw(screen->data, terminal);
}

int cli_application_flush(struct cli_context *ctx)
{
	morph_buf_t *pending = ctx->application_output;

	if (pending && pending->failed)
		return pending->failed;
	if (pending && pending->len) {
		if (fwrite(morph_buf_cstr(pending), 1, pending->len, stdout) != pending->len ||
		    fflush(stdout) != 0)
			return errno ? -errno : -EIO;
		morph_buf_reset(pending);
	}
	return 0;
}

void cli_application_panel_complete(struct cli_context *ctx, void *data,
				    void (*complete)(void *, int), int result)
{
	if (morph_tui_core_app_depth(ctx->application) <= 1) {
		int rc = cli_application_flush(ctx);

		if (rc) {
			result = rc;
			morph_tui_core_app_stop(ctx->application, rc);
		}
	}
	complete(data, result);
	if (ctx->application_command && morph_tui_core_app_depth(ctx->application) <= 1) {
		ctx->application_command = 0;
		if (result < 0 && result != -ECANCELED)
			CMD_ERROR(ctx, "%s", morph_strerror(result));
		cli_presentation_command_end(ctx);
	}
}

static void cli_screen_close(void *data, int result)
{
	struct cli_screen *screen = data;

	cli_application_panel_complete(screen->ctx, screen->data, screen->complete, result);
	free(screen);
}

int cli_screen_present(struct cli_context *ctx, const struct morph_tui_core_screen_ops *ops,
		       void *data, void (*complete)(void *data, int result))
{
	if (!ctx || !ctx->application || !ops || !ops->update || !ops->draw || !complete)
		return -EINVAL;
	struct cli_screen *screen = calloc(1, sizeof(*screen));

	if (!screen)
		return -ENOMEM;
	*screen = (struct cli_screen){.ctx = ctx, .ops = *ops, .data = data, .complete = complete};
	static const struct morph_tui_core_controller_ops controller_ops = {
		.event = cli_screen_event,
		.draw = cli_screen_draw,
		.close = cli_screen_close,
	};
	struct morph_tui_core_controller controller = {
		.ops = &controller_ops,
		.data = screen,
		.input_mode = MORPH_TUI_CORE_INPUT_KEYS,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE,
		.tick_ms = 100,
	};
	int rc = morph_tui_core_app_present(ctx->application, &controller);

	if (rc)
		free(screen);
	return rc;
}
