#include <morph/tui/components.h>
#include "setup_ui.h"
#include "setup_mcp.h"
#include "cli.h"
#include "util/error.h"
#include "util/utf8.h"
#include "util/name.h"
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SETUP_BACK (-EAGAIN)

enum setup_phase {
	SETUP_ROLE,
	SETUP_ACCOUNT,
	SETUP_PREFERENCES,
	SETUP_START
};

enum setup_action {
	SETUP_SAVE,
	SETUP_CHANGE_ROLE = SETUP_MODEL_COUNT + 1,
	SETUP_INSTRUCTIONS,
	SETUP_SAVE_LATER,
	SETUP_CANCEL,
	SETUP_ACTION_COUNT
};

struct setup_ui {
	volatile sig_atomic_t interrupted;
	struct morph_signal_listener signals;
	struct morph_tui_screen screen;
	FILE *output;
	int fd;
	int output_fd;
	int color;
	int step;
	enum setup_phase phase;
	int columns;
	int rows;
	int active_lines;
	int frame_lines;
	int cursor_up;
	int deferred[SETUP_MODEL_COUNT];
	int role_confirmed;
	struct config_model_entry *entries;
	struct cli_setup_state *state;
};

int cli_setup_ui_available(FILE *input, FILE *output)
{
	return morph_tui_available(input, output);
}

int cli_setup_print(FILE *output, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (!isatty(fileno(output))) {
		int written = vfprintf(output, format, args);
		va_end(args);
		return written < 0 ? -EIO : 0;
	}
	morph_buf_t text = {0};
	morph_buf_t rendered = {0};
	struct morph_tui_text_view view = {0};
	int rc = morph_buf_init(&text, 0);
	if (rc == 0) rc = morph_buf_vprintf(&text, format, args);
	va_end(args);
	char *safe = rc == 0 ? utf8_terminal_sanitize_dup(morph_buf_cstr(&text), text.len,
		UTF8_TERMINAL_TEXT_MULTILINE, NULL) : NULL;
	if (rc == 0 && !safe) rc = -ENOMEM;
	int columns, rows;
	morph_tui_dimensions(fileno(output), &columns, &rows);
	size_t width = columns > 4 ? (size_t)columns - 4 : 1;
	if (rc == 0) rc = morph_tui_text_view_wrap(&view, safe, width);
	if (rc == 0) rc = morph_buf_init(&rendered, 0);
	for (size_t i = 0; rc == 0 && i < view.lines.nelts; i++) {
		const struct morph_tui_text_line *row = morph_array_get(&view.lines, i);
		if (row->length) rc = morph_buf_puts(&rendered, "  ");
		if (rc == 0) rc = morph_buf_append(&rendered, safe + row->offset, row->length);
		if (rc == 0) rc = morph_buf_puts(&rendered, "\r\n");
	}
	if (rc == 0) rc = morph_tui_output_write(output, &rendered);
	morph_tui_text_view_cleanup(&view);
	morph_buf_cleanup(&rendered);
	morph_buf_cleanup(&text);
	free(safe);
	return rc;
}

static void setup_signal(int number, void *opaque)
{
	struct setup_ui *ui = opaque;

	if (number != SIGWINCH)
		morph_signal_flag_set(&ui->interrupted, number);
}

static int read_key(struct setup_ui *ui)
{
	for (;;) {
		struct morph_tui_event event;
		int rc = morph_tui_screen_poll(&ui->screen, &event, 100);

		if (morph_signal_flag_get(&ui->interrupted))
			return -ECANCELED;
		if (rc == -EINTR)
			continue;
		if (rc != 0)
			return rc;
		if (event.type == MORPH_TUI_EVENT_EOF)
			return -ECANCELED;
		if (event.type == MORPH_TUI_EVENT_RESIZE)
			return 0;
		if (event.type == MORPH_TUI_EVENT_KEY)
			return event.key;
	}
}

static void dimensions(struct setup_ui *ui)
{
	morph_tui_dimensions(ui->output_fd, &ui->columns, &ui->rows);
}

/* The cursor normally rests on the blank line below the active prompt.
 * Only these owned lines are redrawn; completed answers stay in scrollback. */
static void clear_active(struct setup_ui *ui, morph_buf_t *frame)
{
	int old_columns = ui->columns;
	int old_rows = ui->rows;
	dimensions(ui);
	morph_buf_puts(frame, "\033[?2026h");
	if (ui->active_lines &&
	    (old_columns != ui->columns || old_rows != ui->rows ||
	     ui->active_lines >= old_rows)) {
		/* A terminal may reflow old lines on resize. Do not guess their
		 * new positions and risk erasing history; append a fresh prompt. */
		morph_buf_puts(frame, "\033[999B\r\n");
	} else if (ui->active_lines) {
		morph_buf_putc(frame, '\r');
		if (ui->cursor_up)
			morph_buf_printf(frame, "\033[%dB", ui->cursor_up);
		morph_buf_printf(frame, "\033[%dA", ui->active_lines);
		for (int i = 0; i < ui->active_lines; i++)
			morph_buf_puts(frame, "\033[2K\r\n");
		morph_buf_printf(frame, "\033[%dA\r", ui->active_lines);
	}
	ui->active_lines = 0;
	ui->cursor_up = 0;
	ui->frame_lines = 0;
}

