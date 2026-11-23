#include "sapi/cli/components/setup_form.h"

#include "sapi/cli/components/shortcut_hint.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/configuration/mcp.h"
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/str.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/check_box.h>
#include <nothing/tui/controls/drop_down.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/input.h>
#include <nothing/tui/keymap.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/card_view.h>
#include <nothing/tui/views/form_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/window.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_SETUP_FORM_FOCUS_MAX 32
#define CLI_SETUP_FORM_DRAFT_MAX 4096
#define CLI_SETUP_FORM_MESSAGE_MAX 160
#define CLI_SETUP_FORM_LABEL_WIDTH 14
#define CLI_SETUP_FORM_STACK_BELOW 64

enum cli_setup_form_field_kind {
	CLI_SETUP_FIELD_TEXT,
	CLI_SETUP_FIELD_DROPDOWN,
	CLI_SETUP_FIELD_CHECK,
	CLI_SETUP_FIELD_BUTTON,
	CLI_SETUP_FIELD_TAB
};

enum cli_setup_form_field {
	CLI_SETUP_FIELD_ROLE,
	CLI_SETUP_FIELD_ENABLED,
	CLI_SETUP_FIELD_PROVIDER,
	CLI_SETUP_FIELD_MODEL,
	CLI_SETUP_FIELD_ACCOUNT,
	CLI_SETUP_FIELD_API_KEY,
	CLI_SETUP_FIELD_API_KEY_ENV,
	CLI_SETUP_FIELD_CONTEXT,
	CLI_SETUP_FIELD_MAX_TOKENS,
	CLI_SETUP_FIELD_API_BASE,
	CLI_SETUP_FIELD_THINKING,
	CLI_SETUP_FIELD_INSTRUCTIONS,
	CLI_SETUP_FIELD_SAVE,
	CLI_SETUP_FIELD_SAVE_LATER,
	CLI_SETUP_FIELD_CANCEL,
	CLI_SETUP_FIELD_ADVANCED,
	CLI_SETUP_FIELD_TABS,
	CLI_SETUP_FIELD_COUNT
};

enum cli_setup_form_account_mode {
	CLI_SETUP_ACCOUNT_ENV,
	CLI_SETUP_ACCOUNT_KEY,
	CLI_SETUP_ACCOUNT_LATER
};

struct cli_setup_form_focus {
	enum cli_setup_form_field field;
	enum cli_setup_form_field_kind kind;
	int model;
	struct nt_view *view;
};

struct cli_setup_form_draft {
	char *text;
	size_t cursor;
};

struct cli_setup_form {
	struct nt_stack_view base;
	struct cli_setup_state *state;
	struct nt_builder *builder;
	char path[PATH_MAX];
	int styled, wide, model, rebuilding, needs_rebuild, reveal_focus, compact;
	int layout_width, layout_height;
	int account_mode[CLI_SETUP_MODEL_COUNT];
	int advanced[CLI_SETUP_MODEL_COUNT];
	struct nt_view *header, *path_view, *role_note, *tabs, *scroll, *content;
	struct nt_view *status, *hint, *actions, *account_note;
	struct cli_setup_form_focus focus[CLI_SETUP_FORM_FOCUS_MAX];
	size_t focus_count, fixed_count;
	enum cli_setup_form_field target_field;
	int target_model;
	struct cli_setup_form_draft drafts[CLI_SETUP_FIELD_COUNT];
	char error[CLI_SETUP_FORM_MESSAGE_MAX];
	char message[CLI_SETUP_FORM_MESSAGE_MAX];
	enum nt_theme_role message_role;
};

static int cli_setup_form_event(void *data, const struct nt_view_event *event);
static int cli_setup_form_update_limits(struct cli_setup_form *form, int model);

static struct nt_text_style cli_setup_form_style(const struct cli_setup_form *form,
						 enum nt_theme_role role, unsigned attributes)
{
	struct nt_text_style style = {0};
	if (!form->styled)
		return style;

	style = nt_theme_style(nt_theme_default(), role, NT_VISUAL_NORMAL);
	style.attributes |= attributes;
	return style;
}

static struct nt_view *cli_setup_form_label(struct cli_setup_form *form, struct nt_builder *builder,
					    struct nt_view *parent, const char *text,
					    enum nt_theme_role role, unsigned attributes, int width)
{
	struct nt_view_options options = {
		.width = width, .height = 1, .style = cli_setup_form_style(form, role, attributes)};
	struct nt_view *label = nt_label_add(builder, parent, text ? text : "", &options);
	if (label)
		nt_builder_fail(builder, nt_label_set_wrap_mode(label, NT_TEXT_WRAP_NONE));

	return label;
}

static struct nt_view *cli_setup_form_note(struct cli_setup_form *form, struct nt_builder *builder,
					   struct nt_view *parent, const char *text)
{
	struct nt_view *label = nt_label_add(
		builder, parent, text ? text : "",
		&(struct nt_view_options){.style = cli_setup_form_style(form, NT_THEME_MUTED, 0)});
	if (label)
		nt_builder_fail(builder, nt_label_set_wrap_mode(label, NT_TEXT_WRAP_WORD));

	return label;
}

static void cli_setup_form_blank(struct nt_builder *builder, struct nt_view *parent)
{
	nt_label_add(builder, parent, "", &(struct nt_view_options){.height = 1});
}

static void cli_setup_form_add_focus(struct cli_setup_form *form, enum cli_setup_form_field field,
				     enum cli_setup_form_field_kind kind, int model,
				     struct nt_view *view)
{
	if (!view || form->focus_count >= CLI_SETUP_FORM_FOCUS_MAX)
		return;

