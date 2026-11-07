#include "sapi/cli/commands/session.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/pages/history.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/services/session_history.h"

static int cli_session_command_new(struct cli_application *ctx, int argc, char **argv)
{
	char name_buf[256];
	const char *name = cli_cmd_arg_borrow(argc, argv, 1);
	int auto_named = 0;
	if (!name) {
		auto_named = 1;
		snprintf(name_buf, sizeof(name_buf), "new_%lld", (long long)time(NULL));
		name = name_buf;
	}
	struct session s;
	int rc = runtime_session_create_and_select(ctx->runtime, name, &s);
	if (rc == 0) {
		ctx->session_auto_named = !auto_named;
		CMD_OK(ctx, "created and switched to session: %s [%s]", name, s.display_id);
	} else {
		CMD_ERROR(ctx, "failed to create session: %s", name);
	}
	return rc;
}

static int cli_session_command_switch(struct cli_application *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg_borrow(argc, argv, 1);
	if (!name) {
		CMD_ERROR(ctx, "usage: /switch <name|id|^N>");
		return -EINVAL;
	}

	struct session s;
	int rc = -ENOENT;

	if (name[0] == '^') {
		int idx = 1;
		if (name[1] != '\0') {
			char *end;
			errno = 0;
			long n = strtol(name + 1, &end, 10);
			if (*end != '\0' || errno != 0 || n < 1) {
				CMD_ERROR(ctx, "session not found: %s", name);
				return -ENOENT;
			}
			idx = (int)n;
		}
		struct session *list = NULL;
		int count = 0;
		rc = runtime_session_list_query(ctx->runtime, &list, &count, 0, NULL);
		if (rc != 0)
			return rc;
		struct session current;
		(void)runtime_session_current(ctx->runtime, &current);
		int picked = 0;
		rc = -ENOENT;
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
		CMD_OK(ctx, "switched to session: %s", s.name);
		rc = cli_session_history_restore(ctx);
		if (rc)
			CMD_ERROR(ctx, "failed to restore session history: %s", morph_strerror(rc));
	} else {
		CMD_ERROR(ctx, "session not found: %s", name);
	}
	return rc;
}

static int cli_session_command_resume(struct cli_application *ctx, int argc, char **argv)
{
	if (argc > 1)
		return cli_session_command_switch(ctx, argc, argv);
	return cli_resume_show(ctx);
}

static int cli_session_command_rename(struct cli_application *ctx, int argc, char **argv)
{
	const char *new_name = cli_cmd_arg_borrow(argc, argv, 1);
	if (!new_name) {
		CMD_ERROR(ctx, "usage: /rename <new_name>");
		return -EINVAL;
	}
	int64_t current_id;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	int rc = runtime_session_rename_and_update(ctx->runtime, current_id, new_name);
	if (rc == 0) {
		CMD_OK(ctx, "session renamed to: %s", new_name);
	} else {
		CMD_ERROR(ctx, "failed to rename session");
	}
	return rc;
}

static int cli_session_command_delete(struct cli_application *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg_borrow(argc, argv, 1);
	if (!name) {
		CMD_ERROR(ctx, "usage: /delete <name|id>");
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
		CMD_ERROR(ctx, "session not found: %s", name);
		return -ENOENT;
	}
	int64_t current_id;
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	if (id == current_id) {
		CMD_ERROR(ctx, "cannot delete current session, switch first");
		return -EINVAL;
	}
	int rc = runtime_session_delete_and_update(ctx->runtime, id);
	if (rc == 0) {
		CMD_OK(ctx, "deleted session: %s", name);
	} else {
		CMD_ERROR(ctx, "failed to delete session");
	}
	return rc;
}