static void line(struct setup_ui *ui, morph_buf_t *frame,
		 const char *style, const char *text)
{
	char clipped[BUFSIZ];
	size_t width = ui->columns > 4 ? (size_t)ui->columns - 4 : 1;
	utf8_copy_sanitized_display_width(clipped, sizeof(clipped), text, width);
	morph_buf_printf(frame, "  %s%s%s\r\n",
		ui->color ? style : "", clipped, ui->color ? "\033[0m" : "");
	ui->frame_lines++;
}

static int frame_start(struct setup_ui *ui, morph_buf_t *frame,
		       const char *title, const char *detail)
{
	int rc = morph_buf_init(frame, 0);
	if (rc < 0)
		return rc;
	morph_buf_puts(frame, "\033[?25l");
	clear_active(ui, frame);
	const char *progress[] = {
		"1/2  Role", "Model settings", "Model settings", "2/2  Review"
	};
	if (ui->rows >= 8)
		line(ui, frame, "\033[2m", progress[ui->phase]);
	if (ui->step >= 0 && ui->step < SETUP_MODEL_COUNT) {
		morph_buf_t scope = {0};
		rc = morph_buf_init(&scope, 0);
		if (rc == 0) rc = morph_buf_printf(&scope, "%s · %s / %s",
			cli_setup_model_label(ui->step), ui->entries[ui->step].provider[0] ?
			ui->entries[ui->step].provider : "No provider", ui->entries[ui->step].model[0] ?
			ui->entries[ui->step].model : "Not configured");
		if (rc == 0) line(ui, frame, "\033[36m", morph_buf_cstr(&scope));
		morph_buf_cleanup(&scope);
		if (rc != 0) { morph_buf_cleanup(frame); return rc; }
	}
	line(ui, frame, "\033[1m", title);
	if (ui->rows >= 14 && detail)
		line(ui, frame, "\033[2m", detail);
	if (ui->rows >= 16 && ui->phase == SETUP_START) {
		line(ui, frame, "\033[2m", cli_setup_mcp_summary(ui->state->role));
		line(ui, frame, "\033[2m", "Read/media: auto-approved. Host exec: on.");
	}
	if (ui->rows >= 20)
		line(ui, frame, "", "");
	return 0;
}

static int frame_show(struct setup_ui *ui, morph_buf_t *frame)
{
	morph_buf_puts(frame, "\033[?2026l");
	int rc = morph_tui_output_write(ui->output, frame);
	ui->active_lines = ui->frame_lines;
	morph_buf_cleanup(frame);
	if (rc < 0)
		return rc;
	return 0;
}

static int record_answer(struct setup_ui *ui, const char *answer)
{
	morph_buf_t frame;
	int rc = morph_buf_init(&frame, 0);
	if (rc < 0)
		return rc;
	clear_active(ui, &frame);
	const char *names[] = {"Text", "Vision", "Images", "Video", "Review"};
	char safe[BUFSIZ];
	utf8_copy_sanitized_clamped(safe, sizeof(safe), answer, sizeof(safe) - 1);
	struct morph_tui_text_view view = {0};
	size_t width = ui->columns > 15 ? (size_t)ui->columns - 15 : 1;
	rc = morph_tui_text_view_wrap(&view, safe, width);
	if (rc == 0) rc = morph_buf_printf(&frame, "  %s✓ %-6s%s · ",
		ui->color ? "\033[32m" : "", ui->step < 0 ? "Role" : names[ui->step],
		ui->color ? "\033[0m" : "");
	if (rc == 0 && !view.lines.nelts) rc = morph_buf_puts(&frame, "\r\n");
	for (size_t i = 0; rc == 0 && i < view.lines.nelts; i++) {
		const struct morph_tui_text_line *row = morph_array_get(&view.lines, i);
		if (i) rc = morph_buf_puts(&frame, "             ");
		if (rc == 0) rc = morph_buf_append(&frame, safe + row->offset, row->length);
		if (rc == 0) rc = morph_buf_puts(&frame, "\r\n");
	}
	morph_tui_text_view_cleanup(&view);
	if (rc != 0) { morph_buf_cleanup(&frame); return rc; }
	/* Committed rows are not owned by a subsequent editable frame. */
	return frame_show(ui, &frame);
}

static void card_border(struct setup_ui *ui, morph_buf_t *frame, int selected,
	int width, int bottom)
{
	morph_buf_t border = {0};
	int rc = morph_buf_init(&border, 128);

	if (rc == 0) rc = morph_tui_card_border(&border, width, bottom);
	if (rc == 0)
		line(ui, frame, selected ? "\033[36m" : "\033[2m",
			morph_buf_cstr(&border));
	if (rc != 0) frame->failed = (rc);
	morph_buf_cleanup(&border);
}

