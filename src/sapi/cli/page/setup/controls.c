#include "sapi/cli/page/setup/controls.h"
#include <nb/thread.h>
#include <nb/cleanup.h>
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/configuration/role.h"
#include "config/config.h"
#include <nothing/tui/models/list_navigation.h>
#include <nothing/tui/models/text_editor.h>
#include "sapi/cli/component/setup_prompt.h"
#include <nothing/tui/text.h>
#include <nothing/tui/draw.h>
#include <nothing/tui/screen.h>
#include "sapi/cli/services/configuration/mcp.h"
#include "sapi/cli/application.h"
#include "errors.h"
#include <nb/utf8.h>
#include <morph/tui/terminal/text.h>
#include <nb/str.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_CONTROLS_SETUP_BACK (NB_ERR_AGAIN)

enum cli_controls_setup_phase {
	CLI_CONTROLS_SETUP_ROLE,
	CLI_CONTROLS_SETUP_ACCOUNT,
	CLI_CONTROLS_SETUP_PREFERENCES,
	CLI_CONTROLS_SETUP_START
};

enum cli_controls_setup_action {
	CLI_CONTROLS_SETUP_SAVE,
	CLI_CONTROLS_SETUP_CHANGE_ROLE = CLI_SETUP_MODEL_COUNT + 1,
	CLI_CONTROLS_SETUP_INSTRUCTIONS,
	CLI_CONTROLS_SETUP_SAVE_LATER,
	CLI_CONTROLS_SETUP_CANCEL,
	CLI_CONTROLS_SETUP_ACTION_COUNT
};

struct cli_controls_setup_ui {
	nb_atomic_int interrupted;
	nb_signal_listener *signals;
	struct nt_screen screen;
	FILE *output;
	int fd;
	int output_fd;
	int color;
	int step;
	enum cli_controls_setup_phase phase;
	int columns;
	int rows;
	struct nt_inline_region region;
	int deferred[CLI_SETUP_MODEL_COUNT];
	int role_confirmed;
	struct config_model_entry *entries;
	struct cli_setup_state *state;
};

int cli_setup_controls_available(FILE *input, FILE *output)
{
	return nt_terminal_available(input, output);
}

int cli_setup_print(FILE *output, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (!isatty(fileno(output))) {
		int written = vfprintf(output, format, args);
		va_end(args);
		return written < 0 ? NB_ERR_IO : 0;
	}
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	struct nt_text_layout view;
	nt_text_layout_init(&view);
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_vprintf(&text, format, args) ? 0 : NB_ERR_NOMEM);
	va_end(args);
	char *safe = rc == 0 ? morph_terminal_sanitize_dup(nb_buf_cstr(&text), text.len,
							   MORPH_TERMINAL_TEXT_MULTILINE, NULL)
			     : NULL;
	if (rc == 0 && !safe)
		rc = NB_ERR_NOMEM;
	int columns, rows;
	nt_terminal_dimensions(fileno(output), &columns, &rows);
	size_t width = columns > 4 ? (size_t)columns - 4 : 1;
	if (rc == 0)
		rc = nt_text_layout_build(&view, safe, width, NT_TEXT_WRAP_CHAR);
	if (rc == 0)
		rc = (nb_buf_init(&rendered), nb_buf_reserve(&rendered, 4096) ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; rc == 0 && i < view.lines.len; i++) {
		const struct nt_text_line *row = nb_vec_at(&view.lines, i);
		if (row->length)
			rc = (nb_buf_append_cstr(&rendered, "  ") ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append(&rendered, safe + row->offset, row->length)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&rendered, "\r\n") ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0)
		rc = nt_draw_write(output, &rendered);
	nt_text_layout_deinit(&view);
	nb_buf_cleanup(&rendered);
	nb_buf_cleanup(&text);
	free(safe);
	return rc;
}

static void cli_controls_setup_signal(int number, void *opaque)
{
	struct cli_controls_setup_ui *ui = opaque;

	if (number != SIGWINCH)
		nb_atomic_int_store(&ui->interrupted, number);
}