static int cli_session_history_export_items(struct model_history_item *items, const char *path)
{
	cJSON *array = cJSON_CreateArray();
	char *expanded;
	char *json;
	int rc;

	if (!array || !path) {
		cJSON_Delete(array);
		return -EINVAL;
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		cJSON *entry = cJSON_CreateObject();

		if (!entry) {
			cJSON_Delete(array);
			return -ENOMEM;
		}
		cJSON_AddNumberToObject(entry, "sequence_no", (double)item->sequence_no);
		cJSON_AddStringToObject(entry, "turn_id", item->turn_id ? item->turn_id : "");
		cJSON_AddStringToObject(entry, "kind", item->kind);
		cJSON_AddStringToObject(entry, "role", item->role);
		cJSON_AddStringToObject(entry, "content", item->content ? item->content : "");
		if (item->payload_json) {
			cJSON *payload = cJSON_Parse(item->payload_json);

			if (payload)
				cJSON_AddItemToObject(entry, "payload", payload);
		}
		cJSON_AddBoolToObject(entry, "active", item->active);
		cJSON_AddBoolToObject(entry, "truncated", item->truncated);
		cJSON_AddNumberToObject(entry, "token_count", item->token_count);
		cJSON_AddItemToArray(array, entry);
	}
	json = cJSON_Print(array);
	cJSON_Delete(array);
	if (!json)
		return -ENOMEM;
	expanded = file_expand_path_copy(path);
	if (!expanded) {
		free(json);
		return -ENOMEM;
	}
	rc = file_write_all(expanded, json, strlen(json));
	free(expanded);
	free(json);
	return rc;
}

static int cli_session_command_history(struct cli_application *ctx, int argc, char **argv)
{
	int n = 20;
	int show_all = 0;
	int show_model = 0;
	int diagnose = 0;
	int repair = 0;
	int show_messages = 0;
	int stored = 0;
	int last_request = 0;
	int64_t sequence = 0;
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
		else if (strcmp(argv[i], "--messages") == 0)
			show_messages = 1;
		else if (strcmp(argv[i], "--stored") == 0)
			stored = 1;
		else if (strcmp(argv[i], "--last-request") == 0)
			last_request = 1;
		else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
			char *end;
			const char *value = argv[++i];

			if (*value == '#')
				value++;
			errno = 0;
			sequence = strtoll(value, &end, 10);
			if (errno || end == value || *end || sequence <= 0) {
				CMD_ERROR(ctx, "history entry must be a positive sequence number");
				return -EINVAL;
			}
		} else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc)
			export_path = argv[++i];
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0])) {
			char *end;
			long value;

			errno = 0;
			value = strtol(argv[i], &end, 10);
			if (errno || *end || value <= 0 || value > INT_MAX) {
				CMD_ERROR(ctx, "history count must be a positive integer");
				return -EINVAL;
			}
			n = (int)value;
		} else {
			CMD_ERROR(ctx, "unknown or incomplete history option: %s", argv[i]);
			return -EINVAL;
		}
	}
	if (n <= 0)
		n = 20;
	if (diagnose || repair) {
		struct agent_history_diagnostic report = {0};
		int changed = 0;
		int rc = repair ? runtime_session_history_repair(ctx->runtime, &report, &changed)
				: runtime_session_history_diagnose(ctx->runtime, &report);

		if (rc != 0) {
			CMD_ERROR(ctx, "history %s failed: %s", repair ? "repair" : "diagnosis",
				  morph_strerror(rc));
			return rc;
		}
		printf(ctx,
		       "history: active=%d dangling=%d orphan=%d invalid=%d "
		       "token_mismatches=%d",
		       report.active_items, report.dangling_calls, report.orphan_results,
		       report.invalid_payloads, report.token_mismatches);
		if (repair)
			printf(ctx, " repaired=%d", changed);
		printf(ctx, "\n");
		return 0;
	}
	if (export_path) {
		int count = 0;
		struct model_history_item *items = runtime_session_model_history_current_copy(
			ctx->runtime, show_all ? 0 : 1, &count);
		int rc = cli_session_history_export_items(items, export_path);

		runtime_session_model_history_free(items);
		if (rc != 0) {
			CMD_ERROR(ctx, "history export failed: %s", morph_strerror(rc));
			return rc;
		}
		CMD_OK(ctx, "exported %d model history items to %s", count, export_path);
		return 0;
	}
	if (show_model) {
		int count = 0;
		struct model_history_item *items = runtime_session_model_history_current_copy(
			ctx->runtime, show_all ? 0 : 1, &count);
		struct model_history_item *cur = items;
		int show = show_all ? count : (n > count ? count : n);
		int skip = show_all ? 0 : count - show;

		CMD_HEADER(ctx, "%s model history (showing %s %d of %d)",
			   runtime_session_current_name_borrow(ctx->runtime),
			   show_all ? "all" : "active last", show, count);
		for (int i = 0; i < skip && cur; i++)
			cur = cur->next;
		while (cur) {
			printf(ctx, ANSI_DIM "[#%lld %s%s]" ANSI_RESET " ",
			       (long long)cur->sequence_no, cur->kind,
			       cur->active ? "" : " inactive");
			if (strcmp(cur->kind, "tool_result") == 0) {
				printf(ctx, "%s: %d tokens%s\n",
				       cur->tool_name[0] ? cur->tool_name : "tool",
				       cur->token_count, cur->truncated ? ", truncated" : "");
			} else {
				(void)cli_output_print_untrusted(ctx,
								 cur->content ? cur->content : "",
								 UTF8_TERMINAL_TEXT_MULTILINE);
				printf(ctx, "\n");
			}
			cur = cur->next;
		}
		runtime_session_model_history_free(items);
		return 0;
	}
	if (last_request)
		return cli_history_request_print(ctx);
	if (!show_messages) {
		cJSON *report = NULL;
		int rc = runtime_session_history_inspect_copy(ctx->runtime, &report);

		if (rc == 0 && sequence)
			rc = cli_history_entry_print(ctx, report, sequence, stored);
		else if (rc == 0 && argc == 1 &&
			 ctx->presentation_mode == CLI_PRESENT_INTERACTIVE &&
			 cli_setup_controls_available(stdin, stdout))
			rc = cli_history_view(ctx, report);
		else if (rc == 0)
			cli_history_print(ctx, report, n, show_all);
		cJSON_Delete(report);
		if (rc != 0)
			CMD_ERROR(ctx, "history inspection failed: %s", morph_strerror(rc));
		return rc;
	}
	int count = 0;
	struct message *msgs = runtime_session_messages_current_copy(ctx->runtime, &count);
	int show = show_all ? count : (n > count ? count : n);
	int skip = show_all ? 0 : (count - show);
	CMD_HEADER(ctx, "%s (showing %s %d of %d)",
		   runtime_session_current_name_borrow(ctx->runtime), show_all ? "all" : "last",
		   show, count);
	struct message *cur = msgs;
	for (int i = 0; i < skip && cur; i++)
		cur = cur->next;
	while (cur) {
		const char *role = cur->role;
		const char *label = (strcmp(role, "user") == 0)	       ? "You"
				    : (strcmp(role, "assistant") == 0) ? "AI"
								       : role;
		printf(ctx, ANSI_DIM "[%s]" ANSI_RESET " ", label);
		if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
			(void)cli_output_print_untrusted(ctx,
							 cur->content ? cur->content : "(empty)",
							 UTF8_TERMINAL_TEXT_MULTILINE);
		else if (strcmp(role, "assistant") == 0 && cur->content && *cur->content)
			cli_markdown_render_ansi_with_media(ctx, cur->content, cli_renderer_media,
							    NULL);
		else
			(void)cli_output_print_untrusted(ctx,
							 cur->content ? cur->content : "(empty)",
							 UTF8_TERMINAL_TEXT_MULTILINE);
		printf(ctx, "\n");
		cur = cur->next;
	}
	runtime_session_messages_free(msgs);
	return 0;
}