static void card_text(struct setup_ui *ui, morph_buf_t *frame, const char *text,
	int width, const char *style)
{
	morph_buf_t row = {0};
	int rc = morph_buf_init(&row, 128);

	if (rc == 0) rc = morph_tui_card_row(&row, width, text);
	if (rc == 0) line(ui, frame, style, morph_buf_cstr(&row));
	if (rc != 0) frame->failed = (rc);
	morph_buf_cleanup(&row);
}

struct role_preview {
	morph_buf_t models;
	struct morph_tui_text_view blocks[3];
	size_t visible[3];
	int rows;
	int gap;
};

static void role_preview_cleanup(struct role_preview *preview)
{
	for (int i = 0; i < 3; i++) morph_tui_text_view_cleanup(&preview->blocks[i]);
	morph_buf_cleanup(&preview->models);
}

static int role_preview_init(struct setup_ui *ui, int selected, struct role_preview *preview)
{
	const char *models[] = {"Text", "Vision", "Image", "Video"};
	int rc = morph_buf_init(&preview->models, 0);
	if (rc == 0) rc = morph_buf_puts(&preview->models, "Models: ");
	int count = 0;
	for (int i = 0; rc == 0 && i < SETUP_MODEL_COUNT; i++) {
		if (!cli_setup_role_requires(selected, i)) continue;
		if (count++) rc = morph_buf_puts(&preview->models, " + ");
		if (rc == 0) rc = morph_buf_puts(&preview->models, models[i]);
	}
	struct cli_setup_role role = cli_setup_role_get(selected);
	const char *text[] = {role.description, morph_buf_cstr(&preview->models),
		cli_setup_mcp_summary(selected)};
	size_t width = ui->columns > 4 ? (size_t)ui->columns - 4 : 1;
	int available = ui->rows - ui->frame_lines - 3;
	size_t budget = available > 0 ? (size_t)available : 0;
	/* Keep model requirements before optional context in compact viewports. */
	const int order[] = {1, 0, 2};
	for (int n = 0; rc == 0 && n < 3; n++) {
		int i = order[n];
		if ((i == 0 && ui->rows < 14) || (i == 2 && ui->rows < 20)) continue;
		rc = morph_tui_text_view_wrap(&preview->blocks[i], text[i], width);
		if (rc != 0) break;
		size_t lines = preview->blocks[i].lines.nelts;
		if (i != 1 && lines > budget) continue;
		preview->visible[i] = lines < budget ? lines : budget;
		budget -= preview->visible[i];
		preview->rows += (int)preview->visible[i];
	}
	preview->gap = ui->rows >= 20 && budget > 0 && preview->rows > 0;
	preview->rows += preview->gap;
	return rc;
}

static void role_preview_draw(struct setup_ui *ui, morph_buf_t *frame,
		const struct role_preview *preview)
{
	if (preview->gap) line(ui, frame, "", "");
	for (int i = 0; i < 3; i++) {
		const struct morph_tui_text_view *block = &preview->blocks[i];
		for (size_t row = 0; row < preview->visible[i]; row++) {
			const struct morph_tui_text_line *span = morph_array_get(&block->lines, row);
			morph_buf_puts(frame, ui->color ? "  \033[2m" : "  ");
			morph_buf_append(frame, block->text + span->offset, span->length);
			morph_buf_puts(frame, ui->color ? "\033[0m\r\n" : "\r\n");
			ui->frame_lines++;
		}
	}
}

