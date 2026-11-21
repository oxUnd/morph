#include "sapi/cli/components/setup_form.h"

#include "sapi/cli/components/shortcut_hint.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/configuration/mcp.h"
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include <nb/cleanup.h>
#include <nb/str.h>
#include <nothing/tui/canvas.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/check_box.h>
#include <nothing/tui/controls/drop_down.h>
#include <nothing/tui/controls/radio_button.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/input.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/card_view.h>
#include <nothing/tui/views/form_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLI_SETUP_FORM_FOCUS_MAX 64
#define CLI_SETUP_FORM_DRAFT_MAX 4096
#define CLI_SETUP_FORM_MESSAGE_MAX 160
#define CLI_SETUP_FORM_LABEL_WIDTH 16
#define CLI_SETUP_FORM_STACK_BELOW 72

enum cli_setup_form_field_kind {
	CLI_SETUP_FIELD_TEXT,
	CLI_SETUP_FIELD_DROPDOWN,
	CLI_SETUP_FIELD_RADIO,
	CLI_SETUP_FIELD_CHECK,
	CLI_SETUP_FIELD_BUTTON
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
	CLI_SETUP_FIELD_CANCEL
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

struct cli_setup_form {
	struct cli_setup_state *state;
	char path[PATH_MAX];
	int styled;
	int columns;
	int account_mode[CLI_SETUP_MODEL_COUNT];

	struct nt_surface *surface;
	struct nt_view_controller *controller;
	struct nt_view *root;
	struct nt_view *header;
	struct nt_view *status;
	struct nt_view *scroll;
	struct nt_view *card;
	struct nt_view *actions;
	struct nt_view *hint;

	struct cli_setup_form_focus focus[CLI_SETUP_FORM_FOCUS_MAX];
	size_t focus_count;
	size_t focus_index;
	enum cli_setup_form_field target_field;
	int target_model;

