#include "spin.h"
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define FRAME_INTERVAL_MS 120

static const char *spin_frames_dots[] = {
	"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
};

static const char *spin_frames_arrow[] = {
	"←", "↖", "↑", "↗", "→", "↘", "↓", "↙",
};

static const char *spin_frames_pulse[] = {
	"◐", "◓", "◑", "◒",
};

static const char *spin_frames_braille[] = {
	"⠛", "⠟", "⠿", "⠻", "⠽", "⠾", "⠿", "⠾",
};

static const char *state_prefix(enum spin_state state)
{
	switch (state) {
	case SPIN_STATE_IDLE:       return "";
	case SPIN_STATE_THINKING:   return "💭";
	case SPIN_STATE_LOADING:    return "⏳";
	case SPIN_STATE_EXECUTING:  return "⚙";
	case SPIN_STATE_DOWNLOADING: return "⬇";
	case SPIN_STATE_UPLOADING:  return "⬆";
	case SPIN_STATE_COMPLETE:   return "✅";
	case SPIN_STATE_ERROR:      return "❌";
	default:                    return "";
	}
}

static const char *state_text(enum spin_state state)
{
	switch (state) {
	case SPIN_STATE_IDLE:       return "";
	case SPIN_STATE_THINKING:   return "Thinking";
	case SPIN_STATE_LOADING:    return "Loading";
	case SPIN_STATE_EXECUTING:  return "Executing";
	case SPIN_STATE_DOWNLOADING: return "Downloading";
	case SPIN_STATE_UPLOADING:  return "Uploading";
	case SPIN_STATE_COMPLETE:   return "Done";
	case SPIN_STATE_ERROR:      return "Error";
	default:                    return "";
	}
}

static const char **get_frames(enum spin_style style)
{
	switch (style) {
	case SPIN_STYLE_ARROW:   return spin_frames_arrow;
	case SPIN_STYLE_PULSE:   return spin_frames_pulse;
	case SPIN_STYLE_BRAILLE: return spin_frames_braille;
	case SPIN_STYLE_DOTS:
	default:                 return spin_frames_dots;
	}
}

static int get_frame_count(enum spin_style style)
{
	switch (style) {
	case SPIN_STYLE_ARROW:   return 8;
	case SPIN_STYLE_PULSE:   return 4;
	case SPIN_STYLE_BRAILLE: return 8;
	case SPIN_STYLE_DOTS:
	default:                 return 8;
	}
}

static void format_elapsed(char *buf, size_t len, time_t start)
{
	if (!buf || len == 0) return;
	long elapsed = (long)(time(NULL) - start);
	if (elapsed < 60) {
		snprintf(buf, len, "%lds", elapsed);
	} else if (elapsed < 3600) {
		snprintf(buf, len, "%ldm %ds", elapsed / 60, elapsed % 60);
	} else {
		snprintf(buf, len, "%ldh %dm", elapsed / 3600, (elapsed % 3600) / 60);
	}
}

void spin_render(struct spin_context *ctx)
{
	if (!ctx || !ctx->running) return;

	const char **frames = get_frames(ctx->style);
	int frame_count = get_frame_count(ctx->style);
	const char *frame = frames[ctx->frame % frame_count];
	const char *prefix = state_prefix(ctx->state);

	char elapsed[32];
	format_elapsed(elapsed, sizeof(elapsed), ctx->start_time);

	fprintf(ctx->output, "\r\033[K");

	if (ctx->state == SPIN_STATE_COMPLETE || ctx->state == SPIN_STATE_ERROR) {
		fprintf(ctx->output, "%s %s", prefix, ctx->message);
		if (elapsed[0]) {
			fprintf(ctx->output, " \033[2m(%s)\033[0m", elapsed);
		}
	} else {
		fprintf(ctx->output, "%s %s \033[2m%s\033[0m", frame, ctx->message, elapsed);
		if (ctx->submessage[0]) {
			fprintf(ctx->output, " \033[36m→\033[0m \033[2m%s\033[0m", ctx->submessage);
		}
	}

	fflush(ctx->output);
}

static void *spin_thread_func(void *arg)
{
	struct spin_context *ctx = (struct spin_context *)arg;

	while (ctx->running) {
		pthread_mutex_lock(&ctx->mutex);
		if (ctx->active && ctx->running) {
			ctx->frame++;
			if (ctx->cancel_flag && *ctx->cancel_flag) {
				pthread_mutex_unlock(&ctx->mutex);
				break;
			}
			spin_render(ctx);
		}
		pthread_mutex_unlock(&ctx->mutex);
		usleep((useconds_t)(ctx->interval_ms * 1000));
	}

	return NULL;
}

