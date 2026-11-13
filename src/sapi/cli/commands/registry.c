#include "sapi/cli/commands/registry.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/commands/ext.h"
#include "sapi/cli/commands/hooks.h"
#include "sapi/cli/commands/jobs.h"
#include "sapi/cli/commands/agents.h"
#include "sapi/cli/commands/mcp.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/commands/memory.h"
#include "sapi/cli/commands/permissions.h"
#include "sapi/cli/commands/runtime.h"
#include "sapi/cli/commands/rewind.h"
#include "sapi/cli/commands/session.h"
#include "sapi/cli/commands/share.h"
#include "sapi/cli/commands/skill.h"
#include "sapi/cli/commands/sync.h"
#include "sapi/cli/commands/tasks.h"
#include "sapi/cli/application_internal.h"
#include "cJSON.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/output.h"
#include "util/array.h"
#include "util/cleanup.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Built-in command groups are immutable; lookup requires no registration. */
static struct cli_command_group (*const command_groups[])(void) = {
	cli_runtime_commands, cli_session_commands,    cli_rewind_commands, cli_memory_commands,
	cli_task_commands,    cli_media_commands,      cli_ext_commands,    cli_skill_commands,
	cli_sync_commands,    cli_permission_commands, cli_hook_commands,   cli_share_commands,
	cli_mcp_commands,     cli_job_commands, cli_agent_commands,
};

static const struct cli_command *cli_registry_command_at(int index, const char **category)
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