	int dropdown_open;
	int needs_rebuild;
	int has_draft;
	enum cli_setup_form_field draft_field;
	int draft_model;
	char draft[CLI_SETUP_FORM_DRAFT_MAX];
	char error[CLI_SETUP_FORM_MESSAGE_MAX];
	char message[CLI_SETUP_FORM_MESSAGE_MAX];
	enum nt_theme_role message_role;
};

struct cli_setup_form_controller {
	struct nt_view_controller base;
	struct cli_setup_form *form;
};

static void cli_setup_form_build(struct cli_setup_form *form, struct nt_builder *builder);

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

static void cli_setup_form_note(struct cli_setup_form *form, struct nt_builder *builder,
				struct nt_view *parent, const char *text)
{
	struct nt_view *label = nt_label_add(
		builder, parent, text ? text : "",
		&(struct nt_view_options){
			.style = cli_setup_form_style(form, NT_THEME_MUTED, 0)});
	if (label)
		nt_builder_fail(builder, nt_label_set_wrap_mode(label, NT_TEXT_WRAP_WORD));
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
	if (form->columns >= CLI_SETUP_FORM_STACK_BELOW) {
		struct nt_view *row =
			nt_row_add(builder, parent, 1,
				   &(struct nt_view_options){.width = NT_FILL});
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
					       int secret, enum cli_setup_form_field field, int model)
{
	struct nt_view *host = cli_setup_form_host(form, builder, parent, label);
	struct nt_view *view = nt_text_field_add(
		builder, host,
		&(struct nt_text_field_options){
			.text = text ? text : "",
			.limit = capacity > 1 ? capacity - 1 : 0},
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	if (view && secret)
		nt_builder_fail(builder, nt_text_field_set_echo_mode(view, NT_TEXT_FIELD_ECHO_PASSWORD));

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

static struct nt_view *cli_setup_form_add_radio(struct cli_setup_form *form,
						struct nt_builder *builder, struct nt_view *parent,
						const char *label, const char *const *items,
						size_t count, size_t selected,
						enum cli_setup_form_field field, int model)
{
	struct nt_view *host = cli_setup_form_host(form, builder, parent, label);
	struct nt_view *view = nt_radio_button_add(
		builder, host, items, count, selected, (int)field,
		&(struct nt_view_options){.width = NT_FILL, .height = (int)count});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_RADIO, model, view);
	return view;
}

static struct nt_view *cli_setup_form_add_check(struct cli_setup_form *form,
						struct nt_builder *builder, struct nt_view *parent,
						const char *label, int checked,
						enum cli_setup_form_field field, int model)
{
	struct nt_view *view = nt_check_box_add(
		builder, parent, label, checked, (int)field,
		&(struct nt_view_options){.height = 1});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_CHECK, model, view);
	return view;
}

static void cli_setup_form_add_button(struct cli_setup_form *form, struct nt_builder *builder,
				      struct nt_view *parent, const char *label,
				      enum cli_setup_form_field field)
{
	struct nt_view *view = nt_button_add(
		builder, parent, label, (int)field, &(struct nt_view_options){.height = 1});
	cli_setup_form_add_focus(form, field, CLI_SETUP_FIELD_BUTTON, -1, view);
}

static int cli_setup_form_has_key(const struct config_model_entry *entry)
{
	if (entry->api_key[0])
		return 1;

	const char *value = entry->api_key_env[0] ? getenv(entry->api_key_env) : NULL;
	return value && *value;
}

static int cli_setup_form_complete_required(const struct cli_setup_form *form)
{
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		if (cli_setup_role_requires(form->state->role, i) &&
		    !cli_setup_model_complete(i, &form->state->entries[i]))
			return 0;

	return 1;
}

static void cli_setup_form_message_update(struct cli_setup_form *form)
{
	if (form->error[0]) {
		form->message_role = NT_THEME_ERROR;
		snprintf(form->message, sizeof(form->message), CLI_FEED_FAILED " %s",
			 form->error);
		return;
	}

	if (!cli_setup_form_complete_required(form)) {
		form->message_role = NT_THEME_ERROR;
		snprintf(form->message, sizeof(form->message),
			 CLI_FEED_FAILED " Complete the required model settings.");
		return;
	}

	int missing = 0;
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		const struct config_model_entry *entry = &form->state->entries[i];
		int required = cli_setup_role_requires(form->state->role, i);
		if ((required || entry->provider[0]) && !cli_setup_form_has_key(entry))
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

static void cli_setup_form_add_model(struct cli_setup_form *form, struct nt_builder *builder,
				     struct nt_view *parent, int kind)
{
	struct config_model_entry *entry = &form->state->entries[kind];
	int required = cli_setup_role_requires(form->state->role, kind);
	int active = required || entry->provider[0];
	char title[96];
	snprintf(title, sizeof(title), "%s%s", cli_setup_model_label(kind),
		 required ? " · required" : " · optional");

	if (required)
		cli_setup_form_label(form, builder, parent, title, NT_THEME_TEXT, NT_STYLE_BOLD, 0);
	else {
		cli_setup_form_add_check(form, builder, parent, title, active,
					 CLI_SETUP_FIELD_ENABLED, kind);
		if (!active)
			return;
	}

	const char *const *providers = NULL;
	int provider_count = cli_setup_form_provider_items(kind, &providers);
	int provider = cli_setup_provider_choice(kind, entry);
	if (provider < 1 || provider > provider_count)
		provider = 1;

	cli_setup_form_add_dropdown(form, builder, parent, "Provider", providers,
				    (size_t)provider_count, (size_t)(provider - 1),
				    CLI_SETUP_FIELD_PROVIDER, kind);
	cli_setup_form_add_text(form, builder, parent, "Model", entry->model, sizeof(entry->model),
				0, CLI_SETUP_FIELD_MODEL, kind);

	static const char *const accounts[] = {"Use environment variable", "Save key to config",
					       "Set up later"};
	int mode = form->account_mode[kind];
	if (mode < CLI_SETUP_ACCOUNT_ENV || mode > CLI_SETUP_ACCOUNT_LATER)
		mode = CLI_SETUP_ACCOUNT_LATER;

	cli_setup_form_add_radio(form, builder, parent, "Account", accounts, 3, (size_t)mode,
				 CLI_SETUP_FIELD_ACCOUNT, kind);
	if (mode == CLI_SETUP_ACCOUNT_ENV)
		cli_setup_form_add_text(form, builder, parent, "Variable", entry->api_key_env,
					sizeof(entry->api_key_env), 0,
					CLI_SETUP_FIELD_API_KEY_ENV, kind);
	else if (mode == CLI_SETUP_ACCOUNT_KEY)
		cli_setup_form_add_text(form, builder, parent, "API key", entry->api_key,
					sizeof(entry->api_key), 1, CLI_SETUP_FIELD_API_KEY, kind);

	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) {
		char text[CLI_SETUP_TOKEN_INPUT_MAX] = "";
		if (entry->context_limit > 0)
			snprintf(text, sizeof(text), "%d", entry->context_limit);

		cli_setup_form_add_text(form, builder, parent, "Context", text, sizeof(text), 0,
					CLI_SETUP_FIELD_CONTEXT, kind);
		text[0] = '\0';
		if (entry->max_tokens > 0)
			snprintf(text, sizeof(text), "%d", entry->max_tokens);

		cli_setup_form_add_text(form, builder, parent, "Max output", text, sizeof(text), 0,
					CLI_SETUP_FIELD_MAX_TOKENS, kind);
	}

	cli_setup_form_add_text(form, builder, parent, "API base", entry->api_base,
				sizeof(entry->api_base), 0, CLI_SETUP_FIELD_API_BASE, kind);
	if ((kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) &&
	    strcmp(entry->adapter, "deepseek") == 0) {
		static const char *const thinking[] = {"Off", "On"};
		cli_setup_form_add_dropdown(form, builder, parent, "Thinking", thinking, 2,
					    cli_setup_thinking_enabled(entry) ? 1 : 0,
					    CLI_SETUP_FIELD_THINKING, kind);
	}
}

static void cli_setup_form_add_instructions(struct cli_setup_form *form,
					    struct nt_builder *builder, struct nt_view *parent)
{
	const char *items[] = {
		form->state->existing_prompt ? "Keep existing instructions"
					     : "Use built-in instructions",
		"Use role instructions"};
	cli_setup_form_add_dropdown(form, builder, parent, "Instructions", items, 2,
				    form->state->replace_prompt ? 1 : 0,
				    CLI_SETUP_FIELD_INSTRUCTIONS, -1);
}

static void cli_setup_form_add_readonly(struct cli_setup_form *form, struct nt_builder *builder,
					struct nt_view *parent, const char *label,
					const char *value)
{
	struct nt_view *host = cli_setup_form_host(form, builder, parent, label);
	struct nt_view *text = nt_label_add(
		builder, host, value ? value : "",
		&(struct nt_view_options){
			.width = NT_FILL,
			.style = cli_setup_form_style(form, NT_THEME_MUTED, 0)});
	if (text)
		nt_builder_fail(builder, nt_label_set_wrap_mode(text, NT_TEXT_WRAP_WORD));
}

static void cli_setup_form_build(struct cli_setup_form *form, struct nt_builder *builder)
{
	form->focus_count = 0;
	form->focus_index = 0;
	cli_setup_form_message_update(form);

	form->root = nt_column_add(
		builder, NULL, 0,
		&(struct nt_view_options){.padding = {.left = 2, .right = 2}});
	form->header =
		nt_row_add(builder, form->root, 1,
			   &(struct nt_view_options){.width = NT_FILL, .height = 1});
	cli_setup_form_label(form, builder, form->header, "morph setup", NT_THEME_TEXT,
			     NT_STYLE_BOLD, 0);
	const char *shown_path = form->path;
	const char *slash = strrchr(shown_path, '/');
	if (form->columns < 100 && slash)
		shown_path = slash + 1;
	struct nt_view *path = nt_label_add(
		builder, form->header, shown_path,
		&(struct nt_view_options){.width = NT_FILL,
					  .height = 1,
					  .alignment = NT_VIEW_END,
					  .style = cli_setup_form_style(form, NT_THEME_MUTED, 0)});
	if (path)
		nt_builder_fail(builder, nt_label_set_wrap_mode(path, NT_TEXT_WRAP_NONE));

	form->status = cli_setup_form_label(
		form, builder, form->root, form->message, form->message_role, 0, 0);
	form->scroll = nt_scroll_view_add(
		builder, form->root, &(struct nt_view_options){.width = NT_FILL, .height = 8});
	nt_scroll_view_set_scrollbars(form->scroll, 0);
	nt_scroll_view_follow(form->scroll, 0);
	form->card = nt_card_view_add(
		builder, form->scroll, "Setup",
		&(struct nt_view_options){.width = NT_FILL, .max_width = 100});
	struct nt_view *content = nt_form_view_add(
		builder, form->card,
		&(struct nt_view_options){.padding = {.top = 1, .right = 2, .bottom = 1, .left = 2}});

	const char *roles[CLI_SETUP_ROLE_COUNT];
	for (int i = 0; i < CLI_SETUP_ROLE_COUNT; i++)
		roles[i] = cli_setup_role_get(i).label;

	cli_setup_form_add_dropdown(form, builder, content, "Role", roles,
				    CLI_SETUP_ROLE_COUNT, (size_t)form->state->role,
				    CLI_SETUP_FIELD_ROLE, -1);
	cli_setup_form_note(form, builder, content,
			    "New required models get defaults; existing choices stay.");
	cli_setup_form_blank(builder, content);

	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		cli_setup_form_add_model(form, builder, content, i);
		cli_setup_form_blank(builder, content);
	}

