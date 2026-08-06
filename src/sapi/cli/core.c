#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"
#include "runtime/runtime.h"
#include <stdarg.h>

const char *default_db_path = "~/.morph/data.db";
const char *default_config_path = "~/.morph/config.toml";
static int g_cli_color_enabled = 1;

void cli_set_color_enabled(int enabled)
{
	g_cli_color_enabled = enabled ? 1 : 0;
}

int cli_color_enabled(void)
{
	return g_cli_color_enabled;
}

static size_t cli_strip_ansi_into(char *dst, size_t dst_size,
				  const char *src)
{
	size_t out = 0;
	const unsigned char *p = (const unsigned char *)src;

	if (!src)
		return 0;
	while (*p) {
		if (*p == 0x1b) {
			p++;
			if (*p == '[') {
				p++;
				while (*p && (*p < 0x40 || *p > 0x7e))
					p++;
				if (*p)
					p++;
				continue;
			}
			if (*p)
				p++;
			continue;
		}
		if (dst && dst_size > 0 && out + 1 < dst_size)
			dst[out] = (char)*p;
		out++;
		p++;
	}
	if (dst && dst_size > 0) {
		size_t term = out < dst_size ? out : dst_size - 1;
		dst[term] = '\0';
	}
	return out;
}

int cli_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (g_cli_color_enabled) {
		va_start(ap, fmt);
		n = vfprintf(stdout, fmt, ap);
		va_end(ap);
		return n;
	}

	va_start(ap, fmt);
	va_list ap_size;
	va_copy(ap_size, ap);
	n = vsnprintf(NULL, 0, fmt, ap_size);
	va_end(ap_size);
	if (n < 0) {
		va_end(ap);
		return n;
	}
	char *buf = malloc((size_t)n + 1);
	if (!buf) {
		va_end(ap);
		return -1;
	}
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	size_t stripped_len = cli_strip_ansi_into(NULL, 0, buf);
	char *stripped = malloc(stripped_len + 1);
	if (!stripped) {
		free(buf);
		return -1;
	}
	cli_strip_ansi_into(stripped, stripped_len + 1, buf);
	free(buf);
	fputs(stripped, stdout);
	free(stripped);
	return n;
}

void print_padded(const char *s, int target_width)
{
	size_t width;
	int dw;
	int pad;

	if (!s) s = "";
	width = utf8_display_width(s);
	dw = width > (size_t)INT_MAX ? INT_MAX : (int)width;
	fputs(s, stdout);
	pad = target_width - dw;
	for (int i = 0; i < pad; i++)
		putchar(' ');
}

void cli_record_media_credits(struct cli_context *ctx, const char *kind,
				     int64_t image_units,
				     int64_t video_seconds,
				     const char *provider,
				     const char *model,
				     const char *metadata_json)
{
	if (!ctx || !kind)
		return;
	(void)runtime_credit_record_media(ctx->runtime, kind, image_units,
		video_seconds, provider, model, metadata_json);
}
int cli_handle_command(struct cli_context *ctx, const char *input)
{
	int64_t command_started_at;
	int owns_turn = 0;

	if (!ctx || !input)
		return -EINVAL;
	command_started_at = (int64_t)time(NULL);
	(void)runtime_turn_prepare_tools(ctx->runtime, command_started_at);

	cli_process_due_tasks(ctx);
	{
		int handled = 0;
		int rc = cli_handle_media_path(ctx, input, &handled);

		if (handled) {
			if (rc != 0)
				MORPH_RETURN(rc);
			return 0;
		}
	}

	if (input[0] == '/') {
		int rc = cli_command_dispatch(ctx, input);
		cli_process_due_tasks(ctx);
		return rc;
	}

	morph_buf_t input_buf;
	const char *effective_input = input;
	int has_input_buf = 0;

	memset(&input_buf, 0, sizeof(input_buf));
	if (ctx->image_path[0]) {
		int rc = morph_buf_init(&input_buf, 256);

		if (rc != 0)
			MORPH_RETURN(rc);
		has_input_buf = 1;
		rc = morph_buf_printf(&input_buf, "[Image: %s]\n%s",
				      ctx->image_path, input);
		if (rc != 0) {
			morph_buf_cleanup(&input_buf);
			MORPH_RETURN(rc);
		}
		effective_input = morph_buf_cstr(&input_buf);
		ctx->image_path[0] = '\0';
	}

	if (!ctx->turn_active) {
		cli_turn_begin(ctx);
		owns_turn = 1;
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
		ctx->cancel_monitor = cli_cancel_monitor_start(STDIN_FILENO);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	struct runtime_request request = {
		.session_id = current.id,
		.model_input = effective_input,
		.stored_user_input = input,
		.output_cb = NULL,
		.output_user_data = NULL,
		.turn_flags = AGENT_TURN_DEFAULT_FLAGS |
			AGENT_TURN_SAVE_EMPTY_USER |
			AGENT_TURN_SAVE_EMPTY_ASSISTANT,
	};
	struct runtime_result runtime_result;
	int react_rc = runtime_execute_turn(ctx->runtime, &request,
					    &runtime_result);
	/* Auto-name a lazily created session from its first user input. */
	if (!ctx->session_auto_named) {
		struct session named_session;
		char title[48];
		size_t len = strcspn(input, "\n");
		size_t max_bytes = sizeof(title) - 4;

		if (len > max_bytes) {
			size_t chop = utf8_clamp_bytes(input, max_bytes);

			memcpy(title, input, chop);
			title[chop] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		if (runtime_session_current(ctx->runtime, &named_session) == 0 &&
		    named_session.id > 0) {
			(void)runtime_session_rename_and_update(ctx->runtime,
							 named_session.id, title);
			ctx->session_auto_named = 1;
		}
	}
	if (has_input_buf)
		morph_buf_cleanup(&input_buf);
	struct cli_cancel_monitor *cancel_monitor = ctx->cancel_monitor;
	ctx->cancel_monitor = NULL;
	cli_cancel_monitor_stop(cancel_monitor);
	if (owns_turn)
		cli_turn_finish(ctx, react_rc);
	if (react_rc == -EBUSY)
		return react_rc;

	cli_process_due_tasks(ctx);
	return react_rc;
}

void cli_turn_begin(struct cli_context *ctx)
{
	if (!ctx)
		return;
	cli_cancel_state_reset();
	cli_presentation_reset(ctx);
	ctx->turn_active = 1;
}

void cli_turn_finish(struct cli_context *ctx, int turn_rc)
{
	if (!ctx)
		return;
	cli_cancel_state_reset();
	cli_presentation_finish(ctx);
	ctx->turn_active = 0;
	if (turn_rc >= 0 || ctx->final_rendered ||
	    ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		printf("error: %s\n", morph_strerror(turn_rc));
	else
		CMD_ERROR("%s", morph_strerror(turn_rc));
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	if (!ctx)
		return;
	runtime_close(ctx->runtime);
	ctx->runtime = NULL;
	(void)cli_ui_drain(ctx);
	cli_presentation_cleanup(ctx);
	cli_ui_cleanup(ctx);
	cli_terminal_cleanup(ctx);
	log_info("cli shutdown complete");
}