static int cli_controls_read_key(struct cli_controls_setup_ui *ui)
{
	for (;;) {
		struct nt_event event;
		int rc = nt_screen_poll(&ui->screen, &event, 100);

		if (nb_atomic_int_load(&ui->interrupted))
			return NB_ERR_CANCELLED;
		if (rc == NB_ERR_INTERRUPTED)
			continue;
		if (rc != 0)
			return rc;
		if (event.type == NT_EVENT_EOF)
			return NB_ERR_CANCELLED;
		if (event.type == NT_EVENT_RESIZE)
			return 0;
		if (event.type == NT_EVENT_KEY)
			return event.key;
	}
}

static void cli_controls_dimensions(struct cli_controls_setup_ui *ui)
{
	nt_terminal_dimensions(ui->output_fd, &ui->columns, &ui->rows);
}

static int cli_controls_record_answer(struct cli_controls_setup_ui *ui, const char *answer)
{
	nb_buf frame NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&frame), nb_buf_reserve(&frame, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;
	cli_controls_dimensions(ui);
	const char *names[] = {"Text", "Vision", "Images", "Video", "Review"};
	char safe[BUFSIZ];
	nb_utf8_copy_sanitized_clamped(safe, sizeof(safe), answer, sizeof(safe) - 1);
	struct nt_text_layout view;
	nt_text_layout_init(&view);
	size_t width = ui->columns > 13 ? (size_t)ui->columns - 13 : 1;
	rc = nt_text_layout_build(&view, safe, width, NT_TEXT_WRAP_CHAR);
	if (rc == 0)
		rc = (nb_buf_printf(
			      &frame, "%s" CLI_FEED_DONE " %-6s%s · ", ui->color ? ANSI_GREEN : "",
			      ui->step < 0 ? "Role" : names[ui->step], ui->color ? ANSI_RESET : "")
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0 && !view.lines.len)
		rc = (nb_buf_append_cstr(&frame, "\r\n") ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; rc == 0 && i < view.lines.len; i++) {
		const struct nt_text_line *row = nb_vec_at(&view.lines, i);
		if (i)
			rc = (nb_buf_append_cstr(&frame, "           ") ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append(&frame, safe + row->offset, row->length)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&frame, "\r\n") ? 0 : NB_ERR_NOMEM);
	}
	nt_text_layout_deinit(&view);
	if (rc != 0) {
		nb_buf_cleanup(&frame);
		return rc;
	}
	/* Committed rows are not owned by a subsequent editable frame. */
	return nt_inline_region_commit(&ui->region, ui->output, &frame, ui->columns, ui->rows);
}

static int cli_controls_prompt_create(struct cli_controls_setup_ui *ui,
				      struct cli_setup_prompt_options *options, nb_buf *scope,
				      struct cli_setup_prompt **out)
{
	const char *progress[] = {"1/2  Role", "Model settings", "Model settings", "2/2  Review"};
	options->progress = progress[ui->phase];
	options->styled = ui->color;
	options->current = ui->state->role;
	options->confirmed = ui->role_confirmed || ui->state->existing_config;
	if (ui->phase == CLI_CONTROLS_SETUP_START)
		options->summary = cli_setup_mcp_summary(ui->state->role);
	if (ui->step >= 0 && ui->step < CLI_SETUP_MODEL_COUNT) {
		const struct config_model_entry *entry = &ui->entries[ui->step];
		if (!nb_buf_printf(scope, "%s · %s / %s", cli_setup_model_label(ui->step),
				   *entry->provider ? entry->provider : "No provider",
				   *entry->model ? entry->model : "Not configured"))
			return NB_ERR_NOMEM;
		options->scope = nb_buf_cstr(scope);
	}
	return cli_setup_prompt_create(options, out);
}

static int cli_controls_select_menu(struct cli_controls_setup_ui *ui, const char *title,
				    const char *detail, const char *const *items,
				    const char *const *descriptions, int count, int selected)
{
	struct nt_menu_state menu = {.list = {.index = (size_t)selected,
					      .flags = NT_LIST_VIEW_WRAP | NT_LIST_VIEW_VIM |
						       NT_LIST_VIEW_TAB | NT_LIST_VIEW_DIGITS}};
	struct cli_setup_prompt_options options = {.title = title,
						   .detail = detail,
						   .items = items,
						   .descriptions = descriptions,
						   .count = count,
						   .menu = &menu,
						   .roles = ui->phase == CLI_CONTROLS_SETUP_ROLE};
	nb_buf scope NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_setup_prompt *prompt NB_CLEANUP(cli_setup_prompt_cleanup) = NULL;
	int rc = cli_controls_prompt_create(ui, &options, &scope, &prompt);
	while (!rc) {
		cli_controls_dimensions(ui);
		rc = cli_setup_prompt_present(prompt, &ui->region, ui->output, ui->columns,
					      ui->rows, NULL);
		if (rc)
			return rc;
		int key = cli_controls_read_key(ui);
		if (key < 0)
			return key;
		int result = nt_menu_state_update(&menu, key);
		if (result == NT_NAVIGATION_CANCEL)
			return NB_ERR_CANCELLED;
		if (result == NT_NAVIGATION_BACK)
			return CLI_CONTROLS_SETUP_BACK;
		if (result == NT_NAVIGATION_SUBMIT)
			return (int)menu.list.index;
	}
	return rc;
}

