#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"

/* Built-in command groups are immutable; lookup requires no registration. */
static struct cli_command_group (*const command_groups[])(void) = {
	cli_runtime_commands, cli_session_commands, cli_memory_commands,
	cli_task_commands, cli_media_commands, cli_ext_commands,
	cli_skill_commands, cli_sync_commands, cli_permission_commands,
	cli_hook_commands, cli_share_commands, cli_mcp_commands, cli_job_commands,
};

static const struct cli_command *command_at(int index, const char **category)
{
	for (size_t i = 0; i < sizeof(command_groups) / sizeof(command_groups[0]); i++) {
		struct cli_command_group group = command_groups[i]();

		if (index < group.count) {
			if (category)
				*category = group.category;
			return &group.entries[index];
		}
		index -= group.count;
	}
	return NULL;
}

static int command_event_emit(struct cli_context *ctx, const char *name,
			      const char *phase, const char *input,
			      int argc, char **argv, const char *output,
			      int command_rc)
{
	cJSON *data;
	cJSON *args;
	struct morph_event event;
	int rc;

	if (!ctx || !name || !phase || argc < 1 || !argv || !argv[0])
		MORPH_RETURN(-EINVAL);
	data = cJSON_CreateObject();
	args = cJSON_CreateArray();
	if (!data || !args) {
		cJSON_Delete(data);
		cJSON_Delete(args);
		MORPH_RETURN(-ENOMEM);
	}
	if (!cJSON_AddStringToObject(data, "command", argv[0]) ||
	    !cJSON_AddStringToObject(data, "input", input ? input : "")) {
		cJSON_Delete(args);
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	for (int i = 1; i < argc; i++) {
		cJSON *arg = cJSON_CreateString(argv[i]);

		if (!arg) {
			cJSON_Delete(args);
			cJSON_Delete(data);
			MORPH_RETURN(-ENOMEM);
		}
		cJSON_AddItemToArray(args, arg);
	}
	cJSON_AddItemToObject(data, "args", args);
	if (output && !cJSON_AddStringToObject(data, "output", output)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (command_rc < 0 &&
	    (!cJSON_AddNumberToObject(data, "error_code", command_rc) ||
	     !cJSON_AddStringToObject(data, "error",
				      morph_strerror(command_rc)))) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	event = (struct morph_event){
		.type = MORPH_EVENT_COMMAND,
		.name = name,
		.phase = phase,
		.message = command_rc < 0 ? morph_strerror(command_rc) : NULL,
		.data = data,
		.turn_id = NULL,
	};
	rc = morph_event_emit(ctx->event_cb, ctx->event_user_data, &event);
	cJSON_Delete(data);
	return rc;
}

const struct cli_command *cli_command_find(const char *name)
{
	const struct cli_command *command;

	if (!name)
		return NULL;
	for (int i = 0; (command = command_at(i, NULL)); i++) {
		if (strcmp(command->name, name) == 0)
			return command;
	}
	return NULL;
}

const struct cli_command *cli_command_find_input(const char *input)
{
	const struct cli_command *command;

	if (!input)
		return NULL;
	while (isspace((unsigned char)*input)) input++;
	for (int i = 0; (command = command_at(i, NULL)); i++) {
		size_t length = strlen(command->name);

		if (strncmp(input, command->name, length) == 0 &&
		    (!input[length] || isspace((unsigned char)input[length])))
			return command;
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
	morph_buf_t output;
	int argc;
	int capture_active = 0;
	int event_rc;
	int rc;
	const struct cli_command *e;

	if (!input)
		MORPH_RETURN(-EINVAL);
	snprintf(buf, sizeof(buf), "%s", input);
	argc = cli_argv_split(buf, argv, 32);
	if (argc < 1)
		MORPH_RETURN(-EINVAL);
	if (ctx && ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		event_rc = command_event_emit(ctx, "command.started", "begin",
			input, argc, argv, NULL, 0);
		if (event_rc != 0)
			MORPH_RETURN(event_rc);
		rc = morph_buf_init(&output, BUFSIZ);
		if (rc != 0)
			MORPH_RETURN(rc);
		rc = cli_command_capture_begin(ctx, &output);
		if (rc != 0) {
			morph_buf_cleanup(&output);
			MORPH_RETURN(rc);
		}
		capture_active = 1;
	}
	e = cli_command_find(argv[0]);
	if (!e) {
		CMD_ERROR(ctx, "unknown command: %s. Try /help", argv[0]);
		rc = -ENOENT;
	} else {
		rc = e->handler(ctx, argc, argv);
	}
	if (!capture_active)
		return rc;
	cli_command_capture_end(ctx);
	event_rc = command_event_emit(ctx,
		rc < 0 ? "command.failed" : "command.completed",
		rc < 0 ? "failed" : "end", input, argc, argv,
		morph_buf_cstr(&output), rc);
	morph_buf_cleanup(&output);
	if (event_rc != 0)
		MORPH_RETURN(event_rc);
	return rc;
}

void cli_print_help(struct cli_context *ctx)
{
	int group_start = 0;
	int num_commands = 0;
	int columns = cli_list_columns();

	while (command_at(num_commands, NULL))
		num_commands++;

	printf(ctx, "\n" ANSI_BOLD ANSI_CYAN "Commands" ANSI_RESET "\n");
	while (group_start < num_commands) {
		const char *category = NULL;
		int group_end = group_start;
		int item_count = 0;
		int group_last;

		(void)command_at(group_start, &category);
		while (group_end < num_commands) {
			const char *next_category = NULL;

			(void)command_at(group_end, &next_category);
			if ((category == NULL) != (next_category == NULL) ||
			    (category && strcmp(category, next_category) != 0))
				break;
			group_end++;
		}
		for (int i = group_start; i < group_end; ) {
			int alias_end = i;
			while (alias_end + 1 < group_end &&
			       command_at(alias_end + 1, NULL)->handler ==
			       command_at(i, NULL)->handler)
				alias_end++;
			item_count++;
			i = alias_end + 1;
		}
		group_last = group_end == num_commands;
		cli_list_group(ctx, category ? category : "Other", item_count, group_last);
		for (int i = group_start, item = 0; i < group_end; ) {
			const char *desc = command_at(i, NULL)->desc;
			int alias_end = i;
			char display[128];
			while (alias_end + 1 < group_end &&
			       command_at(alias_end + 1, NULL)->handler ==
			       command_at(i, NULL)->handler)
				alias_end++;
			if (alias_end > i && strncmp(desc, "Alias for", 9) == 0)
				desc = command_at(alias_end, NULL)->desc;
			if (alias_end > i) {
				const char *pri = command_at(i, NULL)->name;
				const char *alt = command_at(i + 1, NULL)->name;
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
				snprintf(display, sizeof(display), "%s", command_at(i, NULL)->name);
			}
			item++;
			cli_list_item(ctx, morph_tui_tree_branch(group_last, 1), item == item_count, "",
				      display, desc, 22, columns);
			i = alias_end + 1;
		}
		group_start = group_end;
	}
	printf(ctx, "\n" ANSI_DIM "Use /help <command> for details." ANSI_RESET "\n");
}

char *cli_command_completion_generator(const char *text, int state)
{
	const struct cli_command *command;
	size_t length;
	int match = 0;

	if (!text || state < 0)
		return NULL;
	length = strlen(text);
	for (int i = 0; (command = command_at(i, NULL)); i++) {
		if (strncmp(command->name, text, length) == 0 && match++ == state)
			return strdup(command->name);
	}
	return NULL;
}
