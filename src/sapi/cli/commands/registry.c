#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"

#define CLI_COMMAND_MAX 128

static struct cli_command commands[CLI_COMMAND_MAX];
static const char *command_categories[CLI_COMMAND_MAX];
static int num_commands;

void cli_command_registry_clear(void)
{
	num_commands = 0;
}

int cli_command_register_many(const struct cli_command *entries, int count,
			      const char *category)
{
	if (!entries || count < 0)
		MORPH_RETURN(-EINVAL);
	if (num_commands + count > CLI_COMMAND_MAX)
		MORPH_RETURN(-ENOSPC);
	for (int i = 0; i < count; i++) {
		commands[num_commands++] = entries[i];
		command_categories[num_commands - 1] = category;
	}
	return 0;
}

const struct cli_command *cli_command_find(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < num_commands; i++) {
		if (strcmp(commands[i].name, name) == 0)
			return &commands[i];
	}
	return NULL;
}

int cli_argv_split(const char *input, char **argv, int max_args)
{
	char *p = (char *)input;
	int argc = 0;

	if (!input || !argv || max_args < 1)
		return 0;
	while (*p && argc < max_args - 1) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (*p == '"' || *p == '\'') {
			char quote = *p++;
			argv[argc++] = p;
			while (*p && *p != quote)
				p++;
			if (*p)
				*p++ = '\0';
		} else {
			argv[argc++] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			if (*p)
				*p++ = '\0';
		}
	}
	argv[argc] = NULL;
	return argc;
}

const char *cli_cmd_arg(int argc, char **argv, int idx)
{
	return (idx >= 0 && idx < argc) ? argv[idx] : NULL;
}

int cli_command_dispatch(struct cli_context *ctx, const char *input)
{
	char buf[8192];
	char *argv[32];
	int argc;
	const struct cli_command *e;

	if (!input)
		MORPH_RETURN(-EINVAL);
	snprintf(buf, sizeof(buf), "%s", input);
	argc = cli_argv_split(buf, argv, 32);
	if (argc < 1)
		return -EINVAL;
	e = cli_command_find(argv[0]);
	if (!e) {
		CMD_ERROR("unknown command: %s. Try /help", argv[0]);
		return -ENOENT;
	}
	return e->handler(ctx, argc, argv);
}

void cli_print_help(void)
{
	int group_start = 0;
	int columns = cli_list_columns();

	printf("\n" ANSI_BOLD ANSI_CYAN "Commands" ANSI_RESET "\n");
	while (group_start < num_commands) {
		const char *category = command_categories[group_start];
		int group_end = group_start;
		int item_count = 0;
		int group_last;

		while (group_end < num_commands &&
		       ((category == NULL && command_categories[group_end] == NULL) ||
			(category != NULL && command_categories[group_end] != NULL &&
			 strcmp(category, command_categories[group_end]) == 0)))
			group_end++;
		for (int i = group_start; i < group_end; ) {
			int alias_end = i;
			while (alias_end + 1 < group_end &&
			       commands[alias_end + 1].handler == commands[i].handler)
				alias_end++;
			item_count++;
			i = alias_end + 1;
		}
		group_last = group_end == num_commands;
		cli_list_group(category ? category : "Other", item_count,
			       group_last);
		for (int i = group_start, item = 0; i < group_end; ) {
			const char *desc = commands[i].desc;
			int alias_end = i;
			char display[128];
			while (alias_end + 1 < group_end &&
			       commands[alias_end + 1].handler == commands[i].handler)
				alias_end++;
			if (alias_end > i && strncmp(desc, "Alias for", 9) == 0)
				desc = commands[alias_end].desc;
			if (alias_end > i) {
				const char *pri = commands[i].name;
				const char *alt = commands[i + 1].name;
				const char *shorter = strlen(pri) <= strlen(alt) ? pri : alt;
				const char *longer = strlen(pri) <= strlen(alt) ? alt : pri;
				const char *s = shorter + 1;
				const char *l = longer + 1;
				size_t slen = strlen(s);
				if (strncmp(s, l, slen) == 0)
					snprintf(display, sizeof(display), "/%s[%s]", s,
						 l + slen);
				else
					snprintf(display, sizeof(display), "%s,%s", shorter,
						 longer);
			} else {
				snprintf(display, sizeof(display), "%s", commands[i].name);
			}
			item++;
			cli_list_item(group_last ? "  " : "│ ", item == item_count,
				      "", display, desc, 22, columns);
			i = alias_end + 1;
		}
		group_start = group_end;
	}
	printf("\n" ANSI_DIM "Use /help <command> for details."
	       ANSI_RESET "\n");
}

char *cli_command_completion_generator(const char *text, int state)
{
	static int idx;
	static int len;

	if (state == 0) {
		idx = 0;
		len = (int)strlen(text);
	}
	while (idx < num_commands) {
		const char *name = commands[idx].name;
		idx++;
		if (strncmp(name, text, (size_t)len) == 0)
			return strdup(name);
	}
	return NULL;
}

int cli_commands_init(void)
{
	int rc;

	cli_command_registry_clear();
	rc = cli_register_runtime_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_session_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_memory_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_task_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_media_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_ext_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_skill_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_sync_commands();
	if (rc != 0)
		return rc;
	rc = cli_register_permission_commands();
	if (rc != 0)
		return rc;
	return cli_register_mcp_commands();
}
