#include "sapi/cli/internal.h"
#include "sapi/cli/recap.h"

#include <poll.h>
#include <math.h>
#include <time.h>

int64_t cli_recap_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int cli_recap_due(const struct cli_recap_state *state, int64_t now)
{
	int64_t since = state->unfocused_since > state->last_finished ?
		state->unfocused_since : state->last_finished;

	if (state->progress.session_id <= 0 || state->progress.history_id <= 0 ||
	    !state->unfocused_since || !state->last_finished || state->pending ||
	    state->progress.completed_turns < 3 ||
	    (state->has_recap && state->progress.completed_turns - state->last_recapped_turns < 2))
		return 0;
	return state->retry_at ? now >= state->retry_at : now - since >= CLI_RECAP_DELAY_MS;
}

void cli_recap_focus(struct cli_context *ctx, int focused, int64_t now)
{
	if (!ctx)
		return;
	if (focused) {
		ctx->recap.unfocused_since = 0;
		ctx->recap.retry_at = 0;
		ctx->recap.retried = 0;
		if (ctx->recap.pending && ctx->recap.automatic)
			runtime_recap_cancel(ctx->runtime);
	} else if (!ctx->recap.unfocused_since) {
		ctx->recap.unfocused_since = now;
	}
}

void cli_recap_turn_finished(struct cli_context *ctx, int64_t now)
{
	ctx->recap.last_finished = now;
	ctx->recap.retry_at = 0;
	ctx->recap.retried = 0;
	ctx->recap.next_poll = 0;
}

static void recap_seen(struct cli_recap_state *state, int save)
{
	size_t oldest = 0;

	for (size_t i = 0; i < CLI_RECAP_SESSION_CACHE_MAX; i++) {
		if (state->seen[i].session_id == state->progress.session_id) {
			state->seen[i].used = ++state->seen_clock;
			if (save)
				state->seen[i].completed_turns = state->last_recapped_turns;
			else {
				state->last_recapped_turns = state->seen[i].completed_turns;
				state->has_recap = 1;
			}
			return;
		}
		if (state->seen[i].used < state->seen[oldest].used)
			oldest = i;
	}
	if (save) {
		state->seen[oldest].session_id = state->progress.session_id;
		state->seen[oldest].completed_turns = state->last_recapped_turns;
		state->seen[oldest].used = ++state->seen_clock;
	}
}

static void recap_failure(struct cli_context *ctx, int rc, int automatic)
{
	if (automatic) {
		if (!ctx->recap.retried) {
			ctx->recap.retried = 1;
			ctx->recap.retry_at = cli_recap_now() + CLI_RECAP_RETRY_MS;
		} else {
			/* Retry only once for this revision or focus interval. */
			ctx->recap.retry_at = INT64_MAX;
		}
		return;
	}
	if (rc == -ECANCELED)
		return;
	if (rc == -EBUSY)
		CMD_ERROR(ctx, "Wait for the current task to finish before running /recap.");
	else if (rc == -EALREADY)
		CMD_ERROR(ctx, "A conversation recap is already being generated.");
	else if (rc == -ENODATA)
		CMD_ERROR(ctx, "There is no conversation history to recap.");
	else
		CMD_ERROR(ctx, "Could not generate a recap: %s", morph_strerror(rc));
}

int cli_recap_start(struct cli_context *ctx, int automatic)
{
	int rc;

	if (ctx->turn_active)
		rc = -EBUSY;
	else
		rc = runtime_recap_request(ctx->runtime, automatic, &ctx->recap.request);
	if (rc != 0) {
		recap_failure(ctx, rc, automatic);
		return rc;
	}
	ctx->recap.pending = 1;
	ctx->recap.result_code = 0;
	ctx->recap.automatic = automatic;
	if (ctx->recap.progress.session_id != ctx->recap.request.session_id) {
		ctx->recap.has_recap = 0;
		ctx->recap.last_finished = cli_recap_now();
		ctx->recap.retry_at = 0;
		ctx->recap.retried = 0;
	}
	/* A metadata request advances the request revision, not the conversation. */
	ctx->recap.progress = ctx->recap.request;
	recap_seen(&ctx->recap, 0);
	/* The interactive loop stays responsive. Other fronts wait only on explicit /recap. */
	while (!automatic && !ctx->input_status.enabled && ctx->ui && ctx->recap.pending) {
		struct pollfd wake = {cli_ui_wake_fd(ctx), POLLIN, 0};

		(void)poll(&wake, 1, 100);
		(void)cli_ui_drain(ctx);
	}
	return ctx->recap.pending ? 0 : ctx->recap.result_code;
}

