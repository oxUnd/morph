#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"

#define SESSION_LIST_DEFAULT 20

static int append_token_count(morph_buf_t *buf, int64_t tokens)
{
	char raw[32];
	const char *digits;
	size_t length;
	int rc;

	snprintf(raw, sizeof(raw), "%lld", (long long)tokens);
	digits = raw;
	if (*digits == '-') {
		rc = morph_buf_putc(buf, *digits++);
		if (rc != 0)
			return rc;
	}
	length = strlen(digits);
	for (size_t i = 0; i < length; i++) {
		if (i > 0 && (length - i) % 3 == 0) {
			rc = morph_buf_putc(buf, ',');
			if (rc != 0)
				return rc;
		}
		rc = morph_buf_putc(buf, digits[i]);
		if (rc != 0)
			return rc;
	}
	return morph_buf_puts(buf, " tokens");
}

static int append_session_time(morph_buf_t *buf, int64_t updated_at,
			       int64_t created_at)
{
	char formatted[16];
	struct tm local;
	time_t timestamp;

	timestamp = (time_t)(updated_at > 0 ? updated_at : created_at);
	if (timestamp <= 0 || !localtime_r(&timestamp, &local) ||
	    strftime(formatted, sizeof(formatted), "%m-%d %H:%M",
		     &local) == 0)
		return morph_buf_puts(buf, " · -- --:--");
	return morph_buf_printf(buf, " · %s", formatted);
}

static int cmd_new(struct cli_context *ctx, int argc, char **argv)
{
	char name_buf[256];
	const char *name = cli_cmd_arg(argc, argv, 1);
	int auto_named = 0;
	if (!name) {
		auto_named = 1;
		snprintf(name_buf, sizeof(name_buf), "new_%lld",
			 (long long)time(NULL));
		name = name_buf;
	}
	struct session s;
	int rc = runtime_session_create_and_select(ctx->runtime, name, &s);
	if (rc == 0) {
		ctx->session_auto_named = !auto_named;
		CMD_OK("created and switched to session: %s [%s]", name,
		       s.display_id);
	} else {
		CMD_ERROR("failed to create session: %s", name);
	}
	return rc;
}

static int cmd_switch(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR("usage: /switch <name|id|^N>");
		return -EINVAL;
	}

	struct session s;
	int rc = -1;

	if (name[0] == '^') {
		int idx = 1;
		if (name[1] != '\0') {
			char *end;
			errno = 0;
			long n = strtol(name + 1, &end, 10);
			if (*end != '\0' || errno != 0 || n < 1) {
				CMD_ERROR("session not found: %s", name);
				return -ENOENT;
			}
			idx = (int)n;
		}
		struct session *list;
		int count = 0;
		rc = runtime_session_list_query(ctx->runtime, &list, &count, 0,
						NULL);
		if (rc != 0)
			return rc;
		struct session current;
		(void)runtime_session_current(ctx->runtime, &current);
		int picked = 0;
		for (int i = 0; i < count; i++) {
			if (list[i].id == current.id)
				continue;
			picked++;
			if (picked == idx) {
				s = list[i];
				rc = 0;
				break;
			}
		}
		runtime_session_list_free(list);
	} else {
		rc = runtime_session_find_ref(ctx->runtime, name, &s);
	}

	if (rc == 0) {
		rc = runtime_session_select_existing(ctx->runtime, s.id, &s);
		if (rc != 0)
			return rc;
		ctx->session_auto_named = 1;
		CMD_OK("switched to session: %s", s.name);
	} else {
		CMD_ERROR("session not found: %s", name);
	}
	return rc;
}

static int cmd_list(struct cli_context *ctx, int argc, char **argv)
{
	int limit = SESSION_LIST_DEFAULT;
	const char *filter = NULL;
	int truncated;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0)
			limit = 0;
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0]))
			limit = atoi(argv[i]);
		else
			filter = argv[i];
	}
	if (limit < 0)
		limit = 0;

	struct session *list;
	int count = 0;
	int rc = runtime_session_list_query(ctx->runtime, &list, &count, limit,
					    filter);
	if (rc != 0)
		return rc;

	int total = runtime_session_count_all(ctx->runtime);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	const struct config *config = runtime_config_get(ctx->runtime);
	truncated = limit > 0 &&
		((filter && filter[0]) ? count == limit : count < total);
	if (filter && filter[0])
		CMD_HEADER("sessions (%d shown, filter \"" ANSI_YELLOW
			   "%s" ANSI_CYAN "\")", count, filter);
	else if (count < total)
		CMD_HEADER("sessions (%d of %d)", count, total);
	else
		CMD_HEADER("sessions (%d)", count);
	if (count == 0) {
		printf("  " ANSI_DIM "└ no sessions" ANSI_RESET "\n");
		runtime_session_list_free(list);
		return 0;
	}
	for (int i = 0; i < count; i++) {
		int is_current = (list[i].id == current.id);
		const char *model = is_current ?
			config->models.text.model : list[i].model;
		morph_buf_t metadata;

		rc = morph_buf_init(&metadata, 64);
		if (rc != 0) {
			runtime_session_list_free(list);
			return rc;
		}
		rc = append_token_count(&metadata, list[i].token_used);
		if (rc == 0 && model && model[0] &&
		    strcmp(model, config->models.text.model) != 0)
			rc = morph_buf_printf(&metadata, " · %s", model);
		if (rc == 0)
			rc = append_session_time(&metadata, list[i].updated_at,
						 list[i].created_at);
		if (rc != 0) {
			morph_buf_cleanup(&metadata);
			runtime_session_list_free(list);
			return rc;
		}
		cli_list_row(list[i].display_id, list[i].name,
			     morph_buf_cstr(&metadata), is_current,
			     i == count - 1 && !truncated,
			     cli_list_columns());
		morph_buf_cleanup(&metadata);
	}
	if (truncated) {
		printf("  " ANSI_DIM "└ … limited to %d · "
		       "use /list --all%s%s" ANSI_RESET "\n",
		       limit, filter && filter[0] ? " " : "",
		       filter && filter[0] ? filter : "");
	}
	runtime_session_list_free(list);
	return 0;
}

