#ifndef CLI_H
#define CLI_H

#include "config.h"
#include "db/database.h"
#include "session.h"
#include "agent/react.h"
#include "agent/plan.h"
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
	int running;
	int streaming;
	int session_auto_named;
	char image_path[512];
	struct spin_context spin;
	char stream_buf[4096];
	size_t stream_buf_len;
};

int cli_init(struct cli_context *ctx, const char *config_path);
void cli_run(struct cli_context *ctx);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(void);

#endif