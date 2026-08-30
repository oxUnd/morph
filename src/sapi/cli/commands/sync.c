#include "sapi/cli/commands/registry.h"
#include "sapi/cli/interaction.h"
#include "sapi/cli/list_ui.h"

static void print_sync_field(const char *ancestor, int is_last,
			     const char *name, const char *value, int columns)
{
	cli_list_item(ancestor, is_last, "", name, value, 20, columns);
}

static void print_sync_status(const struct morph_sync_status *st)
{
	char value[64];
	int columns = cli_list_columns();
	int state_count = st->error_code != 0 ? 3 : 2;

	CMD_HEADER("sync status");
	cli_list_group("state", state_count, 0);
	print_sync_field("│ ", 0, "running", st->running ? "yes" : "no",
			 columns);
	if (st->last_run_at > 0)
		snprintf(value, sizeof(value), "%lld",
			 (long long)st->last_run_at);
	else
		strncpy(value, "never", sizeof(value) - 1);
	print_sync_field("│ ", st->error_code == 0, "last run", value,
			 columns);
	if (st->error_code != 0)
		print_sync_field("│ ", 1, "last error", st->last_error,
				 columns);

	cli_list_group("files", 4, 0);
	snprintf(value, sizeof(value), "%d", st->copied);
	print_sync_field("│ ", 0, "copied", value, columns);
	snprintf(value, sizeof(value), "%d", st->deleted);
	print_sync_field("│ ", 0, "deleted", value, columns);
	snprintf(value, sizeof(value), "%d", st->conflicts);
	print_sync_field("│ ", 0, "conflicts", value, columns);
	snprintf(value, sizeof(value), "%d", st->recycled);
	print_sync_field("│ ", 1, "recycled", value, columns);

	cli_list_group("database", 4, 1);
	snprintf(value, sizeof(value), "%d", st->db_snapshots);
	print_sync_field("  ", 0, "snapshots", value, columns);
	snprintf(value, sizeof(value), "%d", st->db_chunks_uploaded);
	print_sync_field("  ", 0, "chunks uploaded", value, columns);
	snprintf(value, sizeof(value), "%d", st->db_chunks_reused);
	print_sync_field("  ", 0, "chunks reused", value, columns);
	snprintf(value, sizeof(value), "%lld",
		 (long long)st->db_bytes_uploaded);
	print_sync_field("  ", 1, "bytes uploaded", value, columns);
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
	CMD_HEADER("sync conflicts (%d)", count);
	if (count == 0) {
		printf("  " ANSI_DIM "└ none" ANSI_RESET "\n");
		runtime_sync_conflicts_free(items);
		return 0;
	}
	for (int i = 0; i < count; i++) {
		char label[64];
		char description[PATH_MAX + 64];

		snprintf(label, sizeof(label), "#%lld",
			 (long long)items[i].id);
		snprintf(description, sizeof(description), "%s · created %lld",
			 items[i].path, (long long)items[i].created_at);
		cli_list_item("", i == count - 1, "", label, description,
			      12, cli_list_columns());
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

static int sync_backup_compare(const void *left, const void *right)
{
	const struct morph_sync_backup *a = left;
	const struct morph_sync_backup *b = right;
	int rc;

	rc = strcmp(a->path, b->path);
	if (rc != 0)
		return rc;
	if (a->created_at != b->created_at)
		return a->created_at > b->created_at ? -1 : 1;
	return strcmp(a->snapshot_id, b->snapshot_id);
}

static int sync_cmd_backups(struct cli_context *ctx, const char *path)
{
	struct morph_sync_backup *items = NULL;
	int columns = cli_list_columns();
	int count = 0;
	int rc;

	rc = runtime_sync_backups(ctx->runtime, path, &items, &count);
	if (rc != 0)
		return rc;
	CMD_HEADER("database backups (%d)", count);
	if (count == 0) {
		printf("  " ANSI_DIM "└ none" ANSI_RESET "\n");
		morph_sync_backups_free(items);
		return 0;
	}
	qsort(items, (size_t)count, sizeof(items[0]), sync_backup_compare);
	for (int i = 0; i < count;) {
		int end = i + 1;
		int group_last;
		const char *ancestor;

		while (end < count && strcmp(items[i].path, items[end].path) == 0)
			end++;
		group_last = end == count;
		cli_list_group(items[i].path, end - i, group_last);
		ancestor = group_last ? "  " : "│ ";
		for (int j = i; j < end; j++) {
			char description[MORPH_SYNC_DEVICE_ID_MAX + 80];

			snprintf(description, sizeof(description),
				 "%s · created %lld · %lld bytes",
				 items[j].device_id,
				 (long long)items[j].created_at,
				 (long long)items[j].size);
			cli_list_item(ancestor, j == end - 1, "",
				      items[j].snapshot_id, description, 24,
				      columns);
		}
		i = end;
	}
	morph_sync_backups_free(items);
	return 0;
}

static int sync_cmd_restore_db(struct cli_context *ctx, const char *snapshot_id,
			       const char *destination)
{
	int rc;

	if (!snapshot_id || !destination) {
		CMD_ERROR("usage: /sync restore-db <snapshot-id> <destination>");
		return -EINVAL;
	}
	rc = runtime_sync_restore_db(ctx->runtime, snapshot_id, destination);
	if (rc == 0)
		CMD_OK("restored database to %s", destination);
	else
		CMD_ERROR("database restore failed: %s", morph_strerror(rc));
	return rc;
}

static int sync_confirm_db_replace(struct cli_context *ctx, const char *path)
{
	char answer[16];
	FILE *tty;

	if (ctx && ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		cJSON *request = cJSON_CreateObject();
		int confirmed = 0;
		int rc;

		if (!request)
			return 0;
		if (!cJSON_AddStringToObject(request, "action",
					     "replace_database") ||
		    !cJSON_AddStringToObject(request, "path", path ? path : "")) {
			cJSON_Delete(request);
			return 0;
		}
		rc = cli_interaction_confirm(ctx, "command_confirmation",
			request, &confirmed);
		cJSON_Delete(request);
		return rc == 0 && confirmed;
	}

	printf(ANSI_BOLD ANSI_YELLOW
	       "? Replace %s with this backup? Current data will be backed up. "
	       "[y/N]: " ANSI_RESET, path);
	fflush(stdout);
	tty = fopen("/dev/tty", "r");
	if (!tty) {
		putchar('\n');
		return 0;
	}
	if (!fgets(answer, sizeof(answer), tty))
		answer[0] = '\0';
	fclose(tty);
	return answer[0] == 'y' || answer[0] == 'Y';
}

static int sync_cmd_replace_db(struct cli_context *ctx,
			       const char *snapshot_id, int confirmed)
{
	struct morph_sync_restore_plan plan;
	int rc;

	rc = runtime_sync_prepare_db_replace(ctx->runtime, snapshot_id, &plan);
	if (rc != 0) {
		CMD_ERROR("database restore preparation failed: %s",
			  morph_strerror(rc));
		return rc;
	}
	if (!confirmed && !sync_confirm_db_replace(ctx, plan.path)) {
		(void)morph_sync_rollback_db_replace(&plan);
		(void)runtime_sync_start_instance(ctx->runtime, NULL, NULL);
		printf(ANSI_DIM "  cancelled" ANSI_RESET "\n");
		return 0;
	}
	ctx->db_restore_plan = plan;
	ctx->pending_db_restore = 1;
	ctx->running = 0;
	CMD_OK("restore prepared; restarting Morph to replace %s", plan.path);
	return 0;
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
	if (strcmp(sub, "backups") == 0)
		return sync_cmd_backups(ctx, cli_cmd_arg(argc, argv, 2));
	if (strcmp(sub, "restore-db") == 0) {
		const char *snapshot_id = cli_cmd_arg(argc, argv, 2);
		const char *destination = cli_cmd_arg(argc, argv, 3);
		int yes = destination &&
			(strcmp(destination, "--yes") == 0 ||
			 strcmp(destination, "-y") == 0);

		if (!snapshot_id) {
			CMD_ERROR("usage: /sync restore-db <snapshot-id> "
				  "[destination|--yes]");
			return -EINVAL;
		}
		if (destination && !yes)
			return sync_cmd_restore_db(ctx, snapshot_id, destination);
		return sync_cmd_replace_db(ctx, snapshot_id, yes);
	}
	CMD_ERROR("usage: /sync [status|now|conflicts|restore <trash-id>|"
		  "backups [path]|restore-db <snapshot-id> "
		  "[destination|--yes]]");
	return -EINVAL;
}

int cli_register_sync_commands(void)
{
	static const struct cli_command cmds[] = {
		{ "/sync", sync_cmd, "Manage .morph directory sync",
		  "/sync [status|now|conflicts|restore <trash-id>|"
		  "backups [path]|restore-db <snapshot-id> "
		  "[destination|--yes]]" },
	};

	return cli_command_register_many(cmds,
					 (int)(sizeof(cmds) /
					 sizeof(cmds[0])),
					 "Sync");
}