static int cmd_rename(struct cli_context *ctx, int argc, char **argv)
{
	const char *new_name = cli_cmd_arg(argc, argv, 1);
	if (!new_name) {
		CMD_ERROR("usage: /rename <new_name>");
		return -EINVAL;
	}
	int64_t current_id;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	int rc = runtime_session_rename_and_update(ctx->runtime, current_id, new_name);
	if (rc == 0) {
		CMD_OK("session renamed to: %s", new_name);
	} else {
		CMD_ERROR("failed to rename session");
	}
	return rc;
}

static int cmd_delete(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR("usage: /delete <name|id>");
		return -EINVAL;
	}
	int64_t id = -1;
	struct session s;
	if (runtime_session_find_ref(ctx->runtime, name, &s) == 0)
		id = s.id;
	else {
		char *end;
		errno = 0;
		id = strtol(name, &end, 10);
		if (*end != '\0' || errno != 0)
			id = -1;
	}
	if (id < 0) {
		CMD_ERROR("session not found: %s", name);
		return -ENOENT;
	}
	int64_t current_id;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	if (id == current_id) {
		CMD_ERROR("cannot delete current session, switch first");
		return -EINVAL;
	}
	int rc = runtime_session_delete_and_update(ctx->runtime, id);
	if (rc == 0) {
		CMD_OK("deleted session: %s", name);
	} else {
		CMD_ERROR("failed to delete session");
	}
	return rc;
}

static int history_export_items(struct model_history_item *items,
				const char *path)
{
	cJSON *array = cJSON_CreateArray();
	char *expanded;
	char *json;
	int rc;

	if (!array || !path) {
		cJSON_Delete(array);
		MORPH_RETURN(-EINVAL);
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		cJSON *entry = cJSON_CreateObject();

		if (!entry) {
			cJSON_Delete(array);
			MORPH_RETURN(-ENOMEM);
		}
		cJSON_AddNumberToObject(entry, "sequence_no",
			(double)item->sequence_no);
		cJSON_AddStringToObject(entry, "turn_id",
			item->turn_id ? item->turn_id : "");
		cJSON_AddStringToObject(entry, "kind", item->kind);
		cJSON_AddStringToObject(entry, "role", item->role);
		cJSON_AddStringToObject(entry, "content",
			item->content ? item->content : "");
		if (item->payload_json) {
			cJSON *payload = cJSON_Parse(item->payload_json);

			if (payload)
				cJSON_AddItemToObject(entry, "payload", payload);
		}
		cJSON_AddBoolToObject(entry, "active", item->active);
		cJSON_AddBoolToObject(entry, "truncated", item->truncated);
		cJSON_AddNumberToObject(entry, "token_count",
			item->token_count);
		cJSON_AddItemToArray(array, entry);
	}
	json = cJSON_Print(array);
	cJSON_Delete(array);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	expanded = file_expand_path(path);
	if (!expanded) {
		free(json);
		MORPH_RETURN(-ENOMEM);
	}
	rc = file_write_all(expanded, json, strlen(json));
	free(expanded);
	free(json);
	return rc;
}