	cli_setup_form_add_instructions(form, builder, content);
	cli_setup_form_blank(builder, content);
	cli_setup_form_add_readonly(form, builder, content, "Integrations",
				    cli_setup_mcp_summary(form->state->role));
	cli_setup_form_add_readonly(form, builder, content, "Permissions",
				    "Read/media auto-approved · host exec on");

	form->actions = nt_row_add(
		builder, form->root, 2, &(struct nt_view_options){.width = NT_FILL, .height = 1});
	cli_setup_form_add_button(form, builder, form->actions,
				  form->columns < 60 ? "Save" : "Save and exit",
				  CLI_SETUP_FIELD_SAVE);
	cli_setup_form_add_button(form, builder, form->actions,
				  form->columns < 60 ? "Later" : "Save for later",
				  CLI_SETUP_FIELD_SAVE_LATER);
	cli_setup_form_add_button(form, builder, form->actions, "Cancel",
				  CLI_SETUP_FIELD_CANCEL);

	form->hint = cli_shortcut_hint_add(
		builder, form->root,
		form->columns < 60 ? "Tab · Enter · Esc"
				   : "↑↓ Move · Enter Edit · Tab Next · Esc Cancel",
		&(struct nt_view_options){.height = 1});
}

static void cli_setup_form_controller_load(struct nt_view_controller *controller,
				     struct nt_builder *builder)
{
	struct cli_setup_form_controller *view = (struct cli_setup_form_controller *)controller;
	cli_setup_form_build(view->form, builder);
}

static const struct nt_view_controller_class cli_setup_form_controller_type = {
	.superclass = &nt_view_controller_base_class,
	.instance_size = sizeof(struct cli_setup_form_controller),
	.load_view = cli_setup_form_controller_load,
};

static size_t cli_setup_form_find_focus(const struct cli_setup_form *form,
					enum cli_setup_form_field field, int model)
{
	for (size_t i = 0; i < form->focus_count; i++)
		if (form->focus[i].field == field && form->focus[i].model == model)
			return i;

