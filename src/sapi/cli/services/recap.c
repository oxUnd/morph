#include "sapi/cli/services/recap.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/services/event_queue.h"
#include "errors.h"
#include "nb/time.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <time.h>

int64_t cli_recap_now(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

int cli_recap_due(const struct cli_recap_state *state, int64_t now)
{
	int64_t since = state->unfocused_since > state->last_finished ? state->unfocused_since
								      : state->last_finished;

	if (state->progress.session_id <= 0 || state->progress.history_id <= 0 ||
	    !state->unfocused_since || !state->last_finished || state->pending ||
	    state->progress.completed_turns < 3 ||
	    (state->has_recap && state->progress.completed_turns - state->last_recapped_turns < 2))
		return 0;
	return state->retry_at ? now >= state->retry_at : now - since >= CLI_RECAP_DELAY_MS;
}

void cli_recap_set_renderer(struct cli_application *ctx,
			    const struct cli_recap_renderer *renderer)
{
	if (!ctx)
		return;
	ctx->recap.renderer = renderer;
}

void cli_recap_focus(struct cli_application *ctx, int focused, int64_t now)
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

void cli_recap_turn_finished(struct cli_application *ctx, int64_t now)
{
	ctx->recap.last_finished = now;
	ctx->recap.retry_at = 0;
	ctx->recap.retried = 0;
	ctx->recap.next_poll = 0;
}

static void cli_recap_seen(struct cli_recap_state *state, int save)
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

static void cli_recap_failure(struct cli_application *ctx, int rc, int automatic)
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
	if (rc == NB_ERR_CANCELLED)
		return;
	if (ctx->recap.renderer && ctx->recap.renderer->failed)
		ctx->recap.renderer->failed(ctx, rc);
}

int cli_recap_start(struct cli_application *ctx, int automatic)
{
	int rc;

	if (ctx->turn_active)
		rc = NB_ERR_BUSY;
	else
		rc = runtime_recap_request(ctx->runtime, automatic, &ctx->recap.request);
	if (rc != 0) {
		cli_recap_failure(ctx, rc, automatic);
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
	cli_recap_seen(&ctx->recap, 0);
	/* The interactive loop stays responsive. Other fronts wait only on explicit /recap. */
	while (!automatic && !ctx->input_status.enabled && ctx->events && ctx->recap.pending) {
		struct pollfd wake = {cli_event_queue_wake_fd(ctx), POLLIN, 0};

		(void)poll(&wake, 1, 100);
		(void)cli_event_queue_drain(ctx);
	}
	return ctx->recap.pending ? 0 : ctx->recap.result_code;
}

void cli_recap_tick(struct cli_application *ctx, int64_t now)
{
	struct runtime_recap_progress progress;
	struct cli_recap_state *state = &ctx->recap;
	const struct config *config;
	int64_t session_id;

	if (!ctx->runtime || ctx->turn_active || cli_output_overlay_visible(ctx) ||
	    now < state->next_poll)
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
		cli_recap_seen(state, 0);
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
	config = runtime_config_borrow(ctx->runtime);
	if (config && config->general.auto_recap && cli_recap_due(state, now))
		(void)cli_recap_start(ctx, 1);
}

void cli_recap_event(struct cli_application *ctx, const struct morph_event *event)
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
		.session_id =
			(int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "session_id")),
		.history_id =
			(int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "history_id")),
		.revision = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "revision")),
		.completed_turns =
			(int)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "completed_turns")),
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
	if (automatic && !runtime_config_borrow(ctx->runtime)->general.auto_recap)
		return;
	if (rc < 0 || !summary || !summary[0]) {
		ctx->recap.result_code = rc < 0 ? rc : NB_ERR_PROTOCOL;
		if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON)
			cli_recap_failure(ctx, ctx->recap.result_code, automatic);
		return;
	}
	ctx->recap.last_recapped_turns = progress.completed_turns;
	ctx->recap.progress = progress;
	ctx->recap.has_recap = 1;
	cli_recap_seen(&ctx->recap, 1);
	ctx->recap.retry_at = 0;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	if (ctx->recap.renderer && ctx->recap.renderer->show)
		ctx->recap.renderer->show(ctx, summary, next);
}
