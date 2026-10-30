#include "sapi/cli/commands/registry.h"
#include "sapi/cli/share.h"

static int cmd_share(struct cli_context *ctx, int argc, char **argv)
{
	(void)argv;
	if (argc != 1) {
		CMD_ERROR(ctx, "usage: /share");
		return -EINVAL;
	}
	cJSON *document = NULL;
	int rc = runtime_session_share_current(ctx->runtime, &document);
	if (rc != 0) {
		if (rc == -ENODATA)
			CMD_ERROR(ctx, "no completed conversation is available to share");
		else
			CMD_ERROR(ctx, "share failed: %s", morph_strerror(rc));
		return rc;
	}
	char *path = NULL;
	rc = cli_share_save(document, &path);
	cJSON_Delete(document);
	if (rc != 0)
		CMD_ERROR(ctx, "share failed: %s", morph_strerror(rc));
	else
		CMD_OK(ctx, "saved conversation to %s", path);
	free(path);
	return rc;
}

struct cli_command_group cli_share_commands(void)
{
	static const struct cli_command commands[] = {
		{"/share", cmd_share, "Save the latest conversation as offline HTML", "/share"},
	};
	return (struct cli_command_group){
		.entries = commands,
		.count = (int)(sizeof(commands) / sizeof(commands[0])),
		.category = "Session",
	};
}