	return SIZE_MAX;
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

static int cli_setup_form_rebuild(struct cli_setup_form *form, int columns)
{
	form->columns = columns;
	if (form->controller) {
		nt_view_controller_destroy(form->controller);
		form->controller = NULL;
	}

	struct nt_view_controller *controller = NULL;
	int rc = nt_view_controller_create(&cli_setup_form_controller_type, &controller);
	if (rc)
		return rc;

	struct cli_setup_form_controller *view = (struct cli_setup_form_controller *)controller;
	view->form = form;
	form->controller = controller;
	rc = nt_view_controller_load(controller);
	if (rc) {
		nt_view_controller_destroy(controller);
		form->controller = NULL;
		return rc;
	}

	form->root = nt_view_controller_root_view(controller);
	size_t index = cli_setup_form_find_focus(form, form->target_field, form->target_model);
	if (index == SIZE_MAX)
		index = 0;

	if (form->focus_count) {
		form->focus_index = index;
		form->target_field = form->focus[index].field;
		form->target_model = form->focus[index].model;
		rc = nt_view_focus(form->focus[index].view);
	}

	return rc;
}

static void cli_setup_form_capture_draft(struct cli_setup_form *form)
{
	form->has_draft = 0;
	if (form->focus_count == 0)
		return;

	const struct cli_setup_form_focus *item = &form->focus[form->focus_index];
	if (item->kind != CLI_SETUP_FIELD_TEXT)
		return;

	const char *text = nt_text_field_text(item->view);
	snprintf(form->draft, sizeof(form->draft), "%s", text ? text : "");
	form->draft_field = item->field;
	form->draft_model = item->model;
	form->has_draft = 1;
}

static void cli_setup_form_restore_draft(struct cli_setup_form *form)
{
	if (!form->has_draft)
		return;

	size_t index = cli_setup_form_find_focus(form, form->draft_field, form->draft_model);
	if (index != SIZE_MAX && form->focus[index].kind == CLI_SETUP_FIELD_TEXT) {
		nt_view_focus(form->focus[index].view);
		nt_text_field_set_text(form->focus[index].view, form->draft);
		form->focus_index = index;
		form->target_field = form->focus[index].field;
		form->target_model = form->focus[index].model;
	}

	form->has_draft = 0;
}

int cli_setup_form_show(struct cli_setup_form *form, struct nt_inline_region *region,
			   FILE *output, int columns, int rows)
{
	if (columns < 1 || rows < 1)
		return NB_ERR_INVAL;

	if (form->needs_rebuild || columns != form->columns) {
		cli_setup_form_capture_draft(form);
		int rc = cli_setup_form_rebuild(form, columns);
		if (rc)
			return rc;

		cli_setup_form_restore_draft(form);
		form->needs_rebuild = 0;
	}

	int show_header = rows >= 14 && columns >= 46;
	int show_status = rows >= 10;
	int show_hint = rows >= 12 && columns >= 56;
	int show_actions = 1;
	int chrome = show_header + show_status + show_hint + show_actions;
	int body_max = rows - chrome - 1;
	if (body_max < 2)
		body_max = 2;

	nt_view_set_hidden(form->header, !show_header);
	nt_view_set_hidden(form->status, !show_status);
	nt_view_set_hidden(form->hint, !show_hint);
	nt_view_set_hidden(form->actions, !show_actions);

	int measure_width = columns > 10 ? columns - 10 : 1;
	struct nt_view_size card_size = {0};
	int rc = nt_view_measure(form->card, measure_width, &card_size);
	if (rc)
		return rc;

	int body = card_size.height > 0 ? card_size.height : 2;
	if (body > body_max)
		body = body_max;

	struct nt_view_options scroll_options = nt_view_options(form->scroll);
	scroll_options.height = body;
	scroll_options.width = NT_FILL;
	rc = nt_view_set_options(form->scroll, &scroll_options);
	if (rc)
		return rc;

	struct nt_view_size root_size = {0};
	rc = nt_view_measure(form->root, columns, &root_size);
	if (rc)
		return rc;

	if (root_size.height > rows && body > 2) {
		int excess = root_size.height - rows;
		body -= excess;
		if (body < 2)
			body = 2;
		scroll_options.height = body;
		rc = nt_view_set_options(form->scroll, &scroll_options);
		if (rc)
			return rc;

		root_size = (struct nt_view_size){0};
		rc = nt_view_measure(form->root, columns, &root_size);
		if (rc)
			return rc;
	}

	if (root_size.height < 1)
		root_size.height = 1;

	rc = nt_surface_resize(form->surface, columns, root_size.height);
	if (rc)
		return rc;

	nt_surface_clear(form->surface);
	rc = nt_view_arrange(form->root,
			     (struct nt_view_rect){0, 0, columns, root_size.height});
	if (rc)
		return rc;

	if (form->focus_count) {
		struct nt_view_rect viewport = nt_view_content_rect(form->scroll);
		struct nt_view_rect target = nt_view_frame(form->focus[form->focus_index].view);
		int delta = 0;
		if (target.y < viewport.y)
			delta = target.y - viewport.y;
		else if (target.y + target.height > viewport.y + viewport.height)
			delta = target.y + target.height - (viewport.y + viewport.height);

		if (delta && nt_scroll_view_scroll_by(form->scroll, 0, delta) > 0) {
			nt_view_arrange(form->root,
					(struct nt_view_rect){0, 0, columns, root_size.height});
		}
	}

	rc = nt_view_render(form->root, form->surface);
	if (rc)
		return rc;

	return nt_inline_region_present(region, output, form->surface, columns, rows, -1, -1,
					form->styled);
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

	return nb_str_is_alnum(nb_str_from_cstr(name), "_") &&
	       !(name[0] >= '0' && name[0] <= '9');
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

			form->needs_rebuild = 1;
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
		if (!present || cli_setup_parse_token_count(value, 2, ceiling,
							    &entry->context_limit) != 0) {
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
				 "Start with a letter or underscore; use letters, digits and underscores.");
			return NB_ERR_INVAL;
		}

