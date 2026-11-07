#include "sapi/cli/commands/share.h"
#include "sapi/cli/services/share_store.h"
#include "util/json.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/presentation/share.h"

static int cli_share_command_share(struct cli_application *ctx, int argc, char **argv)
{
	(void)argv;
	if (argc != 1) {
		CMD_ERROR(ctx, "usage: /share");
		return -EINVAL;
	}
	struct json *document = NULL;
	int rc = runtime_session_share_current(ctx->runtime, &document);
	if (rc != 0) {
		if (rc == -ENODATA)
			CMD_ERROR(ctx, "no completed conversation is available to share");
		else
			CMD_ERROR(ctx, "share failed: %s", morph_strerror(rc));
		return rc;
	}
	char *path = NULL;
	rc = cli_share_store_save(document, &path);
	json_free(document);
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
		{"/share", cli_share_command_share, "Save the latest conversation as offline HTML",
		 "/share"},
	};
	return (struct cli_command_group){
		.entries = commands,
		.count = (int)(sizeof(commands) / sizeof(commands[0])),
		.category = "Session",
	};
}
