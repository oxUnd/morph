#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"
#include "sapi/cli/setup_ui.h"
#include "sapi/cli/history_ui.h"

#include "tui/components.h"

static int append_token_number(morph_buf_t *buf, int64_t tokens)
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
	return 0;
}

static int append_token_count(morph_buf_t *buf, int64_t tokens)
{
	int rc = append_token_number(buf, tokens);

	return rc == 0 ? morph_buf_puts(buf, " tokens") : rc;
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
		CMD_OK(ctx, "created and switched to session: %s [%s]", name, s.display_id);
	} else {
		CMD_ERROR(ctx, "failed to create session: %s", name);
	}
	return rc;
}

static int cmd_switch(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR(ctx, "usage: /switch <name|id|^N>");
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
				CMD_ERROR(ctx, "session not found: %s", name);
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
		CMD_OK(ctx, "switched to session: %s", s.name);
	} else {
		CMD_ERROR(ctx, "session not found: %s", name);
	}
	return rc;
}

#define RESUME_PAGE_SIZE 10

static int resume_page(struct cli_context *ctx, struct session *list, int count, int page,
		       int64_t current_id, int page_size)
{
	int start = page * page_size;
	int end = count - start > page_size ?
		start + page_size : count;

	CMD_HEADER(ctx, "resume · page %d/%d · %d sessions", page + 1, (count - 1) / page_size + 1,
		   count);
	for (int i = start; i < end; i++) {
		char number[16];
		morph_buf_t metadata;
		int rc = morph_buf_init(&metadata, 64);

		if (rc != 0)
			MORPH_RETURN(rc);
		rc = morph_buf_printf(&metadata, "%s · ", list[i].display_id);
		if (rc == 0)
			rc = append_token_count(&metadata, list[i].token_used);
		if (rc == 0)
			rc = append_session_time(&metadata, list[i].updated_at,
					 list[i].created_at);
		if (rc == 0) {
			snprintf(number, sizeof(number), "%d", i - start + 1);
			cli_list_row(ctx, number, list[i].name, morph_buf_cstr(&metadata),
				     list[i].id == current_id, i == end - 1, cli_list_columns());
		}
		morph_buf_cleanup(&metadata);
		if (rc != 0)
			MORPH_RETURN(rc);
	}
	return 0;
}

#define RESUME_NORMAL "\033[0;38;2;225;229;234;48;2;16;18;20m"
#define RESUME_MUTED "\033[0;38;2;142;150;161;48;2;16;18;20m"
#define RESUME_ACCENT "\033[0;38;2;71;211;230;48;2;16;18;20m"
#define RESUME_RULE "\033[0;38;2;48;57;65;48;2;16;18;20m"
#define RESUME_SELECTED "\033[0;38;2;239;244;248;48;2;29;48;56m"
#define RESUME_SELECTED_META "\033[0;38;2;180;194;203;48;2;29;48;56m"
#define RESUME_BAR "\033[0;38;2;71;211;230;48;2;29;48;56m"

struct resume_layout {
	int rows;
	int columns;
	int first_row;
	int page_size;
	int width;
	int name_width;
	int id_column;
	int tokens_column;
	int updated_column;
};

static struct resume_layout resume_dimensions(const struct tui_screen *screen)
{
	struct resume_layout layout = {.rows = screen->area.height,
		.columns = screen->area.width};
	struct tui_rect body = tui_rect_inset(screen->area, 2, 0);

	layout.width = body.width > 0 ? body.width : 1;
	layout.first_row = layout.rows >= 14 ? 8 : layout.rows >= 6 ? 3 :
		layout.rows >= 3 ? 2 : 1;
	layout.page_size = layout.rows >= 14 ? layout.rows - 12 :
		layout.rows >= 6 ? layout.rows - 5 : layout.rows >= 3 ?
		layout.rows - 2 : 1;
	layout.name_width = layout.width;
	if (layout.width >= 48) {
		layout.updated_column = 3 + layout.width - 14;
		layout.name_width -= 17;
	}
	if (layout.width >= 68) {
		layout.tokens_column = layout.updated_column - 15;
		layout.name_width -= 15;
	}
	if (layout.width >= 88) {
		layout.id_column = layout.tokens_column - 11;
		layout.name_width -= 11;
	}
	return layout;
}

