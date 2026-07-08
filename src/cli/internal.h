#ifndef CLI_INTERNAL_H
#define CLI_INTERNAL_H

#include "cli/cli.h"
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
#include "config.h"
#include "models/image_gen.h"
#include "models/video_gen.h"
#include "render/markdown.h"
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

#define CMD_HEADER(fmt, ...) \
	printf(ANSI_BOLD ANSI_CYAN "--- " fmt ANSI_RESET "\n", ##__VA_ARGS__)
#define CMD_ERROR(fmt, ...) \
	printf(ANSI_BOLD ANSI_RED "error: " ANSI_RESET fmt "\n", ##__VA_ARGS__)
#define CMD_OK(fmt, ...) \
	printf(ANSI_GREEN fmt ANSI_RESET "\n", ##__VA_ARGS__)

extern const char *default_db_path;
extern const char *default_config_path;
extern volatile sig_atomic_t cli_sigint_received;

void session_load_history(struct cli_context *ctx);
struct memory_options cli_memory_options(const struct cli_context *ctx);
void print_padded(const char *s, int target_width);
const char *cli_cmd_arg(int argc, char **argv, int idx);
int cli_argv_split(const char *input, char **argv, int max_args);
void cli_credit_session_key(struct cli_context *ctx, char *buf, size_t size);
void cli_update_tool_runtime_context(struct cli_context *ctx);
void cli_set_usage_context(struct cli_context *ctx);
void cli_record_model_usage(const struct model_usage *usage, void *user_data);
void cli_record_media_credits(struct cli_context *ctx, const char *kind,
				      int64_t image_units,
				      int64_t video_seconds,
				      const char *provider,
				      const char *model,
				      const char *metadata_json);

int cli_emit_startup_event(struct cli_context *ctx,
			   const char *name, const char *phase,
			   const char *message, const char *component,
			   int error_code);
int cli_emit_background_event(struct cli_context *ctx,
			      const char *name, const char *phase,
			      const char *message, const char *task,
			      int count, int error_code);
int cli_emit_mcp_event(struct cli_context *ctx,
		       const char *name, const char *phase,
		       const char *message, const char *server,
		       enum mcp_transport_type transport,
		       int auto_connect, int timeout_seconds,
		       int tools, int resources, int prompts,
		       int error_code);
int cli_discover_mcp_server(struct cli_context *ctx,
			    struct mcp_client *mc, int auto_connect,
			    int timeout_seconds);
int cli_event_callback(const struct morph_event *ev, void *user_data);

struct scheduled_task_event_sink cli_task_event_sink(struct cli_context *ctx);
void cli_process_due_tasks(struct cli_context *ctx);
int cli_scheduled_task_runner(const struct scheduled_task *task,
			      struct scheduled_task_action_result *result,
			      void *user_data);
int cli_scheduler_start(struct cli_context *ctx);
void cli_scheduler_stop(struct cli_context *ctx);

void media_callback(const char *type, const char *path, void *user);
void cli_markdown_render_ansi(const char *md);
void cli_markdown_render_ansi_with_media(const char *md,
					 markdown_media_cb cb,
					 void *user);
int output_callback(const struct react_output_event *event, void *user_data);
int cli_ask_user_callback(const char *question,
			  const char *const *choices,
			  int choices_count,
			  char **answer,
			  void *user_data);
enum hitl_verdict hitl_approval_callback(const char *tool_name,
					 const char *tool_args,
					 void *user_data);
enum tool_operation_verdict operation_approval_callback(
	const struct tool_operation *op, void *user_data);

#define printf cli_printf

#endif
