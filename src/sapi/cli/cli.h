#ifndef CLI_H
#define CLI_H

#include "config/config.h"
#include "runtime/runtime.h"
#include "util/buf.h"
#include "util/spin.h"
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
	int event_stream_visible;
	int final_rendered;
	int status_visible;
	int status_spin_initialized;
	int markdown_stream_kind;
	int markdown_stream_visible;
	int mcp_tree_active;
	char image_path[PATH_MAX];
	char mcp_tree_server[MCP_NAME_MAX];
	morph_buf_t event_stream;
	morph_buf_t markdown_stream_text;
	morph_strmap_t rendered_artifacts;
	struct spin_context status_spin;
	struct morph_md_kitty *markdown_stream;
	int trace_json;
	int pending_db_restore;
	struct morph_sync_restore_plan db_restore_plan;
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
