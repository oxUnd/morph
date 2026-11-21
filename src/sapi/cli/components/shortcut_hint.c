#include "sapi/cli/components/shortcut_hint.h"

#include "sapi/cli/presentation/shortcuts.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>

#include <string.h>

struct cli_shortcut_hint {
	struct nt_stack_view base;
	struct nt_view *text;
};

int cli_shortcut_hint_set_text(struct nt_view *view, const char *text)
{
	if (!view || !text)
		return NB_ERR_INVAL;

	struct cli_shortcut_hint *hint = (struct cli_shortcut_hint *)view;
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(text, strlen(text),
		MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	if (!strcmp(nt_text_view_text(hint->text), safe))
		return 0;

	const struct nt_theme *theme = nt_view_theme(view);
	struct nt_text_span span = {.text = safe,
		.style = nt_theme_style(theme, NT_THEME_MUTED, NT_VISUAL_NORMAL)};
	return cli_shortcuts_set_spans(hint->text, &span, 1, theme->color);
}

static int cli_shortcut_hint_get(const struct nt_view *view, struct nt_value *value)
{
	const struct cli_shortcut_hint *hint = (const struct cli_shortcut_hint *)view;
	return nt_value_set_string(value, nt_text_view_text(hint->text));
}

static int cli_shortcut_hint_set(struct nt_view *view, const struct nt_value *value)
{
	return value->type == NT_VALUE_STRING ? cli_shortcut_hint_set_text(view, value->as.string)
		: NB_ERR_INVAL;
}

struct nt_view *cli_shortcut_hint_add(struct nt_builder *builder, struct nt_view *parent,
	const char *text, const struct nt_view_options *options)
{
	static const struct nt_property_descriptor properties[] = {
		{NT_PROP_TEXT, NT_VALUE_STRING, cli_shortcut_hint_get, cli_shortcut_hint_set},
	};
	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_shortcut_hint),
		.kind = "cli_shortcut_hint",
		.properties = properties,
		.property_count = sizeof(properties) / sizeof(properties[0]),
	};
	struct nt_view_options opts = options ? *options : (struct nt_view_options){0};
	opts.style.attributes = 0;
	opts.style.foreground = (struct nt_color){0};
	struct cli_shortcut_hint *hint = (struct cli_shortcut_hint *)nt_view_add(builder, parent,
		&type, &opts);
	if (!hint)
		return NULL;

	struct nt_view *root = &hint->base.base;
	hint->text = nt_text_view_add(builder, root, "",
		&(struct nt_view_options){.width = NT_FILL});
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_view_set_focusable(hint->text, 0));
		nt_builder_fail(builder, nt_view_set_selectable(hint->text, 0));
		nt_builder_fail(builder, nt_text_view_set_fill_lines(hint->text, 1));
		nt_builder_fail(builder, nt_text_view_set_wrap_mode(hint->text, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, cli_shortcut_hint_set_text(root, text ? text : ""));
	}

	return nt_builder_error(builder) ? NULL : root;
}

int cli_shortcut_hint_set_wrap_mode(struct nt_view *view, enum nt_text_wrap mode)
{
	if (!view)
		return NB_ERR_INVAL;

	return nt_text_view_set_wrap_mode(((struct cli_shortcut_hint *)view)->text, mode);
}
