#include "cli/commands/registry.h"

#define SESSION_LIST_DEFAULT 20

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
	int rc = session_create(&ctx->database, name,
				ctx->config.models.text.model, &s);
	if (rc == 0) {
		ctx->current_session = s;
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
		cli_update_tool_runtime_context(ctx);
		ctx->session_auto_named = !auto_named;
		CMD_OK("created and switched to session: %s [%s]", name,
		       ctx->current_session.display_id);
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
		session_list(&ctx->database, &list, &count, 0, NULL);
		int picked = 0;
		for (int i = 0; i < count; i++) {
			if (list[i].id == ctx->current_session.id)
				continue;
			picked++;
			if (picked == idx) {
				s = list[i];
				rc = 0;
				break;
			}
		}
		free(list);
	} else {
		rc = session_get_by_name(&ctx->database, name, &s);
		if (rc < 0)
			rc = session_get_by_display_id(&ctx->database, name, &s);
		if (rc < 0) {
			char *end;
			errno = 0;
			long id = strtol(name, &end, 10);
			if (*end == '\0' && errno == 0)
				rc = session_get_by_id(&ctx->database,
						       (int64_t)id, &s);
		}
	}

	if (rc == 0) {
		ctx->current_session = s;
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
		cli_update_tool_runtime_context(ctx);
		ctx->session_auto_named = 1;
		CMD_OK("switched to session: %s", ctx->current_session.name);
	} else {
		CMD_ERROR("session not found: %s", name);
	}
	return rc;
}

#define SESSION_LIST_DEFAULT 20

static int cmd_list(struct cli_context *ctx, int argc, char **argv)
{
	int limit = SESSION_LIST_DEFAULT;
	const char *filter = NULL;
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
	session_list(&ctx->database, &list, &count, limit, filter);

	int total = session_count(&ctx->database);

	if (filter && filter[0]) {
		if (limit > 0 && count < total)
			printf(ANSI_BOLD ANSI_CYAN
			       "--- sessions (%d of %d, \""
			       ANSI_YELLOW "%s"
			       ANSI_CYAN "\")"
			       ANSI_RESET "\n",
			       count, total, filter);
		else
			printf(ANSI_BOLD ANSI_CYAN
			       "--- sessions (%d of %d, \""
			       ANSI_YELLOW "%s"
			       ANSI_CYAN "\")"
			       ANSI_RESET "\n",
			       count, total, filter);
	} else if (limit > 0 && count < total) {
		printf(ANSI_BOLD ANSI_CYAN
		       "--- sessions (%d of %d, use --all for more)"
		       ANSI_RESET "\n",
		       count, total);
	} else {
		printf(ANSI_BOLD ANSI_CYAN
		       "--- sessions (%d)"
		       ANSI_RESET "\n",
		       count);
	}

	printf("  ");
	print_padded("ID", 10); putchar(' ');
	print_padded("Name", 45); putchar(' ');
	print_padded("Model", 30); putchar(' ');
	printf("Tokens\n");
	printf("  ");
	print_padded("---", 10); putchar(' ');
	print_padded("---", 45); putchar(' ');
	print_padded("---", 30); putchar(' ');
	printf("---\n");
	for (int i = 0; i < count; i++) {
		int is_current = (list[i].id == ctx->current_session.id);
		const char *model = is_current ? ctx->config.models.text.model : list[i].model;
		printf("  ");
		if (is_current && cli_color_enabled())
			fputs(ANSI_GREEN, stdout);
		print_padded(list[i].display_id, 10);
		if (is_current && cli_color_enabled())
			fputs(ANSI_RESET, stdout);
		putchar(' ');
		print_padded(list[i].name, 45); putchar(' ');
		print_padded(model, 30); putchar(' ');
		printf("%lld\n", (long long)list[i].token_used);
	}
	free(list);
	return 0;
}

static int cmd_rename(struct cli_context *ctx, int argc, char **argv)
{
	const char *new_name = cli_cmd_arg(argc, argv, 1);
	if (!new_name) {
		CMD_ERROR("usage: /rename <new_name>");
		return -EINVAL;
	}
	int rc = session_rename(&ctx->database, ctx->current_session.id, new_name);
	if (rc == 0) {
		strncpy(ctx->current_session.name, new_name,
			sizeof(ctx->current_session.name) - 1);
		ctx->current_session.name[sizeof(ctx->current_session.name) - 1] = '\0';
		utf8_sanitize_inplace(ctx->current_session.name);
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
	if (session_get_by_name(&ctx->database, name, &s) == 0)
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
	if (id == ctx->current_session.id) {
		CMD_ERROR("cannot delete current session, switch first");
		return -EINVAL;
	}
	int rc = session_delete(&ctx->database, id);
	if (rc == 0)
		CMD_OK("deleted session: %s", name);
	else
		CMD_ERROR("failed to delete session");
	return rc;
}

static int cmd_history(struct cli_context *ctx, int argc, char **argv)
{
	int n = 20;
	int show_all = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "--from-db") == 0)
			show_all = 1;
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0]))
			n = atoi(argv[i]);
	}
	if (n <= 0)
		n = 20;
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					     ctx->current_session.id, &count);
	int show = show_all ? count : (n > count ? count : n);
	int skip = show_all ? 0 : (count - show);
	CMD_HEADER("%s (showing %s %d of %d)", ctx->current_session.name,
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
	message_free_list(msgs);
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
	{ "/history", cmd_history, "Show recent messages",              "/history [n|--all]" },
	{ "/hi",      cmd_history, "Alias for /history",                "/hi [n|--all]" },
};

int cli_register_session_commands(void)
{
	return cli_command_register_many(session_commands,
		(int)(sizeof(session_commands) / sizeof(session_commands[0])));
}
