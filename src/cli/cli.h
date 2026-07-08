#ifndef CLI_H
#define CLI_H

#include "config.h"
#include "db/database.h"
#include "session.h"
#include "agent/react.h"
#include "agent/plan.h"
#include "agent/tool_context.h"
#include "agent/sub_agent.h"
#include "event/event.h"
#include "models/llm.h"
#include "skill/skill.h"
#include "mcp/mcp.h"
#include <pthread.h>
#include <stdio.h>

enum cli_event_mode {
	CLI_EVENTS_HUMAN,
	CLI_EVENTS_JSON,
	CLI_EVENTS_NONE,
};

struct cli_context {
	struct config config;
	struct db database;
	struct session current_session;
	struct tool_registry tools;
	struct plan_registry plans;
	struct skill_registry *skills;
	struct react_context *react;
	struct tokenizer *tokenizer;
	struct model *llm;
	struct model *img_llm;
	struct model *vid_llm;
	struct mcp_registry mcp;
	struct tool_context *tctx;
	struct sub_agent_runtime *sub_agents;
	char config_path[PATH_MAX];
	char workdir[PATH_MAX];
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
	pthread_t scheduler_thread;
	pthread_mutex_t react_lock;
	int react_lock_ready;
	pthread_mutex_t scheduler_lock;
	pthread_cond_t scheduler_cond;
	int scheduler_started;
	int scheduler_stop;
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
