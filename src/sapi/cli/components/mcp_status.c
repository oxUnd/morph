#include "sapi/cli/components/mcp_status.h"

#include "sapi/cli/presentation/notice.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

#include <stdlib.h>
#include <string.h>

struct cli_mcp_status_row {
	struct nt_stack_view base;
	char *server;
	struct nt_view *marker, *name, *title, *detail, *footer;
};

struct cli_mcp_status {
	struct nt_stack_view base;
	struct nt_builder *builder;
	int styled;
};

static void cli_mcp_status_row_destroy(struct nt_view *view)
{
	struct cli_mcp_status_row *row = (struct cli_mcp_status_row *)view;
	free(row->server);
}

static const struct nt_view_class cli_mcp_status_row_class = {
	.superclass = &nt_stack_view_class,
	.instance_size = sizeof(struct cli_mcp_status_row),
	.kind = "cli_mcp_status_row",
	.deinit = cli_mcp_status_row_destroy,
};

static const struct nt_view_class cli_mcp_status_class = {
	.superclass = &nt_stack_view_class,
	.instance_size = sizeof(struct cli_mcp_status),
	.kind = "cli_mcp_status",
};

struct nt_view *cli_mcp_status_add(struct nt_builder *builder, struct nt_view *parent,
	int styled)
{
	struct cli_mcp_status *widget = (struct cli_mcp_status *)nt_view_add(builder, parent,
		&cli_mcp_status_class, &(struct nt_view_options){.width = NT_FILL});
	if (!widget)
		return NULL;

	widget->builder = builder;
	widget->styled = styled;
	return &widget->base.base;
}

static struct cli_mcp_status_row *cli_mcp_status_row_add(struct cli_mcp_status *widget,
	const char *server)
{
	struct nt_builder *builder = widget->builder;
	struct cli_mcp_status_row *row = (struct cli_mcp_status_row *)nt_view_add(builder,
		&widget->base.base, &cli_mcp_status_row_class,
		&(struct nt_view_options){.width = NT_FILL});
	if (!row)
		return NULL;

	row->server = strdup(server);
	if (!row->server) {
		nt_builder_fail(builder, NB_ERR_NOMEM);
		return NULL;
	}

	struct nt_view *line = nt_row_add(builder, &row->base.base, 0,
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	row->marker = nt_label_add(builder, line, "",
		&(struct nt_view_options){.width = 2});
	row->name = nt_label_add(builder, line, "",
		&(struct nt_view_options){.grow = 1, .width = NT_FILL});
	row->title = nt_label_add(builder, line, "",
		&(struct nt_view_options){.margin = {.left = 2}});
	struct nt_view_options detail = {.width = NT_FILL};
	if (widget->styled)
		detail.style.attributes = NT_STYLE_DIM;

	row->detail = nt_label_add(builder, &row->base.base, "", &detail);
	row->footer = nt_label_add(builder, &row->base.base, "", &detail);
	if (!nt_builder_error(builder)) {
		struct nt_view *labels[] = {
			row->marker, row->name, row->title, row->detail, row->footer,
		};
		for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
			nt_builder_fail(builder, nt_view_set_selectable(labels[i], 0));

		nt_builder_fail(builder, nt_label_set_wrap_mode(row->marker, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(row->name, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(row->title, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_view_set_hidden(row->detail, 1));
		nt_builder_fail(builder, nt_view_set_hidden(row->footer, 1));
	}

	return nt_builder_error(builder) ? NULL : row;
}

static int cli_mcp_status_text(struct nt_view *label, const char *text, const char *prefix)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text ? text : "", text ? strlen(text) : 0, MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	nb_buf value NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_printf(&value, "%s%s", *safe ? prefix : "", safe))
		return NB_ERR_NOMEM;

	return strcmp(nt_label_text(label), nb_buf_cstr(&value))
		? nt_label_set_text(label, nb_buf_cstr(&value)) : 0;
}

int cli_mcp_status_update(struct nt_view *view, const char *server,
	const struct cli_notice *notice)
{
	if (!view || view->type != &cli_mcp_status_class || !server || !server[0] ||
	    !notice || !notice->title)
		return NB_ERR_INVAL;

	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	struct cli_mcp_status *widget = (struct cli_mcp_status *)view;
	struct cli_mcp_status_row *row = NULL;
	for (size_t i = 0; i < nt_view_child_count(view); i++) {
		struct cli_mcp_status_row *candidate =
			(struct cli_mcp_status_row *)nt_view_child(view, i);
		if (!strcmp(candidate->server, server)) {
			row = candidate;
			break;
		}
	}

	if (!row)
		row = cli_mcp_status_row_add(widget, server);

	if (!row)
		return nt_builder_error(widget->builder);

	int rc = cli_mcp_status_text(row->marker, cli_notice_marker(notice->status), "");
	if (!rc)
		rc = cli_mcp_status_text(row->name, server, "");

	if (!rc)
		rc = cli_mcp_status_text(row->title, notice->title, "");

	if (!rc)
		rc = cli_mcp_status_text(row->detail, notice->detail,
			notice->footer && *notice->footer ? "├ " : "└ ");

	if (!rc)
		rc = cli_mcp_status_text(row->footer, notice->footer, "└ ");

	if (!rc)
		rc = nt_view_set_hidden(row->detail, !notice->detail || !*notice->detail);

	if (!rc)
		rc = nt_view_set_hidden(row->footer, !notice->footer || !*notice->footer);

	struct nt_text_style style = cli_notice_native_style(notice->status, widget->styled);
	if (!rc)
		rc = nt_view_set_style(row->marker, style);

	if (!rc)
		rc = nt_view_set_style(row->title, style);

	return rc;
}