static int cli_controls_menu(struct cli_controls_setup_ui *ui, const char *title,
			     const char *detail, const char *const *items, int count, int selected)
{
	return cli_controls_select_menu(ui, title, detail, items, NULL, count, selected);
}

static int cli_controls_edit(struct cli_controls_setup_ui *ui, const char *title,
			     const char *detail, char *value, size_t capacity, int secret)
{
	struct nt_text_editor_state editor = {0};
	int rc = nt_text_editor_state_init(&editor, secret ? "" : value, capacity, secret);
	if (rc)
		return rc;
	nb_buf scope NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_setup_prompt *prompt NB_CLEANUP(cli_setup_prompt_cleanup) = NULL;
	struct cli_setup_prompt_options options = {
		.title = title, .detail = detail, .editor = &editor};
	rc = cli_controls_prompt_create(ui, &options, &scope, &prompt);
	const char *validation = NULL;
	while (!rc) {
		cli_controls_dimensions(ui);
		rc = cli_setup_prompt_present(prompt, &ui->region, ui->output, ui->columns,
					      ui->rows, validation);
		if (rc)
			break;
		int key = cli_controls_read_key(ui);

		if (key < 0) {
			rc = key;
			break;
		}
		if (key == 4 && !editor.pasting) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		int result = nt_text_editor_prompt_key(&editor, key);

		if (result < 0) {
			rc = result;
			break;
		}
		if (result == NT_NAVIGATION_CANCEL) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (result == NT_NAVIGATION_BACK) {
			rc = CLI_CONTROLS_SETUP_BACK;
			break;
		}
		if (result == NT_NAVIGATION_SUBMIT) {
			if ((editor.text.len && !editor.overflow && !editor.utf8_pending_len)) {
				memcpy(value, editor.text.ptr, editor.text.len + 1);
				rc = cli_controls_record_answer(
					ui, secret ? "API key entered (hidden)" : value);
				break;
			}
			validation = editor.text.len ? "Enter valid UTF-8 text."
						     : "Enter a value, or press Esc to go back.";
		} else if (result == NT_NAVIGATION_CHANGED) {
			validation = NULL;
		}
	}
	nt_text_editor_state_deinit(&editor);
	return rc;
}

static int cli_controls_has_key(struct cli_controls_setup_ui *ui, struct config_model_entry *entry)
{
	(void)ui;
	if (entry->api_key[0])
		return 2;
	const char *env = getenv(entry->api_key_env);
	if (env && *env)
		return 1;
	return 0;
}