static int select_menu(struct setup_ui *ui, const char *title, const char *detail,
	const char *const *items, const char *const *descriptions, int count, int selected)
{
	struct morph_tui_menu menu_state = {0};

	menu_state.list.index = (size_t)selected;
	menu_state.list.flags = MORPH_TUI_LIST_WRAP | MORPH_TUI_LIST_VIM |
		MORPH_TUI_LIST_TAB | MORPH_TUI_LIST_DIGITS;
	for (;;) {
		morph_buf_t frame;
		int roles = ui->phase == SETUP_ROLE;
		int rc = frame_start(ui, &frame, title, roles ? NULL : detail);
		if (rc < 0)
			return rc;
		struct role_preview preview = {0};
		if (roles) rc = role_preview_init(ui, (int)menu_state.list.index, &preview);
		if (rc != 0) {
			role_preview_cleanup(&preview);
			morph_buf_cleanup(&frame);
			return rc;
		}
		int available = ui->rows - ui->frame_lines - 2 - preview.rows;
		int cards = !roles && descriptions && ui->columns >= 44 && available >= 8;
		int visible = cards ? available / 4 : available;
		if (visible < 1) visible = 1;
		morph_tui_list_layout(&menu_state.list, (size_t)count, (size_t)visible);
		selected = (int)menu_state.list.index;
		int first = (int)menu_state.list.first;
		for (int i = first; i < count && i < first + visible; i++) {
			morph_buf_t label;
			rc = morph_buf_init(&label, 0);
			if (rc < 0) {
				role_preview_cleanup(&preview);
				morph_buf_cleanup(&frame);
				return rc;
			}
			if (roles) {
				const char *tag = i != ui->state->role ? "" :
					ui->role_confirmed || ui->state->existing_config ?
					"  (current)" : "  (default)";
				morph_buf_printf(&label, "%s %d  %s%s", i == selected ? "›" : " ",
					i + 1, items[i], tag);
			} else morph_buf_printf(&label, "%s %s", i == selected ? "›" : " ", items[i]);
			if (cards) {
				int width = ui->columns - 4;
				if (width > 72) width = 72;
				card_border(ui, &frame, i == selected, width, 0);
				card_text(ui, &frame, morph_buf_cstr(&label), width,
					i == selected ? "\033[1;36;48;5;235m" : "");
				card_text(ui, &frame, descriptions[i], width,
					i == selected ? "\033[2;36;48;5;235m" : "\033[2m");
				card_border(ui, &frame, i == selected, width, 1);
			} else {
				line(ui, &frame, i == selected ? "\033[1;30;46m" : "",
					morph_buf_cstr(&label));
			}
			if (label.failed) frame.failed = (label.failed);
			morph_buf_cleanup(&label);
		}
		if (roles) role_preview_draw(ui, &frame, &preview);
		role_preview_cleanup(&preview);
		if (ui->rows >= 4)
			line(ui, &frame, "\033[2m", roles ?
				(ui->columns < 50 ? "↑↓ Move  Enter OK  Esc" :
				 "↑↓ Move · 1–8 Jump · Enter Select · Esc Back") : ui->columns < 60 ?
				"↑↓ Move  Enter OK  Esc Back" :
				"↑↓ Choose · Enter Select · Esc Back · Ctrl-C Exit");
		rc = frame_show(ui, &frame);
		if (rc < 0)
			return rc;
		int key = read_key(ui);
		if (key < 0) return key;
		int result = morph_tui_menu_update(&menu_state, key);

		if (result == MORPH_TUI_CANCEL) return -ECANCELED;
		if (result == MORPH_TUI_BACK) return SETUP_BACK;
		if (result == MORPH_TUI_SUBMIT) return (int)menu_state.list.index;
	}
}

static int menu(struct setup_ui *ui, const char *title, const char *detail,
	const char *const *items, int count, int selected)
{
	return select_menu(ui, title, detail, items, NULL, count, selected);
}

static int edit(struct setup_ui *ui, const char *title, const char *detail,
		char *value, size_t capacity, int secret)
{
	struct morph_tui_editor editor = {0};
	morph_buf_t shown = {0};
	int rc = morph_tui_editor_init(&editor, secret ? "" : value, capacity, secret);

	if (rc == 0) rc = morph_buf_init(&shown, 128);
	if (rc != 0) {
		morph_tui_editor_cleanup(&editor);
		return rc;
	}
	const char *validation = NULL;

	for (;;) {
		morph_buf_t frame;
		rc = frame_start(ui, &frame,
			validation && ui->rows < 14 ? "Value required" : title, editor.overflow ?
			"Value is too long. Shorten it before continuing." :
			validation ? validation : detail);
		if (rc < 0) break;
		size_t width = ui->columns > 4 ? (size_t)ui->columns - 4 : 1;
		size_t column;

		morph_buf_reset(&shown);
		rc = morph_tui_editor_view(&editor, &shown, width, &column);
		if (rc != 0) {
			morph_buf_cleanup(&frame);
			break;
		}
		int input_row = ui->frame_lines;

		line(ui, &frame, "\033[36m", morph_buf_cstr(&shown));
		line(ui, &frame, "\033[2m", ui->columns < 60 ?
			"Enter OK  Esc Back  ^U Clear" :
			"Enter Continue · Esc Back · Ctrl-U Clear · Ctrl-C Exit");
		ui->cursor_up = ui->frame_lines - input_row;
		morph_buf_printf(&frame, "\033[%dA\r\033[%zuC\033[?25h",
			ui->cursor_up, column + 2);
		rc = frame_show(ui, &frame);
		if (rc < 0) break;
		int key = read_key(ui);

		if (key < 0) { rc = key; break; }
		int result = morph_tui_editor_update(&editor, key);

		if (result < 0) { rc = result; break; }
		if (result == MORPH_TUI_CANCEL) { rc = -ECANCELED; break; }
		if (result == MORPH_TUI_BACK) { rc = SETUP_BACK; break; }
		if (result == MORPH_TUI_SUBMIT) {
			if (morph_tui_editor_valid(&editor)) {
				memcpy(value, editor.text.data, editor.text.len + 1);
				rc = record_answer(ui, secret ?
					"API key entered (hidden)" : value);
				break;
			}
			validation = editor.text.len ? "Enter valid UTF-8 text." :
				"Enter a value, or press Esc to go back.";
		} else if (result == MORPH_TUI_CHANGED) {
			validation = NULL;
		}
	}
	morph_tui_editor_cleanup(&editor);
	morph_buf_cleanup(&shown);
	return rc;
}