	form->focus[form->focus_count++] = (struct cli_setup_form_focus){
		.field = field, .kind = kind, .model = model, .view = view};
}

static struct nt_view *cli_setup_form_host(struct cli_setup_form *form, struct nt_builder *builder,
					   struct nt_view *parent, const char *label)
{
	if (form->wide) {
		struct nt_view *row =
			nt_row_add(builder, parent, 1, &(struct nt_view_options){.width = NT_FILL});
		cli_setup_form_label(form, builder, row, label, NT_THEME_MUTED, 0,
				     CLI_SETUP_FORM_LABEL_WIDTH);
		return row;
	}

	cli_setup_form_label(form, builder, parent, label, NT_THEME_MUTED, 0, 0);
	return parent;
}

static struct nt_view *cli_setup_form_add_text(struct cli_setup_form *form,
					       struct nt_builder *builder, struct nt_view *parent,
					       const char *label, const char *text, size_t capacity,
					       int secret, enum cli_setup_form_field field,
					       int model)
{
	struct nt_view *host = cli_setup_form_host(form, builder, parent, label);
	struct nt_view *view = nt_text_field_add(
		builder, host,
		&(struct nt_text_field_options){.text = text ? text : "", .limit = capacity},
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	if (view && secret)
		nt_builder_fail(builder,
				nt_text_field_set_echo_mode(view, NT_TEXT_FIELD_ECHO_PASSWORD));

	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_TEXT, model, view);
	return view;
}

static struct nt_view *cli_setup_form_add_dropdown(struct cli_setup_form *form,
						   struct nt_builder *builder,
						   struct nt_view *parent, const char *label,
						   const char *const *items, size_t count,
						   size_t selected, enum cli_setup_form_field field,
						   int model)
{
	struct nt_view *host = cli_setup_form_host(form, builder, parent, label);
	struct nt_view *view = nt_drop_down_add(
		builder, host,
		&(struct nt_drop_down_options){
			.items = items, .count = count, .selected = selected, .tag = (int)field},
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_DROPDOWN, model, view);
	return view;
}

static struct nt_view *cli_setup_form_add_check(struct cli_setup_form *form,
						struct nt_builder *builder, struct nt_view *parent,
						const char *label, int checked,
						enum cli_setup_form_field field, int model)
{
	struct nt_view *view = nt_check_box_add(builder, parent, label, checked, (int)field,
						&(struct nt_view_options){.height = 1});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_CHECK, model, view);
	return view;
}

static void cli_setup_form_add_button(struct cli_setup_form *form, struct nt_builder *builder,
				      struct nt_view *parent, const char *label,
				      enum cli_setup_form_field field)
{
	struct nt_view *view = nt_button_add(builder, parent, label, (int)field,
					     &(struct nt_view_options){.height = 1});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_BUTTON, -1, view);
}

static int cli_setup_form_has_key(const struct cli_setup_form *form, int model)
{
	const struct config_model_entry *entry = &form->state->entries[model];
	if (form->account_mode[model] == CLI_SETUP_ACCOUNT_KEY)
		return entry->api_key[0] != '\0';

	if (form->account_mode[model] == CLI_SETUP_ACCOUNT_LATER)
		return 0;

	const char *value = entry->api_key_env[0] ? getenv(entry->api_key_env) : NULL;
	return value && *value;
}

static int cli_setup_form_complete_models(const struct cli_setup_form *form)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		if ((cli_setup_role_requires(form->state->role, i) ||
		     form->state->entries[i].provider[0]) &&
		    !cli_setup_model_complete(i, &form->state->entries[i]))
			return 0;

	return 1;
}

static void cli_setup_form_message_update(struct cli_setup_form *form)
{
	if (form->error[0]) {
		form->message_role = NT_THEME_ERROR;
		snprintf(form->message, sizeof(form->message), CLI_FEED_FAILED " %s", form->error);
		return;
	}

	if (!cli_setup_form_complete_models(form)) {
		form->message_role = NT_THEME_ERROR;
		snprintf(form->message, sizeof(form->message),
			 CLI_FEED_FAILED " Complete the active model settings.");
		return;
	}

	int missing = 0;
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		const struct config_model_entry *entry = &form->state->entries[i];
		int required = cli_setup_role_requires(form->state->role, i);
		if ((required || entry->provider[0]) && !cli_setup_form_has_key(form, i))
			missing++;
	}

	if (missing) {
		form->message_role = NT_THEME_WARNING;
		snprintf(form->message, sizeof(form->message),
			 CLI_FEED_WARNING " %d account%s need%s a key.", missing,
			 missing == 1 ? "" : "s", missing == 1 ? "s" : "");
		return;
	}

	form->message_role = NT_THEME_SUCCESS;
	snprintf(form->message, sizeof(form->message), CLI_FEED_DONE " Ready to save.");
}

static int cli_setup_form_provider_items(int kind, const char *const **items)
{
	static const char *const chat[] = {"OpenAI", "DeepSeek", "Volcengine / Ark",
					   "Other · Compatible"};
	static const char *const image[] = {"OpenAI", "Volcengine / Ark", "Other · Compatible"};
	static const char *const video[] = {"Volcengine / Ark", "Other · Compatible"};
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) {
		*items = chat;
		return 4;
	}

	if (kind == CLI_SETUP_IMAGE) {
		*items = image;
		return 3;
	}

	*items = video;
	return 2;
}

static void cli_setup_form_add_instructions(struct cli_setup_form *form, struct nt_builder *builder,
					    struct nt_view *parent)
{
	const char *items[] = {form->state->existing_prompt ? "Keep existing instructions"
							    : "Use built-in instructions",
			       "Use role instructions"};
	cli_setup_form_add_dropdown(form, builder, parent, "Instructions", items, 2,
				    form->state->replace_prompt ? 1 : 0,
				    CLI_SETUP_FIELD_INSTRUCTIONS, -1);
}

