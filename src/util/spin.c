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
	case SPIN_STATE_THINKING:   return "\033[1;36m>>\033[0m";
	case SPIN_STATE_LOADING:    return "\033[1;33m~~\033[0m";
	case SPIN_STATE_EXECUTING:  return "\033[1;33m->\033[0m";
	case SPIN_STATE_DOWNLOADING: return "\033[1;34mvv\033[0m";
	case SPIN_STATE_UPLOADING:  return "\033[1;34m^^\033[0m";
	case SPIN_STATE_COMPLETE:   return "\033[1;32mOK\033[0m";
	case SPIN_STATE_ERROR:      return "\033[1;31mERR\033[0m";
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

static size_t utf8_visible_len(const char *s)
{
	size_t n = 0;
	while (*s) {
		if ((*s & 0xC0) != 0x80)
			n++;
		s++;
	}
	return n;
}

static const char *utf8_skip_back(const char *start, const char *end, size_t chars)
{
	while (end > start && chars > 0) {
		end--;
		if ((*end & 0xC0) != 0x80)
			chars--;
	}
	while (end > start && (*end & 0xC0) == 0x80)
		end--;
	return end;
}

static int utf8_char_bytes(const char *s)
{
	unsigned char c = (unsigned char)*s;
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

static const char *utf8_skip_forward(const char *s, size_t chars)
{
	while (*s && chars > 0) {
		int cb = utf8_char_bytes(s);
		s += cb;
		chars--;
	}
	return s;
}

static size_t utf8_copy_vis(char *dst, size_t dst_cap, const char *src, size_t max_vis)
{
	size_t written = 0;
	size_t vis = 0;
	while (*src && vis < max_vis) {
		int cb = utf8_char_bytes(src);
		if (written + cb >= dst_cap) break;
		memcpy(dst + written, src, cb);
		written += cb;
		src += cb;
		vis++;
	}
	dst[written] = '\0';
	return vis;
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
			size_t msg_vis = utf8_visible_len(ctx->message);
			size_t elapsed_vis = elapsed[0] ? utf8_visible_len(elapsed) + 3 : 0;
			size_t used = 4 + msg_vis + 1 + elapsed_vis + 4;
			size_t max_sub = 80 > used ? 80 - used : 20;
			size_t sub_vis = utf8_visible_len(ctx->submessage);
			if (sub_vis <= max_sub) {
				fprintf(ctx->output, " \033[36m→\033[0m \033[2m%s\033[0m",
					ctx->submessage);
			} else {
				size_t scroll_range = sub_vis - max_sub + 1;
				size_t scroll_speed = 2;
				size_t offset = ((size_t)ctx->frame / scroll_speed) % (scroll_range + 8);
				if (offset > scroll_range)
					offset = scroll_range;
				const char *start = utf8_skip_forward(ctx->submessage, offset);
				char buf[512];
				utf8_copy_vis(buf, sizeof(buf), start, max_sub);
				fprintf(ctx->output, " \033[36m→\033[0m \033[2m%s\033[0m", buf);
			}
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
		size_t len = strlen(submessage);
		size_t max = sizeof(ctx->submessage) - 1;
		const char *src = submessage;
		size_t src_len = len;
		if (len > max) {
			src = submessage + len - max;
			src_len = max;
			while (src_len > 0 && (*src & 0xC0) == 0x80) {
				src++;
				src_len--;
			}
		}
		memcpy(ctx->submessage, src, src_len);
		ctx->submessage[src_len] = '\0';
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
