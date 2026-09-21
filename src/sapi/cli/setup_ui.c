#include "setup_ui.h"
#include "cli.h"
#include "util/error.h"
#include "util/utf8.h"
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SETUP_BACK (-EAGAIN)
#define SETUP_KEY_UP 256
#define SETUP_KEY_DOWN 257
#define SETUP_KEY_LEFT 258
#define SETUP_KEY_RIGHT 259
#define SETUP_KEY_PASTE_START 260
#define SETUP_KEY_PASTE_END 261

struct setup_ui {
	FILE *output;
	int fd;
	int output_fd;
	int color;
	int step;
	int columns;
	int rows;
	int active_lines;
	int frame_lines;
	int cursor_up;
	int deferred[SETUP_MODEL_COUNT];
	struct config_model_entry *entries;
};

int cli_setup_ui_available(FILE *input, FILE *output)
{
	const char *term = getenv("TERM");
	return isatty(fileno(input)) && isatty(fileno(output)) &&
		(!term || strcmp(term, "dumb") != 0);
}

static volatile sig_atomic_t setup_interrupted;

static void setup_signal(int signal_number)
{
	setup_interrupted = signal_number;
}

static int read_byte(struct setup_ui *ui, int timeout)
{
	if (setup_interrupted)
		MORPH_RETURN(-ECANCELED);
	struct pollfd pfd = {.fd = ui->fd, .events = POLLIN};
	int rc = poll(&pfd, 1, timeout);
	if (rc < 0)
		MORPH_RETURN(setup_interrupted ? -ECANCELED : -errno);
	if (rc == 0)
		return 0;
	unsigned char byte;
	ssize_t count = read(ui->fd, &byte, 1);
	if (count < 0)
		MORPH_RETURN(-errno);
	if (count == 0)
		MORPH_RETURN(-ECANCELED);
	return byte;
}

static int read_key(struct setup_ui *ui)
{
	int key;
	for (;;) {
		key = read_byte(ui, 100);
		if (key != 0) break;
		struct winsize ws;
		if (ioctl(ui->output_fd, TIOCGWINSZ, &ws) == 0 &&
		    ((ws.ws_col && ws.ws_col != ui->columns) ||
		     (ws.ws_row && ws.ws_row != ui->rows)))
			return 0;
	}
	if (key != 27)
		return key;
	key = read_byte(ui, 80);
	if (key == 0)
		return SETUP_BACK;
	if (key != '[' && key != 'O')
		return key < 0 ? key : SETUP_BACK;
	char sequence[16];
	size_t len = 0;
	while (len + 1 < sizeof(sequence)) {
		key = read_byte(ui, 80);
		if (key <= 0)
			return key < 0 ? key : SETUP_BACK;
		sequence[len++] = (char)key;
		if (key >= '@' && key <= '~')
			break;
	}
	sequence[len] = '\0';
	if (strcmp(sequence, "A") == 0) return SETUP_KEY_UP;
	if (strcmp(sequence, "B") == 0) return SETUP_KEY_DOWN;
	if (strcmp(sequence, "C") == 0) return SETUP_KEY_RIGHT;
	if (strcmp(sequence, "D") == 0) return SETUP_KEY_LEFT;
	if (strcmp(sequence, "200~") == 0) return SETUP_KEY_PASTE_START;
	if (strcmp(sequence, "201~") == 0) return SETUP_KEY_PASTE_END;
	return 0;
}

static void dimensions(struct setup_ui *ui)
{
	struct winsize ws;
	ui->columns = 80;
	ui->rows = 24;
	if (ioctl(ui->output_fd, TIOCGWINSZ, &ws) == 0) {
		if (ws.ws_col) ui->columns = ws.ws_col;
		if (ws.ws_row) ui->rows = ws.ws_row;
	}
}

/* The cursor normally rests on the blank line below the active prompt.
 * Only these owned lines are redrawn; completed answers stay in scrollback. */