static void cli_setup_form_account_modes_init(struct cli_setup_form *form)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		const struct config_model_entry *entry = &form->state->entries[i];
		if (entry->api_key[0])
			form->account_mode[i] = CLI_SETUP_ACCOUNT_KEY;
		else if (entry->api_key_env[0])
			form->account_mode[i] = CLI_SETUP_ACCOUNT_ENV;
		else
			form->account_mode[i] = CLI_SETUP_ACCOUNT_LATER;
	}
}

static void cli_setup_form_account_modes_refresh(struct cli_setup_form *form, int model)
{
	const struct config_model_entry *entry = &form->state->entries[model];
	if (entry->api_key[0])
		form->account_mode[model] = CLI_SETUP_ACCOUNT_KEY;
	else if (entry->api_key_env[0])
		form->account_mode[model] = CLI_SETUP_ACCOUNT_ENV;
	else
		form->account_mode[model] = CLI_SETUP_ACCOUNT_LATER;
}

static int cli_setup_form_trim(char *text)
{
	if (!text)
		return 0;

	char *start = text;
	while (*start == ' ' || *start == '\t')
		start++;

	char *end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;

	*end = '\0';
	if (start != text)
		memmove(text, start, strlen(start) + 1);

	return text[0] != '\0';
}

static int cli_setup_form_url_valid(const char *url)
{
	return (strncmp(url, "https://", 8) == 0 && url[8]) ||
	       (strncmp(url, "http://", 7) == 0 && url[7]);
}

static int cli_setup_form_env_valid(const char *name)
{
	if (!name || !*name)
		return 1;

	return nb_str_is_alnum(nb_str_from_cstr(name), "_") && !(name[0] >= '0' && name[0] <= '9');
}

static int cli_setup_form_text_commit(struct cli_setup_form *form,
				      const struct cli_setup_form_focus *item)
{
	struct config_model_entry *entry = &form->state->entries[item->model];
	const char *text = nt_text_field_text(item->view);
	char value[CLI_SETUP_FORM_DRAFT_MAX];
	snprintf(value, sizeof(value), "%s", text ? text : "");
	int present = cli_setup_form_trim(value);

	switch (item->field) {
	case CLI_SETUP_FIELD_MODEL: {
		if (!present) {
			snprintf(form->error, sizeof(form->error), "Enter a model name.");
			return NB_ERR_INVAL;
		}

		if (strcmp(entry->model, value) != 0) {
			snprintf(entry->model, sizeof(entry->model), "%s", value);
			if (item->model == CLI_SETUP_TEXT || item->model == CLI_SETUP_VISION)
				cli_setup_reset_token_limits(entry);

			return cli_setup_form_update_limits(form, item->model);
		}

		return 0;
	}
	case CLI_SETUP_FIELD_API_BASE:
		if (!present || !cli_setup_form_url_valid(value)) {
			snprintf(form->error, sizeof(form->error),
				 "Use an http:// or https:// URL.");
			return NB_ERR_INVAL;
		}

		snprintf(entry->api_base, sizeof(entry->api_base), "%s", value);
		return 0;
	case CLI_SETUP_FIELD_CONTEXT: {
		int ceiling = cli_setup_token_ceiling(entry, 0);
		if (!present ||
		    cli_setup_parse_token_count(value, 2, ceiling, &entry->context_limit) != 0) {
			snprintf(form->error, sizeof(form->error),
				 "Enter a whole token count from 2 to %d.", ceiling);
			return NB_ERR_INVAL;
		}

		if (entry->max_tokens >= entry->context_limit)
			entry->max_tokens = 0;

		return 0;
	}
	case CLI_SETUP_FIELD_MAX_TOKENS: {
		int ceiling = cli_setup_token_ceiling(entry, 1);
		if (ceiling >= entry->context_limit)
			ceiling = entry->context_limit > 1 ? entry->context_limit - 1 : 1;

		if (!present ||
		    cli_setup_parse_token_count(value, 1, ceiling, &entry->max_tokens) != 0) {
			snprintf(form->error, sizeof(form->error),
				 "Enter a whole token count from 1 to %d.", ceiling);
			return NB_ERR_INVAL;
		}

		return 0;
	}
	case CLI_SETUP_FIELD_API_KEY:
		snprintf(entry->api_key, sizeof(entry->api_key), "%s", text ? text : "");
		return 0;
	case CLI_SETUP_FIELD_API_KEY_ENV:
		if (!cli_setup_form_env_valid(value)) {
			snprintf(form->error, sizeof(form->error),
				 "Start with a letter or underscore; use letters, digits and "
				 "underscores.");
			return NB_ERR_INVAL;
		}

		snprintf(entry->api_key_env, sizeof(entry->api_key_env), "%s", value);
		return 0;
	default:
		return 0;
	}
}

static void cli_setup_form_apply_check(struct cli_setup_form *form,
				       const struct cli_setup_form_focus *item)
{
	if (item->field != CLI_SETUP_FIELD_ENABLED)
		return;

	int checked = nt_check_box_is_checked(item->view);
	struct config_model_entry *entry = &form->state->entries[item->model];
	if (checked) {
		if (!entry->provider[0])
			cli_setup_prepare_model(form->state, item->model);

		cli_setup_form_account_modes_refresh(form, item->model);
	} else
		memset(entry, 0, sizeof(*entry));

	form->needs_rebuild = 1;
}

