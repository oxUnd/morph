#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"
#include "util/array.h"

struct skill_list_item {
	int index;
	int enabled;
	int activated;
	char name[SKILL_NAME_MAX];
};

static int skill_item_compare(const void *left, const void *right)
{
	const struct skill_list_item *a = left;
	const struct skill_list_item *b = right;
	int state_a = !a->enabled ? 2 : (a->activated ? 0 : 1);
	int state_b = !b->enabled ? 2 : (b->activated ? 0 : 1);

	if (state_a != state_b)
		return state_a - state_b;
	return strcmp(a->name, b->name);
}

static int skill_collect(struct cli_context *ctx, morph_array_t *items,
			 int *active_count)
{
	int count = runtime_skill_count(ctx->runtime);
	int rc;

	*active_count = 0;
	rc = morph_array_init(items, (size_t)(count > 0 ? count : 1),
			      sizeof(struct skill_list_item));
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		struct skill_entry entry;
		struct skill_list_item *item;

		rc = runtime_skill_info(ctx->runtime, i, &entry);
		if (rc != 0)
			goto out;
		item = morph_array_push(items);
		if (!item) {
			rc = -ENOMEM;
			goto out;
		}
		memset(item, 0, sizeof(*item));
		item->index = i;
		item->enabled = entry.enabled;
		item->activated = entry.activated;
		strncpy(item->name, entry.fm.name, sizeof(item->name) - 1);
		if (entry.activated)
			(*active_count)++;
	}
	qsort(items->elts, items->nelts, items->size, skill_item_compare);
	return 0;
out:
	morph_array_cleanup(items);
	return rc;
}

static const char *skill_marker(const struct skill_list_item *item)
{
	if (!item->enabled)
		return ANSI_RED "×" ANSI_RESET;
	if (item->activated)
		return ANSI_GREEN "●" ANSI_RESET;
	return ANSI_DIM "○" ANSI_RESET;
}

static int skill_name_width(const morph_array_t *items)
{
	struct skill_list_item *item;
	int width = 12;

	morph_array_foreach(item, items, struct skill_list_item) {
		size_t item_width = utf8_display_width(item->name);

		if (item_width > (size_t)width)
			width = item_width > 28u ? 28 : (int)item_width;
	}
	return width;
}

static int skill_print_list(struct cli_context *ctx)
{
	morph_array_t items;
	struct skill_list_item *item;
	int active_count;
	int columns = cli_list_columns();
	int name_width;
	int index = 0;
	int rc;

	memset(&items, 0, sizeof(items));
	rc = skill_collect(ctx, &items, &active_count);
	if (rc != 0)
		return rc;
	CMD_HEADER("Skills %zu" ANSI_DIM " · %d active" ANSI_RESET,
		   items.nelts, active_count);
	if (items.nelts == 0) {
		printf("  " ANSI_DIM "(none — install to ~/.morph/skills/ "
		       "or ~/.agents/skills/)" ANSI_RESET "\n");
		goto out;
	}
	name_width = skill_name_width(&items);
	morph_array_foreach(item, &items, struct skill_list_item) {
		struct skill_entry entry;

		if (runtime_skill_info(ctx->runtime, item->index, &entry) != 0)
			continue;
		index++;
		cli_list_item("", index == (int)items.nelts,
			      skill_marker(item), entry.fm.name,
			      entry.fm.description, name_width, columns);
	}
	printf("\n  " ANSI_DIM "● active   ○ available   × disabled"
	       ANSI_RESET "\n");
out:
	morph_array_cleanup(&items);
	return 0;
}

static const char *skill_status(const struct skill_entry *entry)
{
	if (!entry->enabled)
		return "× disabled";
	if (entry->activated)
		return "● active";
	return "○ available";
}

static int skill_optional_field_count(const struct skill_entry *entry)
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

static void skill_print_metadata(const struct skill_entry *entry, int columns)
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
		cli_list_json_field("Metadata", json, 1, columns);
		free(json);
	}
	cJSON_Delete(metadata);
}

static int skill_print_info(struct cli_context *ctx, const char *name)
{
	struct skill_entry entry;
	int columns = cli_list_columns();
	int optional;

	if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
		CMD_ERROR("skill not found: %s", name);
		return -ENOENT;
	}
	CMD_HEADER("Skill %s", entry.fm.name);
	cli_list_value_field("Status", skill_status(&entry), 0, 14,
			     columns);
	cli_list_value_field("Directory", entry.skill_dir, 0, 14, columns);
	optional = skill_optional_field_count(&entry);
	cli_list_text_field("Description", entry.fm.description,
			    optional == 0, columns);
	if (entry.fm.license[0]) {
		optional--;
		cli_list_value_field("License", entry.fm.license,
				     optional == 0, 14, columns);
	}
	if (entry.fm.compatibility[0]) {
		optional--;
		cli_list_text_field("Compatibility", entry.fm.compatibility,
				    optional == 0, columns);
	}
	if (entry.fm.allowed_tools[0]) {
		optional--;
		cli_list_text_field("Allowed tools", entry.fm.allowed_tools,
				    optional == 0, columns);
	}
	if (entry.fm.metadata_count > 0)
		skill_print_metadata(&entry, columns);
	return 0;
}

static int cmd_skill(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		return skill_print_list(ctx);
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill info <name>");
			return -EINVAL;
		}
		return skill_print_info(ctx, name);
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill activate <name>");
			return -EINVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		int changed = 0;
		int rc = runtime_skill_set_active(ctx->runtime, name, 1, &changed);
		if (rc == 0 && changed)
			CMD_OK("skill '%s' activated", name);
		else if (rc < 0)
			CMD_ERROR("failed to activate skill '%s': %s", name, morph_strerror(rc));
		else
			CMD_OK("skill '%s' already activated", name);
		return rc < 0 ? rc : 0;
	}
	if (sub && strcmp(sub, "deactivate") == 0) {
		const char *name = cli_cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill deactivate <name>");
			return -EINVAL;
		}
		struct skill_entry entry;
		if (runtime_skill_find(ctx->runtime, name, &entry) != 0) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		(void)runtime_skill_set_active(ctx->runtime, name, 0, NULL);
		CMD_OK("skill '%s' deactivated", name);
		return 0;
	}
	return skill_print_list(ctx);
}


static const struct cli_command skill_commands[] = {
	{ "/skill",   cmd_skill,   "List or manage skills",             "/skill list" },
	{ "/sk",      cmd_skill,   "Alias for /skill",                  "/sk list" },
};

int cli_register_skill_commands(void)
{
	return cli_command_register_many(skill_commands,
					 (int)(sizeof(skill_commands) /
					 sizeof(skill_commands[0])),
					 "Skills");
}