		snprintf(entry->api_key_env, sizeof(entry->api_key_env), "%s", value);
		return 0;
	default:
		return 0;
	}
}

static int cli_setup_form_commit_current(struct cli_setup_form *form)
{
	if (form->focus_count == 0)
		return 0;

	const struct cli_setup_form_focus *item = &form->focus[form->focus_index];
	if (item->kind != CLI_SETUP_FIELD_TEXT)
		return 0;

	int rc = cli_setup_form_text_commit(form, item);
	if (rc)
		form->needs_rebuild = 1;
	else
		form->error[0] = '\0';

	return rc;
}

static int cli_setup_form_commit_all(struct cli_setup_form *form)
{
	for (size_t i = 0; i < form->focus_count; i++) {
		if (form->focus[i].kind != CLI_SETUP_FIELD_TEXT)
			continue;

		int rc = cli_setup_form_text_commit(form, &form->focus[i]);
		if (rc)
			return rc;
	}

	form->error[0] = '\0';
	return 0;
}

static int cli_setup_form_move_focus(struct cli_setup_form *form, int direction)
{
	if (form->focus_count == 0)
		return CLI_SETUP_FORM_CONTINUE;

	if (cli_setup_form_commit_current(form) != 0) {
		form->needs_rebuild = 1;
		return CLI_SETUP_FORM_CONTINUE;
	}

	size_t count = form->focus_count;
	size_t index = (form->focus_index + (direction > 0 ? 1 : count - 1)) % count;
	form->focus_index = index;
	form->target_field = form->focus[index].field;
	form->target_model = form->focus[index].model;
	form->error[0] = '\0';
	form->needs_rebuild = 1;
	return CLI_SETUP_FORM_CONTINUE;
}