static int cli_setup_form_validate_active(struct cli_setup_form *form, char *error, size_t capacity,
					  int defer_accounts)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &form->state->entries[i];
		int required = cli_setup_role_requires(form->state->role, i);
		int active = required || entry->provider[0];
		if (!active)
			continue;

		if (!entry->provider[0]) {
			snprintf(error, capacity, "Choose a provider for the %s.",
				 cli_setup_model_label(i));
			form->target_field = CLI_SETUP_FIELD_PROVIDER;
			form->target_model = i;
			return NB_ERR_INVAL;
		}

		if (!entry->model[0]) {
			snprintf(error, capacity, "Enter a model name for the %s.",
				 cli_setup_model_label(i));
			form->target_field = CLI_SETUP_FIELD_MODEL;
			form->target_model = i;
			return NB_ERR_INVAL;
		}

		if (!cli_setup_form_url_valid(entry->api_base)) {
			snprintf(error, capacity, "Use an http:// or https:// API URL.");
			form->target_field = CLI_SETUP_FIELD_API_BASE;
			form->target_model = i;
			return NB_ERR_INVAL;
		}

		if (!cli_setup_model_complete(i, entry)) {
			snprintf(error, capacity, "Complete the %s token limits.",
				 cli_setup_model_label(i));
			form->target_field = CLI_SETUP_FIELD_CONTEXT;
			form->target_model = i;
			return NB_ERR_INVAL;
		}

		if (form->account_mode[i] == CLI_SETUP_ACCOUNT_ENV) {
			if (!entry->api_key_env[0] ||
			    !cli_setup_form_env_valid(entry->api_key_env)) {
				snprintf(error, capacity,
					 "Use letters, digits and underscores for the API key "
					 "variable.");
				form->target_field = CLI_SETUP_FIELD_API_KEY_ENV;
				form->target_model = i;
				return NB_ERR_INVAL;
			}

			entry->api_key[0] = '\0';
		} else if (form->account_mode[i] == CLI_SETUP_ACCOUNT_KEY) {
			if (!entry->api_key[0]) {
				if (defer_accounts)
					continue;

				snprintf(error, capacity,
					 "Enter an API key or choose Set up later.");
				form->target_field = CLI_SETUP_FIELD_API_KEY;
				form->target_model = i;
				return NB_ERR_INVAL;
			}

			entry->api_key_env[0] = '\0';
		} else
			entry->api_key[0] = '\0';
	}

	return 0;
}

static const struct nt_view_class cli_setup_form_class;

static struct cli_setup_form_focus *cli_setup_form_find(struct cli_setup_form *form,
							enum cli_setup_form_field field, int model)
{
	for (size_t i = 0; i < form->focus_count; i++) {
		if (form->focus[i].field == field && form->focus[i].model == model)
			return &form->focus[i];
	}

	return NULL;
}

static int cli_setup_form_status_update(struct cli_setup_form *form)
{
	cli_setup_form_message_update(form);
	int rc = nt_label_set_text(form->status, form->message);
	if (!rc)
		rc = nt_view_set_style(form->status,
				       cli_setup_form_style(form, form->message_role, 0));

	if (!rc && form->account_note && form->account_mode[form->model] == CLI_SETUP_ACCOUNT_ENV)
		rc = nt_label_set_text(
			form->account_note,
			cli_setup_form_has_key(form, form->model)
				? "Key found in your environment. Its value stays hidden."
				: "Key not found. You can save now and export it later.");

	return rc;
}

static int cli_setup_form_update_limits(struct cli_setup_form *form, int model)
{
	const struct config_model_entry *entry = &form->state->entries[model];
	for (size_t i = 0; i < form->focus_count; i++) {
		const struct cli_setup_form_focus *item = &form->focus[i];
		if (item->model != model || (item->field != CLI_SETUP_FIELD_CONTEXT &&
					     item->field != CLI_SETUP_FIELD_MAX_TOKENS))
			continue;

		char value[CLI_SETUP_TOKEN_INPUT_MAX];
		snprintf(value, sizeof(value), "%d",
			 item->field == CLI_SETUP_FIELD_CONTEXT ? entry->context_limit
								: entry->max_tokens);
		int rc = nt_text_field_set_text(item->view, value);
		if (rc)
			return rc;
	}

	return NB_OK;
}

static void cli_setup_form_invalidate(struct cli_setup_form *form)
{
	form->needs_rebuild = 1;
	nt_view_invalidate(&form->base.base, 1);
}

static int cli_setup_form_commit_all(struct cli_setup_form *form)
{
	for (size_t i = 0; i < form->focus_count; i++) {
		struct cli_setup_form_focus *item = &form->focus[i];
		if (item->kind != CLI_SETUP_FIELD_TEXT)
			continue;

		int rc = cli_setup_form_text_commit(form, item);
		if (rc) {
			form->target_field = item->field;
			form->target_model = item->model;
			form->reveal_focus = 1;
			return rc;
		}
	}

	form->error[0] = '\0';
	return NB_OK;
}

