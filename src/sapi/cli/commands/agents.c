#include "sapi/cli/commands/agents.h"
#include "nb/err.h"
#include "runtime/runtime.h"

#include "sapi/cli/application.h"
#include "sapi/cli/pages/subagents.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/list.h"
#include "nb/cleanup.h"
#include <string.h>

struct cli_agents_snapshot {
	struct agent_info *items;
	int count;
};

static void cli_agents_snapshot_cleanup(struct cli_agents_snapshot *snapshot)
{
	runtime_agent_list_free(snapshot->items, snapshot->count);
}

static int cli_agents_print_detail(struct cli_application *ctx, const char *label,
				   const char *value)
{
	nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&detail), nb_buf_reserve(&detail, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_agents_detail(&detail, label, value, cli_list_columns(),
				       cli_output_color_enabled(ctx));
	if (rc == 0)
		cli_output_printf(ctx, "%s", nb_buf_cstr(&detail));
	return rc;
}

static int cli_agents_command_agents(struct cli_application *ctx, int argc, char **argv)
{
	if (argc > 2) {
		CMD_ERROR(ctx, "usage: /agents [/root/name]");
		return NB_ERR_INVAL;
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
		int rc = cli_subagents_show_fullscreen(ctx, cli_cmd_arg_borrow(argc, argv, 1));
		if (rc != 0)
			CMD_ERROR(ctx, "cannot open agents: %s",
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	struct cli_agents_snapshot snapshot NB_CLEANUP(cli_agents_snapshot_cleanup) = {0};
	int64_t session = 0;
	(void)runtime_session_current_id(ctx->runtime, &session);
	int rc = runtime_agent_list(ctx->runtime, session, &snapshot.items, &snapshot.count);
	if (rc == NB_ERR_NOT_FOUND) {
		CMD_HEADER(ctx, "Agents");
		cli_output_printf(ctx, "  Multi-agent collaboration is unavailable.\n");
		return 0;
	}
	if (rc != 0) {
		CMD_ERROR(ctx, "cannot inspect agents: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	const char *id = cli_cmd_arg_borrow(argc, argv, 1);
	CMD_HEADER(ctx, "Agents (%d)", snapshot.count);
	struct agent_budget budget = {0};
	if (runtime_agent_budget(ctx->runtime, &budget) == 0 && budget.limit > 0)
		cli_output_printf(ctx, "  Shared session budget: %lld / %lld weighted units%s\n", (long long)budget.used,
		       (long long)budget.limit, budget.exhausted ? " (exhausted)" : "");
	if (budget.limit == 0)
		cli_output_printf(ctx, "  Shared session usage: %lld weighted units (limit disabled).\n",
		       (long long)budget.used);
	if (budget.request_tokens > 0)
		cli_output_printf(ctx, "  Last refused request: up to %lld weighted units "
		       "(input + configured maximum output).\n",
		       (long long)budget.request_tokens);
	if (!snapshot.count && !id)
		cli_output_printf(ctx, "  No agents have been spawned in this session.\n");
	int found = !id;
	for (int i = 0; i < snapshot.count; i++) {
		const struct agent_info *agent = &snapshot.items[i];
		if (id && strcmp(id, agent->id))
			continue;
		found = 1;
		cli_list_text_field(ctx, agent->id, cli_agents_status(agent->status), 0,
				    cli_list_columns());
		if (id) {
			rc = cli_agents_print_detail(ctx, "Task",
						     agent->description ? agent->description : "");
			if (rc != 0)
				return rc;
		} else {
			cli_list_text_field(ctx, "Task",
					    agent->description ? agent->description : "", 0,
					    cli_list_columns());
		}
		cli_list_text_field(ctx, "Activity",
				    agent->activity ? agent->activity
						    : cli_agents_status(agent->status),
				    !id, cli_list_columns());
		if (id) {
			rc = cli_agents_print_detail(
				ctx, "Result", agent->result ? agent->result : "Not finished");
			if (rc != 0)
				return rc;
		}
	}
	if (!found) {
		CMD_ERROR(ctx, "agent not found: %s", id);
		return NB_ERR_NOT_FOUND;
	}
	return 0;
}

static int cli_agents_command_subagents(struct cli_application *ctx, int argc, char **argv)
{
	if (argc > 2) {
		CMD_ERROR(ctx, "usage: /subagents [/root/name]");
		return NB_ERR_INVAL;
	}
	int rc = cli_subagents_show(ctx, cli_cmd_arg_borrow(argc, argv, 1));
	if (rc != 0)
		CMD_ERROR(ctx, "cannot open subagents: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc;
}

struct cli_command_group cli_agent_commands(void)
{
	static const struct cli_command commands[] = {
		{"/subagents", cli_agents_command_subagents, "Switch agent threads, follow up or interrupt",
		 "/subagents [/root/name]"},
		{"/agents", cli_agents_command_agents, "Open the full-screen agent browser",
		 "/agents [/root/name]"},
	};
	return (struct cli_command_group){commands, (int)(sizeof(commands) / sizeof(commands[0])),
					  "Runtime"};
}