static int has_key(struct setup_ui *ui, struct config_model_entry *entry)
{
	(void)ui;
	if (entry->api_key[0]) return 2;
	const char *env = getenv(entry->api_key_env);
	if (env && *env) return 1;
	return 0;
}

static int credentials(struct setup_ui *ui, struct config_model_entry *entry)
{
	const char *items[] = {"Save API key to config", "Use an environment variable",
		"Set up later"};
	const char *descriptions[] = {
		"Hidden input. Saved locally for future sessions.",
		"Use a key already set in your shell.",
		"Save your preferences and connect when ready."
	};
	ui->phase = ui->step == SETUP_TEXT ? SETUP_ACCOUNT : SETUP_PREFERENCES;
	for (;;) {
		int selected = select_menu(ui, "Connect your account",
			"Models sharing this account need only one API key.", items, descriptions, 3, 0);
		if (selected < 0) return selected;
		if (selected == 2) {
			ui->deferred[ui->step] = 1;
			return record_answer(ui, "Account · Set up later");
		}
		if (selected == 0) {
			int rc = edit(ui, "Paste your API key",
				"Hidden input. Saved in your config file for future sessions.",
				entry->api_key, sizeof(entry->api_key), 1);
			if (rc == SETUP_BACK) continue;
			if (rc == 0) memset(entry->api_key_env, 0, sizeof(entry->api_key_env));
			return rc;
		}
		char name[sizeof(entry->api_key_env)];
		strcpy(name, entry->api_key_env);
		const char *hint = "Use a variable name, such as OPENAI_API_KEY; not the key itself.";
		int rc;
		for (;;) {
			rc = edit(ui, "API key environment variable", hint, name, sizeof(name), 0);
			if (rc < 0 || morph_env_name_valid(name)) break;
			hint = "Start with a letter or underscore; use letters, digits and underscores.";
		}
		if (rc == SETUP_BACK) continue;
		if (rc < 0) return rc;
		if (morph_env_name_valid(name)) {
			memset(entry->api_key, 0, sizeof(entry->api_key));
			strcpy(entry->api_key_env, name);
			ui->deferred[ui->step] = 1;
			return 0;
		}
	}
}

static int edit_token_count(struct setup_ui *ui, struct config_model_entry *entry,
			    int output)
{
	int *value = output ? &entry->max_tokens : &entry->context_limit;
	int ceiling = cli_setup_token_ceiling(entry, output);
	if (output && ceiling >= entry->context_limit)
		ceiling = entry->context_limit - 1;
	char text[CLI_SETUP_TOKEN_INPUT_MAX] = "";
	if (*value > 0) snprintf(text, sizeof(text), "%d", *value);
	morph_buf_t hint;
	int rc = morph_buf_init(&hint, 0);
	if (rc < 0) return rc;
	morph_buf_printf(&hint, "Enter %d-%d tokens from your model's documentation.",
		output ? 1 : 2, ceiling);
	if (hint.failed) { morph_buf_cleanup(&hint); return -ENOMEM; }
	for (;;) {
		rc = edit(ui, output ? "Max output (tokens)" : "Context window (tokens)",
			morph_buf_cstr(&hint), text, sizeof(text), 0);
		if (rc < 0) break;
		rc = cli_setup_parse_token_count(text, output ? 1 : 2, ceiling, value);
		if (rc == 0) break;
		morph_buf_reset(&hint);
		morph_buf_printf(&hint, "Invalid token count. Enter a whole number from %d to %d.",
			output ? 1 : 2, ceiling);
		if (hint.failed) { rc = -ENOMEM; break; }
	}
	morph_buf_cleanup(&hint);
	if (rc < 0) return rc;
	if (!output && entry->max_tokens >= entry->context_limit)
		entry->max_tokens = 0;
	return 0;
}

static int ensure_token_limits(struct setup_ui *ui, struct config_model_entry *entry)
{
	if (entry->context_limit <= 0) {
		int rc = edit_token_count(ui, entry, 0);
		if (rc < 0) return rc;
	}
	if (entry->max_tokens <= 0)
		return edit_token_count(ui, entry, 1);
	return 0;
}

static int choose_thinking(struct setup_ui *ui, struct config_model_entry *entry)
{
	const char *items[] = {"Off · Faster responses", "On · More time to reason"};
	const char *descriptions[] = {
		"Keep things moving. Thinking is off.",
		"For complex tasks. Replies take longer."
	};
	ui->phase = SETUP_PREFERENCES;
	int selected = select_menu(ui, "How should morph think?",
		"You can change this later.", items, descriptions, 2,
		cli_setup_thinking_enabled(entry));
	int rc;

	if (selected < 0)
		return selected;
	rc = cli_setup_set_thinking(entry, selected);
	if (rc == 0) {
		rc = record_answer(ui, selected ? "Thinking · On" : "Thinking · Off");
	}
	return rc;
}

