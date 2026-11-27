#include "sapi/cli/components/disclosure.h"

#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <string.h>

struct cli_disclosure {
	struct nt_stack_view base;
	struct nt_view *header;
	struct nt_component_state *title, *expanded;
};

static int cli_disclosure_body_hidden(void *data, struct nt_value *value)
{
	return nt_value_set_bool(value, !nt_component_state_get_bool(data));
}

static int cli_disclosure_label(struct cli_disclosure *disclosure, int expanded)
{
	nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&label, "%s · %s details",
		nt_component_state_get_string(disclosure->title), expanded ? "Hide" : "Show"))
		return nb_buf_status(&label);

	return nt_button_set_label(disclosure->header, nb_buf_cstr(&label));
}

static int cli_disclosure_activate(void *data, const struct nt_view_event *event)
{
	struct cli_disclosure *disclosure = data;
	(void)event;
	int expanded = !nt_component_state_get_bool(disclosure->expanded);
	int rc = nt_component_state_set_bool(disclosure->expanded, expanded != 0);
	if (!rc)
		rc = cli_disclosure_label(disclosure, expanded);

	return rc ? rc : NT_VIEW_EVENT_STOP;
}

struct nt_view *cli_disclosure_add_preview(struct nt_builder *builder, struct nt_view *parent,
	const char *title, const char *preview, const char *body, int styled)
{
	if (!title || !body) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}

	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_disclosure),
		.kind = "cli_disclosure",
	};
	struct cli_disclosure *disclosure = (struct cli_disclosure *)nt_view_add(
		builder, parent, &type,
		&(struct nt_view_options){.width = NT_FILL, .margin = {.top = 1}});
	if (!disclosure)
		return NULL;

	struct nt_view *view = &disclosure->base.base;
	nt_builder_fail(builder, nt_view_use_state_string(view, title, &disclosure->title));
	nt_builder_fail(builder, nt_view_use_state_bool(view, false, &disclosure->expanded));
	disclosure->header = nt_button_add(builder, view, "", 0, NULL);
	if (!disclosure->header)
		return NULL;

	nt_builder_fail(builder, cli_disclosure_label(disclosure, 0));
	if (!nt_view_on(disclosure->header, NT_VIEW_EVENT_ACTIVATE,
		cli_disclosure_activate, disclosure))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		body, strlen(body), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe) {
		nt_builder_fail(builder, NB_ERR_NOMEM);
		return NULL;
	}

	struct nt_view_options options = {.width = NT_FILL, .padding = {.left = 2}};
	if (styled)
		options.style.foreground = (struct nt_color){NT_COLOR_INDEXED, 245};

	if (preview && *preview) {
		char *safe_preview NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			preview, strlen(preview), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe_preview) {
			nt_builder_fail(builder, NB_ERR_NOMEM);
			return NULL;
		}

		nt_label_add(builder, view, safe_preview,
			&(struct nt_view_options){.width = NT_FILL, .style = options.style});
	}

	struct nt_view *content = nt_label_add(builder, view, safe, &options);
	if (content)
		nt_builder_fail(builder, nt_view_bind_compute(content, NT_PROP_HIDDEN,
			cli_disclosure_body_hidden, disclosure->expanded));

	return view;
}

struct nt_view *cli_disclosure_add(struct nt_builder *builder, struct nt_view *parent,
	const char *title, const char *body, int styled)
{
	return cli_disclosure_add_preview(builder, parent, title, NULL, body, styled);
}