static int cmd_history(struct cli_context *ctx, int argc, char **argv)
{
	int n = 20;
	int show_all = 0;
	int show_model = 0;
	int diagnose = 0;
	int repair = 0;
	const char *export_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "--from-db") == 0)
			show_all = 1;
		else if (strcmp(argv[i], "--model") == 0)
			show_model = 1;
		else if (strcmp(argv[i], "--diagnose") == 0)
			diagnose = 1;
		else if (strcmp(argv[i], "--repair") == 0)
			repair = 1;
		else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc)
			export_path = argv[++i];
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0]))
			n = atoi(argv[i]);
	}
	if (n <= 0)
		n = 20;
	if (diagnose || repair) {
		struct agent_history_diagnostic report = {0};
		int changed = 0;
		int rc = repair ? runtime_session_history_repair(ctx->runtime,
			&report, &changed) :
			runtime_session_history_diagnose(ctx->runtime, &report);

		if (rc != 0) {
			CMD_ERROR("history %s failed: %s",
				repair ? "repair" : "diagnosis",
				morph_strerror(rc));
			return rc;
		}
		printf("history: active=%d dangling=%d orphan=%d invalid=%d "
		       "token_mismatches=%d",
		       report.active_items, report.dangling_calls,
		       report.orphan_results, report.invalid_payloads,
		       report.token_mismatches);
		if (repair)
			printf(" repaired=%d", changed);
		printf("\n");
		return 0;
	}
	if (export_path) {
		int count = 0;
		struct model_history_item *items =
			runtime_session_model_history_current(ctx->runtime,
				show_all ? 0 : 1, &count);
		int rc = history_export_items(items, export_path);

		runtime_session_model_history_free(items);
		if (rc != 0) {
			CMD_ERROR("history export failed: %s", morph_strerror(rc));
			return rc;
		}
		CMD_OK("exported %d model history items to %s", count,
		       export_path);
		return 0;
	}
	if (show_model) {
		int count = 0;
		struct model_history_item *items =
			runtime_session_model_history_current(ctx->runtime,
				show_all ? 0 : 1, &count);
		struct model_history_item *cur = items;
		int show = show_all ? count : (n > count ? count : n);
		int skip = show_all ? 0 : count - show;

		CMD_HEADER("%s model history (showing %s %d of %d)",
			runtime_session_current_name(ctx->runtime),
			show_all ? "all" : "active last", show, count);
		for (int i = 0; i < skip && cur; i++)
			cur = cur->next;
		while (cur) {
			printf(ANSI_DIM "[#%lld %s%s]" ANSI_RESET " ",
				(long long)cur->sequence_no, cur->kind,
				cur->active ? "" : " inactive");
			if (strcmp(cur->kind, "tool_result") == 0) {
				printf("%s: %d tokens%s\n",
					cur->tool_name[0] ? cur->tool_name : "tool",
					cur->token_count,
					cur->truncated ? ", truncated" : "");
			} else {
				printf("%s\n", cur->content ? cur->content : "");
			}
			cur = cur->next;
		}
		runtime_session_model_history_free(items);
		return 0;
	}
	int count = 0;
	struct message *msgs = runtime_session_messages_current(ctx->runtime, &count);
	int show = show_all ? count : (n > count ? count : n);
	int skip = show_all ? 0 : (count - show);
	CMD_HEADER("%s (showing %s %d of %d)", runtime_session_current_name(ctx->runtime),
		   show_all ? "all" : "last", show, count);
	struct message *cur = msgs;
	for (int i = 0; i < skip && cur; i++)
		cur = cur->next;
	while (cur) {
		const char *role = cur->role;
		const char *label = (strcmp(role, "user") == 0) ? "You" :
				   (strcmp(role, "assistant") == 0) ? "AI" : role;
		printf(ANSI_DIM "[%s]" ANSI_RESET " ", label);
		if (strcmp(role, "assistant") == 0 && cur->content && *cur->content)
			cli_markdown_render_ansi_with_media(cur->content,
							    media_callback,
							    NULL);
		else
			printf("%s\n", cur->content ? cur->content : "(empty)");
		cur = cur->next;
	}
	runtime_session_messages_free(msgs);
	return 0;
}


static const struct cli_command session_commands[] = {
	{ "/new",     cmd_new,     "Create a new session",              "/new [name]" },
	{ "/n",       cmd_new,     "Alias for /new",                    "/n [name]" },
	{ "/switch",  cmd_switch,  "Switch to another session",         "/switch <name|id|^N>" },
	{ "/s",       cmd_switch,  "Alias for /switch",                 "/s <name|id|^N>" },
	{ "/list",    cmd_list,    "List sessions",                     "/list [n|query|--all]" },
	{ "/ls",      cmd_list,    "Alias for /list",                   "/ls [n|query|--all]" },
	{ "/rename",  cmd_rename,  "Rename current session",            "/rename <new_name>" },
	{ "/rn",      cmd_rename,  "Alias for /rename",                 "/rn <new_name>" },
	{ "/delete",  cmd_delete,  "Delete a session",                  "/delete <name|id>" },
	{ "/del",     cmd_delete,  "Alias for /delete",                 "/del <name|id>" },
	{ "/history", cmd_history, "Show, diagnose, repair, or export history", "/history [n|--all] [--model|--diagnose|--repair|--export path]" },
	{ "/hi",      cmd_history, "Alias for /history",                "/hi [n|--all] [--model|--diagnose|--repair|--export path]" },
};

int cli_register_session_commands(void)
{
	return cli_command_register_many(session_commands,
					 (int)(sizeof(session_commands) /
					 sizeof(session_commands[0])),
					 "Sessions");
}