void spin_init(struct spin_context *ctx, FILE *output)
{
	if (!ctx) return;
	memset(ctx, 0, sizeof(*ctx));
	ctx->style = SPIN_STYLE_DOTS;
	ctx->output = output ? output : stdout;
	ctx->interval_ms = FRAME_INTERVAL_MS;
	ctx->running = 0;
	ctx->active = 0;
	ctx->frame = 0;
	pthread_mutex_init(&ctx->mutex, NULL);
}

void spin_set_cancel_flag(struct spin_context *ctx, volatile sig_atomic_t *flag)
{
	if (!ctx) return;
	ctx->cancel_flag = flag;
}

void spin_start(struct spin_context *ctx, enum spin_state state, const char *message)
{
	if (!ctx) return;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->running) {
		ctx->active = 0;
		pthread_mutex_unlock(&ctx->mutex);
		pthread_join(ctx->thread, NULL);
		pthread_mutex_lock(&ctx->mutex);
	}

	ctx->state = state;
	ctx->running = 1;
	ctx->active = 1;
	ctx->frame = 0;
	ctx->start_time = time(NULL);
	ctx->last_update = ctx->start_time;
	ctx->submessage[0] = '\0';

	if (message) {
		strncpy(ctx->message, message, sizeof(ctx->message) - 1);
		ctx->message[sizeof(ctx->message) - 1] = '\0';
	} else {
		strncpy(ctx->message, state_text(state), sizeof(ctx->message) - 1);
		ctx->message[sizeof(ctx->message) - 1] = '\0';
	}

	ctx->frame = 0;
	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);

	if (pthread_create(&ctx->thread, NULL, spin_thread_func, ctx) != 0) {
		ctx->running = 0;
		ctx->active = 0;
	}

	pthread_mutex_unlock(&ctx->mutex);
}

void spin_update(struct spin_context *ctx, const char *message)
{
	if (!ctx) return;
	pthread_mutex_lock(&ctx->mutex);
	if (message && ctx->running) {
		strncpy(ctx->message, message, sizeof(ctx->message) - 1);
		ctx->message[sizeof(ctx->message) - 1] = '\0';
	}
	ctx->last_update = time(NULL);
	pthread_mutex_unlock(&ctx->mutex);
}

void spin_set_sub(struct spin_context *ctx, const char *submessage)
{
	if (!ctx) return;
	pthread_mutex_lock(&ctx->mutex);
	if (submessage) {
		strncpy(ctx->submessage, submessage, sizeof(ctx->submessage) - 1);
		ctx->submessage[sizeof(ctx->submessage) - 1] = '\0';
	} else {
		ctx->submessage[0] = '\0';
	}
	pthread_mutex_unlock(&ctx->mutex);
}

void spin_stop(struct spin_context *ctx, enum spin_state final_state, const char *message)
{
	if (!ctx) return;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->running) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	ctx->active = 0;
	ctx->running = 0;
	ctx->state = final_state;

	if (message) {
		strncpy(ctx->message, message, sizeof(ctx->message) - 1);
		ctx->message[sizeof(ctx->message) - 1] = '\0';
	} else {
		strncpy(ctx->message, state_text(final_state), sizeof(ctx->message) - 1);
		ctx->message[sizeof(ctx->message) - 1] = '\0';
	}

	pthread_mutex_unlock(&ctx->mutex);

	pthread_join(ctx->thread, NULL);

	ctx->frame = 0;
	fprintf(ctx->output, "\r\033[K");

	char elapsed[32];
	format_elapsed(elapsed, sizeof(elapsed), ctx->start_time);
	const char *prefix = state_prefix(final_state);

	fprintf(ctx->output, "%s %s", prefix, ctx->message);
	if (elapsed[0]) {
		fprintf(ctx->output, " \033[2m(%s)\033[0m", elapsed);
	}
	fflush(ctx->output);
}

void spin_clear(struct spin_context *ctx)
{
	if (!ctx) return;
	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);
}

void spin_destroy(struct spin_context *ctx)
{
	if (!ctx) return;

	if (ctx->running) {
		ctx->running = 0;
		ctx->active = 0;
		pthread_join(ctx->thread, NULL);
	}

	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);

	pthread_mutex_destroy(&ctx->mutex);
	memset(ctx, 0, sizeof(*ctx));
}