static void cli_setup_form_build_model(struct cli_setup_form *form)
{
	form->account_note = NULL;
	struct nt_builder *builder = form->builder;
	struct nt_view *parent = form->content;
	int kind = form->model;
	struct config_model_entry *entry = &form->state->entries[kind];
	int required = cli_setup_role_requires(form->state->role, kind);
	cli_setup_form_label(form, builder, parent, cli_setup_model_label(kind), NT_THEME_TEXT,
			     NT_STYLE_BOLD, 0);
	if (required)
		cli_setup_form_note(form, builder, parent, "Required for your selected role.");
	else {
		cli_setup_form_add_check(form, builder, parent, "Enable this model",
					 entry->provider[0] != '\0', CLI_SETUP_FIELD_ENABLED, kind);
		if (!entry->provider[0]) {
			cli_setup_form_note(form, builder, parent,
					    "Optional. Enable it when you need this capability.");
			return;
		}
	}

	cli_setup_form_blank(builder, parent);
	const char *const *providers = NULL;
	int count = cli_setup_form_provider_items(kind, &providers);
	int provider = cli_setup_provider_choice(kind, entry);
	cli_setup_form_add_dropdown(form, builder, parent, "Provider", providers, (size_t)count,
				    (size_t)(provider > 0 && provider <= count ? provider - 1 : 0),
				    CLI_SETUP_FIELD_PROVIDER, kind);
	cli_setup_form_add_text(form, builder, parent, "Model", entry->model, sizeof(entry->model),
				0, CLI_SETUP_FIELD_MODEL, kind);
	static const char *const accounts[] = {"Use environment variable", "Save key to config",
					       "Set up later"};
	int mode = form->account_mode[kind];
	cli_setup_form_add_dropdown(form, builder, parent, "Account", accounts, 3, (size_t)mode,
				    CLI_SETUP_FIELD_ACCOUNT, kind);
	if (mode == CLI_SETUP_ACCOUNT_ENV) {
		cli_setup_form_add_text(form, builder, parent, "Variable", entry->api_key_env,
					sizeof(entry->api_key_env), 0, CLI_SETUP_FIELD_API_KEY_ENV,
					kind);
		form->account_note = cli_setup_form_note(
			form, builder, parent,
			cli_setup_form_has_key(form, kind)
				? "Key found in your environment. Its value stays hidden."
				: "Key not found. You can save now and export it later.");
	} else if (mode == CLI_SETUP_ACCOUNT_KEY) {
		cli_setup_form_add_text(form, builder, parent, "API key", entry->api_key,
					sizeof(entry->api_key), 1, CLI_SETUP_FIELD_API_KEY, kind);
		form->account_note = cli_setup_form_note(
			form, builder, parent, "Stored in your private configuration file.");
	} else {
		cli_setup_form_note(form, builder, parent,
				    "You can finish setting up this account later.");
	}

	cli_setup_form_blank(builder, parent);
	cli_setup_form_add_check(form, builder, parent, "Advanced settings", form->advanced[kind],
				 CLI_SETUP_FIELD_ADVANCED, kind);
	if (!form->advanced[kind])
		return;

	cli_setup_form_add_text(form, builder, parent, "API base", entry->api_base,
				sizeof(entry->api_base), 0, CLI_SETUP_FIELD_API_BASE, kind);
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) {
		char value[CLI_SETUP_TOKEN_INPUT_MAX];
		snprintf(value, sizeof(value), "%d", entry->context_limit);
		cli_setup_form_add_text(form, builder, parent, "Context", value, sizeof(value), 0,
					CLI_SETUP_FIELD_CONTEXT, kind);
		snprintf(value, sizeof(value), "%d", entry->max_tokens);
		cli_setup_form_add_text(form, builder, parent, "Max output", value, sizeof(value),
					0, CLI_SETUP_FIELD_MAX_TOKENS, kind);
		if (!strcmp(entry->adapter, "deepseek")) {
			static const char *const thinking[] = {"Off", "On"};
			cli_setup_form_add_dropdown(form, builder, parent, "Thinking", thinking, 2,
						    cli_setup_thinking_enabled(entry) ? 1 : 0,
						    CLI_SETUP_FIELD_THINKING, kind);
		}
	}

	if (kind == CLI_SETUP_TEXT) {
		cli_setup_form_blank(builder, parent);
		cli_setup_form_label(form, builder, parent, "Role defaults", NT_THEME_TEXT,
				     NT_STYLE_BOLD, 0);
		nb_buf integrations NB_CLEANUP(nb_buf_cleanup) = {0};
		if (!nb_buf_printf(&integrations, "Integrations: %s",
				   cli_setup_mcp_summary(form->state->role))) {
			nt_builder_fail(builder, nb_buf_status(&integrations));
			return;
		}

		cli_setup_form_note(form, builder, parent, nb_buf_cstr(&integrations));
		cli_setup_form_note(form, builder, parent,
				    "Read/media auto-approved · host exec on");
	}
}

static int cli_setup_form_drafts_capture(struct cli_setup_form *form)
{
	for (size_t i = form->fixed_count; i < form->focus_count; i++) {
		const struct cli_setup_form_focus *item = &form->focus[i];
		if (item->kind != CLI_SETUP_FIELD_TEXT)
			continue;

		char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nt_text_field_text(item->view));
		if (!copy)
			return NB_ERR_NOMEM;

		struct cli_setup_form_draft *draft = &form->drafts[item->field];
		free(draft->text);
		draft->text = nb_charp_steal(&copy);
		draft->cursor = nt_text_field_cursor(item->view);
	}

	return NB_OK;
}

static int cli_setup_form_drafts_restore(struct cli_setup_form *form)
{
	for (size_t i = form->fixed_count; i < form->focus_count; i++) {
		struct cli_setup_form_focus *item = &form->focus[i];
		struct cli_setup_form_draft *draft = &form->drafts[item->field];
		if (item->kind != CLI_SETUP_FIELD_TEXT || !draft->text)
			continue;

		int rc = nt_text_field_set_text(item->view, draft->text);
		if (!rc)
			rc = nt_text_field_set_cursor(item->view, draft->cursor);

		free(draft->text);
		draft->text = NULL;
		if (rc)
			return rc;
	}

	return NB_OK;
}