static int cli_controls_credentials(struct cli_controls_setup_ui *ui,
				    struct config_model_entry *entry)
{
	const char *items[] = {"Save API key to config", "Use an environment variable",
			       "Set up later"};
	const char *descriptions[] = {"Hidden input. Saved locally for future sessions.",
				      "Use a key already set in your shell.",
				      "Save your preferences and connect when ready."};
	ui->phase = ui->step == CLI_SETUP_TEXT ? CLI_CONTROLS_SETUP_ACCOUNT
					       : CLI_CONTROLS_SETUP_PREFERENCES;
	for (;;) {
		int selected = cli_controls_select_menu(
			ui, "Connect your account",
			"Models sharing this account need only one API key.", items, descriptions,
			3, 0);
		if (selected < 0)
			return selected;
		if (selected == 2) {
			ui->deferred[ui->step] = 1;
			return cli_controls_record_answer(ui, "Account · Set up later");
		}
		if (selected == 0) {
			int rc = cli_controls_edit(
				ui, "Paste your API key",
				"Hidden input. Saved in your config file for future sessions.",
				entry->api_key, sizeof(entry->api_key), 1);
			if (rc == CLI_CONTROLS_SETUP_BACK)
				continue;
			if (rc == 0)
				memset(entry->api_key_env, 0, sizeof(entry->api_key_env));
			return rc;
		}
		char name[sizeof(entry->api_key_env)];
		strcpy(name, entry->api_key_env);
		const char *hint =
			"Use a variable name, such as OPENAI_API_KEY; not the key itself.";
		int rc;
		for (;;) {
			rc = cli_controls_edit(ui, "API key environment variable", hint, name,
					       sizeof(name), 0);
			if (rc < 0 || (nb_str_is_alnum(nb_str_from_cstr(name), "_") &&
				       !((name)[0] >= '0' && (name)[0] <= '9')))
				break;
			hint = "Start with a letter or underscore; use letters, digits and "
			       "underscores.";
		}
		if (rc == CLI_CONTROLS_SETUP_BACK)
			continue;
		if (rc < 0)
			return rc;
		if ((nb_str_is_alnum(nb_str_from_cstr(name), "_") &&
		     !((name)[0] >= '0' && (name)[0] <= '9'))) {
			memset(entry->api_key, 0, sizeof(entry->api_key));
			strcpy(entry->api_key_env, name);
			ui->deferred[ui->step] = 1;
			return 0;
		}
	}
}

static int cli_controls_edit_token_count(struct cli_controls_setup_ui *ui,
					 struct config_model_entry *entry, int output)
{
	int *value = output ? &entry->max_tokens : &entry->context_limit;
	int ceiling = cli_setup_token_ceiling(entry, output);
	if (output && ceiling >= entry->context_limit)
		ceiling = entry->context_limit - 1;
	char text[CLI_SETUP_TOKEN_INPUT_MAX] = "";
	if (*value > 0)
		snprintf(text, sizeof(text), "%d", *value);
	nb_buf hint NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&hint), nb_buf_reserve(&hint, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;
	nb_buf_printf(&hint, "Enter %d-%d tokens from your model's documentation.", output ? 1 : 2,
		      ceiling);
	if (nb_buf_status(&hint)) {
		nb_buf_cleanup(&hint);
		return NB_ERR_NOMEM;
	}
	for (;;) {
		rc = cli_controls_edit(ui,
				       output ? "Max output (tokens)" : "Context window (tokens)",
				       nb_buf_cstr(&hint), text, sizeof(text), 0);
		if (rc < 0)
			break;
		rc = cli_setup_parse_token_count(text, output ? 1 : 2, ceiling, value);
		if (rc == 0)
			break;
		nb_buf_clear(&hint);
		nb_buf_printf(&hint, "Invalid token count. Enter a whole number from %d to %d.",
			      output ? 1 : 2, ceiling);
		if (nb_buf_status(&hint)) {
			rc = NB_ERR_NOMEM;
			break;
		}
	}
	nb_buf_cleanup(&hint);
	if (rc < 0)
		return rc;
	if (!output && entry->max_tokens >= entry->context_limit)
		entry->max_tokens = 0;
	return 0;
}

static int cli_controls_ensure_token_limits(struct cli_controls_setup_ui *ui,
					    struct config_model_entry *entry)
{
	if (entry->context_limit <= 0) {
		int rc = cli_controls_edit_token_count(ui, entry, 0);
		if (rc < 0)
			return rc;
	}
	if (entry->max_tokens <= 0)
		return cli_controls_edit_token_count(ui, entry, 1);
	return 0;
}

static int cli_controls_choose_thinking(struct cli_controls_setup_ui *ui,
					struct config_model_entry *entry)
{
	const char *items[] = {"Off · Faster responses", "On · More time to reason"};
	const char *descriptions[] = {"Keep things moving. Thinking is off.",
				      "For complex tasks. Replies take longer."};
	ui->phase = CLI_CONTROLS_SETUP_PREFERENCES;
	int selected = cli_controls_select_menu(ui, "How should morph think?",
						"You can change this later.", items, descriptions,
						2, cli_setup_thinking_enabled(entry));
	int rc;

	if (selected < 0)
		return selected;
	rc = cli_setup_set_thinking(entry, selected);
	if (rc == 0) {
		rc = cli_controls_record_answer(ui, selected ? "Thinking · On" : "Thinking · Off");
	}
	return rc;
}

