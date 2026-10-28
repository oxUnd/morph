#include "sapi/cli/commands/registry.h"
#include "sapi/cli/share.h"

static int cmd_share(struct cli_context *ctx, int argc, char **argv)
{
	(void)argv;
	if (argc != 1) {
		CMD_ERROR("usage: /share");
		MORPH_RETURN(-EINVAL);
	}
	cJSON *document = NULL;
	int rc = runtime_session_share_current(ctx->runtime, &document);
	if (rc != 0) {
		if (rc == -ENODATA)
			CMD_ERROR("no completed conversation is available to share");
		else
			CMD_ERROR("share failed: %s", morph_strerror(rc));
		MORPH_RETURN(rc);
	}
	char *path = NULL;
	rc = cli_share_save(document, &path);
	cJSON_Delete(document);
	if (rc != 0)
		CMD_ERROR("share failed: %s", morph_strerror(rc));
	else
		CMD_OK("saved conversation to %s", path);
	free(path);
	MORPH_RETURN(rc);
}

int cli_register_share_commands(void)
{
	static const struct cli_command commands[] = {
		{"/share", cmd_share, "Save the latest conversation as offline HTML", "/share"},
	};
	return cli_command_register_many(commands,
		(int)(sizeof(commands) / sizeof(commands[0])), "Session");
}