static int cli_setup_form_rebuild(struct cli_setup_form *form, int wide)
{
	int resize = !form->needs_rebuild;
	int rc = resize ? cli_setup_form_drafts_capture(form) : NB_OK;
	if (rc)
		return rc;

	form->rebuilding = 1;
	form->wide = wide;
	form->needs_rebuild = 0;
	form->focus_count = form->fixed_count;
	rc = nt_view_clear_children(form->content);
	if (!rc) {
		cli_setup_form_build_model(form);
		rc = nt_builder_error(form->builder);
	}

	if (!rc && resize)
		rc = cli_setup_form_drafts_restore(form);

	if (!rc)
		rc = nt_label_set_text(form->role_note,
				       cli_setup_role_get(form->state->role).description);

	if (!rc)
		rc = cli_setup_form_status_update(form);

	form->reveal_focus = 1;
	form->rebuilding = 0;
	return rc;
}

static int cli_setup_form_layout(struct nt_view *view)
{
	struct cli_setup_form *form = (struct cli_setup_form *)view;
	struct nt_view_rect rect = nt_view_content_rect(view);
	if (rect.width != form->layout_width || rect.height != form->layout_height) {
		form->layout_width = rect.width;
		form->layout_height = rect.height;
		form->reveal_focus = 1;
	}

	int wide = rect.width >= CLI_SETUP_FORM_STACK_BELOW;
	int rc = NB_OK;
	rc = nt_view_set_hidden(form->path_view, rect.height < 20);
	if (!rc)
		rc = nt_view_set_hidden(form->role_note, rect.height < 16);

	int compact = rect.width < 56;
	if (!rc && compact != form->compact) {
		struct cli_setup_form_focus *save =
			cli_setup_form_find(form, CLI_SETUP_FIELD_SAVE, -1);
		struct cli_setup_form_focus *later =
			cli_setup_form_find(form, CLI_SETUP_FIELD_SAVE_LATER, -1);
		rc = nt_button_set_label(save->view, compact ? "Save" : "Save and exit");
		if (!rc)
			rc = nt_button_set_label(later->view, compact ? "Later" : "Save for later");

		if (!rc)
			rc = cli_shortcut_hint_set_text(form->hint,
							"F10 Save · Tab Next · Esc Cancel");

		if (!rc)
			form->compact = compact;
	}

	if (rc)
		return rc;

	char shown_path[PATH_MAX];
	char *safe_path NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		form->path, strlen(form->path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe_path)
		return NB_ERR_NOMEM;

	morph_terminal_copy_ellipsized_display_width(shown_path, sizeof(shown_path), safe_path,
						     (size_t)(rect.width > 0 ? rect.width : 1), 1);
	rc = nt_label_set_text(form->path_view, shown_path);
	if (rc)
		return rc;

	if (form->needs_rebuild || form->wide != wide)
		rc = cli_setup_form_rebuild(form, wide);

	if (!rc)
		rc = nt_view_super_methods(view, &cli_setup_form_class)->layout(view);

	if (!rc && form->reveal_focus) {
		form->reveal_focus = 0;
		struct cli_setup_form_focus *item =
			cli_setup_form_find(form, form->target_field, form->target_model);
		if (item)
			rc = nt_view_focus(item->view);

		if (!rc && item && item->model >= 0) {
			struct nt_view_rect viewport = nt_view_content_rect(form->scroll);
			struct nt_view_rect target = nt_view_frame(item->view);
			int delta =
				target.y < viewport.y ? target.y - viewport.y
				: target.y + target.height > viewport.y + viewport.height
					? target.y + target.height - viewport.y - viewport.height
					: 0;
			if (delta) {
				int moved = nt_scroll_view_scroll_by(form->scroll, 0, delta);
				if (moved < 0)
					rc = moved;
			}
		}
	}

	return rc;
}

static int cli_setup_form_submit(struct cli_setup_form *form, int defer_accounts)
{
	int rc = cli_setup_form_commit_all(form);
	if (!rc)
		rc = cli_setup_form_validate_active(form, form->error, sizeof(form->error),
						    defer_accounts);

	if (rc) {
		if (form->target_model >= 0 && form->target_model != form->model) {
			form->model = form->target_model;
			form->rebuilding = 1;
			nt_tab_bar_select(form->tabs, (size_t)form->model);
			form->rebuilding = 0;
			cli_setup_form_invalidate(form);
		}

		if (!form->advanced[form->model] &&
		    (form->target_field == CLI_SETUP_FIELD_CONTEXT ||
		     form->target_field == CLI_SETUP_FIELD_API_BASE)) {
			form->advanced[form->model] = 1;
			cli_setup_form_invalidate(form);
		}

		form->reveal_focus = 1;
		nt_view_invalidate(&form->base.base, 1);
		return cli_setup_form_status_update(form);
	}

	return nt_view_emit(&form->base.base, NT_VIEW_EVENT_SUBMIT, 0, NULL);
}