static int cli_controls_advanced_settings(struct cli_controls_setup_ui *ui,
					  struct config_model_entry *entry)
{
	int chat = ui->step == CLI_SETUP_TEXT || ui->step == CLI_SETUP_VISION;
	ui->phase = CLI_CONTROLS_SETUP_PREFERENCES;

	for (;;) {
		ui->phase = CLI_CONTROLS_SETUP_PREFERENCES;
		nb_buf labels NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = (nb_buf_init(&labels), nb_buf_reserve(&labels, 4096) ? 0 : NB_ERR_NOMEM);
		if (rc < 0)
			return rc;
		/* Store offsets until the buffer stops growing. */
		size_t offsets[5];
		offsets[0] = labels.len;
		nb_buf_printf(&labels, "Model    %s",
			      entry->model[0] ? entry->model : "Choose a model");
		nb_buf_append_char(&labels, '\0');
		offsets[1] = labels.len;
		nb_buf_printf(&labels, "API URL  %s",
			      entry->api_base[0] ? entry->api_base : "Set URL");
		nb_buf_append_char(&labels, '\0');
		offsets[2] = labels.len;
		int key_status = cli_controls_has_key(ui, entry);
		if (key_status == 2)
			nb_buf_append_cstr(&labels, "API key  Save to config (hidden)");
		else if (key_status)
			nb_buf_append_cstr(&labels, "API key  Ready");
		else if (ui->deferred[ui->step] && entry->api_key_env[0])
			nb_buf_printf(&labels, "API key  %s (not exported)", entry->api_key_env);
		else
			nb_buf_append_cstr(&labels, "API key  Not set yet");
		nb_buf_append_char(&labels, '\0');
		for (int i = 3; i < 5; i++) {
			offsets[i] = labels.len;
			int tokens = i == 3 ? entry->context_limit : entry->max_tokens;
			nb_buf_printf(&labels, "%s  ", i == 3 ? "Context window" : "Max output");
			if (tokens > 0)
				nb_buf_printf(&labels, "%d tokens", tokens);
			else
				nb_buf_append_cstr(&labels, "Required for this model");
			nb_buf_append_char(&labels, '\0');
		}
		if (nb_buf_status(&labels)) {
			nb_buf_cleanup(&labels);
			return NB_ERR_NOMEM;
		}
		const char *items[] = {"Done",
				       labels.ptr + offsets[0],
				       labels.ptr + offsets[1],
				       labels.ptr + offsets[2],
				       labels.ptr + offsets[3],
				       labels.ptr + offsets[4]};
		int selected =
			cli_controls_menu(ui, "Advanced settings",
					  "Defaults work for most people. Edit only what you need.",
					  items, chat ? 6 : 4, 0);
		nb_buf_cleanup(&labels);
		if (selected < 0)
			return selected;
		if (selected == 0) {
			if (!entry->model[0])
				selected = 1;
			else if (!entry->api_base[0])
				selected = 2;
			else if (chat) {
				rc = cli_controls_ensure_token_limits(ui, entry);
				if (rc == CLI_CONTROLS_SETUP_BACK)
					continue;
				return rc;
			} else
				return 0;
		}
		if (selected == 1) {
			char previous[sizeof(entry->model)];
			strcpy(previous, entry->model);
			rc = cli_controls_edit(ui, "Model name / endpoint ID",
					       "Use the model name from your provider.",
					       entry->model, sizeof(entry->model), 0);
			if (rc == 0 && chat && strcmp(previous, entry->model) != 0)
				cli_setup_reset_token_limits(entry);
		} else if (selected == 2) {
			char url[sizeof(entry->api_base)];
			strcpy(url, entry->api_base);
			const char *hint = "Use the API base URL, including its version path.";
			for (;;) {
				rc = cli_controls_edit(ui, "API base URL", hint, url, sizeof(url),
						       0);
				if (rc < 0)
					break;
				if ((strncmp(url, "https://", 8) == 0 && url[8]) ||
				    (strncmp(url, "http://", 7) == 0 && url[7])) {
					strcpy(entry->api_base, url);
					break;
				}
				hint = "Enter a URL starting with http:// or https://.";
			}
		} else if (selected >= 4) {
			if (selected == 5 && entry->context_limit <= 0) {
				rc = cli_controls_edit_token_count(ui, entry, 0);
				if (rc == CLI_CONTROLS_SETUP_BACK)
					continue;
				if (rc < 0)
					return rc;
			}
			rc = cli_controls_edit_token_count(ui, entry, selected == 5);
		} else
			rc = cli_controls_credentials(ui, entry);
		if (rc < 0 && rc != CLI_CONTROLS_SETUP_BACK)
			return rc;
	}
}