static void cli_setup_form_apply_dropdown(struct cli_setup_form *form,
					  const struct cli_setup_form_focus *item)
{
	size_t selected = nt_drop_down_selected(item->view);
	struct config_model_entry *entry = &form->state->entries[item->model];
	switch (item->field) {
	case CLI_SETUP_FIELD_ROLE:
		if ((int)selected != form->state->role) {
			form->state->role = (int)selected;
			cli_setup_role_defaults(form->state);
			cli_setup_form_account_modes_init(form);
			form->needs_rebuild = 1;
		}

		break;
	case CLI_SETUP_FIELD_PROVIDER: {
		int previous = cli_setup_provider_choice(item->model, entry);
		if ((int)selected + 1 != previous) {
			cli_setup_model_defaults(item->model, (int)selected + 1, entry);
			cli_setup_form_account_modes_refresh(form, item->model);
			form->needs_rebuild = 1;
		}

		break;
	}
	case CLI_SETUP_FIELD_THINKING:
		cli_setup_set_thinking(entry, selected == 1);
		break;
	case CLI_SETUP_FIELD_INSTRUCTIONS:
		form->state->replace_prompt = selected == 1;
		break;
	default:
		break;
	}
}

static void cli_setup_form_apply_radio(struct cli_setup_form *form,
				       const struct cli_setup_form_focus *item)
{
	if (item->field != CLI_SETUP_FIELD_ACCOUNT)
		return;

	size_t selected = nt_radio_button_selected(item->view);
	struct config_model_entry *entry = &form->state->entries[item->model];
	form->account_mode[item->model] = (int)selected;
	if (selected == CLI_SETUP_ACCOUNT_ENV)
		entry->api_key[0] = '\0';

	form->needs_rebuild = 1;
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

static int cli_setup_form_validate_active(struct cli_setup_form *form, char *error,
					  size_t capacity)
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
					 "Use letters, digits and underscores for the API key variable.");
				form->target_field = CLI_SETUP_FIELD_API_KEY_ENV;
				form->target_model = i;
				return NB_ERR_INVAL;
			}

			entry->api_key[0] = '\0';
		} else if (form->account_mode[i] == CLI_SETUP_ACCOUNT_KEY) {
			if (!entry->api_key[0]) {
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

static int cli_setup_form_action(struct cli_setup_form *form, enum cli_setup_form_field field)
{
	if (field == CLI_SETUP_FIELD_CANCEL)
		return CLI_SETUP_FORM_CANCEL;

	if (field != CLI_SETUP_FIELD_SAVE && field != CLI_SETUP_FIELD_SAVE_LATER)
		return CLI_SETUP_FORM_CONTINUE;

	form->error[0] = '\0';
	int rc = cli_setup_form_commit_all(form);
	if (rc == 0)
		rc = cli_setup_form_validate_active(form, form->error, sizeof(form->error));

	if (rc != 0) {
		form->needs_rebuild = 1;
		return CLI_SETUP_FORM_CONTINUE;
	}

	return CLI_SETUP_FORM_SUBMIT;
}

static int cli_setup_form_dispatch_view(struct nt_view *view, const struct nt_event *event)
{
	if (!view)
		return 0;

	const struct nt_view_class *methods = nt_view_class_methods(view);
	if (!methods || !methods->handle_event)
		return 0;

	return methods->handle_event(view, event);
}

int cli_setup_form_dispatch(struct cli_setup_form *form, const struct nt_event *event)
{
	if (!event || form->focus_count == 0)
		return CLI_SETUP_FORM_CONTINUE;

	struct cli_setup_form_focus *item = &form->focus[form->focus_index];
	if (event->type == NT_EVENT_TEXT || event->type == NT_EVENT_PASTE) {
		if (item->kind == CLI_SETUP_FIELD_TEXT) {
			form->error[0] = '\0';
			cli_setup_form_dispatch_view(item->view, event);
		}

		return CLI_SETUP_FORM_CONTINUE;
	}

	if (event->type != NT_EVENT_KEY || event->action == NT_KEY_ACTION_RELEASE)
		return CLI_SETUP_FORM_CONTINUE;

	if (event->key == 3)
		return CLI_SETUP_FORM_CANCEL;

	if (event->key == 27) {
		if (form->dropdown_open) {
			nt_drop_down_close(item->view);
			form->dropdown_open = 0;
			return CLI_SETUP_FORM_CONTINUE;
		}

		return CLI_SETUP_FORM_CANCEL;
	}

	if (event->key == '\t' ||
	    (event->key == NT_KEY_BACK_TAB)) {
		int backward = event->key == NT_KEY_BACK_TAB ||
			       (event->modifiers & NT_MOD_SHIFT);
		return cli_setup_form_move_focus(form, backward ? -1 : 1);
	}

	if (item->kind == CLI_SETUP_FIELD_TEXT) {
		if (event->key == '\r' || event->key == '\n')
			return cli_setup_form_move_focus(form, 1);

		if (event->key == NT_KEY_UP)
			return cli_setup_form_move_focus(form, -1);

		if (event->key == NT_KEY_DOWN)
			return cli_setup_form_move_focus(form, 1);

		form->error[0] = '\0';
		cli_setup_form_dispatch_view(item->view, event);
		return CLI_SETUP_FORM_CONTINUE;
	}

	if (item->kind == CLI_SETUP_FIELD_DROPDOWN) {
		if (event->key == '\r' || event->key == '\n') {
			if (form->dropdown_open) {
				cli_setup_form_dispatch_view(item->view, event);
				form->dropdown_open = 0;
				cli_setup_form_apply_dropdown(form, item);
			} else {
				int rc = nt_drop_down_open(item->view);
				if (rc == 0)
					form->dropdown_open = 1;
			}

			return CLI_SETUP_FORM_CONTINUE;
		}

		if (event->key == NT_KEY_UP || event->key == NT_KEY_DOWN) {
			if (form->dropdown_open) {
				cli_setup_form_dispatch_view(item->view, event);
				return CLI_SETUP_FORM_CONTINUE;
			}

			return cli_setup_form_move_focus(
				form, event->key == NT_KEY_UP ? -1 : 1);
		}

		cli_setup_form_dispatch_view(item->view, event);
		return CLI_SETUP_FORM_CONTINUE;
	}

	if (item->kind == CLI_SETUP_FIELD_RADIO) {
		if (event->key == NT_KEY_UP)
			return cli_setup_form_move_focus(form, -1);

		if (event->key == NT_KEY_DOWN)
			return cli_setup_form_move_focus(form, 1);

		if (event->key == NT_KEY_LEFT || event->key == NT_KEY_RIGHT ||
		    event->key == ' ' || event->key == '\r' || event->key == '\n') {
			cli_setup_form_dispatch_view(item->view, event);
			cli_setup_form_apply_radio(form, item);
		}

		return CLI_SETUP_FORM_CONTINUE;
	}

	if (item->kind == CLI_SETUP_FIELD_CHECK) {
		if (event->key == NT_KEY_UP)
			return cli_setup_form_move_focus(form, -1);

		if (event->key == NT_KEY_DOWN)
			return cli_setup_form_move_focus(form, 1);

		if (event->key == ' ' || event->key == '\r' || event->key == '\n') {
			cli_setup_form_dispatch_view(item->view, event);
			cli_setup_form_apply_check(form, item);
		}

		return CLI_SETUP_FORM_CONTINUE;
	}

	if (item->kind == CLI_SETUP_FIELD_BUTTON) {
		if (event->key == '\r' || event->key == '\n' || event->key == ' ')
			return cli_setup_form_action(form, item->field);

		if (event->key == NT_KEY_LEFT)
			return cli_setup_form_move_focus(form, -1);

		if (event->key == NT_KEY_RIGHT)
			return cli_setup_form_move_focus(form, 1);
	}

	return CLI_SETUP_FORM_CONTINUE;
}

int cli_setup_form_create(struct cli_setup_state *state, const char *path, int styled, int columns,
			  struct cli_setup_form **out)
{
	if (!state || !path || !out || *out)
		return NB_ERR_INVAL;

	struct cli_setup_form *form = calloc(1, sizeof(*form));
	if (!form)
		return NB_ERR_NOMEM;

	form->state = state;
	form->styled = styled;
	form->columns = columns;
	form->target_field = CLI_SETUP_FIELD_ROLE;
	form->target_model = -1;
	snprintf(form->path, sizeof(form->path), "%s", path);
	cli_setup_form_account_modes_init(form);

	int rc = nt_surface_create(&form->surface);
	if (rc == 0)
		rc = cli_setup_form_rebuild(form, columns);

	if (rc != 0) {
		cli_setup_form_destroy(&form);
		return rc;
	}

	*out = form;
	return 0;
}

void cli_setup_form_destroy(struct cli_setup_form **owner)
{
	if (!owner || !*owner)
		return;

	struct cli_setup_form *form = *owner;
	if (form->controller)
		nt_view_controller_destroy(form->controller);

	nt_surface_destroy(form->surface);
	free(form);
	*owner = NULL;
}
