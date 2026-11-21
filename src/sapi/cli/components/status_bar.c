#include "sapi/cli/components/status_bar.h"

#include "sapi/cli/presentation/style.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

#include <string.h>

struct cli_status_bar {
	struct nt_stack_view base;
	struct nt_view *leading, *button, *caption, *trailing, *extra;
};

static const struct nt_view_class cli_status_bar_class = {
	.superclass = &nt_stack_view_class,
	.instance_size = sizeof(struct cli_status_bar),
	.kind = "cli_status_bar",
};

struct nt_view *cli_status_bar_add(struct nt_builder *builder, struct nt_view *parent, int styled)
{
	struct cli_status_bar *bar = (struct cli_status_bar *)nt_view_add(builder, parent,
		&cli_status_bar_class,
		&(struct nt_view_options){.padding = {.left = 2, .right = 2}});
	if (!bar)
		return NULL;

	struct nt_view *root = &bar->base.base;
	struct nt_view_options label = {0};
	if (styled)
		label.style.attributes = NT_STYLE_DIM;

	struct nt_view *row = nt_row_add(builder, root, 0, NULL);
	bar->leading = nt_label_add(builder, row, "", &label);
	struct nt_view_options button = {.height = 1};
	if (styled) {
		button.style.foreground =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_LINK_INDEX};
		button.style.attributes = NT_STYLE_UNDERLINE;
	}

	bar->button = nt_button_add(builder, row, "MCP", 0, &button);
	bar->caption = nt_label_add(builder, bar->button, "MCP",
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	bar->trailing = nt_label_add(builder, row, "", &label);
	bar->extra = nt_label_add(builder, root, "", &label);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_label_set_opaque(bar->caption, 1));
		nt_builder_fail(builder, nt_view_set_selectable(bar->caption, 0));
		nt_builder_fail(builder, nt_view_set_hidden(bar->button, 1));
		nt_builder_fail(builder, nt_view_set_hidden(bar->extra, 1));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->leading, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->caption, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->trailing, NT_TEXT_WRAP_NONE));
	}

	return nt_builder_error(builder) ? NULL : root;
}

int cli_status_bar_set_text(struct nt_view *view, const char *text)
{
	if (!view || view->type != &cli_status_bar_class || !text)
		return NB_ERR_INVAL;

	struct cli_status_bar *bar = (struct cli_status_bar *)view;
	char *plain NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!plain)
		return NB_ERR_NOMEM;

	char *extra = strchr(plain, '\n');
	if (extra)
		*extra++ = '\0';

	char *mcp = strstr(plain, "MCP ");
	char *tail = mcp ? strstr(mcp, " · ") : NULL;
	nb_buf leading NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf caption NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_append(&leading, plain, mcp ? (size_t)(mcp - plain) : strlen(plain)) ||
	    (mcp && !nb_buf_append(&caption, mcp, tail ? (size_t)(tail - mcp) : strlen(mcp))))
		return NB_ERR_NOMEM;

	int rc = nt_label_set_text(bar->leading, nb_buf_cstr(&leading));
	if (!rc)
		rc = nt_button_set_label(bar->button, nb_buf_cstr(&caption));

	if (!rc)
		rc = nt_label_set_text(bar->caption, nb_buf_cstr(&caption));

	struct nt_view_options options = nt_view_options(bar->button);
	options.width = (int)nb_utf8_width(nb_buf_str(&caption));
	if (!rc)
		rc = nt_view_set_options(bar->button, &options);

	if (!rc)
		rc = nt_view_set_hidden(bar->button, !mcp);

	if (!rc)
		rc = nt_label_set_text(bar->trailing, tail ? tail : "");

	if (!rc)
		rc = nt_label_set_text(bar->extra, extra ? extra : "");

	if (!rc)
		rc = nt_view_set_hidden(bar->extra, !extra || !*extra);

	return rc;
}