static int cli_controls_choose_provider(struct cli_controls_setup_ui *ui,
					struct config_model_entry *entry)
{
	const char *text[] = {"OpenAI", "DeepSeek", "Volcengine / Ark", "Other · Compatible"};
	const char *image[] = {"OpenAI", "Volcengine / Ark", "Other · Compatible"};
	const char *video[] = {"Volcengine / Ark", "Other · Compatible"};
	int kind = ui->step;
	int previous = cli_setup_provider_choice(kind, entry);
	int selected =
		cli_controls_menu(ui, "Choose your provider",
				  "Changing provider resets this model's defaults and account.",
				  kind == CLI_SETUP_VIDEO   ? video
				  : kind == CLI_SETUP_IMAGE ? image
							    : text,
				  kind == CLI_SETUP_VIDEO   ? 2
				  : kind == CLI_SETUP_IMAGE ? 3
							    : 4,
				  previous - 1);
	if (selected < 0)
		return selected;
	if (!entry->provider[0] || selected + 1 != previous) {
		cli_setup_model_defaults(kind, selected + 1, entry);
		ui->deferred[kind] = 0;
	}
	return 0;
}

static int cli_controls_configure_model_ui(struct cli_controls_setup_ui *ui)
{
	struct config_model_entry *entry = &ui->entries[ui->step];
	ui->phase = CLI_CONTROLS_SETUP_PREFERENCES;
	if (!entry->provider[0]) {
		const char *items[] = {"Enable with defaults", "Back to overview"};
		int selected =
			cli_controls_menu(ui, "Enable this model?",
					  "Optional. Enable only if you need it.", items, 2, 0);
		if (selected < 0)
			return selected == CLI_CONTROLS_SETUP_BACK ? 0 : selected;
		if (selected == 1)
			return 0;
		cli_setup_prepare_model(ui->state, ui->step);
	}
	for (;;) {
		int thinking = strcmp(entry->adapter, "deepseek") == 0;
		int optional = !cli_setup_role_requires(ui->state->role, ui->step);
		const char *items[] = {"Done →",
				       "Provider",
				       "Model name / endpoint ID",
				       "Account / API key",
				       "Advanced settings",
				       NULL,
				       NULL};
		int count = 5;
		if (thinking)
			items[count++] = cli_setup_thinking_enabled(entry) ? "Thinking · On"
									   : "Thinking · Off";
		if (optional)
			items[count++] = "Disable this model";
		int selected = cli_controls_menu(
			ui, "Model settings", "Edit only what you need; Done returns to overview.",
			items, count, 0);
		if (selected == CLI_CONTROLS_SETUP_BACK || selected == 0)
			return 0;
		if (selected < 0)
			return selected;
		int rc;
		if (selected == 1)
			rc = cli_controls_choose_provider(ui, entry);
		else if (selected == 2) {
			char previous[sizeof(entry->model)];
			strcpy(previous, entry->model);
			rc = cli_controls_edit(ui, "Model name / endpoint ID",
					       "Use the model name from your provider.",
					       entry->model, sizeof(entry->model), 0);
			if (rc == 0 && ui->step <= CLI_SETUP_VISION &&
			    strcmp(previous, entry->model) != 0)
				cli_setup_reset_token_limits(entry);
		} else if (selected == 3) {
			char previous_env[sizeof(entry->api_key_env)];
			strcpy(previous_env, entry->api_key_env);
			rc = cli_controls_credentials(ui, entry);
			if (rc == 0)
				cli_setup_share_credentials(ui->state, ui->step, previous_env);
		} else if (selected == 4) {
			char previous_env[sizeof(entry->api_key_env)];
			strcpy(previous_env, entry->api_key_env);
			rc = cli_controls_advanced_settings(ui, entry);
			if (rc == 0 || rc == CLI_CONTROLS_SETUP_BACK)
				cli_setup_share_credentials(ui->state, ui->step, previous_env);
		} else if (thinking && selected == 5)
			rc = cli_controls_choose_thinking(ui, entry);
		else {
			memset(entry, 0, sizeof(*entry));
			return 0;
		}
		if (rc < 0 && rc != CLI_CONTROLS_SETUP_BACK)
			return rc;
	}
}

