#include "sapi/cli/components/notice.h"
#include <nb/err.h>

#include "sapi/cli/presentation/style.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include "sapi/cli/components/report.h"
#include <nothing/tui/component_state.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_notice_widget {
	struct cli_report report;
	enum cli_notice_status status;
	int tree_branch;
	struct nt_component_state *title, *detail, *footer;
};

static int cli_notice_widget_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_notice_widget *widget = (struct cli_notice_widget *)report;
	const struct cli_notice notice = {.status = widget->status,
					  .tree_branch = widget->tree_branch,
					  .title = nt_component_state_get_string(widget->title),
					  .detail = nt_component_state_get_string(widget->detail),
					  .footer = nt_component_state_get_string(widget->footer)};
	return cli_notice_append(out, &notice, width + (int)CLI_CONTENT_RIGHT_PADDING,
				 report->styled);
}

static const struct nt_view_class cli_notice_widget_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_notice_widget),
	.kind = "cli_notice_widget",
};

struct nt_view *cli_notice_add_borrow(struct nt_builder *builder, struct nt_view *parent,
				      const struct cli_notice *notice, int styled)
{
	if (!notice || !notice->title) {
		nt_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	const struct nt_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_notice_widget *widget = (struct cli_notice_widget *)cli_report_add(
		builder, parent, &cli_notice_widget_class, &options, styled,
		cli_notice_widget_format);
	if (!widget)
		return NULL;
	struct nt_view *view = &widget->report.base;
	widget->status = notice->status;
	widget->tree_branch = notice->tree_branch;
	int rc = nt_view_use_state_string(view, notice->title, &widget->title);
	if (!rc)
		rc = nt_view_use_state_string(view, notice->detail ? notice->detail : "",
					      &widget->detail);
	if (!rc)
		rc = nt_view_use_state_string(view, notice->footer ? notice->footer : "",
					      &widget->footer);
	nt_builder_fail(builder, rc);
	return view;
}

const char *cli_notice_marker(enum cli_notice_status status)
{
	switch (status) {
	case CLI_NOTICE_PENDING:
		return CLI_FEED_PENDING;
	case CLI_NOTICE_RUNNING:
		return CLI_FEED_RUNNING;
	case CLI_NOTICE_DONE:
		return CLI_FEED_DONE;
	case CLI_NOTICE_FAILED:
		return CLI_FEED_FAILED;
	case CLI_NOTICE_QUESTION:
		return CLI_FEED_QUESTION;
	case CLI_NOTICE_CANCELLED:
		return CLI_FEED_CANCELLED;
	case CLI_NOTICE_WARNING:
		return CLI_FEED_WARNING;
	case CLI_NOTICE_SYSTEM:
		return CLI_FEED_SYSTEM;
	}
	return CLI_FEED_SYSTEM;
}

const char *cli_notice_color(enum cli_notice_status status)
{
	switch (status) {
	case CLI_NOTICE_RUNNING:
		return ANSI_CYAN;
	case CLI_NOTICE_DONE:
		return ANSI_GREEN;
	case CLI_NOTICE_FAILED:
		return CLI_FEED_FAILED_STYLE;
	case CLI_NOTICE_QUESTION:
		return CLI_FEED_QUESTION_STYLE;
	case CLI_NOTICE_WARNING:
		return ANSI_YELLOW;
	default:
		return ANSI_DIM;
	}
}

static int cli_notice_text(nb_buf *out, const char *text, int columns, const char *continuation)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	if (columns < 1)
		return 0;
	const char *line = safe;
	int rc = 0;
	while (*line && rc == 0) {
		const char *end = nt_terminal_advance_display_width(line, (size_t)columns);
		if (*end) {
			const char *space = end;
			while (space > line && *space != ' ')
				space--;
			if (space > line)
				end = space;
		}
		if (end == line)
			return (nb_buf_append_cstr(out, "…") ? 0 : NB_ERR_NOMEM);
		rc = (nb_buf_append(out, line, (size_t)(end - line)) ? 0 : NB_ERR_NOMEM);
		line = end;
		while (*line == ' ')
			line++;
		if (*line && rc == 0)
			rc = (nb_buf_printf(out, "\n%s", continuation) ? 0 : NB_ERR_NOMEM);
	}
	return rc;
}

static int cli_notice_children(nb_buf *out, const char *text, int columns, int last)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	const char *line = safe;
	int first = 1;
	int width = columns > 4 ? columns - 4 : 1;
	int rc = 0;
	while (*line && rc == 0) {
		const char *newline = strchr(line, '\n');
		const char *end = nt_terminal_advance_display_width(line, (size_t)width);
		if (newline && newline < end)
			end = newline;
		if (end == line && *line != '\n')
			end += nb_utf8_next_codepoint_len(line, strlen(line));
		rc = (nb_buf_append_cstr(out, first ? (last ? "└ " : "├ ") : (last ? "  " : "│ "))
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append(out, line, (size_t)(end - line)) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
		line = *end == '\n' ? end + 1 : end;
		first = 0;
	}
	return rc;
}

int cli_notice_append(nb_buf *out, const struct cli_notice *notice, int columns, int styled)
{
	if (!out || !notice || !notice->title)
		return NB_ERR_INVAL;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING;
	const char *prefix = notice->tree_branch == 1 ? "├ " : notice->tree_branch == 2 ? "└ " : "";
	const char *color = styled ? cli_notice_color(notice->status) : "";
	int rc = (nb_buf_printf(out, "%s%s%s ", prefix, color, cli_notice_marker(notice->status))
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_notice_text(out, notice->title, width - 2 - (notice->tree_branch ? 2 : 0),
				     notice->tree_branch == 1	? "│   "
				     : notice->tree_branch == 2 ? "    "
								: "  ");
	if (rc == 0)
		rc = (nb_buf_append_cstr(out, styled ? ANSI_RESET "\n" : "\n") ? 0 : NB_ERR_NOMEM);
	int footer = notice->footer && *notice->footer;
	if (rc == 0 && notice->detail && *notice->detail)
		rc = cli_notice_children(out, notice->detail, width, !footer);
	if (rc == 0 && footer)
		rc = cli_notice_children(out, notice->footer, width, 1);
	return rc;
}