static int advanced_settings(struct setup_ui *ui, struct config_model_entry *entry)
{
	int chat = ui->step == SETUP_TEXT || ui->step == SETUP_VISION;
	ui->phase = SETUP_PREFERENCES;

	for (;;) {
		ui->phase = SETUP_PREFERENCES;
		morph_buf_t labels;
		int rc = morph_buf_init(&labels, 0);
		if (rc < 0) return rc;
		/* Store offsets until the buffer stops growing. */
		size_t offsets[5];
		offsets[0] = labels.len;
		morph_buf_printf(&labels, "Model    %s", entry->model[0] ? entry->model : "Choose a model");
		morph_buf_putc(&labels, '\0');
		offsets[1] = labels.len;
		morph_buf_printf(&labels, "API URL  %s", entry->api_base[0] ? entry->api_base : "Set URL");
		morph_buf_putc(&labels, '\0');
		offsets[2] = labels.len;
		int key_status = has_key(ui, entry);
		if (key_status == 2)
			morph_buf_puts(&labels, "API key  Save to config (hidden)");
		else if (key_status)
			morph_buf_puts(&labels, "API key  Ready");
		else if (ui->deferred[ui->step] && entry->api_key_env[0])
			morph_buf_printf(&labels, "API key  %s (not exported)",
				 entry->api_key_env);
		else
			morph_buf_puts(&labels, "API key  Not set yet");
		morph_buf_putc(&labels, '\0');
		for (int i = 3; i < 5; i++) {
			offsets[i] = labels.len;
			int tokens = i == 3 ? entry->context_limit : entry->max_tokens;
			morph_buf_printf(&labels, "%s  ", i == 3 ? "Context window" : "Max output");
			if (tokens > 0) morph_buf_printf(&labels, "%d tokens", tokens);
			else morph_buf_puts(&labels, "Required for this model");
			morph_buf_putc(&labels, '\0');
		}
		if (labels.failed) { morph_buf_cleanup(&labels); return -ENOMEM; }
		const char *items[] = {"Done", labels.data + offsets[0],
			labels.data + offsets[1], labels.data + offsets[2],
			labels.data + offsets[3], labels.data + offsets[4]};
		int selected = menu(ui, "Advanced settings",
			"Defaults work for most people. Edit only what you need.",
			items, chat ? 6 : 4, 0);
		morph_buf_cleanup(&labels);
		if (selected < 0) return selected;
		if (selected == 0) {
			if (!entry->model[0]) selected = 1;
			else if (!entry->api_base[0]) selected = 2;
			else if (chat) {
				rc = ensure_token_limits(ui, entry);
				if (rc == SETUP_BACK) continue;
				return rc;
			} else return 0;
		}
		if (selected == 1) {
			char previous[sizeof(entry->model)];
			strcpy(previous, entry->model);
			rc = edit(ui, "Model name / endpoint ID", "Use the model name from your provider.",
				entry->model, sizeof(entry->model), 0);
			if (rc == 0 && chat && strcmp(previous, entry->model) != 0)
				cli_setup_reset_token_limits(entry);
		}
		else if (selected == 2) {
			char url[sizeof(entry->api_base)];
			strcpy(url, entry->api_base);
			const char *hint = "Use the API base URL, including its version path.";
			for (;;) {
				rc = edit(ui, "API base URL", hint, url, sizeof(url), 0);
				if (rc < 0) break;
				if ((strncmp(url, "https://", 8) == 0 && url[8]) ||
				    (strncmp(url, "http://", 7) == 0 && url[7])) {
					strcpy(entry->api_base, url);
					break;
				}
				hint = "Enter a URL starting with http:// or https://.";
			}
		} else if (selected >= 4) {
			if (selected == 5 && entry->context_limit <= 0) {
				rc = edit_token_count(ui, entry, 0);
				if (rc == SETUP_BACK) continue;
				if (rc < 0) return rc;
			}
			rc = edit_token_count(ui, entry, selected == 5);
		} else rc = credentials(ui, entry);
		if (rc < 0 && rc != SETUP_BACK) return rc;
	}
}

static int choose_provider(struct setup_ui *ui, struct config_model_entry *entry)
{
	const char *text[] = {"OpenAI", "DeepSeek", "Volcengine / Ark", "Other · Compatible"};
	const char *image[] = {"OpenAI", "Volcengine / Ark", "Other · Compatible"};
	const char *video[] = {"Volcengine / Ark", "Other · Compatible"};
	int kind = ui->step;
	int previous = cli_setup_provider_choice(kind, entry);
	int selected = menu(ui, "Choose your provider",
		"Changing provider resets this model's defaults and account.",
		kind == SETUP_VIDEO ? video : kind == SETUP_IMAGE ? image : text,
		kind == SETUP_VIDEO ? 2 : kind == SETUP_IMAGE ? 3 : 4, previous - 1);
	if (selected < 0) return selected;
	if (!entry->provider[0] || selected + 1 != previous) {
		cli_setup_model_defaults(kind, selected + 1, entry);
		ui->deferred[kind] = 0;
	}
	return 0;
}

