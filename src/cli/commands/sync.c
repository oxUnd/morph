#include "cli/commands/registry.h"
#include <sqlite3.h>

static int sync_manifest_path(struct cli_context *ctx, char *path, size_t size)
{
	char meta[PATH_MAX];
	int rc;

	if (!ctx || !path || size == 0)
		MORPH_RETURN(-EINVAL);
	rc = file_path_join(meta, sizeof(meta), ctx->config.sync.dir,
			    ".morph-sync");
	if (rc != 0)
		return rc;
	return file_path_join(path, size, meta, "manifest.db");
}

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

	if (ctx->sync_started) {
		rc = morph_sync_worker_status(&ctx->sync_worker, &st);
		if (rc != 0)
			return rc;
	} else {
		memset(&st, 0, sizeof(st));
	}
	print_sync_status(&st);
	return 0;
}

static int sync_cmd_now(struct cli_context *ctx)
{
	struct morph_sync_config cfg;
	struct morph_sync_status st;
	int rc;

	rc = cli_build_sync_config(ctx, &cfg);
	if (rc != 0)
		return rc;
	if (!cfg.sync_dir[0]) {
		CMD_ERROR("sync.dir is not configured");
		return -EINVAL;
	}
	rc = morph_sync_once(&cfg, &st);
	print_sync_status(&st);
	if (rc == 0)
		CMD_OK("sync completed");
	else
		CMD_ERROR("sync failed: %s", morph_strerror(rc));
	return rc;
}

static int sync_cmd_conflicts(struct cli_context *ctx)
{
	char path[PATH_MAX];
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sync_manifest_path(ctx, path, sizeof(path));
	if (rc != 0)
		return rc;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		CMD_ERROR("sync manifest not found");
		return -ENOENT;
	}
	rc = sqlite3_prepare_v2(db,
		"SELECT id,path,created_at FROM conflicts ORDER BY id DESC",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(db);
		MORPH_RETURN(MORPH_ERR_DB);
	}
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		printf("#%lld %s %lld\n",
		       (long long)sqlite3_column_int64(stmt, 0),
		       (const char *)sqlite3_column_text(stmt, 1),
		       (long long)sqlite3_column_int64(stmt, 2));
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return 0;
}

static int sync_cmd_restore(struct cli_context *ctx, const char *id)
{
	struct morph_sync_config cfg;
	int64_t trash_id;
	int rc;

	if (!id || !id[0]) {
		CMD_ERROR("usage: /sync restore <trash-id>");
		return -EINVAL;
	}
	trash_id = atoll(id);
	rc = cli_build_sync_config(ctx, &cfg);
	if (rc != 0)
		return rc;
	rc = morph_sync_restore_trash(&cfg, trash_id);
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