static void clear_active(struct setup_ui *ui, morph_buf_t *frame)
{
	int old_columns = ui->columns;
	int old_rows = ui->rows;
	dimensions(ui);
	if (ui->active_lines &&
	    (old_columns != ui->columns || old_rows != ui->rows)) {
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
		MORPH_RETURN(rc);
	morph_buf_puts(frame, "\033[?25l");
	clear_active(ui, frame);
	const char *progress[] = {
		"1/5  Text model", "2/5  Vision / image understanding (optional)",
		"3/5  Image generation (optional)",
		"4/5  Video generation (optional)", "5/5  Review & finish"
	};
	line(ui, frame, "\033[2m", progress[ui->step]);
	line(ui, frame, "\033[1m", title);
	if (ui->rows >= 12 && detail)
		line(ui, frame, "\033[2m", detail);
	return 0;
}

static int frame_show(struct setup_ui *ui, morph_buf_t *frame)
{
	int rc = frame->failed ? -ENOMEM : 0;
	if (rc == 0 && (fputs(morph_buf_cstr(frame), ui->output) == EOF ||
		       fflush(ui->output) != 0))
		rc = errno ? -errno : -EIO;
	ui->active_lines = ui->frame_lines;
	morph_buf_cleanup(frame);
	if (rc < 0)
		MORPH_RETURN(rc);
	return 0;
}

static int record_answer(struct setup_ui *ui, const char *answer)
{
	morph_buf_t frame;
	int rc = morph_buf_init(&frame, 0);
	if (rc < 0)
		MORPH_RETURN(rc);
	clear_active(ui, &frame);
	const char *names[] = {"Text", "Vision", "Images", "Video", "Review"};
	char safe[BUFSIZ];
	utf8_copy_sanitized_clamped(safe, sizeof(safe), answer, sizeof(safe) - 1);
	morph_buf_printf(&frame, "  %s✓ %s%s · %s\r\n",
		ui->color ? "\033[32m" : "", names[ui->step],
		ui->color ? "\033[0m" : "", safe);
	/* Committed text can wrap naturally and is never owned by a new frame. */
	return frame_show(ui, &frame);
}

static int menu(struct setup_ui *ui, const char *title, const char *detail,
		const char *const *items, int count, int selected)
{
	int paste = 0;
	for (;;) {
		morph_buf_t frame;
		int rc = frame_start(ui, &frame, title, detail);
		if (rc < 0)
			MORPH_RETURN(rc);
		int visible = ui->rows - ui->frame_lines - 2;
		if (visible < 1) visible = 1;
		int first = selected >= visible ? selected - visible + 1 : 0;
		for (int i = first; i < count && i < first + visible; i++) {
			morph_buf_t label;
			rc = morph_buf_init(&label, 0);
			if (rc < 0) {
				morph_buf_cleanup(&frame);
				MORPH_RETURN(rc);
			}
			morph_buf_printf(&label, "%s %s", i == selected ? "›" : " ", items[i]);
			line(ui, &frame,
				i == selected ? "\033[1;36m" : "", morph_buf_cstr(&label));
			if (label.failed) frame.failed = 1;
			morph_buf_cleanup(&label);
		}
		line(ui, &frame, "\033[2m",
			ui->columns < 60 ? "↑↓ Move  Enter OK  Esc Back" :
			"↑↓ Choose · Enter Select · Esc Back · Ctrl-C Exit");
		rc = frame_show(ui, &frame);
		if (rc < 0)
			MORPH_RETURN(rc);
		int key = read_key(ui);
		if (key < 0) MORPH_RETURN(key);
		if (key == 3 || key == 4) MORPH_RETURN(-ECANCELED);
		if (key == SETUP_KEY_PASTE_START) { paste = 1; continue; }
		if (key == SETUP_KEY_PASTE_END) { paste = 0; continue; }
		if (paste) continue;
		if (key == SETUP_KEY_UP || key == 'k')
			selected = (selected + count - 1) % count;
		if (key == SETUP_KEY_DOWN || key == 'j' || key == '\t')
			selected = (selected + 1) % count;
		if (key >= '1' && key < '1' + count)
			selected = key - '1';
		if (key == '\r' || key == '\n') {
			rc = record_answer(ui, items[selected]);
			if (rc < 0) MORPH_RETURN(rc);
			return selected;
		}
	}
}

static int edit(struct setup_ui *ui, const char *title, const char *detail,
		char *value, size_t capacity, int secret)
{
	morph_buf_t input;
	int rc = morph_buf_init(&input, 0);
	if (rc < 0)
		MORPH_RETURN(rc);
	if (!secret) morph_buf_puts(&input, value);
	size_t cursor = input.len;
	int paste = 0;
	int overflow = 0;
	for (;;) {
		morph_buf_t frame;
		rc = frame_start(ui, &frame, title, overflow ?
			"Value is too long. Shorten it before continuing." : detail);
		if (rc < 0) break;
		const char *text = morph_buf_cstr(&input);
		const char *shown = secret ? (input.len ? "••••••••" : "") : text;
		size_t max_column = ui->columns > 4 ? (size_t)ui->columns - 4 : 1;
		size_t column = secret ? (input.len ? 8 : 0) : 0;
		if (!secret) {
			char saved = input.data[cursor];
			input.data[cursor] = '\0';
			shown = utf8_suffix_display_width(input.data, max_column - 1);
			column = utf8_display_width(shown);
			input.data[cursor] = saved;
		}
		int input_row = ui->frame_lines;
		line(ui, &frame, "\033[36m", shown);
		line(ui, &frame, "\033[2m",
			ui->columns < 60 ? "Enter OK  Esc Back  ^U Clear" :
			"Enter Continue · Esc Back · Ctrl-U Clear · Ctrl-C Exit");
		if (column > max_column) column = max_column;
		ui->cursor_up = ui->frame_lines - input_row;
		morph_buf_printf(&frame, "\033[%dA\r\033[%zuC\033[?25h",
			ui->cursor_up, column + 2);
		rc = frame_show(ui, &frame);
		if (rc < 0) break;
		int key = read_key(ui);
		if (key < 0) { rc = key; break; }
		if (key == 3 || key == 4) { rc = -ECANCELED; break; }
		if (key == SETUP_KEY_PASTE_START) { paste = 1; continue; }
		if (key == SETUP_KEY_PASTE_END) { paste = 0; continue; }
		if ((key == '\r' || key == '\n') && !paste) {
			if (!overflow && input.len && utf8valid(morph_buf_cstr(&input)) == NULL) {
				memcpy(value, input.data, input.len + 1);
				rc = record_answer(ui, secret ?
					"API key entered (hidden)" : value);
				break;
			}
			continue;
		}
		if (key == 21) {
			morph_buf_reset(&input);
			cursor = 0;
			overflow = 0;
		} else if ((key == 127 || key == 8) && cursor) {
			size_t previous = (size_t)(utf8_prev_codepoint(input.data,
				input.data + cursor) - input.data);
			memmove(input.data + previous, input.data + cursor,
				input.len - cursor + 1);
			input.len -= cursor - previous;
			cursor = previous;
			overflow = 0;
		} else if (key == SETUP_KEY_LEFT && cursor) {
			cursor = (size_t)(utf8_prev_codepoint(input.data,
				input.data + cursor) - input.data);
		} else if (key == SETUP_KEY_RIGHT && cursor < input.len) {
			cursor += utf8_next_codepoint_len(input.data + cursor, input.len - cursor);
		} else if (key >= 32 && key < 256 && key != 127) {
			if (input.len + 1 >= capacity) { overflow = 1; continue; }
			if (morph_buf_reserve(&input, 1) < 0) { rc = -ENOMEM; break; }
			memmove(input.data + cursor + 1, input.data + cursor,
				input.len - cursor + 1);
			input.data[cursor++] = (char)key;
			input.len++;
		}
		if (input.failed) { rc = -ENOMEM; break; }
	}
	if (input.data) memset(input.data, 0, input.cap);
	morph_buf_cleanup(&input);
	if (rc < 0) MORPH_RETURN(rc);
	return 0;
}

static int env_valid(const char *name)
{
	for (size_t i = 0; name[i]; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      c == '_' || (i > 0 && c >= '0' && c <= '9')))
			return 0;
	}
	return *name != '\0';
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
	for (;;) {
		int selected = menu(ui, "Connect your account", entry->api_key_env, items, 3, 0);
		if (selected < 0) MORPH_RETURN(selected);
		if (selected == 2) {
			ui->deferred[ui->step] = 1;
			return 0;
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
			if (rc < 0 || env_valid(name)) break;
			hint = "Start with a letter or underscore; use letters, digits and underscores.";
		}
		if (rc == SETUP_BACK) continue;
		if (rc < 0) MORPH_RETURN(rc);
		if (env_valid(name)) {
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
	if (rc < 0) MORPH_RETURN(rc);
	morph_buf_printf(&hint, "Enter %d-%d tokens from your model's documentation.",
		output ? 1 : 2, ceiling);
	if (hint.failed) { morph_buf_cleanup(&hint); MORPH_RETURN(-ENOMEM); }
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
	if (rc < 0) MORPH_RETURN(rc);
	if (!output && entry->max_tokens >= entry->context_limit)
		entry->max_tokens = 0;
	return 0;
}

static int ensure_token_limits(struct setup_ui *ui, struct config_model_entry *entry)
{
	if (entry->context_limit <= 0) {
		int rc = edit_token_count(ui, entry, 0);
		if (rc < 0) MORPH_RETURN(rc);
	}
	if (entry->max_tokens <= 0)
		return edit_token_count(ui, entry, 1);
	return 0;
}

static int model_settings(struct setup_ui *ui, struct config_model_entry *entry)
{
	int chat = ui->step == SETUP_TEXT || ui->step == SETUP_VISION;
	for (;;) {
		morph_buf_t labels;
		int rc = morph_buf_init(&labels, 0);
		if (rc < 0) MORPH_RETURN(rc);
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
		if (labels.failed) { morph_buf_cleanup(&labels); MORPH_RETURN(-ENOMEM); }
		const char *items[] = {"Continue", labels.data + offsets[0],
			labels.data + offsets[1], labels.data + offsets[2],
			labels.data + offsets[3], labels.data + offsets[4]};
		int selected = menu(ui, "Make it yours", "Choose a setting to edit, or continue with defaults.",
			items, chat ? 6 : 4, 0);
		morph_buf_cleanup(&labels);
		if (selected < 0) MORPH_RETURN(selected);
		if (selected == 0) {
			if (!entry->model[0]) selected = 1;
			else if (!entry->api_base[0]) selected = 2;
			else {
				if (chat) {
					rc = ensure_token_limits(ui, entry);
					if (rc == SETUP_BACK) continue;
					if (rc < 0) MORPH_RETURN(rc);
				}
				if (!has_key(ui, entry) && !ui->deferred[ui->step]) {
					rc = credentials(ui, entry);
					if (rc == SETUP_BACK) continue;
					return rc;
				}
				return 0;
			}
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
				if (rc < 0) MORPH_RETURN(rc);
			}
			rc = edit_token_count(ui, entry, selected == 5);
		} else rc = credentials(ui, entry);
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
	for (;;) {
		int selected = menu(ui, "Choose your provider",
			"Use your own account with any supported provider.",
			items, kind == SETUP_TEXT || kind == SETUP_VISION ? 4 :
			kind == SETUP_VIDEO ? 2 : 3, *provider);
		if (selected < 0) MORPH_RETURN(selected);
		if (!entry->provider[0] || selected != *provider) {
			cli_setup_model_defaults(kind, selected + 1, entry);
			ui->deferred[kind] = 0;
		}
		*provider = selected;
		int rc = model_settings(ui, entry);
		if (rc == SETUP_BACK) continue;
		return rc;
	}
}

static int model_step(struct setup_ui *ui, int kind, int *provider)
{
	if (kind == SETUP_TEXT)
		return choose_provider(ui, kind, provider);
	struct config_model_entry *entry = &ui->entries[kind];
	const char *items[] = {"Skip for now", "Set up this capability"};
	for (;;) {
		int selected = menu(ui, kind == SETUP_VISION ? "Understand images with morph?" :
			kind == SETUP_IMAGE ? "Create images with morph?" : "Create videos with morph?",
			kind == SETUP_VISION ? "Read images and answer visual questions. Optional." :
			"Optional. You can enable this later in your configuration.",
			items, 2, entry->model[0] ? 1 : 0);
		if (selected < 0) MORPH_RETURN(selected);
		if (selected == 0) {
			memset(entry, 0, sizeof(*entry));
			return 0;
		}
		int rc = choose_provider(ui, kind, provider);
		if (rc != SETUP_BACK) return rc;
	}
}

static int review(struct setup_ui *ui, const char *path)
{
	morph_buf_t labels;
	int rc = morph_buf_init(&labels, 0);
	if (rc < 0) MORPH_RETURN(rc);
	const char *names[] = {"Text", "Vision", "Images", "Video"};
	size_t offsets[SETUP_MODEL_COUNT];
	for (int i = 0; i < SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		offsets[i] = labels.len;
		morph_buf_printf(&labels, "%s  %s%s", names[i],
			entry->model[0] ? entry->model : "Skipped",
			entry->model[0] ? (has_key(ui, entry) == 2 ? " · Config key" :
			 has_key(ui, entry) ? " · Ready" : " · Key needed") : "");
		if ((i == SETUP_TEXT || i == SETUP_VISION) && entry->model[0])
			morph_buf_printf(&labels, " · ctx=%d out=%d",
				entry->context_limit, entry->max_tokens);
		morph_buf_putc(&labels, '\0');
	}
	if (labels.failed) { morph_buf_cleanup(&labels); MORPH_RETURN(-ENOMEM); }
	const char *items[SETUP_MODEL_COUNT + 2] = {"Save & finish"};
	for (int i = 0; i < SETUP_MODEL_COUNT; i++)
		items[i + 1] = labels.data + offsets[i];
	items[SETUP_MODEL_COUNT + 1] = "Cancel without saving";
	int selected = menu(ui, "Ready when you are", path, items, SETUP_MODEL_COUNT + 2, 0);
	if (selected == 0) {
		for (int i = 1; i <= SETUP_MODEL_COUNT; i++) {
			rc = record_answer(ui, items[i]);
			if (rc < 0) { selected = rc; break; }
		}
	}
	morph_buf_cleanup(&labels);
	return selected;
}

int cli_setup_ui_collect(const char *path, FILE *input, FILE *output,
			 struct config_model_entry entries[SETUP_MODEL_COUNT])
{
	struct setup_ui ui = {.output = output, .fd = fileno(input),
		.output_fd = fileno(output), .color = cli_color_enabled() && !getenv("NO_COLOR"),
		.entries = entries};
	struct termios saved;
	if (tcgetattr(ui.fd, &saved) != 0) MORPH_RETURN(-errno);
	struct termios raw = saved;
	raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
	raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(ui.fd, TCSANOW, &raw) != 0) MORPH_RETURN(-errno);
	const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
	struct sigaction old_actions[4];
	struct sigaction action = {0};
	action.sa_handler = setup_signal;
	sigemptyset(&action.sa_mask);
	setup_interrupted = 0;
	int installed = 0;
	for (; installed < 4; installed++) {
		if (sigaction(signals[installed], &action, &old_actions[installed]) != 0) {
			int signal_rc = -errno;
			while (installed-- > 0)
				sigaction(signals[installed], &old_actions[installed], NULL);
			tcsetattr(ui.fd, TCSANOW, &saved);
			MORPH_RETURN(signal_rc);
		}
	}
	fputs("\r\n  morph  /  First-time setup\r\n\r\n"
		"\033[?2004h\033[?25l", output);
	int providers[SETUP_MODEL_COUNT] = {0};
	int rc = 0;
	int reviewing = 0;
	for (;;) {
		if (ui.step < SETUP_MODEL_COUNT) {
			rc = model_step(&ui, ui.step, &providers[ui.step]);
			if (rc == SETUP_BACK) {
				if (reviewing) ui.step = SETUP_MODEL_COUNT;
				else if (ui.step > 0) ui.step--;
				else { rc = -ECANCELED; break; }
			} else if (rc < 0) break;
			else ui.step = reviewing ? SETUP_MODEL_COUNT : ui.step + 1;
		} else {
			reviewing = 1;
			rc = review(&ui, path);
			if (rc == 0) break;
			if (rc == SETUP_MODEL_COUNT + 1) { rc = -ECANCELED; break; }
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
	fputs("\033[?2004l\033[?25h", output);
	fflush(output);
	if (tcsetattr(ui.fd, TCSANOW, &saved) != 0 && rc == 0)
		rc = -errno;
	for (int i = 0; i < installed; i++)
		sigaction(signals[i], &old_actions[i], NULL);
	if (rc < 0) MORPH_RETURN(rc);
	return 0;
}
