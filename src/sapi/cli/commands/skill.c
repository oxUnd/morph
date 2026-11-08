#include "sapi/cli/commands/skill.h"
#include "util/cleanup.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"
#include "util/array.h"

struct cli_skill_list_item {
	int index;
	int enabled;
	int activated;
	char name[SKILL_NAME_MAX];
};

static int cli_skill_item_compare(const void *left, const void *right)
{
	const struct cli_skill_list_item *a = left;
	const struct cli_skill_list_item *b = right;
	int state_a = !a->enabled ? 2 : (a->activated ? 0 : 1);
	int state_b = !b->enabled ? 2 : (b->activated ? 0 : 1);

	if (state_a != state_b)
		return state_a - state_b;
	return strcmp(a->name, b->name);
}

static int cli_skill_collect(struct cli_application *ctx, morph_array_t *items, int *active_count)
{
	morph_array_t collected MORPH_CLEANUP(morph_array_cleanup) = {0};
	int count = runtime_skill_count(ctx->runtime);
	int rc;

	*active_count = 0;
	rc = morph_array_init(&collected, (size_t)(count > 0 ? count : 1),
			      sizeof(struct cli_skill_list_item));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		struct skill_entry entry;
		struct cli_skill_list_item *item;

		rc = runtime_skill_info_borrow(ctx->runtime, i, &entry);
		if (rc != 0)
			return rc;
		item = morph_array_push_borrow(&collected);
		if (!item) {
			rc = -ENOMEM;
			return rc;
		}
		memset(item, 0, sizeof(*item));
		item->index = i;
		item->enabled = entry.enabled;
		item->activated = entry.activated;
		strncpy(item->name, entry.fm.name, sizeof(item->name) - 1);
		if (entry.activated)
			(*active_count)++;
	}
	qsort(collected.elts, collected.nelts, collected.size, cli_skill_item_compare);
	return morph_array_take(items, &collected);
}

static int cli_skill_render_list(struct cli_application *ctx,
	const morph_array_t *items, struct morph_tui_widget_tree *tree, int columns)
{
	morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct cli_skill_list_item *item;
	int rc;

	rc = morph_buf_init(&output, 1024);
	if (rc != 0)
		return rc;
	morph_array_foreach (item, items, struct cli_skill_list_item) {
		struct skill_entry entry;
		morph_buf_t label MORPH_CLEANUP(morph_buf_cleanup) = {0};

		if (runtime_skill_info_borrow(ctx->runtime, item->index, &entry) != 0)
			continue;
		rc = morph_buf_init(&label, 128);
		if (rc != 0)
			break;
		/* Markers are data here; terminal styles stay out of the model. */
		const char *marker =
			!item->enabled ? "×" : (item->activated ? CLI_FEED_DONE : CLI_FEED_PENDING);
		morph_buf_printf(&label, "%s %s", marker, entry.fm.name);
		if (label.failed)
			rc = label.failed;
		else
			morph_tui_widget_tree_add(tree, MORPH_TUI_WIDGET_TREE_ROOT,
						  morph_buf_cstr_borrow(&label),
						  entry.fm.description);
		if (rc != 0)
			break;
	}
	if (rc == 0)
		rc = morph_tui_widget_tree_render(tree, &output, columns, 0, SIZE_MAX);
	if (rc == 0)
		fputs(ctx, morph_buf_cstr_borrow(&output), stdout);
	return rc;
}

static int cli_skill_print_list(struct cli_application *ctx)
{
	morph_array_t items MORPH_CLEANUP(morph_array_cleanup) = {0};
	int active_count;
	int columns = cli_list_columns();
	struct morph_tui_widget_tree tree
		MORPH_CLEANUP(morph_tui_widget_tree_cleanup) = {0};
	int rc;

	rc = cli_skill_collect(ctx, &items, &active_count);
	if (rc != 0)
		return rc;
	CMD_HEADER(ctx, "Skills %zu" ANSI_DIM " · %d active" ANSI_RESET, items.nelts, active_count);
	if (items.nelts == 0) {
		printf(ctx, "  " ANSI_DIM "(none — install to ~/.morph/skills/ "
			    "or ~/.agents/skills/)" ANSI_RESET "\n");
		return rc;
	}
	rc = morph_tui_widget_tree_init(&tree);
	if (rc != 0)
		return rc;
	rc = cli_skill_render_list(ctx, &items, &tree, columns);
	printf(ctx, "\n  " ANSI_DIM CLI_FEED_DONE " active   " CLI_FEED_PENDING
		    " available   × disabled" ANSI_RESET "\n");
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
		return -ENOENT;
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
			return -EINVAL;
		}
		return cli_skill_print_info(ctx, name);
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /skill activate <name>");
			return -EINVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find_borrow(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR(ctx, "skill not found: %s", name);
			return -ENOENT;
		}
		int changed = 0;
		int rc = runtime_skill_set_active(ctx->runtime, name, 1, &changed);
		if (rc == 0 && changed)
			CMD_OK(ctx, "skill '%s' activated", name);
		else if (rc < 0)
			CMD_ERROR(ctx, "failed to activate skill '%s': %s", name,
				  morph_strerror(rc));
		else
			CMD_OK(ctx, "skill '%s' already activated", name);
		return rc < 0 ? rc : 0;
	}
	if (sub && strcmp(sub, "deactivate") == 0) {
		const char *name = cli_cmd_arg_borrow(argc, argv, 2);
		if (!name) {
			CMD_ERROR(ctx, "usage: /skill deactivate <name>");
			return -EINVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find_borrow(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR(ctx, "skill not found: %s", name);
			return -ENOENT;
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