/* All data passes through the single-line sanitizer before raw frame output. */
static void resume_cell(morph_buf_t *frame, const struct resume_layout *layout,
			int row, int column, int width, const char *text,
			const char *style, int right)
{
	char compact[BUFSIZ];

	cli_list_compact_text(compact, sizeof(compact), text);
	(void)tui_cell_append_aligned(frame, layout->columns, layout->rows,
		row, column, width, compact, style, right);
}

static void resume_rule(morph_buf_t *frame, int row, int width)
{
	morph_buf_printf(frame, "\033[%d;3H%s", row, RESUME_RULE);
	for (int i = 0; i < width; i++)
		morph_buf_puts(frame, "─");
}

static int resume_frame(morph_buf_t *frame, struct session *list, int count, int focus,
			const struct resume_layout *layout)
{
	int page = focus / layout->page_size;
	int start = page * layout->page_size;
	int end = count - start > layout->page_size ?
		start + layout->page_size : count;
	char label[128];
	morph_buf_t tokens;
	int rc = morph_buf_init(&tokens, 32);

	if (rc != 0)
		MORPH_RETURN(rc);
	morph_buf_reset(frame);
	morph_buf_printf(frame, "\033[?2026h%s\033[H\033[J", RESUME_NORMAL);
	if (layout->columns < 5) {
		resume_cell(frame, layout, 1, 1, layout->columns,
			list[focus].name, RESUME_SELECTED, 0);
		morph_buf_puts(frame, "\033[0m\033[?2026l");
		morph_buf_cleanup(&tokens);
		return tui_output_write(stdout, frame);
	}
	if (layout->rows >= 14) {
		resume_cell(frame, layout, 1, 3, 5, "morph", RESUME_ACCENT, 0);
		resume_cell(frame, layout, 1, 9, layout->width - 6, "/ sessions", RESUME_MUTED, 0);
		resume_cell(frame, layout, 3, 3, layout->width,
			"Resume a session", RESUME_NORMAL "\033[1m", 0);
		snprintf(label, sizeof(label), "%d sessions · sorted by recent activity", count);
		resume_cell(frame, layout, 4, 3, layout->width, label, RESUME_MUTED, 0);
		resume_rule(frame, 6, layout->width);
	} else if (layout->rows >= 3) {
		resume_cell(frame, layout, 1, 3, layout->width, "Sessions", RESUME_ACCENT, 0);
	}
	if (layout->rows >= 3 && layout->width >= 28) {
		snprintf(label, sizeof(label), "%d / %d", page + 1,
			 (count - 1) / layout->page_size + 1);
		int width = (int)strlen(label);

		resume_cell(frame, layout, layout->rows >= 14 ? 3 : 1, 3 + layout->width - width,
			    width, label, RESUME_MUTED, 1);
	}
	if (layout->rows >= 6) {
		int row = layout->first_row - 1;

		resume_cell(frame, layout, row, 3, layout->name_width, "SESSION", RESUME_MUTED, 0);
		if (layout->id_column)
			resume_cell(frame, layout, row, layout->id_column, 8,
				"ID", RESUME_MUTED, 0);
		if (layout->tokens_column)
			resume_cell(frame, layout, row, layout->tokens_column, 12,
				"TOKENS", RESUME_MUTED, 1);
		if (layout->updated_column)
			resume_cell(frame, layout, row, layout->updated_column, 14,
				"UPDATED", RESUME_MUTED, 0);
	}
	for (int i = start; i < end; i++) {
		int row = layout->first_row + i - start;
		int selected = i == focus;
		const char *text = selected ? RESUME_SELECTED "\033[1m" : RESUME_NORMAL;
		const char *metadata = selected ? RESUME_SELECTED_META : RESUME_MUTED;

		resume_cell(frame, layout, row, 3, layout->width, "", text, 0);
		if (selected && layout->columns > 4)
			resume_cell(frame, layout, row, 2, 1, "▏", RESUME_BAR, 0);
		resume_cell(frame, layout, row, 3, layout->name_width, list[i].name, text, 0);
		if (layout->id_column)
			resume_cell(frame, layout, row, layout->id_column, 8,
				list[i].display_id, metadata, 0);
		if (layout->tokens_column) {
			morph_buf_reset(&tokens);
			rc = append_token_number(&tokens, list[i].token_used);
			if (rc != 0)
				break;
			resume_cell(frame, layout, row, layout->tokens_column, 12,
				    morph_buf_cstr(&tokens), metadata, 1);
		}
		if (layout->updated_column) {
			morph_buf_reset(&tokens);
			rc = append_session_time(&tokens, list[i].updated_at, list[i].created_at);
			if (rc != 0)
				break;
			resume_cell(frame, layout, row, layout->updated_column, 14,
				    morph_buf_cstr(&tokens) + strlen(" · "), metadata, 0);
		}
	}
	if (layout->rows >= 6) {
		snprintf(label, sizeof(label), "Showing %d–%d of %d", start + 1, end, count);
		if (layout->rows >= 14) resume_rule(frame, layout->rows - 4, layout->width);
		resume_cell(frame, layout, layout->rows - 3 + (layout->rows < 14), 3,
			    layout->width, label, RESUME_MUTED, 0);
		resume_rule(frame, layout->rows - 1, layout->width);
	}
	if (layout->rows >= 3) {
		const char *hint = layout->width >= 68 ?
			"↑ ↓  Select    Enter  Resume    ← →  Page" :
			layout->width >= 36 ? "↑↓ Select  Enter Resume  Esc Cancel" :
			"↑↓ Enter Esc";

		resume_cell(frame, layout, layout->rows, 3, layout->width, hint, RESUME_MUTED, 0);
		if (layout->width >= 68)
			resume_cell(frame, layout, layout->rows, 3 + layout->width - 10, 10,
				    "Esc Cancel", RESUME_NORMAL, 1);
	}
	morph_buf_puts(frame, "\033[0m\033[?2026l");
	if (rc == 0) rc = tui_output_write(stdout, frame);
	morph_buf_cleanup(&tokens);
	return rc;
}

