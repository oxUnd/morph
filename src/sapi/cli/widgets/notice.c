#include "sapi/cli/widgets/notice.h"

#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include "sapi/cli/presentation/messages.h"
#include <morph/tui/widget.h>
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_notice_widget {
	struct morph_tui_widget base;
	struct morph_tui_view *body;
	struct cli_notice notice;
	char *title, *detail, *footer;
	int columns, styled;
};

static int cli_notice_widget_measure(struct morph_tui_view *view, int width,
				     struct morph_tui_view_size *size)
{
	struct cli_notice_widget *widget = (struct cli_notice_widget *)view;
	if (widget->columns != width) {
		morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = cli_notice_append(&rendered, &widget->notice,
			width + (int)CLI_CONTENT_RIGHT_PADDING, widget->styled);
		while (!rc && rendered.len && rendered.data[rendered.len - 1] == '\n')
			rendered.data[--rendered.len] = '\0';
		if (!rc)
			rc = cli_messages_set_rendered_view(
				widget->body, morph_buf_cstr_borrow(&rendered), widget->styled);
		if (rc)
			return rc;
		widget->columns = width;
	}
	return morph_tui_view_measure(widget->body, width, size);
}

static int cli_notice_widget_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_notice_widget *)view)->body,
				      morph_tui_view_content_rect(view));
}

static void cli_notice_widget_destroy(struct morph_tui_view *view)
{
	struct cli_notice_widget *widget = (struct cli_notice_widget *)view;
	free(widget->title);
	free(widget->detail);
	free(widget->footer);
}

static const struct morph_tui_view_ops cli_notice_widget_ops = {
	.measure = cli_notice_widget_measure,
	.layout = cli_notice_widget_layout,
	.destroy = cli_notice_widget_destroy,
};

struct morph_tui_view *cli_notice_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const struct cli_notice *notice, int styled)
{
	if (!notice || !notice->title) {
		morph_tui_builder_fail(builder, -EINVAL);
		return NULL;
	}
	const struct morph_tui_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_notice_widget *widget = (struct cli_notice_widget *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*widget), &cli_notice_widget_ops, &options);
	if (!widget)
		return NULL;
	struct morph_tui_view *view = &widget->base.view;
	widget->title = strdup(notice->title);
	widget->detail = strdup(notice->detail ? notice->detail : "");
	widget->footer = strdup(notice->footer ? notice->footer : "");
	if (!widget->title || !widget->detail || !widget->footer)
		morph_tui_builder_fail(builder, -ENOMEM);
	widget->notice = (struct cli_notice){.status = notice->status,
		.title = widget->title, .detail = widget->detail, .footer = widget->footer,
		.tree_branch = notice->tree_branch};
	widget->columns = -1;
	widget->styled = styled;
	widget->body = morph_tui_widget_text_view_add_borrow(builder, view, "", NULL);
	if (widget->body) {
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_view_set_content_sizing(widget->body, 1));
		morph_tui_view_set_focusable(widget->body, 0);
	}
	return view;
}

const char *cli_notice_marker(enum cli_notice_status status)
{
	switch (status) {
	case CLI_NOTICE_PENDING: return CLI_FEED_PENDING;
	case CLI_NOTICE_RUNNING: return CLI_FEED_RUNNING;
	case CLI_NOTICE_DONE: return CLI_FEED_DONE;
	case CLI_NOTICE_FAILED: return CLI_FEED_FAILED;
	case CLI_NOTICE_QUESTION: return CLI_FEED_QUESTION;
	case CLI_NOTICE_CANCELLED: return CLI_FEED_CANCELLED;
	case CLI_NOTICE_WARNING: return CLI_FEED_WARNING;
	case CLI_NOTICE_SYSTEM: return CLI_FEED_SYSTEM;
	}
	return CLI_FEED_SYSTEM;
}

const char *cli_notice_color(enum cli_notice_status status)
{
	switch (status) {
	case CLI_NOTICE_RUNNING: return ANSI_CYAN;
	case CLI_NOTICE_DONE: return ANSI_GREEN;
	case CLI_NOTICE_FAILED: return CLI_FEED_FAILED_STYLE;
	case CLI_NOTICE_QUESTION: return CLI_FEED_QUESTION_STYLE;
	case CLI_NOTICE_WARNING: return ANSI_YELLOW;
	default: return ANSI_DIM;
	}
}

static int cli_notice_text(morph_buf_t *out, const char *text, int columns,
			   const char *continuation)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	if (columns < 1)
		return 0;
	const char *line = safe;
	int rc = 0;
	while (*line && rc == 0) {
		const char *end = utf8_advance_display_width_borrow(line, (size_t)columns);
		if (*end) {
			const char *space = end;
			while (space > line && *space != ' ')
				space--;
			if (space > line)
				end = space;
		}
		if (end == line)
			return morph_buf_puts(out, "…");
		rc = morph_buf_append(out, line, (size_t)(end - line));
		line = end;
		while (*line == ' ')
			line++;
		if (*line && rc == 0)
			rc = morph_buf_printf(out, "\n%s", continuation);
	}
	return rc;
}

static int cli_notice_children(morph_buf_t *out, const char *text, int columns, int last)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return -ENOMEM;
	const char *line = safe;
	int first = 1;
	int width = columns > 4 ? columns - 4 : 1;
	int rc = 0;
	while (*line && rc == 0) {
		const char *newline = strchr(line, '\n');
		const char *end = utf8_advance_display_width_borrow(line, (size_t)width);
		if (newline && newline < end)
			end = newline;
		if (end == line && *line != '\n')
			end += utf8_next_codepoint_len(line, strlen(line));
		rc = morph_buf_puts(out, first ? (last ? "└ " : "├ ")
					     : (last ? "  " : "│ "));
		if (rc == 0)
			rc = morph_buf_append(out, line, (size_t)(end - line));
		if (rc == 0)
			rc = morph_buf_putc(out, '\n');
		line = *end == '\n' ? end + 1 : end;
		first = 0;
	}
	return rc;
}

int cli_notice_append(morph_buf_t *out, const struct cli_notice *notice, int columns, int styled)
{
	if (!out || !notice || !notice->title)
		return -EINVAL;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING;
	const char *prefix = notice->tree_branch == 1 ? "├ "
		: notice->tree_branch == 2 ? "└ " : "";
	const char *color = styled ? cli_notice_color(notice->status) : "";
	int rc = morph_buf_printf(out, "%s%s%s ", prefix, color, cli_notice_marker(notice->status));
	if (rc == 0)
		rc = cli_notice_text(out, notice->title, width - 2 - (notice->tree_branch ? 2 : 0),
			notice->tree_branch == 1 ? "│   " :
			notice->tree_branch == 2 ? "    " : "  ");
	if (rc == 0)
		rc = morph_buf_puts(out, styled ? ANSI_RESET "\n" : "\n");
	int footer = notice->footer && *notice->footer;
	if (rc == 0 && notice->detail && *notice->detail)
		rc = cli_notice_children(out, notice->detail, width, !footer);
	if (rc == 0 && footer)
		rc = cli_notice_children(out, notice->footer, width, 1);
	return rc;
}
