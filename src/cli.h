#ifndef CLI_H
#define CLI_H

#include "config.h"
#include "db/database.h"
#include "session.h"
#include "agent/react.h"
#include "agent/plan.h"
#include "agent/tool_context.h"
#include "models/llm.h"
#include "skill/skill.h"
#include "util/spin.h"
#include "mcp/mcp.h"

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
	char workdir[PATH_MAX];
	int running;
	int streaming;
	int session_auto_named;
	int last_tool_was_plan;
	char image_path[PATH_MAX];
	struct spin_context spin;
	char stream_buf[BUFSIZ];
	size_t stream_buf_len;
	int trace_json;
};

int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir);
void cli_run(struct cli_context *ctx);
void cli_run_once(struct cli_context *ctx, const char *prompt);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(void);

#endif