struct resume_picker {
	struct session *sessions;
	int count;
	int *selected;
	struct tui_menu menu;
	struct resume_layout layout;
	morph_buf_t frame;
};

static int resume_draw(void *data, const struct tui_screen *screen)
{
	struct resume_picker *picker = data;

	picker->layout = resume_dimensions(screen);
	tui_list_layout(&picker->menu.list, (size_t)picker->count,
		(size_t)picker->layout.page_size);
	return resume_frame(&picker->frame, picker->sessions, picker->count,
		(int)picker->menu.list.index, &picker->layout);
}

static int resume_update(void *data, const struct tui_event *event)
{
	struct resume_picker *picker = data;

	if (event->type != TUI_EVENT_KEY || event->pasting)
		return TUI_SCREEN_CONTINUE;
	int key = event->key;

	if (key == 'q') return TUI_SCREEN_CLOSE;
	if (key == 'n') key = TUI_KEY_PAGE_DOWN;
	if (key == 'p') key = TUI_KEY_PAGE_UP;
	int result = tui_menu_update(&picker->menu, key);

	if (result == TUI_SUBMIT) {
		*picker->selected = (int)picker->menu.list.index;
		return TUI_SCREEN_CLOSE;
	}
	if (result == TUI_BACK || result == TUI_CANCEL)
		return TUI_SCREEN_CLOSE;
	return result == TUI_CHANGED ? TUI_SCREEN_REDRAW : TUI_SCREEN_CONTINUE;
}

/* Own a temporary screen so navigation never fills the transcript. */
static int resume_pick(struct session *list, int count, int *selected)
{
	struct resume_picker picker = {.sessions = list, .count = count,
		.selected = selected};
	struct tui_screen screen = {0};
	const struct tui_screen_ops ops = {.update = resume_update, .draw = resume_draw};
	int rc = morph_buf_init(&picker.frame, BUFSIZ);

	*selected = -1;
	picker.menu.list.flags = TUI_LIST_VIM | TUI_LIST_TAB |
		TUI_LIST_HORIZONTAL_PAGES | TUI_LIST_PAGED;
	if (rc == 0)
		rc = tui_screen_open(&screen, stdin, stdout,
			TUI_SESSION_ALTERNATE | TUI_SESSION_PASTE);
	if (rc == 0) rc = tui_screen_run(&screen, &ops, &picker);
	int leave_rc = tui_screen_close(&screen);

	if (rc == 0) rc = leave_rc;
	morph_buf_cleanup(&picker.frame);
	MORPH_RETURN(rc);
}