static int configure_model_ui(struct setup_ui *ui)
{
	struct config_model_entry *entry = &ui->entries[ui->step];
	ui->phase = SETUP_PREFERENCES;
	if (!entry->provider[0]) {
		const char *items[] = {"Enable with defaults", "Back to overview"};
		int selected = menu(ui, "Enable this model?", "Optional. Enable only if you need it.",
			items, 2, 0);
		if (selected < 0) return selected == SETUP_BACK ? 0 : selected;
		if (selected == 1) return 0;
		cli_setup_prepare_model(ui->state, ui->step);
	}
	for (;;) {
		int thinking = strcmp(entry->adapter, "deepseek") == 0;
		int optional = !cli_setup_role_requires(ui->state->role, ui->step);
		const char *items[] = {"Done →", "Provider", "Model name / endpoint ID",
			"Account / API key", "Advanced settings", NULL, NULL};
		int count = 5;
		if (thinking) items[count++] = cli_setup_thinking_enabled(entry) ?
			"Thinking · On" : "Thinking · Off";
		if (optional) items[count++] = "Disable this model";
		int selected = menu(ui, "Model settings", "Edit only what you need; Done returns to overview.",
			items, count, 0);
		if (selected == SETUP_BACK || selected == 0) return 0;
		if (selected < 0) return selected;
		int rc;
		if (selected == 1) rc = choose_provider(ui, entry);
		else if (selected == 2) {
			char previous[sizeof(entry->model)];
			strcpy(previous, entry->model);
			rc = edit(ui, "Model name / endpoint ID", "Use the model name from your provider.",
				entry->model, sizeof(entry->model), 0);
			if (rc == 0 && ui->step <= SETUP_VISION && strcmp(previous, entry->model) != 0)
				cli_setup_reset_token_limits(entry);
		} else if (selected == 3) {
			char previous_env[sizeof(entry->api_key_env)];
			strcpy(previous_env, entry->api_key_env);
			rc = credentials(ui, entry);
			if (rc == 0) cli_setup_share_credentials(ui->state, ui->step, previous_env);
		} else if (selected == 4) {
			char previous_env[sizeof(entry->api_key_env)];
			strcpy(previous_env, entry->api_key_env);
			rc = advanced_settings(ui, entry);
			if (rc == 0 || rc == SETUP_BACK)
				cli_setup_share_credentials(ui->state, ui->step, previous_env);
		}
		else if (thinking && selected == 5) rc = choose_thinking(ui, entry);
		else {
			memset(entry, 0, sizeof(*entry));
			return 0;
		}
		if (rc < 0 && rc != SETUP_BACK) return rc;
	}
}

static int review(struct setup_ui *ui)
{
	morph_buf_t labels = {0};
	int rc = morph_buf_init(&labels, 0);
	if (rc != 0) return rc;
	int ready = 1;
	int complete = 1;
	size_t offsets[SETUP_MODEL_COUNT + 1];
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		int required = cli_setup_role_requires(ui->state->role, i);
		int active = required || entry->provider[0];
		int valid = !active || cli_setup_model_complete(i, entry);
		if (!valid) complete = 0;
		if (!valid || (active && !has_key(ui, entry))) ready = 0;
		offsets[i] = labels.len;
		morph_buf_printf(&labels, "%s · %s%s%s", cli_setup_model_label(i),
			active ? entry->model[0] ? entry->model : "Model required" : "Off",
			active ? (has_key(ui, entry) ? " · Key ready" : " · Key needed") : "",
			required ? "" : " (optional)");
		morph_buf_putc(&labels, '\0');
	}
	offsets[SETUP_MODEL_COUNT] = labels.len;
	morph_buf_printf(&labels, "Instructions · %s", ui->state->replace_prompt ?
		"Use role prompt" : ui->state->existing_prompt ? "Keep existing" : "Built-in");
	if (labels.failed) { morph_buf_cleanup(&labels); return -ENOMEM; }
	const char *items[SETUP_ACTION_COUNT] = {
		!complete ? "Complete required settings →" : ready ? "Save and exit →" :
		"Connect accounts and save →"
	};
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) items[i + 1] = labels.data + offsets[i];
	items[SETUP_CHANGE_ROLE] = "Change role";
	items[SETUP_INSTRUCTIONS] = labels.data + offsets[SETUP_MODEL_COUNT];
	items[SETUP_SAVE_LATER] = "Save for later (without connecting)";
	items[SETUP_CANCEL] = "Cancel without saving";
	ui->phase = SETUP_START;
	ui->step = SETUP_MODEL_COUNT;
	int selected = menu(ui, "Review your setup",
		"Defaults are ready. Select a setting only to change it.", items, SETUP_ACTION_COUNT, 0);
	morph_buf_cleanup(&labels);
	return selected;
}