static int cli_setup_form_capture(struct nt_view *view, const struct nt_event *event)
{
	if (event->type != NT_EVENT_KEY || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	struct cli_setup_form *form = (struct cli_setup_form *)view;
	if (event->key == 3) {
		int rc = nt_view_emit(view, NT_VIEW_EVENT_CANCEL, 0, NULL);
		return rc < 0 ? rc : 1;
	}

	if (event->key == NT_KEY_F10 ||
	    ((event->key == '\r' || event->key == '\n') && (event->modifiers & NT_MOD_CTRL))) {
		int rc = cli_setup_form_submit(form, 0);
		return rc < 0 ? rc : 1;
	}

	if (event->key == '\r' || event->key == '\n') {
		struct nt_window *window = nt_application_window(
			nt_view_controller_application(nt_view_controller(view)));
		struct nt_view *focused = nt_window_focused_view(window);
		for (size_t i = 0; i < form->focus_count; i++) {
			struct cli_setup_form_focus *item = &form->focus[i];
			if (item->view != focused || item->kind != CLI_SETUP_FIELD_TEXT)
				continue;

			int rc = cli_setup_form_text_commit(form, item);
			if (!rc)
				rc = nt_window_focus_next(window);

			if (rc < 0 && rc != NB_ERR_INVAL)
				return rc;

			if (form->needs_rebuild)
				nt_view_invalidate(view, 1);

			int status = cli_setup_form_status_update(form);
			return status < 0 ? status : 1;
		}
	}

	return 0;
}

static int cli_setup_form_event(void *data, const struct nt_view_event *event)
{
	struct cli_setup_form *form = data;
	if (form->rebuilding)
		return NT_VIEW_EVENT_PASS;

	struct cli_setup_form_focus *found = NULL;
	for (size_t i = 0; i < form->focus_count; i++) {
		if (form->focus[i].view == event->target) {
			found = &form->focus[i];
			break;
		}
	}

	if (!found)
		return NT_VIEW_EVENT_PASS;

	struct cli_setup_form_focus item = *found;
	if (event->type == NT_VIEW_EVENT_FOCUS_GAINED) {
		form->target_field = item.field;
		form->target_model = item.model;
		form->reveal_focus = 1;
		nt_view_invalidate(&form->base.base, 1);
		return NT_VIEW_EVENT_PASS;
	}

	if (item.kind == CLI_SETUP_FIELD_TEXT) {
		if (event->type == NT_VIEW_EVENT_FOCUS_LOST) {
			int rc = cli_setup_form_text_commit(form, &item);
			if (rc) {
				form->target_field = item.field;
				form->target_model = item.model;
			} else {
				form->error[0] = '\0';
			}

			if (form->needs_rebuild)
				nt_view_invalidate(&form->base.base, 1);

			int status = cli_setup_form_status_update(form);
			if (status)
				return status;
		}

		return NT_VIEW_EVENT_PASS;
	}

	if (event->type == NT_VIEW_EVENT_ACTIVATE && item.kind == CLI_SETUP_FIELD_BUTTON) {
		return item.field == CLI_SETUP_FIELD_CANCEL
			       ? nt_view_emit(&form->base.base, NT_VIEW_EVENT_CANCEL, 0, NULL)
			       : cli_setup_form_submit(form,
						       item.field == CLI_SETUP_FIELD_SAVE_LATER);
	}

	if (event->type != NT_VIEW_EVENT_CHANGED)
		return NT_VIEW_EVENT_PASS;

	int rc = item.field == CLI_SETUP_FIELD_PROVIDER || item.field == CLI_SETUP_FIELD_ENABLED
			 ? NB_OK
			 : cli_setup_form_commit_all(form);
	if (rc) {
		form->rebuilding = 1;
		if (item.field == CLI_SETUP_FIELD_TABS)
			nt_tab_bar_select(form->tabs, (size_t)form->model);
		else if (item.field == CLI_SETUP_FIELD_ROLE)
			nt_drop_down_select(item.view, (size_t)form->state->role);
		else if (item.field == CLI_SETUP_FIELD_ACCOUNT)
			nt_drop_down_select(item.view, (size_t)form->account_mode[item.model]);
		else if (item.field == CLI_SETUP_FIELD_ADVANCED)
			nt_check_box_set_checked(item.view, form->advanced[item.model]);

		form->rebuilding = 0;
		nt_view_invalidate(&form->base.base, 1);
		return cli_setup_form_status_update(form);
	}

	form->error[0] = '\0';
	switch (item.field) {
	case CLI_SETUP_FIELD_ROLE:
		form->state->role = (int)nt_drop_down_selected(item.view);
		cli_setup_role_defaults(form->state);
		cli_setup_form_account_modes_init(form);
		break;
	case CLI_SETUP_FIELD_TABS:
		form->model = (int)nt_tab_bar_selected(item.view);
		rc = nt_scroll_view_set_offset(form->scroll, 0);
		if (rc < 0)
			return rc;

		break;
	case CLI_SETUP_FIELD_PROVIDER:
		cli_setup_model_defaults(item.model, (int)nt_drop_down_selected(item.view) + 1,
					 &form->state->entries[item.model]);
		cli_setup_form_account_modes_refresh(form, item.model);
		if (!strcmp(form->state->entries[item.model].provider, "custom"))
			form->advanced[item.model] = 1;

		break;
	case CLI_SETUP_FIELD_ACCOUNT:
		form->account_mode[item.model] = (int)nt_drop_down_selected(item.view);
		if (form->account_mode[item.model] == CLI_SETUP_ACCOUNT_ENV)
			form->state->entries[item.model].api_key[0] = '\0';

		break;
	case CLI_SETUP_FIELD_ADVANCED:
		form->advanced[item.model] = nt_check_box_is_checked(item.view);
		break;
	case CLI_SETUP_FIELD_ENABLED:
		cli_setup_form_apply_check(form, &item);
		break;
	case CLI_SETUP_FIELD_INSTRUCTIONS:
		form->state->replace_prompt = nt_drop_down_selected(item.view) == 1;
		return cli_setup_form_status_update(form);
	case CLI_SETUP_FIELD_THINKING:
		rc = cli_setup_set_thinking(&form->state->entries[item.model],
					    nt_drop_down_selected(item.view) == 1);
		if (rc)
			return rc;

		break;
	default:
		return NT_VIEW_EVENT_PASS;
	}

	cli_setup_form_invalidate(form);
	form->reveal_focus = 1;
	return NT_VIEW_EVENT_HANDLED;
}

static void cli_setup_form_deinit(struct nt_view *view)
{
	struct cli_setup_form *form = (struct cli_setup_form *)view;
	for (size_t i = 0; i < CLI_SETUP_FIELD_COUNT; i++)
		free(form->drafts[i].text);
}

static const struct nt_keymap *cli_setup_form_keymap(const struct nt_view *view)
{
	(void)view;
	static const struct nt_key_binding bindings[] = {
		{.key = NT_KEY_F10, .description = "Save configuration"},
		{.key = '\r', .modifiers = NT_MOD_CTRL, .description = "Save configuration"},
		{.key = 27, .description = "Cancel setup"},
		{.key = 'c', .modifiers = NT_MOD_CTRL, .description = "Cancel setup"},
	};
	static const struct nt_keymap map = {bindings, NT_ARRAY_SIZE(bindings)};
	return &map;
}

static const struct nt_view_class cli_setup_form_class = {
	.superclass = &nt_stack_view_class,
	.instance_size = sizeof(struct cli_setup_form),
	.kind = "cli_setup_form",
	.layout = cli_setup_form_layout,
	.capture_event = cli_setup_form_capture,
	.keymap = cli_setup_form_keymap,
	.deinit = cli_setup_form_deinit,
};

struct nt_view *cli_setup_form_add_borrow(struct nt_builder *builder, struct nt_view *parent,
					  struct cli_setup_state *state, const char *path,
					  int styled)
{
	if (!state || !path) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	struct cli_setup_form *form = (struct cli_setup_form *)nt_view_add(
		builder, parent, &cli_setup_form_class,
		&(struct nt_view_options){.width = NT_FILL,
					  .height = NT_FILL,
					  .max_width = 104,
					  .alignment = NT_VIEW_CENTER,
					  .padding = {1, 2, 1, 2}});
	if (!form)
		return NULL;

	struct nt_view *view = &form->base.base;
	nt_builder_fail(builder, nt_view_set_help_label(view, "Setup"));
	form->state = state;
	form->builder = builder;
	form->styled = styled;
	form->wide = 1;
	form->rebuilding = 1;
	form->target_field = CLI_SETUP_FIELD_ROLE;
	form->target_model = -1;
	snprintf(form->path, sizeof(form->path), "%s", path);
	cli_setup_form_account_modes_init(form);
	form->header = nt_row_add(builder, view, 1, &(struct nt_view_options){.height = 1});
	cli_setup_form_label(form, builder, form->header, "Set up morph", NT_THEME_TEXT,
			     NT_STYLE_BOLD, 0);
	form->path_view =
		cli_setup_form_label(form, builder, view, form->path, NT_THEME_MUTED, 0, 0);
	struct nt_view *role_row = nt_row_add(builder, view, 2, NULL);
	cli_setup_form_label(form, builder, role_row, "Role", NT_THEME_MUTED, 0, 12);
	const char *roles[CLI_SETUP_ROLE_COUNT];
	for (int i = 0; i < CLI_SETUP_ROLE_COUNT; i++)
		roles[i] = cli_setup_role_get(i).label;

	struct nt_view *role =
		nt_drop_down_add(builder, role_row,
				 &(struct nt_drop_down_options){.items = roles,
								.count = CLI_SETUP_ROLE_COUNT,
								.selected = (size_t)state->role},
				 &(struct nt_view_options){.width = NT_FILL});
	cli_setup_form_add_focus(form, CLI_SETUP_FIELD_ROLE, CLI_SETUP_FIELD_DROPDOWN, -1, role);
	form->role_note = nt_label_add(
		builder, view, cli_setup_role_get(state->role).description,
		&(struct nt_view_options){.style = cli_setup_form_style(form, NT_THEME_MUTED, 0)});
	static const char *const models[] = {"Text", "Vision", "Images", "Video"};
	form->tabs = nt_tab_bar_add(builder, view, models, CLI_SETUP_MODEL_COUNT, 0, 0,
				    &(struct nt_view_options){.height = 1, .margin = {.top = 1}});
	cli_setup_form_add_focus(form, CLI_SETUP_FIELD_TABS, CLI_SETUP_FIELD_TAB, -1, form->tabs);
	form->scroll = nt_scroll_view_add(
		builder, view,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL, .grow = 1});
	nt_builder_fail(builder, nt_scroll_view_follow(form->scroll, 0));
	nt_builder_fail(builder, nt_view_set_focusable(form->scroll, 0));
	struct nt_view *card = nt_card_view_add(builder, form->scroll, "Model settings", NULL);
	form->content =
		nt_form_view_add(builder, card, &(struct nt_view_options){.padding = {1, 2, 1, 2}});
	form->status = cli_setup_form_label(form, builder, view, "", NT_THEME_TEXT, 0, 0);
	cli_setup_form_add_instructions(form, builder, view);
	form->actions = nt_row_add(builder, view, 2, &(struct nt_view_options){.height = 1});
	cli_setup_form_add_button(form, builder, form->actions, "Save and exit",
				  CLI_SETUP_FIELD_SAVE);
	cli_setup_form_add_button(form, builder, form->actions, "Save for later",
				  CLI_SETUP_FIELD_SAVE_LATER);
	cli_setup_form_add_button(form, builder, form->actions, "Cancel", CLI_SETUP_FIELD_CANCEL);
	form->hint = cli_shortcut_hint_add(builder, view, "F10 Save · Tab Next · Esc Cancel",
					   &(struct nt_view_options){.height = 1});
	form->fixed_count = form->focus_count;
	cli_setup_form_build_model(form);
	form->rebuilding = 0;
	nt_builder_fail(builder, cli_setup_form_status_update(form));
	if (!nt_view_on(view, NT_VIEW_EVENT_ANY, cli_setup_form_event, form))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	return view;
}
