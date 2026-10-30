#include <morph/tui/components.h>
#include "setup_ui.h"
#include "cli.h"
#include "util/error.h"
#include "util/utf8.h"
#include "util/name.h"
#include <signal.h>
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
	int thinking_confirmed[SETUP_MODEL_COUNT];
	struct config_model_entry *entries;
	struct cli_setup_state *state;
};

int cli_setup_ui_available(FILE *input, FILE *output)
{
	return morph_tui_available(input, output);
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
		"1/4  Role", "2/4  Account", "3/4  Preferences", "4/4  Save"
	};
	if (ui->rows >= 8)
		line(ui, frame, "\033[2m", progress[ui->phase]);
	line(ui, frame, "\033[1m", title);
	if (ui->rows >= 14 && detail)
		line(ui, frame, "\033[2m", detail);
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
	morph_buf_printf(&frame, "  %s✓ %s%s · %s\r\n",
		ui->color ? "\033[32m" : "", ui->step < 0 ? "Role" : names[ui->step],
		ui->color ? "\033[0m" : "", safe);
	/* Committed text can wrap naturally and is never owned by a new frame. */
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

static int select_menu(struct setup_ui *ui, const char *title, const char *detail,
	const char *const *items, const char *const *descriptions, int count, int selected)
{
	struct morph_tui_menu menu_state = {0};

	menu_state.list.index = (size_t)selected;
	menu_state.list.flags = MORPH_TUI_LIST_WRAP | MORPH_TUI_LIST_VIM |
		MORPH_TUI_LIST_TAB | MORPH_TUI_LIST_DIGITS;
	for (;;) {
		morph_buf_t frame;
		int rc = frame_start(ui, &frame, title, detail);
		if (rc < 0)
			return rc;
		int available = ui->rows - ui->frame_lines - 2;
		int cards = descriptions && ui->columns >= 44 && available >= 8;
		int visible = cards ? available / 4 : available;
		if (visible < 1) visible = 1;
		morph_tui_list_layout(&menu_state.list, (size_t)count, (size_t)visible);
		selected = (int)menu_state.list.index;
		int first = (int)menu_state.list.first;
		for (int i = first; i < count && i < first + visible; i++) {
			morph_buf_t label;
			rc = morph_buf_init(&label, 0);
			if (rc < 0) {
				morph_buf_cleanup(&frame);
				return rc;
			}
			morph_buf_printf(&label, "%s %s", i == selected ? "›" : " ", items[i]);
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
		if (ui->rows >= 4)
			line(ui, &frame, "\033[2m", ui->columns < 60 ?
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
			"Use your own provider account.", items, descriptions, 3, 0);
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
		ui->thinking_confirmed[ui->step] = 1;
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

static int model_settings(struct setup_ui *ui, struct config_model_entry *entry)
{
	int thinking = strcmp(entry->adapter, "deepseek") == 0;
	int chat = ui->step == SETUP_TEXT || ui->step == SETUP_VISION;

	for (;;) {
		const char *items[] = {"Continue →", "Account", "Advanced settings", NULL};
		const char *descriptions[] = {"Use these preferences and move on.",
			has_key(ui, entry) ? "Connected. Change how your key is stored." :
			"Not connected yet. You can set this up later.",
			"Model, API URL, context and output limits.", NULL};
		if (thinking) {
			items[2] = cli_setup_thinking_enabled(entry) ?
				"Thinking · On" : "Thinking · Off";
			descriptions[2] = "Choose quick replies or more time to think.";
			items[3] = "Advanced settings";
			descriptions[3] = "Model, API URL, context and output limits.";
		}
		ui->phase = SETUP_PREFERENCES;
		int selected = select_menu(ui, "Make morph yours",
			"You can change these preferences later.", items, descriptions,
			thinking ? 4 : 3, 0);
		if (selected < 0) return selected;
		int rc = 0;
		if (selected == 0) {
			if (!entry->model[0] || !entry->api_base[0] ||
			    (chat && (entry->context_limit <= 0 || entry->max_tokens <= 0))) {
				rc = advanced_settings(ui, entry);
				if (rc < 0 && rc != SETUP_BACK) return rc;
				continue;
			}
			if (thinking && !ui->thinking_confirmed[ui->step]) {
				rc = choose_thinking(ui, entry);
				if (rc == SETUP_BACK) continue;
				if (rc < 0) return rc;
			}
			return 0;
		} else if (selected == 1) {
			rc = credentials(ui, entry);
		} else if (thinking && selected == 2) {
			rc = choose_thinking(ui, entry);
		} else {
			rc = advanced_settings(ui, entry);
		}
		if (rc < 0 && rc != SETUP_BACK) return rc;
	}
}

static int choose_provider(struct setup_ui *ui, int kind, int *provider)
{
	struct config_model_entry *entry = &ui->entries[kind];
	const char *text[] = {"OpenAI", "DeepSeek", "Volcengine / Ark", "Other · OpenAI compatible"};
	const char *image[] = {"OpenAI", "Volcengine / Ark", "Other · OpenAI Images compatible"};
	const char *video[] = {"Volcengine / Ark", "Other · Volcengine Video compatible"};
	const char *vision[] = {"OpenAI", "DeepSeek", "Volcengine / Ark",
		"Other · Vision chat compatible"};
	const char *const *items = kind == SETUP_TEXT ? text :
		kind == SETUP_VISION ? vision : kind == SETUP_IMAGE ? image : video;
	const char *chat_details[] = {"Connect your OpenAI account.",
		"Connect your DeepSeek account.", "Use your Volcengine / Ark account.",
		"Bring your own compatible endpoint."};
	const char *image_details[] = {chat_details[0], chat_details[2], chat_details[3]};
	const char *video_details[] = {chat_details[2], chat_details[3]};
	const char *const *descriptions = kind == SETUP_IMAGE ? image_details :
		kind == SETUP_VIDEO ? video_details : chat_details;
	for (;;) {
		ui->phase = kind == SETUP_TEXT ? SETUP_ACCOUNT : SETUP_PREFERENCES;
		const char *required[] = {"Text model required for your role.",
			"Vision model required for your role.", "Image model required for your role.",
			"Video model required for your role."};
		int selected = select_menu(ui, "Choose your provider",
			cli_setup_role_requires(ui->state->role, kind) ?
			required[kind] :
			"Use your own account with any supported provider.",
			items, descriptions, kind == SETUP_TEXT || kind == SETUP_VISION ? 4 :
			kind == SETUP_VIDEO ? 2 : 3, *provider);
		if (selected < 0) return selected;
		if (!entry->provider[0] || selected != *provider) {
			cli_setup_model_defaults(kind, selected + 1, entry);
			ui->deferred[kind] = 0;
			ui->thinking_confirmed[kind] = 0;
		}
		*provider = selected;
		int rc;
		if (!has_key(ui, entry) && !ui->deferred[kind]) {
			rc = credentials(ui, entry);
			if (rc == SETUP_BACK) continue;
			if (rc < 0) return rc;
		}
		rc = model_settings(ui, entry);
		if (rc == SETUP_BACK) continue;
		if (rc == 0) rc = record_answer(ui, items[selected]);
		return rc;
	}
}

static int model_step(struct setup_ui *ui, int kind, int *provider)
{
	if (cli_setup_role_requires(ui->state->role, kind))
		return choose_provider(ui, kind, provider);
	struct config_model_entry *entry = &ui->entries[kind];
	const char *items[] = {"Skip for now", "Set up this capability"};
	const char *descriptions[] = {"Start simple. You can add this later.",
		"Connect a separate model for this capability."};
	for (;;) {
		ui->phase = SETUP_PREFERENCES;
		const char *title = kind == SETUP_VISION ? "Understand images with morph?" :
			kind == SETUP_IMAGE ? "Create images with morph?" :
			"Create videos with morph?";
		int selected = select_menu(ui, title,
			kind == SETUP_VISION ? "Read images and answer visual questions. Optional." :
			"Optional. You can enable this later in your configuration.",
			items, descriptions, 2, entry->model[0] ? 1 : 0);
		if (selected < 0) return selected;
		if (selected == 0) {
			memset(entry, 0, sizeof(*entry));
			ui->thinking_confirmed[kind] = 0;
			return record_answer(ui, "Skip for now");
		}
		int rc = choose_provider(ui, kind, provider);
		if (rc != SETUP_BACK) return rc;
	}
}

static int review(struct setup_ui *ui, const char *path)
{
	morph_buf_t labels;
	int rc = morph_buf_init(&labels, 0);
	if (rc < 0) return rc;
	const char *names[] = {"Text", "Vision", "Images", "Video"};
	int ready = 1;
	size_t offsets[SETUP_MODEL_COUNT];
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		if ((entry->model[0] && !has_key(ui, entry)) ||
		    (cli_setup_role_requires(ui->state->role, i) && !entry->model[0])) ready = 0;
		offsets[i] = labels.len;
		morph_buf_printf(&labels, "%s  ", names[i]);
		if (entry->model[0] && strcmp(entry->adapter, "deepseek") == 0)
			morph_buf_printf(&labels, "Thinking %s · ",
				cli_setup_thinking_enabled(entry) ? "On" : "Off");
		const char *provider = strcmp(entry->provider, "openai") == 0 ? "OpenAI" :
			strcmp(entry->provider, "deepseek") == 0 ? "DeepSeek" :
			strcmp(entry->provider, "volcengine") == 0 ? "Volcengine / Ark" : "Custom";
		morph_buf_printf(&labels, "%s%s",
			entry->model[0] ? provider : "Set up later",
			entry->model[0] ? (has_key(ui, entry) == 2 ? " · Config key" :
			 has_key(ui, entry) ? " · Ready" : " · Key needed") : "");
		morph_buf_putc(&labels, '\0');
	}
	if (labels.failed) { morph_buf_cleanup(&labels); return -ENOMEM; }
	const char *items[SETUP_MODEL_COUNT + 3] = {
		ready ? "Save and exit →" : "Save setup for later →"
	};
	for (int i = 0; i < SETUP_MODEL_COUNT; i++)
		items[i + 1] = labels.data + offsets[i];
	items[SETUP_MODEL_COUNT + 1] = cli_setup_role_get(ui->state->role).label;
	items[SETUP_MODEL_COUNT + 2] = "Cancel without saving";
	(void)path;
	ui->phase = SETUP_START;
	int selected = menu(ui, ready ? "You're ready." : "Save your setup",
		ready ? "Save these settings, then run morph again to start." :
		"Add your API key later to start chatting.",
		items, SETUP_MODEL_COUNT + 3, 0);
	if (selected == 0) {
		for (int i = 0; i < SETUP_MODEL_COUNT; i++)
			if (cli_setup_role_requires(ui->state->role, i) && !ui->entries[i].model[0]) {
				morph_buf_cleanup(&labels);
				return i + 1;
			}
		for (int i = 1; i <= SETUP_MODEL_COUNT; i++) {
			rc = record_answer(ui, items[i]);
			if (rc < 0) { selected = rc; break; }
		}
	}
	morph_buf_cleanup(&labels);
	return selected;
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
	int providers[SETUP_MODEL_COUNT];
	for (int i = 0; i < SETUP_MODEL_COUNT; i++)
		providers[i] = cli_setup_provider_choice(i, &state->entries[i]) - 1;
	int rc = 0;
	int reviewing = 0;
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
			rc = select_menu(&ui, "Choose your role",
				"Your role determines the required models and default instructions.",
				items, descriptions, SETUP_ROLE_COUNT, state->role);
			if (rc == SETUP_BACK) {
				if (reviewing) { ui.step = SETUP_MODEL_COUNT; continue; }
				rc = -ECANCELED;
			}
			if (rc < 0) break;
			state->role = rc;
			if (state->existing_prompt) {
				const char *choices[] = {"Keep existing instructions", "Use role instructions"};
				rc = menu(&ui, "Prompt instructions",
					"Existing prompt files are preserved. Role instructions use a new file.",
					choices, 2, state->replace_prompt);
				if (rc == SETUP_BACK) continue;
				if (rc < 0) break;
				state->replace_prompt = rc;
			} else state->replace_prompt = 1;
			rc = record_answer(&ui, cli_setup_role_get(state->role).label);
			if (rc < 0) break;
			ui.step = 0;
			reviewing = 0;
		} else if (ui.step < SETUP_MODEL_COUNT) {
			rc = model_step(&ui, ui.step, &providers[ui.step]);
			if (rc == SETUP_BACK) {
				if (reviewing) ui.step = SETUP_MODEL_COUNT;
				else ui.step--;
			} else if (rc < 0) break;
			else ui.step = reviewing ? SETUP_MODEL_COUNT : ui.step + 1;
		} else {
			reviewing = 1;
			rc = review(&ui, path);
			if (rc == 0) break;
			if (rc == SETUP_MODEL_COUNT + 2) { rc = -ECANCELED; break; }
			if (rc == SETUP_MODEL_COUNT + 1) { ui.step = -1; continue; }
			if (rc == SETUP_BACK) ui.step = SETUP_MODEL_COUNT - 1;
			else if (rc < 0) break;
			else ui.step = rc - 1;
		}
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
