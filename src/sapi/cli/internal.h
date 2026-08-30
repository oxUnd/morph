#ifndef CLI_INTERNAL_H
#define CLI_INTERNAL_H

#include "sapi/cli/cli.h"
#include "sapi/cli/terminal.h"
#include "sapi/cli/ui_event.h"
#include "sapi/cli/command_job.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/arena.h"
#include "util/utf8.h"
#include "agent/tokenizer.h"
#include "agent/compress.h"
#include "ext/ext.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_qa.h"
#include "agent/tools/img_inpaint.h"
#include "agent/tools/img_compose.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/img_convert.h"
#include "agent/tools/vid_gen.h"
#include "agent/tools/file_read.h"
#include "agent/tools/file_list.h"
#include "agent/tools/file_info.h"
#include "agent/tools/config_write.h"
#include "agent/tools/skill_activate.h"
#include "agent/tools/bash_exec.h"
#include "agent/tools/ask_user.h"
#include "agent/tools/img_annotate.h"
#include "agent/plan.h"
#include "agent/guardrail.h"
#include "agent/tool_context.h"
#include "agent/tools/plan.h"
#include "agent/tools/sub_agent_tools.h"
#include "agent/tools/scheduled_tasks.h"
#include "agent/tools/runtime_query.h"
#include "mcp/mcp.h"
#include "db/database.h"
#include "db/scheduled_task.h"
#include "credits.h"
#include "agent/memory.h"
#include "agent/turn.h"
#include "ext/install.h"
#include "config/config.h"
#include "models/image_gen.h"
#include "models/video_gen.h"
#include "morph_markdown_kitty.h"
#include "render/image.h"
#include "render/video.h"
#include "agent_ui/agent_ui.h"
#include "stb_image.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <poll.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_RESET  "\033[0m"

#ifdef HAVE_READLINE
#define CLI_RL_IGNORE_START "\001"
#define CLI_RL_IGNORE_END   "\002"
#else
#define CLI_RL_IGNORE_START ""
#define CLI_RL_IGNORE_END   ""
#endif

#define CMD_HEADER(fmt, ...) \
	printf("\n" ANSI_BOLD ANSI_CYAN "• " fmt ANSI_RESET "\n", \
	       ##__VA_ARGS__)
#define CMD_ERROR(fmt, ...) \
	printf(ANSI_BOLD ANSI_RED "• Error  " ANSI_RESET fmt "\n", \
	       ##__VA_ARGS__)
#define CMD_OK(fmt, ...) \
	printf(ANSI_BOLD ANSI_GREEN "• " ANSI_RESET fmt "\n", ##__VA_ARGS__)

extern const char *default_db_path;
extern const char *default_config_path;
extern volatile sig_atomic_t cli_sigint_received;
void cli_sigint_handler(int sig);
const char *cli_input_prompt(void);

void print_padded(const char *s, int target_width);
/* Use for model, tool, file, database, MCP, and user-provided text. */
int cli_print_untrusted_text(const char *text,
			     enum utf8_terminal_text_mode mode);
const char *cli_cmd_arg(int argc, char **argv, int idx);
int cli_argv_split(const char *input, char **argv, int max_args);
int cli_command_capture_begin(morph_buf_t *output);
void cli_command_capture_end(void);
void cli_record_media_credits(struct cli_context *ctx, const char *kind,
				      int64_t image_units,
				      int64_t video_seconds,
				      const char *provider,
				      const char *model,
				      const char *metadata_json);
int cli_attach_image(struct cli_context *ctx, const char *path);
int cli_clipboard_save_image(struct cli_context *ctx, char **out_path);
int cli_handle_media_path(struct cli_context *ctx, const char *input,
			  int *handled);

int cli_event_callback(const struct morph_event *ev, void *user_data);
void cli_turn_begin(struct cli_context *ctx);
void cli_turn_finish(struct cli_context *ctx, int turn_rc);
int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_reset(struct cli_context *ctx);
void cli_presentation_finish(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
void cli_presentation_prepare_prompt(struct cli_context *ctx);
int cli_presentation_event(struct cli_context *ctx,
			   const struct morph_event *ev);

struct cli_cancel_monitor;
struct cli_cancel_monitor *cli_cancel_monitor_start(int fd);
void cli_cancel_monitor_stop(struct cli_cancel_monitor *monitor);
void cli_cancel_state_reset(void);

void cli_process_due_tasks(struct cli_context *ctx);
int cli_scheduled_task_runner(const struct scheduled_task *task,
			      struct scheduled_task_action_result *result,
			      void *user_data);
int cli_scheduler_start(struct cli_context *ctx);
int cli_sync_start(struct cli_context *ctx);

void media_callback(const char *type, const char *path, void *user);
void cli_markdown_render_ansi(const char *md);
typedef void (*cli_markdown_media_cb)(const char *type, const char *path,
				      void *user);
void cli_markdown_render_ansi_with_media(const char *md,
					 cli_markdown_media_cb cb,
					 void *user);
void cli_markdown_render_ansi_with_media_indented(const char *md,
						  unsigned int indent,
						  cli_markdown_media_cb cb,
						  void *user);
int cli_markdown_stream_append(struct cli_context *ctx, const char *delta,
			       int kind);
void cli_markdown_stream_reset(struct cli_context *ctx, int finish_output);
int cli_ask_user_callback(const char *question,
			  const char *const *choices,
			  int choices_count,
			  const char *selection_mode,
			  int min_choices,
			  int max_choices,
			  char ***answers,
			  int *answers_count,
			  void *user_data);
enum hitl_verdict hitl_approval_callback(const char *tool_name,
					 const char *tool_args,
					 void *user_data);
enum tool_operation_verdict operation_approval_callback(
	const struct tool_operation *op, void *user_data);

#define printf cli_printf
#define fputs cli_fputs
#define putchar cli_putchar

#endif
