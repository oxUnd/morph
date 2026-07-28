#include "spin.h"
#include "utf8.h"
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

#define FRAME_INTERVAL_MS 120
#define SHIMMER_INTERVAL_MS 80

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
	case SPIN_STATE_ABORT:      return "\033[1;33m!!\033[0m";
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
	case SPIN_STATE_ABORT:      return "Aborted";
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
	case SPIN_STYLE_SHIMMER: return spin_frames_dots;
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
	case SPIN_STYLE_SHIMMER: return 8;
	case SPIN_STYLE_DOTS:
	default:                 return 8;
	}
}

static void spin_render_shimmer(struct spin_context *ctx, size_t budget)
{
	static const int shades[] = {240, 242, 245, 250, 255, 250, 245, 242};
	char message[1024];
	const char *p;
	size_t message_budget;
	size_t message_width;
	size_t index = 0;
	size_t shade_count = sizeof(shades) / sizeof(shades[0]);

	message_budget = budget > 2 ? budget - 2 : 0;
	message_width = utf8_copy_display_width(message, sizeof(message),
						ctx->message,
						message_budget);
	fprintf(ctx->output, "\r\033[K\033[1;36m•\033[0m ");
	p = message;
	while (*p) {
		size_t bytes = utf8codepointcalcsize(p);
		size_t shade = (index + (size_t)ctx->frame) % shade_count;

		if (bytes == 0)
			break;
		fprintf(ctx->output, "\033[38;5;%dm%.*s",
			shades[shade], (int)bytes, p);
		p += bytes;
		index++;
	}
	fprintf(ctx->output, "\033[0m");
	if (message_width + 2 < ctx->last_render_width) {
		size_t pad = ctx->last_render_width - message_width - 2;

		if (pad > budget)
			pad = budget;
		for (size_t i = 0; i < pad; i++)
			fputc(' ', ctx->output);
	}
	ctx->last_render_width = message_width + 2;
	fflush(ctx->output);
}

static void format_elapsed(char *buf, size_t len, time_t start)
{
	if (!buf || len == 0) return;
	long elapsed = (long)(time(NULL) - start);
	if (elapsed < 60) {
		snprintf(buf, len, "%lds", elapsed);
	} else if (elapsed < 3600) {
		snprintf(buf, len, "%ldm %lds", elapsed / 60, elapsed % 60);
	} else {
		snprintf(buf, len, "%ldh %ldm", elapsed / 3600, (elapsed % 3600) / 60);
	}
}


static size_t get_term_width(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (size_t)ws.ws_col;
	return 80;
}

static void spin_copy_sanitized(char *dst, size_t dst_cap, const char *src)
{
	if (!dst || dst_cap == 0)
		return;
	(void)utf8_copy_sanitized_clamped(dst, dst_cap, src, dst_cap - 1);
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

	size_t term_w = get_term_width();
	/* Reserve the rightmost cell so we never trip terminal autowrap. */
	size_t budget = term_w > 1 ? term_w - 1 : 79;
	size_t vis_width = 0;

	if (ctx->style == SPIN_STYLE_SHIMMER) {
		spin_render_shimmer(ctx, budget);
		return;
	}

	int is_final = (ctx->state == SPIN_STATE_COMPLETE ||
			ctx->state == SPIN_STATE_ABORT ||
			ctx->state == SPIN_STATE_ERROR);

	fprintf(ctx->output, "\r\033[K");

	if (is_final) {
		size_t prefix_w = utf8_display_width_ansi(prefix);
		size_t elapsed_w = elapsed[0]
			? utf8_display_width(elapsed) + 3 /* " (Xs)" */ : 0;
		size_t fixed = prefix_w + 1; /* prefix + space */
		size_t avail = budget > fixed ? budget - fixed : 0;
		size_t msg_budget = avail > elapsed_w
			? avail - elapsed_w : avail;
		char msg_buf[1024];
		size_t msg_vis = utf8_copy_display_width(msg_buf, sizeof(msg_buf),
					       ctx->message, msg_budget);
		fprintf(ctx->output, "%s %s", prefix, msg_buf);
		vis_width = fixed + msg_vis;
		if (elapsed_w && vis_width + elapsed_w <= budget) {
			fprintf(ctx->output, " \033[2m(%s)\033[0m", elapsed);
			vis_width += elapsed_w;
		}
	} else {
		size_t frame_w = utf8_display_width(frame);
		size_t elapsed_w = elapsed[0]
			? utf8_display_width(elapsed) + 1 /* leading space */
			: 0;
		size_t fixed = frame_w + 1; /* frame + space */
		size_t avail = budget > fixed ? budget - fixed : 0;
		size_t msg_budget = avail > elapsed_w
			? avail - elapsed_w : avail;
		char msg_buf[1024];
		size_t msg_vis = utf8_copy_display_width(msg_buf, sizeof(msg_buf),
					       ctx->message, msg_budget);
		fprintf(ctx->output, "%s %s", frame, msg_buf);
		vis_width = fixed + msg_vis;
		if (elapsed_w && vis_width + elapsed_w <= budget) {
			fprintf(ctx->output, " \033[2m%s\033[0m", elapsed);
			vis_width += elapsed_w;
		}

		if (ctx->submessage[0]) {
			/* " → " is 3 visible columns: space + arrow + space. */
			const size_t arrow_w = 3;
			size_t remaining = budget > vis_width
				? budget - vis_width : 0;
			if (remaining > arrow_w + 1) {
				size_t sub_max = remaining - arrow_w;
				if (sub_max > 60)
					sub_max = 60;
				size_t sub_vis = utf8_display_width(ctx->submessage);
				char sub_buf[1024];
				size_t copied_vis;
				if (sub_vis <= sub_max) {
					copied_vis = utf8_copy_display_width(
						sub_buf, sizeof(sub_buf),
						ctx->submessage, sub_max);
				} else {
					size_t scroll_range = sub_vis - sub_max + 1;
					size_t scroll_speed = 2;
					size_t offset =
						((size_t)ctx->frame / scroll_speed)
						% (scroll_range + 8);
					if (offset > scroll_range)
						offset = scroll_range;
					const char *start = utf8_advance_display_width(
						ctx->submessage, offset);
					copied_vis = utf8_copy_display_width(
						sub_buf, sizeof(sub_buf),
						start, sub_max);
				}
				fprintf(ctx->output,
					" \033[36m→\033[0m \033[2m%s\033[0m",
					sub_buf);
				vis_width += arrow_w + copied_vis;
			}
		}
	}

	if (vis_width < ctx->last_render_width) {
		size_t pad = ctx->last_render_width - vis_width;
		if (pad > budget)
			pad = budget;
		for (size_t i = 0; i < pad; i++)
			fputc(' ', ctx->output);
	}
	ctx->last_render_width = vis_width;

	fflush(ctx->output);
}