static int cli_registry_command_event_emit(struct cli_application *ctx, const char *name,
					   const char *phase, const char *input, int argc,
					   char **argv, const char *output, int command_rc)
{
	cJSON *data;
	cJSON *args;
	struct morph_event event;
	int rc;

	if (!ctx || !name || !phase || argc < 1 || !argv || !argv[0])
		return -EINVAL;
	data = cJSON_CreateObject();
	args = cJSON_CreateArray();
	if (!data || !args) {
		cJSON_Delete(data);
		cJSON_Delete(args);
		return -ENOMEM;
	}
	if (!cJSON_AddStringToObject(data, "command", argv[0]) ||
	    !cJSON_AddStringToObject(data, "input", input ? input : "")) {
		cJSON_Delete(args);
		cJSON_Delete(data);
		return -ENOMEM;
	}
	for (int i = 1; i < argc; i++) {
		cJSON *arg = cJSON_CreateString(argv[i]);

		if (!arg) {
			cJSON_Delete(args);
			cJSON_Delete(data);
			return -ENOMEM;
		}
		cJSON_AddItemToArray(args, arg);
	}
	cJSON_AddItemToObject(data, "args", args);
	if (output && !cJSON_AddStringToObject(data, "output", output)) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	if (command_rc < 0 &&
	    (!cJSON_AddNumberToObject(data, "error_code", command_rc) ||
	     !cJSON_AddStringToObject(data, "error", morph_strerror(command_rc)))) {
		cJSON_Delete(data);
		return -ENOMEM;
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

const struct cli_command *cli_command_find_borrow(const char *name)
{
	const struct cli_command *command;

	if (!name)
		return NULL;
	for (int i = 0; (command = cli_registry_command_at(i, NULL)); i++) {
		if (strcmp(command->name, name) == 0)
			return command;
	}
	return NULL;
}

const struct cli_command *cli_command_find_input_borrow(const char *input)
{
	const struct cli_command *command;

	if (!input)
		return NULL;
	while (isspace((unsigned char)*input))
		input++;
	for (int i = 0; (command = cli_registry_command_at(i, NULL)); i++) {
		size_t length = strlen(command->name);

		if (strncmp(input, command->name, length) == 0 &&
		    (!input[length] || isspace((unsigned char)input[length])))
			return command;
	}
	return NULL;
}

static char *cli_registry_argv_next(char **cursor)
{
	char *position = *cursor;
	char *argument;

	while (*position && isspace((unsigned char)*position))
		position++;
	if (!*position) {
		*cursor = position;
		return NULL;
	}
	if (*position == '"' || *position == '\'') {
		char quote = *position++;
		argument = position;
		while (*position && *position != quote)
			position++;
	} else {
		argument = position;
		while (*position && !isspace((unsigned char)*position))
			position++;
	}
	if (*position)
		*position++ = '\0';
	*cursor = position;
	return argument;
}

int cli_argv_split(const char *input, char **argv, int max_args)
{
	char *cursor = (char *)input;
	char *argument;
	int count = 0;

	if (!input || !argv || max_args < 1)
		return -EINVAL;
	while ((argument = cli_registry_argv_next(&cursor))) {
		if (count >= max_args - 1) {
			argv[count] = NULL;
			return -E2BIG;
		}
		argv[count++] = argument;
	}
	argv[count] = NULL;
	return count;
}

static int cli_registry_arguments_create(char *input, morph_array_t *arguments)
{
	char *cursor = input;
	char *argument;
	int rc = morph_array_init(arguments, MORPH_ARRAY_INIT_CAP, sizeof(argument));

	if (rc != 0)
		return rc;
	while ((argument = cli_registry_argv_next(&cursor))) {
		if (arguments->nelts >= INT_MAX)
			return -E2BIG;
		char **slot = morph_array_push_borrow(arguments);
		if (!slot)
			return -ENOMEM;
		*slot = argument;
	}
	char **last = morph_array_push_borrow(arguments);
	if (!last)
		return -ENOMEM;
	*last = NULL;
	return 0;
}

const char *cli_cmd_arg_borrow(int argc, char **argv, int idx)
{
	return (idx >= 0 && idx < argc) ? argv[idx] : NULL;
}

int cli_command_dispatch(struct cli_application *ctx, const char *input)
{
	char *buf MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_array_t arguments MORPH_CLEANUP(morph_array_cleanup) = {0};
	char **argv;
	morph_buf_t output;
	int argc;
	int capture_active = 0;
	int event_rc;
	int rc;
	const struct cli_command *e;

	if (!input)
		return -EINVAL;
	buf = strdup(input);
	if (!buf)
		return -ENOMEM;
	rc = cli_registry_arguments_create(buf, &arguments);
	if (rc != 0)
		return rc;
	argv = arguments.elts;
	argc = (int)(arguments.nelts - 1);
	if (argc < 1)
		return -EINVAL;
	if (ctx && ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		event_rc = cli_registry_command_event_emit(ctx, "command.started", "begin", input,
							   argc, argv, NULL, 0);
		if (event_rc != 0)
			return event_rc;
		rc = morph_buf_init(&output, BUFSIZ);
		if (rc != 0)
			return rc;
		rc = cli_output_capture_begin(ctx, &output);
		if (rc != 0) {
			morph_buf_cleanup(&output);
			return rc;
		}
		capture_active = 1;
	}
	e = cli_command_find_borrow(argv[0]);
	if (!e) {
		CMD_ERROR(ctx, "unknown command: %s. Try /help", argv[0]);
		rc = -ENOENT;
	} else {
		rc = e->handler(ctx, argc, argv);
	}
	if (!capture_active)
		return rc;
	cli_output_capture_end(ctx);
	event_rc = cli_registry_command_event_emit(
		ctx, rc < 0 ? "command.failed" : "command.completed", rc < 0 ? "failed" : "end",
		input, argc, argv, morph_buf_cstr_borrow(&output), rc);
	morph_buf_cleanup(&output);
	if (event_rc != 0)
		return event_rc;
	return rc;
}

void cli_print_help(struct cli_application *ctx)
{
	int group_start = 0;
	int num_commands = 0;

	while (cli_registry_command_at(num_commands, NULL))
		num_commands++;

	cli_help_begin(ctx);
	while (group_start < num_commands) {
		const char *category = NULL;
		int group_end = group_start;
		morph_array_t items MORPH_CLEANUP(morph_array_cleanup) = {0};
		int rc;

		(void)cli_registry_command_at(group_start, &category);
		while (group_end < num_commands) {
			const char *next_category = NULL;

			(void)cli_registry_command_at(group_end, &next_category);
			if ((category == NULL) != (next_category == NULL) ||
			    (category && strcmp(category, next_category) != 0))
				break;
			group_end++;
		}
		rc = morph_array_init(&items, (size_t)(group_end - group_start),
				      sizeof(struct cli_help_item));
		if (rc != 0) {
			cli_help_end(ctx);
			return;
		}
		for (int i = group_start; i < group_end;) {
			const char *desc = cli_registry_command_at(i, NULL)->desc;
			const char *alias = NULL;
			int alias_end = i;
			struct cli_help_item *item;

			while (alias_end + 1 < group_end &&
			       cli_registry_command_at(alias_end + 1, NULL)->handler ==
				       cli_registry_command_at(i, NULL)->handler)
				alias_end++;
			if (alias_end > i && strncmp(desc, "Alias for", 9) == 0)
				desc = cli_registry_command_at(alias_end, NULL)->desc;
			if (alias_end > i)
				alias = cli_registry_command_at(i + 1, NULL)->name;
			item = morph_array_push_borrow(&items);
			if (!item) {
				cli_help_end(ctx);
				return;
			}
			item->name = cli_registry_command_at(i, NULL)->name;
			item->alias = alias;
			item->description = desc;
			i = alias_end + 1;
		}
		cli_help_group(ctx, category ? category : "Other", items.elts, (int)items.nelts,
			       group_end == num_commands);
		group_start = group_end;
	}
	cli_help_end(ctx);
}

char *cli_command_completion_generator(const char *text, int state)
{
	const struct cli_command *command;
	size_t length;
	int match = 0;

	if (!text || state < 0)
		return NULL;
	length = strlen(text);
	for (int i = 0; (command = cli_registry_command_at(i, NULL)); i++) {
		if (strncmp(command->name, text, length) == 0 && match++ == state)
			return strdup(command->name);
	}
	return NULL;
}