void cli_recap_tick(struct cli_context *ctx, int64_t now)
{
	struct runtime_recap_progress progress;
	struct cli_recap_state *state = &ctx->recap;
	const struct config *config;
	int64_t session_id;

	if (!ctx->runtime || ctx->turn_active || ctx->details_open || now < state->next_poll)
		return;
	state->next_poll = now + 1000;
	/* A new CLI has no session until its first submitted turn. */
	if (runtime_session_current_id(ctx->runtime, &session_id) != 0)
		return;
	if (session_id <= 0) {
		state->progress = (struct runtime_recap_progress){0};
		state->has_recap = 0;
		state->last_finished = 0;
		state->retry_at = 0;
		state->retried = 0;
		return;
	}
	if (runtime_recap_progress(ctx->runtime, &progress) != 0)
		return;
	if (progress.session_id != state->progress.session_id) {
		state->has_recap = 0;
		state->progress = progress;
		recap_seen(state, 0);
		state->last_finished = now;
		state->retry_at = 0;
		state->retried = 0;
		if (state->unfocused_since)
			state->unfocused_since = now;
	} else if (progress.revision != state->progress.revision ||
		   progress.history_id != state->progress.history_id) {
		cli_recap_turn_finished(ctx, now);
	}
	state->progress = progress;
	config = runtime_config_get(ctx->runtime);
	if (config && config->general.auto_recap && cli_recap_due(state, now))
		(void)cli_recap_start(ctx, 1);
}

static void recap_print_text(struct cli_context *ctx, const char *label, const char *text)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *line = safe;
	size_t columns = (size_t)cli_terminal_columns(ctx);
	size_t indent = utf8_display_width(label);
	int first = 1;

	if (!safe)
		return;
	if (columns <= indent + 3) {
		printf(ctx, "%s\n", label);
		indent = 0;
		first = 0;
	}
	while (*line) {
		size_t width = columns > indent + 1 ? columns - indent - 1 : 2;
		const char *end = utf8_advance_display_width(line, width);

		if (end == line)
			end = utf8_advance_codepoints(line, 1);
		printf(ctx, "%s%.*s%.*s\n", first ? label : "", first ? 0 : (int)indent,
		       "                                  ", (int)(end - line), line);
		first = 0;
		line = end;
		while (*line == ' ')
			line++;
	}
	free(safe);
}

void cli_recap_event(struct cli_context *ctx, const struct morph_event *event)
{
	const cJSON *data = event->data;
	const char *keys[] = {"session_id", "history_id", "revision", "completed_turns"};
	const cJSON *error = cJSON_GetObjectItem(data, "error_code");

	if (!event->phase || strcmp(event->phase, "end") != 0)
		return;
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const cJSON *field = cJSON_GetObjectItem(data, keys[i]);

		if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
		    field->valuedouble < 0 || field->valuedouble >= (double)INT64_MAX ||
		    (i == 3 && field->valuedouble > INT_MAX))
			return;
	}
	if (!cJSON_IsNumber(error) || !isfinite(error->valuedouble) ||
	    error->valuedouble < INT_MIN || error->valuedouble > INT_MAX)
		return;
	struct runtime_recap_progress progress = {
		.session_id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "session_id")),
		.history_id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "history_id")),
		.revision = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "revision")),
		.completed_turns = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "completed_turns")),
	};
	int automatic = cJSON_IsTrue(cJSON_GetObjectItem(data, "automatic"));
	int rc = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "error_code"));
	const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(data, "summary"));
	const char *next = cJSON_GetStringValue(cJSON_GetObjectItem(data, "next_action"));

	if (progress.session_id != ctx->recap.request.session_id ||
	    progress.revision != ctx->recap.request.revision ||
	    progress.history_id != ctx->recap.request.history_id)
		return;
	ctx->recap.pending = 0;
	ctx->recap.result_code = rc;
	if (ctx->turn_active || !runtime_recap_is_current(ctx->runtime, &progress))
		return;
	if (automatic && !runtime_config_get(ctx->runtime)->general.auto_recap)
		return;
	if (rc < 0 || !summary || !summary[0]) {
		ctx->recap.result_code = rc < 0 ? rc : MORPH_ERR_PROTOCOL;
		if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON)
			recap_failure(ctx, ctx->recap.result_code, automatic);
		return;
	}
	ctx->recap.last_recapped_turns = progress.completed_turns;
	ctx->recap.progress = progress;
	ctx->recap.has_recap = 1;
	recap_seen(&ctx->recap, 1);
	ctx->recap.retry_at = 0;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	cli_terminal_history_begin(ctx);
	if (cli_color_enabled(ctx))
		printf(ctx, ANSI_DIM "\033[3m");
	recap_print_text(ctx, "  ↳ Recap: ", summary);
	if (next && next[0])
		recap_print_text(ctx, "    Next: ", next);
	if (cli_color_enabled(ctx))
		printf(ctx, ANSI_RESET);
	printf(ctx, "\n");
	fflush(stdout);
	cli_terminal_history_end(ctx);
}