static int cmd_resume(struct cli_context *ctx, int argc, char **argv)
{
	struct session *list = NULL;
	struct session selected;
	int64_t current_id = 0;
	int count = 0;
	int page = 0;
	int rc;

	if (argc > 1)
		return cmd_switch(ctx, argc, argv);
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		CMD_ERROR(ctx, "use /resume <name|id> in JSON mode");
		MORPH_RETURN(-EINVAL);
	}
	rc = runtime_session_list_query(ctx->runtime, &list, &count, 0, NULL);
	if (rc != 0)
		MORPH_RETURN(rc);
	(void)runtime_session_current_id(ctx->runtime, &current_id);
	if (count == 0)
		CMD_OK(ctx, "no sessions to resume");
	if (count > 0 && cli_setup_ui_available(stdin, stdout)) {
		int index;

		rc = resume_pick(list, count, &index);
		if (rc == 0 && index >= 0) {
			rc = runtime_session_select_existing(ctx->runtime,
				list[index].id, &selected);
			if (rc == 0) {
				ctx->session_auto_named = 1;
				CMD_OK(ctx, "resumed session: %s", selected.name);
			} else {
				CMD_ERROR(ctx, "failed to resume session: %s", morph_strerror(rc));
			}
		}
		runtime_session_list_free(list);
		return rc;
	}
	while (count > 0) {
		char answer[BUFSIZ];
		char *end;
		long number;

		rc = resume_page(ctx, list, count, page, current_id, RESUME_PAGE_SIZE);
		if (rc != 0)
			break;
		printf(ctx, "  Select 1-%d · n next · p previous · q cancel: ",
		       count - page * RESUME_PAGE_SIZE > RESUME_PAGE_SIZE
			       ? RESUME_PAGE_SIZE
			       : count - page * RESUME_PAGE_SIZE);
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin))
			break;
		if (!strchr(answer, '\n') && !feof(stdin)) {
			int ch;

			while ((ch = getchar()) != '\n' && ch != EOF) {}
			CMD_ERROR(ctx, "invalid selection");
			continue;
		}
		answer[strcspn(answer, "\r\n")] = '\0';
		if (!answer[0] || strcmp(answer, "q") == 0)
			break;
		if (strcmp(answer, "n") == 0) {
			if (page < (count - 1) / RESUME_PAGE_SIZE)
				page++;
			else
				CMD_ERROR(ctx, "already on the last page");
			continue;
		}
		if (strcmp(answer, "p") == 0) {
			if (page > 0)
				page--;
			else
				CMD_ERROR(ctx, "already on the first page");
			continue;
		}
		errno = 0;
		number = strtol(answer, &end, 10);
		if (errno || end == answer || *end || number < 1 ||
		    number > RESUME_PAGE_SIZE ||
		    number > count - page * RESUME_PAGE_SIZE) {
			CMD_ERROR(ctx, "invalid selection");
			continue;
		}
		rc = runtime_session_select_existing(ctx->runtime,
			list[page * RESUME_PAGE_SIZE + (int)number - 1].id,
			&selected);
		if (rc == 0) {
			ctx->session_auto_named = 1;
			CMD_OK(ctx, "resumed session: %s", selected.name);
		} else {
			CMD_ERROR(ctx, "failed to resume session: %s", morph_strerror(rc));
		}
		break;
	}
	runtime_session_list_free(list);
	return rc;
}