static int finish_setup(struct setup_ui *ui, int connect)
{
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		if ((!cli_setup_role_requires(ui->state->role, i) && !entry->provider[0]) ||
		    cli_setup_model_complete(i, entry)) continue;
		ui->step = i;
		ui->phase = SETUP_PREFERENCES;
		int rc = advanced_settings(ui, entry);
		return rc < 0 && rc != SETUP_BACK ? rc : 1;
	}
	for (int i = 0; connect && i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		if (!entry->model[0] || has_key(ui, entry)) continue;
		ui->step = i;
		char previous_env[sizeof(entry->api_key_env)];
		strcpy(previous_env, entry->api_key_env);
		int rc = credentials(ui, entry);
		if (rc == SETUP_BACK) return 1;
		if (rc != 0) return rc;
		cli_setup_share_credentials(ui->state, i, previous_env);
		if (!has_key(ui, entry)) break;
	}
	ui->step = SETUP_MODEL_COUNT;
	return record_answer(ui, "Configuration confirmed");
}

int cli_setup_ui_collect(struct cli_context *ctx, const char *path, FILE *input, FILE *output,
			 struct cli_setup_state *state)
{
	struct setup_ui ui = {.output = output,
			      .fd = fileno(input),
			      .output_fd = fileno(output),
			      .color = cli_color_enabled(ctx) && !getenv("NO_COLOR"),
			      .step = -1, .entries = state->entries, .state = state};
	int signal_rc = morph_signal_listener_start(&ui.signals, setup_signal, &ui);

	if (signal_rc != 0)
		return signal_rc;
	ui.screen.interrupted = &ui.interrupted;
	int enter_rc = morph_tui_screen_open(&ui.screen, input, output, MORPH_TUI_SESSION_PASTE);

	if (enter_rc != 0) {
		morph_signal_listener_stop(&ui.signals);
		return enter_rc;
	}
	fputs("\r\n  morph  /  Setup\r\n\r\n", output);
	(void)path;
	int rc = 0;
	for (;;) {
		if (ui.step < 0) {
			const char *items[SETUP_ROLE_COUNT];
			const char *descriptions[SETUP_ROLE_COUNT];
			for (int i = 0; i < SETUP_ROLE_COUNT; i++) {
				struct cli_setup_role role = cli_setup_role_get(i);
				items[i] = role.label;
				descriptions[i] = role.description;
			}
			ui.phase = SETUP_ROLE;
			rc = select_menu(&ui, ui.role_confirmed ? "Change your role" : "Choose your role", NULL,
				items, descriptions, SETUP_ROLE_COUNT, state->role);
			if (rc == SETUP_BACK && ui.role_confirmed) {
				ui.step = SETUP_MODEL_COUNT;
				continue;
			}
			if (rc < 0) { if (rc == SETUP_BACK) rc = -ECANCELED; break; }
			int changed = !ui.role_confirmed || state->role != rc;
			state->role = rc;
			if (!ui.role_confirmed) state->replace_prompt = !state->existing_prompt;
			cli_setup_role_defaults(state);
			ui.role_confirmed = 1;
			rc = changed ? record_answer(&ui, cli_setup_role_get(state->role).label) : 0;
			if (rc < 0) break;
			ui.step = SETUP_MODEL_COUNT;
		}
		rc = review(&ui);
		if (rc == SETUP_BACK || rc == SETUP_CHANGE_ROLE) { ui.step = -1; continue; }
		if (rc < 0) break;
		if (rc == SETUP_CANCEL) { rc = -ECANCELED; break; }
		if (rc == SETUP_SAVE || rc == SETUP_SAVE_LATER) {
			rc = finish_setup(&ui, rc == SETUP_SAVE);
			if (rc <= 0) break;
		} else if (rc == SETUP_INSTRUCTIONS) {
			const char *items[] = {state->existing_prompt ? "Keep existing instructions" :
				"Use built-in instructions", "Use role instructions"};
			rc = menu(&ui, "Prompt instructions", "Existing files are preserved.",
				items, 2, state->replace_prompt);
			if (rc >= 0) state->replace_prompt = rc;
		} else {
			ui.step = rc - 1;
			rc = configure_model_ui(&ui);
		}
		if (rc < 0 && rc != SETUP_BACK) break;
		ui.step = SETUP_MODEL_COUNT;
	}
	morph_buf_t ending;
	int ending_rc = morph_buf_init(&ending, 0);
	if (ending_rc == 0) {
		clear_active(&ui, &ending);
		ending_rc = frame_show(&ui, &ending);
	}
	if (rc == 0 && ending_rc < 0) rc = ending_rc;
	int leave_rc = morph_tui_screen_close(&ui.screen);

	if (rc == 0) rc = leave_rc;
	morph_signal_listener_stop(&ui.signals);
	if (rc < 0) return rc;
	return 0;
}
