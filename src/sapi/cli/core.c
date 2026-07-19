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

int cli_turn_background_cb(void *user_data, const char *name,
			   const char *phase, const char *message,
			   const char *task, int count, int error_code)
{
	struct cli_context *ctx = user_data;

	return cli_emit_background_event(ctx, name, phase, message, task,
					 count, error_code);
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

	if (!ctx || !input)
		return -EINVAL;
	command_started_at = (int64_t)time(NULL);
	(void)runtime_turn_prepare_tools(ctx->runtime, command_started_at);

	cli_process_due_tasks(ctx);

	if (input[0] == '/') {
		int rc = cli_command_dispatch(ctx, input);
		cli_process_due_tasks(ctx);
		return rc;
	}

	char input_buf[8192];
	const char *effective_input = input;
	if (ctx->image_path[0]) {
		int n = snprintf(input_buf, sizeof(input_buf),
				 "[Image: %s]\n%s", ctx->image_path, input);
		if (n > 0 && (size_t)n < sizeof(input_buf))
			effective_input = input_buf;
		ctx->image_path[0] = '\0';
	}

	/* Auto-name session from first user input */
	if (!ctx->session_auto_named && input[0] != '/') {
		struct session current;
		char title[48];
		size_t len = strlen(input);
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
		if (runtime_session_current(ctx->runtime, &current) == 0)
			(void)runtime_session_rename_and_update(ctx->runtime,
							 current.id, title);
		ctx->session_auto_named = 1;
	}

	if (ctx->event_mode != CLI_EVENTS_JSON) {
		printf(ANSI_BOLD ANSI_CYAN "▸ %s" ANSI_RESET "\n", input);
		fflush(stdout);
	}

	cli_sigint_received = 0;
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	struct runtime_request request = {
		.session_id = current.id,
		.model_input = effective_input,
		.stored_user_input = input,
		.output_cb = ctx->event_mode == CLI_EVENTS_JSON ? NULL : output_callback,
		.output_user_data = ctx,
		.turn_flags = AGENT_TURN_DEFAULT_FLAGS |
			AGENT_TURN_SAVE_EMPTY_USER |
			AGENT_TURN_SAVE_EMPTY_ASSISTANT,
	};
	struct runtime_result runtime_result;
	int react_rc = runtime_execute_turn(ctx->runtime, &request,
					    &runtime_result);
	if (react_rc == -EBUSY)
		return react_rc;

	struct runtime_turn_status status;
	int have_status = runtime_turn_status_get(ctx->runtime, &status) == 0;
	if (have_status && status.state == REACT_STATE_ABORT &&
	    ctx->event_mode != CLI_EVENTS_JSON) {
		const char *outcome = react_outcome_name(status.outcome);
		const char *error = react_rc < 0 ? morph_strerror(react_rc) :
			"aborted";
		printf(ANSI_YELLOW "[%s] %s" ANSI_RESET,
		       outcome, error);
		if (status.outcome_reason && status.outcome_reason[0])
			printf(ANSI_DIM " (%s)" ANSI_RESET,
			       status.outcome_reason);
		printf("\n");
	}
	if (have_status)
		runtime_turn_status_cleanup(&status);

	ctx->streaming = 0;
	cli_process_due_tasks(ctx);
	return react_rc;
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	if (!ctx)
		return;
	runtime_close(ctx->runtime);
	ctx->runtime = NULL;
	log_info("cli shutdown complete");
}
