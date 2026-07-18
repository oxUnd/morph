#ifndef CLI_H
#define CLI_H

#include "config.h"
#include "runtime/runtime.h"
#include <stdio.h>

enum cli_event_mode {
	CLI_EVENTS_HUMAN,
	CLI_EVENTS_JSON,
	CLI_EVENTS_NONE,
};

struct cli_context {
	struct runtime *runtime;
	int running;
	int streaming;
	int session_auto_named;
	int last_tool_was_plan;
	char image_path[PATH_MAX];
	char stream_buf[BUFSIZ];
	size_t stream_buf_len;
	enum react_step_type stream_type;
	int trace_json;
	enum cli_event_mode event_mode;
	morph_event_cb event_cb;
	void *event_user_data;
};

int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir, enum cli_event_mode event_mode);
void cli_run(struct cli_context *ctx);
void cli_run_once(struct cli_context *ctx, const char *prompt);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(void);
void cli_set_color_enabled(int enabled);
int cli_color_enabled(void);
int cli_printf(const char *fmt, ...);

#endif