static int cli_controls_review(struct cli_controls_setup_ui *ui)
{
	nb_buf labels NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&labels), nb_buf_reserve(&labels, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	int ready = 1;
	int complete = 1;
	size_t offsets[CLI_SETUP_MODEL_COUNT + 1];
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		int required = cli_setup_role_requires(ui->state->role, i);
		int active = required || entry->provider[0];
		int valid = !active || cli_setup_model_complete(i, entry);
		if (!valid)
			complete = 0;
		if (!valid || (active && !cli_controls_has_key(ui, entry)))
			ready = 0;
		offsets[i] = labels.len;
		nb_buf_printf(&labels, "%s · %s%s%s", cli_setup_model_label(i),
			      active ? entry->model[0] ? entry->model : "Model required" : "Off",
			      active ? (cli_controls_has_key(ui, entry) ? " · Key ready"
									: " · Key needed")
				     : "",
			      required ? "" : " (optional)");
		nb_buf_append_char(&labels, '\0');
	}
	offsets[CLI_SETUP_MODEL_COUNT] = labels.len;
	nb_buf_printf(&labels, "Instructions · %s",
		      ui->state->replace_prompt	   ? "Use role prompt"
		      : ui->state->existing_prompt ? "Keep existing"
						   : "Built-in");
	if (nb_buf_status(&labels)) {
		nb_buf_cleanup(&labels);
		return NB_ERR_NOMEM;
	}
	const char *items[CLI_CONTROLS_SETUP_ACTION_COUNT] = {
		!complete ? "Complete required settings →"
		: ready	  ? "Save and exit →"
			  : "Connect accounts and save →"};
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		items[i + 1] = labels.ptr + offsets[i];
	items[CLI_CONTROLS_SETUP_CHANGE_ROLE] = "Change role";
	items[CLI_CONTROLS_SETUP_INSTRUCTIONS] = labels.ptr + offsets[CLI_SETUP_MODEL_COUNT];
	items[CLI_CONTROLS_SETUP_SAVE_LATER] = "Save for later (without connecting)";
	items[CLI_CONTROLS_SETUP_CANCEL] = "Cancel without saving";
	ui->phase = CLI_CONTROLS_SETUP_START;
	ui->step = CLI_SETUP_MODEL_COUNT;
	int selected = cli_controls_menu(ui, "Review your setup",
					 "Defaults are ready. Select a setting only to change it.",
					 items, CLI_CONTROLS_SETUP_ACTION_COUNT, 0);
	nb_buf_cleanup(&labels);
	return selected;
}

static int cli_controls_finish_setup(struct cli_controls_setup_ui *ui, int connect)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		if ((!cli_setup_role_requires(ui->state->role, i) && !entry->provider[0]) ||
		    cli_setup_model_complete(i, entry))
			continue;
		ui->step = i;
		ui->phase = CLI_CONTROLS_SETUP_PREFERENCES;
		int rc = cli_controls_advanced_settings(ui, entry);
		return rc < 0 && rc != CLI_CONTROLS_SETUP_BACK ? rc : 1;
	}
	for (int i = 0; connect && i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &ui->entries[i];
		if (!entry->model[0] || cli_controls_has_key(ui, entry))
			continue;
		ui->step = i;
		char previous_env[sizeof(entry->api_key_env)];
		strcpy(previous_env, entry->api_key_env);
		int rc = cli_controls_credentials(ui, entry);
		if (rc == CLI_CONTROLS_SETUP_BACK)
			return 1;
		if (rc != 0)
			return rc;
		cli_setup_share_credentials(ui->state, i, previous_env);
		if (!cli_controls_has_key(ui, entry))
			break;
	}
	ui->step = CLI_SETUP_MODEL_COUNT;
	return cli_controls_record_answer(ui, "Configuration confirmed");
}