static int cmd_rename(struct cli_context *ctx, int argc, char **argv)
{
	const char *new_name = cli_cmd_arg(argc, argv, 1);
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

static int cmd_delete(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
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

			if (*value == '#') value++;
			errno = 0;
			sequence = strtoll(value, &end, 10);
			if (errno || end == value || *end || sequence <= 0) {
				CMD_ERROR(ctx, "history entry must be a positive sequence number");
				MORPH_RETURN(-EINVAL);
			}
		}
		else if (strcmp(argv[i], "--export") == 0 && i + 1 < argc)
			export_path = argv[++i];
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0])) {
			char *end;
			long value;

			errno = 0;
			value = strtol(argv[i], &end, 10);
			if (errno || *end || value <= 0 || value > INT_MAX) {
				CMD_ERROR(ctx, "history count must be a positive integer");
				MORPH_RETURN(-EINVAL);
			}
			n = (int)value;
		}
		else {
			CMD_ERROR(ctx, "unknown or incomplete history option: %s", argv[i]);
			MORPH_RETURN(-EINVAL);
		}
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
		struct model_history_item *items =
			runtime_session_model_history_current(ctx->runtime,
				show_all ? 0 : 1, &count);
		int rc = history_export_items(items, export_path);

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
		struct model_history_item *items =
			runtime_session_model_history_current(ctx->runtime,
				show_all ? 0 : 1, &count);
		struct model_history_item *cur = items;
		int show = show_all ? count : (n > count ? count : n);
		int skip = show_all ? 0 : count - show;

		CMD_HEADER(ctx, "%s model history (showing %s %d of %d)",
			   runtime_session_current_name(ctx->runtime),
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
				(void)cli_print_untrusted_text(ctx,
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
		int rc = runtime_session_history_inspect(ctx->runtime, &report);

		if (rc == 0 && sequence)
			rc = cli_history_entry_print(ctx, report, sequence, stored);
		else if (rc == 0 && argc == 1 &&
			 ctx->presentation_mode == CLI_PRESENT_INTERACTIVE &&
			 cli_setup_ui_available(stdin, stdout))
			rc = cli_history_view(ctx, report);
		else if (rc == 0)
			cli_history_print(ctx, report, n, show_all);
		cJSON_Delete(report);
		if (rc != 0)
			CMD_ERROR(ctx, "history inspection failed: %s", morph_strerror(rc));
		return rc;
	}
	int count = 0;
	struct message *msgs = runtime_session_messages_current(ctx->runtime, &count);
	int show = show_all ? count : (n > count ? count : n);
	int skip = show_all ? 0 : (count - show);
	CMD_HEADER(ctx, "%s (showing %s %d of %d)", runtime_session_current_name(ctx->runtime),
		   show_all ? "all" : "last", show, count);
	struct message *cur = msgs;
	for (int i = 0; i < skip && cur; i++)
		cur = cur->next;
	while (cur) {
		const char *role = cur->role;
		const char *label = (strcmp(role, "user") == 0) ? "You" :
				   (strcmp(role, "assistant") == 0) ? "AI" : role;
		printf(ctx, ANSI_DIM "[%s]" ANSI_RESET " ", label);
		if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
			(void)cli_print_untrusted_text(ctx, cur->content ? cur->content : "(empty)",
						       UTF8_TERMINAL_TEXT_MULTILINE);
		else if (strcmp(role, "assistant") == 0 && cur->content && *cur->content)
			cli_markdown_render_ansi_with_media(ctx, cur->content, media_callback,
							    NULL);
		else
			(void)cli_print_untrusted_text(ctx, cur->content ? cur->content : "(empty)",
						       UTF8_TERMINAL_TEXT_MULTILINE);
		printf(ctx, "\n");
		cur = cur->next;
	}
	runtime_session_messages_free(msgs);
	return 0;
}


static const struct cli_command session_commands[] = {
	{ "/new",     cmd_new,     "Create a new session",              "/new [name]" },
	{ "/n",       cmd_new,     "Alias for /new",                    "/n [name]" },
	{ "/resume",  cmd_resume,  "Choose a session to resume",         "/resume [name|id]" },
	{ "/switch",  cmd_switch,  "Switch to another session",         "/switch <name|id|^N>" },
	{ "/s",       cmd_switch,  "Alias for /switch",                 "/s <name|id|^N>" },
	{ "/rename",  cmd_rename,  "Rename current session",            "/rename <new_name>" },
	{ "/rn",      cmd_rename,  "Alias for /rename",                 "/rn <new_name>" },
	{ "/delete",  cmd_delete,  "Delete a session",                  "/delete <name|id>" },
	{ "/del",     cmd_delete,  "Alias for /delete",                 "/del <name|id>" },
	{ "/history", cmd_history, "Inspect full history and context membership", "/history [n|--all|--messages|--model|--last-request|--entry N [--stored]|--diagnose|--repair|--export path]" },
	{ "/hi",      cmd_history, "Alias for /history",                "/hi [n|--all|--messages|--model|--last-request|--entry N [--stored]]" },
};

struct cli_command_group cli_session_commands(void)
{
	return (struct cli_command_group){
		.entries = session_commands,
		.count = (int)(sizeof(session_commands) / sizeof(session_commands[0])),
		.category = "Sessions",
	};
}
