#ifndef CLI_H
#define CLI_H

#include "config.h"
#include "runtime/runtime.h"
#include "util/buf.h"
#include "util/strmap.h"
#include <stdio.h>

enum cli_presentation_mode {
	CLI_PRESENT_INTERACTIVE,
	CLI_PRESENT_ONCE_PLAIN,
	CLI_PRESENT_EVENTS_JSON,
};

struct morph_md_kitty;

struct cli_context {
	struct runtime *runtime;
	enum cli_presentation_mode presentation_mode;
	int running;
	int session_auto_named;
	int presentation_ready;
	int turn_active;
	int event_stream_kind;
	int event_stream_has_delta;
	int event_stream_complete;
	int final_rendered;
	int status_visible;
	int markdown_stream_kind;
	int markdown_stream_visible;
	char image_path[PATH_MAX];
	morph_buf_t event_stream;
	morph_buf_t markdown_stream_text;
	morph_strmap_t rendered_artifacts;
	struct morph_md_kitty *markdown_stream;
	int trace_json;
	morph_event_cb event_cb;
	void *event_user_data;
};

int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir, enum cli_presentation_mode mode);
void cli_run(struct cli_context *ctx);
void cli_run_once(struct cli_context *ctx, const char *prompt);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(void);
void cli_set_color_enabled(int enabled);
int cli_color_enabled(void);
int cli_printf(const char *fmt, ...);

#endif