int cli_setup_controls_collect(struct cli_application *ctx, const char *path, FILE *input,
			       FILE *output, struct cli_setup_state *state)
{
	struct cli_controls_setup_ui ui = {.output = output,
					   .fd = fileno(input),
					   .output_fd = fileno(output),
					   .color = cli_output_color_enabled(ctx) &&
						    !getenv("NO_COLOR"),
					   .step = -1,
					   .entries = state->entries,
					   .state = state};
	int signal_rc = nb_signal_listener_start(
		&ui.signals, (const int[]){SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH}, 5,
		cli_controls_setup_signal, &ui, NULL);

	if (signal_rc != 0)
		return signal_rc;
	int enter_rc = nt_screen_open(&ui.screen, input, output,
				      NT_TERMINAL_PASTE | (ui.color ? 0 : NT_TERMINAL_NO_SGR));

	ui.screen.interrupted = &ui.interrupted;
	if (enter_rc != 0) {
		nb_signal_listener_stop(&ui.signals, NULL);
		return enter_rc;
	}
	fputs("\r\n  morph  /  Setup\r\n\r\n", output);
	(void)path;
	int rc = 0;
	for (;;) {
		if (ui.step < 0) {
			const char *items[CLI_SETUP_ROLE_COUNT];
			const char *descriptions[CLI_SETUP_ROLE_COUNT];
			for (int i = 0; i < CLI_SETUP_ROLE_COUNT; i++) {
				struct cli_setup_role role = cli_setup_role_get(i);
				items[i] = role.label;
				descriptions[i] = role.description;
			}
			ui.phase = CLI_CONTROLS_SETUP_ROLE;
			rc = cli_controls_select_menu(
				&ui, ui.role_confirmed ? "Change your role" : "Choose your role",
				NULL, items, descriptions, CLI_SETUP_ROLE_COUNT, state->role);
			if (rc == CLI_CONTROLS_SETUP_BACK && ui.role_confirmed) {
				ui.step = CLI_SETUP_MODEL_COUNT;
				continue;
			}
			if (rc < 0) {
				if (rc == CLI_CONTROLS_SETUP_BACK)
					rc = NB_ERR_CANCELLED;
				break;
			}
			int changed = !ui.role_confirmed || state->role != rc;
			state->role = rc;
			if (!ui.role_confirmed)
				state->replace_prompt = !state->existing_prompt;
			cli_setup_role_defaults(state);
			ui.role_confirmed = 1;
			rc = changed ? cli_controls_record_answer(
					       &ui, cli_setup_role_get(state->role).label)
				     : 0;
			if (rc < 0)
				break;
			ui.step = CLI_SETUP_MODEL_COUNT;
		}
		rc = cli_controls_review(&ui);
		if (rc == CLI_CONTROLS_SETUP_BACK || rc == CLI_CONTROLS_SETUP_CHANGE_ROLE) {
			ui.step = -1;
			continue;
		}
		if (rc < 0)
			break;
		if (rc == CLI_CONTROLS_SETUP_CANCEL) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (rc == CLI_CONTROLS_SETUP_SAVE || rc == CLI_CONTROLS_SETUP_SAVE_LATER) {
			rc = cli_controls_finish_setup(&ui, rc == CLI_CONTROLS_SETUP_SAVE);
			if (rc <= 0)
				break;
		} else if (rc == CLI_CONTROLS_SETUP_INSTRUCTIONS) {
			const char *items[] = {state->existing_prompt ? "Keep existing instructions"
								      : "Use built-in instructions",
					       "Use role instructions"};
			rc = cli_controls_menu(&ui, "Prompt instructions",
					       "Existing files are preserved.", items, 2,
					       state->replace_prompt);
			if (rc >= 0)
				state->replace_prompt = rc;
		} else {
			ui.step = rc - 1;
			rc = cli_controls_configure_model_ui(&ui);
		}
		if (rc < 0 && rc != CLI_CONTROLS_SETUP_BACK)
			break;
		ui.step = CLI_SETUP_MODEL_COUNT;
	}
	cli_controls_dimensions(&ui);
	int ending_rc = nt_inline_region_commit(&ui.region, ui.output, NULL, ui.columns, ui.rows);
	if (rc == 0 && ending_rc < 0)
		rc = ending_rc;
	int leave_rc = nt_screen_close(&ui.screen);
	if (!leave_rc && fputc('\r', output) == EOF)
		leave_rc = NB_ERR_IO;

	if (rc == 0)
		rc = leave_rc;
	nb_signal_listener_stop(&ui.signals, NULL);
	if (rc < 0)
		return rc;
	return 0;
}
