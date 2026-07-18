#include "cli/commands/registry.h"

static void print_sync_status(const struct morph_sync_status *st)
{
	printf("running: %s\n", st->running ? "yes" : "no");
	printf("copied: %d\n", st->copied);
	printf("deleted: %d\n", st->deleted);
	printf("conflicts: %d\n", st->conflicts);
	printf("recycled: %d\n", st->recycled);
	if (st->last_run_at > 0)
		printf("last_run_at: %lld\n", (long long)st->last_run_at);
	if (st->error_code != 0)
		printf("last_error: %s\n", st->last_error);
}

static int sync_cmd_status(struct cli_context *ctx)
{
	struct morph_sync_status st;
	int rc;

	rc = runtime_sync_status_instance(ctx->runtime, &st);
	if (rc != 0)
		return rc;
	print_sync_status(&st);
	return 0;
}

static int sync_cmd_now(struct cli_context *ctx)
{
	struct morph_sync_status st;
	int rc;

	rc = runtime_sync_now_instance(ctx->runtime, NULL, NULL, &st);
	print_sync_status(&st);
	if (rc == 0)
		CMD_OK("sync completed");
	else
		CMD_ERROR("sync failed: %s", morph_strerror(rc));
	return rc;
}

static int sync_cmd_conflicts(struct cli_context *ctx)
{
	struct runtime_sync_conflict *items = NULL;
	int count = 0;
	int rc;

	rc = runtime_sync_conflicts(ctx->runtime, &items, &count);
	if (rc == -ENOENT) {
		CMD_ERROR("sync manifest not found");
		return -ENOENT;
	}
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		printf("#%lld %s %lld\n",
		       (long long)items[i].id, items[i].path,
		       (long long)items[i].created_at);
	}
	runtime_sync_conflicts_free(items);
	return 0;
}

static int sync_cmd_restore(struct cli_context *ctx, const char *id)
{
	int64_t trash_id;
	int rc;

	if (!id || !id[0]) {
		CMD_ERROR("usage: /sync restore <trash-id>");
		return -EINVAL;
	}
	trash_id = atoll(id);
	rc = runtime_sync_restore(ctx->runtime, trash_id);
	if (rc == 0)
		CMD_OK("restored trash entry %s", id);
	else
		CMD_ERROR("restore failed: %s", morph_strerror(rc));
	return rc;
}

static int sync_cmd(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "status") == 0)
		return sync_cmd_status(ctx);
	if (strcmp(sub, "now") == 0)
		return sync_cmd_now(ctx);
	if (strcmp(sub, "conflicts") == 0)
		return sync_cmd_conflicts(ctx);
	if (strcmp(sub, "restore") == 0)
		return sync_cmd_restore(ctx, cli_cmd_arg(argc, argv, 2));
	CMD_ERROR("usage: /sync [status|now|conflicts|restore <trash-id>]");
	return -EINVAL;
}

int cli_register_sync_commands(void)
{
	static const struct cli_command cmds[] = {
		{ "/sync", sync_cmd, "Manage .morph directory sync",
		  "/sync [status|now|conflicts|restore <trash-id>]" },
	};

	return cli_command_register_many(cmds,
		(int)(sizeof(cmds) / sizeof(cmds[0])));
}
