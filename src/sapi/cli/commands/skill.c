#include "sapi/cli/commands/skill.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/style.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"

static int cli_skill_item_compare(const void *left, const void *right)
{
	const struct cli_skill_list_row *a = left;
	const struct cli_skill_list_row *b = right;

	if (a->state != b->state)
		return (int)a->state - (int)b->state;
	return strcmp(a->name, b->name);
}

static void cli_skill_rows_cleanup(struct cli_skill_list_row *rows, size_t count)
{
	if (!rows)
		return;
	for (size_t i = 0; i < count; i++) {
		free((char *)rows[i].name);
		free((char *)rows[i].description);
	}
	free(rows);
}

static int cli_skill_collect_rows(struct cli_application *ctx, struct cli_skill_list_row **out,
				  size_t *count_out, int *active_count)
{
	struct cli_skill_list_row *rows;
	int count = runtime_skill_count(ctx->runtime);
	int rc = 0;

	*out = NULL;
	*count_out = 0;
	*active_count = 0;
	if (count <= 0)
		return 0;
	rows = calloc((size_t)count, sizeof(*rows));
	if (!rows)
		return NB_ERR_NOMEM;
	for (int i = 0; i < count; i++) {
		struct skill_entry entry;

		rc = runtime_skill_info_borrow(ctx->runtime, i, &entry);
		if (rc != 0)
			break;
		rows[i].state = !entry.enabled ? CLI_SKILL_LIST_DISABLED
			       : entry.activated ? CLI_SKILL_LIST_ACTIVE
						 : CLI_SKILL_LIST_AVAILABLE;
		rows[i].name = strdup(entry.fm.name);
		rows[i].description = strdup(entry.fm.description);
		if (!rows[i].name || !rows[i].description) {
			rc = NB_ERR_NOMEM;
			break;
		}
		if (entry.activated)
			(*active_count)++;
	}
	if (rc != 0) {
		cli_skill_rows_cleanup(rows, (size_t)count);
		return rc;
	}
	qsort(rows, (size_t)count, sizeof(*rows), cli_skill_item_compare);
	*out = rows;
	*count_out = (size_t)count;
	return 0;
}

static int cli_skill_print_list(struct cli_application *ctx)
{
	struct cli_skill_list_row *rows = NULL;
	size_t count = 0;
	int active_count = 0;
	int rc;

	rc = cli_skill_collect_rows(ctx, &rows, &count, &active_count);
	if (rc != 0)
		return rc;
	rc = cli_skill_list_render(ctx, rows, count, active_count);
	cli_skill_rows_cleanup(rows, count);
	return rc;
}

static const char *cli_skill_status(const struct skill_entry *entry)
{
	if (!entry->enabled)
		return "× disabled";
	if (entry->activated)
		return CLI_FEED_DONE " active";
	return CLI_FEED_PENDING " available";
}

static int cli_skill_optional_field_count(const struct skill_entry *entry)
{
	int count = entry->fm.metadata_count > 0 ? 1 : 0;

	if (entry->fm.license[0])
		count++;
	if (entry->fm.compatibility[0])
		count++;
	if (entry->fm.allowed_tools[0])
		count++;
	return count;
}

static void cli_skill_print_metadata(struct cli_application *ctx, const struct skill_entry *entry,
				     int columns)
{
	cJSON *metadata = cJSON_CreateObject();
	char *json;

	if (!metadata)
		return;
	for (int i = 0; i < entry->fm.metadata_count; i++)
		cJSON_AddStringToObject(metadata, entry->fm.metadata[i].key,
					entry->fm.metadata[i].value);
	json = cJSON_PrintUnformatted(metadata);
	if (json) {
		cli_list_json_field(ctx, "Metadata", json, 1, columns);
		free(json);
	}
	cJSON_Delete(metadata);
}

static int cli_skill_print_info(struct cli_application *ctx, const char *name)
{
	struct skill_entry entry;
	int columns = cli_list_columns();
	int optional;

	if (runtime_skill_find_borrow(ctx->runtime, name, &entry) != 0) {
		CMD_ERROR(ctx, "skill not found: %s", name);
		return NB_ERR_NOT_FOUND;
	}
	CMD_HEADER(ctx, "Skill %s", entry.fm.name);
	cli_list_value_field(ctx, "Status", cli_skill_status(&entry), 0, 14, columns);
	cli_list_value_field(ctx, "Directory", entry.skill_dir, 0, 14, columns);
	optional = cli_skill_optional_field_count(&entry);
	cli_list_text_field(ctx, "Description", entry.fm.description, optional == 0, columns);
	if (entry.fm.license[0]) {
		optional--;
		cli_list_value_field(ctx, "License", entry.fm.license, optional == 0, 14, columns);
	}
	if (entry.fm.compatibility[0]) {
		optional--;
		cli_list_text_field(ctx, "Compatibility", entry.fm.compatibility, optional == 0,
				    columns);
	}
	if (entry.fm.allowed_tools[0]) {
		optional--;
		cli_list_text_field(ctx, "Allowed tools", entry.fm.allowed_tools, optional == 0,
				    columns);
	}
	if (entry.fm.metadata_count > 0)
		cli_skill_print_metadata(ctx, &entry, columns);
	return 0;
}

static int cli_skill_command_skill(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg_borrow(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		return cli_skill_print_list(ctx);
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /skill info <name>");
			return NB_ERR_INVAL;
		}
		return cli_skill_print_info(ctx, name);
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /skill activate <name>");
			return NB_ERR_INVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find_borrow(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR(ctx, "skill not found: %s", name);
			return NB_ERR_NOT_FOUND;
		}
		int changed = 0;
		int rc = runtime_skill_set_active(ctx->runtime, name, 1, &changed);
		if (rc == 0 && changed)
			CMD_OK(ctx, "skill '%s' activated", name);
		else if (rc < 0)
			CMD_ERROR(ctx, "failed to activate skill '%s': %s", name,
				  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		else
			CMD_OK(ctx, "skill '%s' already activated", name);
		return rc < 0 ? rc : 0;
	}
	if (sub && strcmp(sub, "deactivate") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /skill deactivate <name>");
			return NB_ERR_INVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find_borrow(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR(ctx, "skill not found: %s", name);
			return NB_ERR_NOT_FOUND;
		}
		(void)runtime_skill_set_active(ctx->runtime, name, 0, NULL);
		CMD_OK(ctx, "skill '%s' deactivated", name);
		return 0;
	}
	return cli_skill_print_list(ctx);
}

static const struct cli_command cli_skill_entries[] = {
	{"/skill", cli_skill_command_skill, "List or manage skills", "/skill list"},
	{"/sk", cli_skill_command_skill, "Alias for /skill", "/sk list"},
};

struct cli_command_group cli_skill_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_skill_entries,
		.count = (int)(sizeof(cli_skill_entries) / sizeof(cli_skill_entries[0])),
		.category = "Skills",
	};
}