static const struct cli_command cli_session_entries[] = {
	{"/new", cli_session_command_new, "Create a new session", "/new [name]"},
	{"/n", cli_session_command_new, "Alias for /new", "/n [name]"},
	{"/resume", cli_session_command_resume, "Choose a session to resume", "/resume [name|id]"},
	{"/switch", cli_session_command_switch, "Switch to another session",
	 "/switch <name|id|^N>"},
	{"/s", cli_session_command_switch, "Alias for /switch", "/s <name|id|^N>"},
	{"/rename", cli_session_command_rename, "Rename current session", "/rename <new_name>"},
	{"/rn", cli_session_command_rename, "Alias for /rename", "/rn <new_name>"},
	{"/delete", cli_session_command_delete, "Delete a session", "/delete <name|id>"},
	{"/del", cli_session_command_delete, "Alias for /delete", "/del <name|id>"},
	{"/history", cli_session_command_history, "Inspect full history and context membership",
	 "/history [n|--all|--messages|--model|--last-request|--entry N "
	 "[--stored]|--diagnose|--repair|--export path]"},
	{"/hi", cli_session_command_history, "Alias for /history",
	 "/hi [n|--all|--messages|--model|--last-request|--entry N [--stored]]"},
};

struct cli_command_group cli_session_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_session_entries,
		.count = (int)(sizeof(cli_session_entries) / sizeof(cli_session_entries[0])),
		.category = "Sessions",
	};
}
