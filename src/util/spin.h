#ifndef SPIN_H
#define SPIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

enum spin_style {
	SPIN_STYLE_DOTS,
	SPIN_STYLE_ARROW,
	SPIN_STYLE_PULSE,
	SPIN_STYLE_BRAILLE,
};

enum spin_state {
	SPIN_STATE_IDLE,
	SPIN_STATE_THINKING,
	SPIN_STATE_LOADING,
	SPIN_STATE_EXECUTING,
	SPIN_STATE_DOWNLOADING,
	SPIN_STATE_UPLOADING,
	SPIN_STATE_COMPLETE,
	SPIN_STATE_ERROR,
};

struct spin_context {
	enum spin_style style;
	enum spin_state state;
	char message[256];
	char submessage[512];
	FILE *output;
	int interval_ms;
	volatile int running;
	volatile int active;
	int frame;
	time_t start_time;
	time_t last_update;
	size_t last_render_width;
	pthread_t thread;
	pthread_mutex_t mutex;
	volatile sig_atomic_t *cancel_flag;
};

void spin_init(struct spin_context *ctx, FILE *output);
void spin_start(struct spin_context *ctx, enum spin_state state, const char *message);
void spin_update(struct spin_context *ctx, const char *message);
void spin_set_sub(struct spin_context *ctx, const char *submessage);
void spin_stop(struct spin_context *ctx, enum spin_state final_state, const char *message);
void spin_pause(struct spin_context *ctx);
void spin_resume(struct spin_context *ctx);
void spin_render(struct spin_context *ctx);
void spin_clear(struct spin_context *ctx);
void spin_destroy(struct spin_context *ctx);
void spin_set_cancel_flag(struct spin_context *ctx, volatile sig_atomic_t *flag);

#ifdef __cplusplus
}
#endif

#endif
