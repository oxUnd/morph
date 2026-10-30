#ifndef CLI_H
#define CLI_H

struct morph_log;
#include "config/config.h"
#include "runtime/runtime.h"
#include "sapi/cli/recap.h"
#include "util/buf.h"
#include "sapi/cli/composer.h"
#include "sapi/cli/callbacks.h"
#include "util/signals.h"
#include "util/strmap.h"
#include "util/utf8.h"
#include <stdint.h>
#include <stdio.h>

enum cli_presentation_mode {
	CLI_PRESENT_INTERACTIVE,
	CLI_PRESENT_ONCE_PLAIN,
	CLI_PRESENT_EVENTS_JSON,
};

struct morph_md_kitty;
struct cli_cancel_monitor;
struct cli_ui;
struct cli_terminal;
struct cli_command_job;
struct cli_transcript;
struct cli_jobs;

struct cli_input_status {
	int enabled;
	int padding_rows;
	int distance;
	int footer_rows;
	int suspended;
	int64_t updated;
	int tokens;
	int limit;
	int64_t credits;
	int session_titles_pending;
	struct scheduled_task_counts tasks;
};

struct cli_markdown_output {
	struct cli_context *ctx;
	char font_path[PATH_MAX];
	int prefix_pending;
	int trailing_newlines;
};

struct cli_output {
	int color_disabled;
	int structured;
	int command_result_pending;
	morph_buf_t *capture;
	int capture_styled;
};

struct cli_context {
	struct morph_log *logger;
	struct cli_output output;
	pthread_mutex_t prompt_lock;
	int prompt_lock_ready;
	struct cli_callback *callbacks;
	int callback_error;
	int64_t input_deadline_ms;
	int input_timed_out;
	const struct morph_cancel_token *input_cancel_token;
	struct cli_composer composer;
	char *readline_ready_input;
	int details_hint_visible;
	int details_hint_column;
	char *footer_text;
	struct session *completion_sessions;
	int completion_count;
	int completion_index;
	int completion_length;
	struct morph_signal_listener signals;
	int signals_ready;
	volatile sig_atomic_t terminate_requested;
	volatile sig_atomic_t interrupted;
	volatile sig_atomic_t resized;

	struct cli_recap_state recap;
	struct runtime *runtime;
	struct config_file_error startup_error;
	struct cli_ui *ui;
	struct cli_terminal *terminal;
	struct cli_command_job *input_job;
	struct cli_transcript *transcript;
	struct cli_jobs *jobs;
	struct cli_input_status input_status;
	int tool_details;
	int details_open;
	int details_visible;
	enum cli_presentation_mode presentation_mode;
	int running;
	int session_auto_named;
	int presentation_ready;
	int turn_active;
	struct cli_cancel_monitor *cancel_monitor;
	int event_stream_kind;
	int event_stream_has_delta;
	int event_stream_complete;
	int event_stream_visible;
	size_t reasoning_stream_offset;
	int final_rendered;
	int native_content_started;
	int native_content_visible;
	int markdown_stream_kind;
	int markdown_stream_visible;
	struct cli_markdown_output markdown_output;
	int mcp_tree_active;
	char image_path[PATH_MAX];
	char workdir[PATH_MAX];
	char mcp_tree_server[MCP_NAME_MAX];
	morph_buf_t event_stream;
	morph_buf_t native_content;
	morph_buf_t markdown_stream_text;
	struct utf8_terminal_sanitizer event_stream_sanitizer;
	struct utf8_terminal_sanitizer markdown_stream_sanitizer;
	morph_strmap_t rendered_artifacts;
	morph_strmap_t announced_artifacts;
	struct morph_md_kitty *markdown_stream;
	int trace_json;
	int pending_db_restore;
	uint64_t interaction_sequence;
	struct morph_sync_restore_plan db_restore_plan;
	morph_event_cb event_cb;
	void *event_user_data;
};

int cli_init(struct cli_context *ctx, struct morph_log *logger, const char *config_path,
	     const char *db_path, const char *workdir, const char *session_name,
	     enum cli_presentation_mode mode);
void cli_run(struct cli_context *ctx);
void cli_run_once(struct cli_context *ctx, const char *prompt);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(struct cli_context *ctx);
void cli_set_color_enabled(struct cli_context *ctx, int enabled);
int cli_color_enabled(struct cli_context *ctx);
int cli_printf(struct cli_context *ctx, const char *fmt, ...);
int cli_fputs(struct cli_context *ctx, const char *text, FILE *stream);
int cli_putchar(struct cli_context *ctx, int ch);
int cli_write_ndjson(const char *json);
void cli_set_structured_output(struct cli_context *ctx, int enabled);
int cli_structured_output_enabled(struct cli_context *ctx);

#endif