static void *spin_thread_func(void *arg)
{
	struct spin_context *ctx = (struct spin_context *)arg;

	for (;;) {
		pthread_mutex_lock(&ctx->mutex);
		if (!ctx->running) {
			pthread_mutex_unlock(&ctx->mutex);
			break;
		}
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
		ctx->running = 0;
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
	ctx->last_render_width = 0;
	ctx->submessage[0] = '\0';
	ctx->interval_ms = ctx->style == SPIN_STYLE_SHIMMER ?
		SHIMMER_INTERVAL_MS : FRAME_INTERVAL_MS;

	if (message) {
		spin_copy_sanitized(ctx->message, sizeof(ctx->message),
				    message);
	} else {
		spin_copy_sanitized(ctx->message, sizeof(ctx->message),
				    state_text(state));
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
		spin_copy_sanitized(ctx->message, sizeof(ctx->message),
				    message);
	}
	ctx->last_update = time(NULL);
	pthread_mutex_unlock(&ctx->mutex);
}

void spin_set_sub(struct spin_context *ctx, const char *submessage)
{
	if (!ctx) return;
	pthread_mutex_lock(&ctx->mutex);
	if (submessage) {
		spin_copy_sanitized(ctx->submessage, sizeof(ctx->submessage),
				    submessage);
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
		spin_copy_sanitized(ctx->message, sizeof(ctx->message),
				    message);
	} else {
		spin_copy_sanitized(ctx->message, sizeof(ctx->message),
				    state_text(final_state));
	}

	pthread_mutex_unlock(&ctx->mutex);

	pthread_join(ctx->thread, NULL);

	ctx->frame = 0;
	ctx->last_render_width = 0;
	fprintf(ctx->output, "\r\033[K");

	char elapsed[32];
	format_elapsed(elapsed, sizeof(elapsed), ctx->start_time);
	const char *prefix = state_prefix(final_state);

	/* Match spin_render's hard budget so the final line never wraps
	 * either, even if `message` is longer than the terminal width. */
	size_t term_w = get_term_width();
	size_t budget = term_w > 1 ? term_w - 1 : 79;
	size_t prefix_w = utf8_display_width_ansi(prefix);
	size_t elapsed_w = elapsed[0]
		? utf8_display_width(elapsed) + 3 /* " (Xs)" */ : 0;
	size_t fixed = prefix_w + 1;
	size_t avail = budget > fixed ? budget - fixed : 0;
	size_t msg_budget = avail > elapsed_w ? avail - elapsed_w : avail;
	char msg_buf[1024];
	size_t msg_vis = utf8_copy_display_width(msg_buf, sizeof(msg_buf),
				       ctx->message, msg_budget);

	fprintf(ctx->output, "%s %s", prefix, msg_buf);
	if (elapsed_w && fixed + msg_vis + elapsed_w <= budget)
		fprintf(ctx->output, " \033[2m(%s)\033[0m", elapsed);
	fflush(ctx->output);
}

void spin_cancel(struct spin_context *ctx)
{
	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	if (!ctx->running) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	ctx->active = 0;
	ctx->running = 0;
	pthread_mutex_unlock(&ctx->mutex);
	pthread_join(ctx->thread, NULL);
	ctx->frame = 0;
	ctx->last_render_width = 0;
	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);
}

void spin_clear(struct spin_context *ctx)
{
	if (!ctx) return;
	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);
}

void spin_pause(struct spin_context *ctx)
{
	if (!ctx) return;

	pthread_mutex_lock(&ctx->mutex);
	if (!ctx->running) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	ctx->active = 0;
	pthread_mutex_unlock(&ctx->mutex);

	fprintf(ctx->output, "\r\033[K");
	fflush(ctx->output);
}

void spin_resume(struct spin_context *ctx)
{
	if (!ctx) return;

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->running) {
		ctx->active = 1;
		ctx->frame = 0;
		ctx->last_render_width = 0;
	}
	pthread_mutex_unlock(&ctx->mutex);
}

/* ---- spin_destroy ---- */

void spin_destroy(struct spin_context *ctx)
{
	if (!ctx) return;

	spin_cancel(ctx);

	pthread_mutex_destroy(&ctx->mutex);
	memset(ctx, 0, sizeof(*ctx));
}
