#include "sapi/cli/commands/files.h"

#include "sapi/cli/application.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/status.h"
#include "sapi/cli/presentation/output.h"
#include <nb/err.h>

static int cli_files_command(struct cli_application *ctx, int argc, char **argv)
{
	if (argc > 2) {
		CMD_ERROR(ctx, "usage: /side [path]");
		return NB_ERR_INVAL;
	}

	if (!cli_chat_active(ctx)) {
		CMD_HEADER(ctx, "Files");
		cli_output_printf(ctx, "  Open the file sidebar in an interactive session.\n");
		return NB_OK;
	}

	int rc = cli_chat_files_show(ctx, cli_cmd_arg_borrow(argc, argv, 1));
	if (rc)
		CMD_ERROR(ctx, "cannot open file sidebar: %s", nb_status_str(rc));

	return rc;
}

struct cli_command_group cli_files_commands(void)
{
	static const struct cli_command cli_files_entries[] = {
		{"/side", cli_files_command, "Open conversation files in the sidebar",
		 "/side [path]"},
		{"/sidebar", cli_files_command, "Open the file sidebar", "/sidebar [path]"},
	};
	return (struct cli_command_group){
		.entries = cli_files_entries,
		.count = (int)(sizeof(cli_files_entries) / sizeof(cli_files_entries[0])),
		.category = "Workspace",
	};
}
