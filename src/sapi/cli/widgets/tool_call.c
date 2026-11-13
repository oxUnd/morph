#include "sapi/cli/widgets/tool_call.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/transcript.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_view.h>
#include <morph/tui/widgets/tree.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_tool_call {
	struct morph_tui_widget base;
	struct morph_tui_view *stack, *heading, *body;
	struct cli_transcript_tool *tool;
	int columns, styled, details;
};

static void cli_tool_call_snapshot_destroy(struct cli_transcript_tool *tool)
{
	if (!tool)
		return;
	free(tool->id);
	free(tool->name);
	free(tool->agent_id);
	free(tool->args);
	free(tool->subject);
	free(tool->result);
	free(tool->error);
	free(tool->summary);
	free(tool->patch_preview);
	morph_buf_cleanup(&tool->stream);
	free(tool);
}

static void cli_tool_call_snapshot_cleanup(struct cli_transcript_tool **tool)
{
	cli_tool_call_snapshot_destroy(*tool);
	*tool = NULL;
}

static char *cli_tool_call_string_copy(const char *text)
{
	return strdup(text ? text : "");
}

static int cli_tool_call_snapshot_copy(const struct cli_transcript_tool *source,
				       struct cli_transcript_tool **out)
{
	struct cli_transcript_tool *tool MORPH_CLEANUP(cli_tool_call_snapshot_cleanup) =
		calloc(1, sizeof(*tool));
	if (!tool)
		return -ENOMEM;
	tool->id = cli_tool_call_string_copy(source->id);
	tool->name = cli_tool_call_string_copy(source->name);
	tool->agent_id = cli_tool_call_string_copy(source->agent_id);
	tool->args = cli_tool_call_string_copy(source->args);
	tool->subject = cli_tool_call_string_copy(source->subject);
	tool->result = cli_tool_call_string_copy(source->result);
	tool->error = cli_tool_call_string_copy(source->error);
	tool->summary = cli_tool_call_string_copy(source->summary);
	tool->patch_preview = cli_tool_call_string_copy(source->patch_preview);
	if (!tool->id || !tool->name || !tool->agent_id || !tool->args || !tool->subject ||
	    !tool->result || !tool->error || !tool->summary || !tool->patch_preview)
		return -ENOMEM;
	int rc = morph_buf_append(&tool->stream, morph_buf_cstr_borrow(&source->stream),
				  source->stream.len);
	if (rc)
		return rc;
	tool->state = source->state;
	tool->timed_out = source->timed_out;
	tool->historical = source->historical;
	tool->interrupted = source->interrupted;
	tool->shell = source->shell;
	tool->process_poll = source->process_poll;
	tool->process_running = source->process_running;
	tool->started = source->started;
	tool->ended = source->ended;
	*out = tool;
	tool = NULL;
	return 0;
}

/* The live preview keeps the same three tail rows and branches as the original feed. */
static int cli_tool_call_live_body(struct cli_tool_call *widget, morph_buf_t *out, int columns)
{
	const struct cli_transcript_tool *tool = widget->tool;
	if (cli_agent_tools_matches(tool->name))
		return 0;
	const char *stream = morph_buf_cstr_borrow(&tool->stream);
	const char *end = stream + tool->stream.len;
	while (end > stream && end[-1] == '\n')
		end--;
	if (end == stream)
		return morph_buf_printf(out, "%s    Running…%s", widget->styled ? ANSI_DIM : "",
					 widget->styled ? ANSI_RESET : "");
	const char *start = end;
	int rows = 0;
	while (start > stream) {
		if (start[-1] == '\n' && ++rows == 3)
			break;
		start--;
	}
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - 4;
	rows = 0;
	int rc = 0;
	while (!rc && start < end) {
		const char *newline = memchr(start, '\n', (size_t)(end - start));
		char *line MORPH_CLEANUP(morph_charp_cleanup) =
			strndup(start, (size_t)((newline ? newline : end) - start));
		if (!line)
			return -ENOMEM;
		const char *limit = utf8_advance_display_width_borrow(line,
								  width > 0 ? (size_t)width : 0);
		int clipped = width > 0 && *limit;
		if (clipped)
			limit = utf8_advance_display_width_borrow(line, (size_t)(width - 1));
		if (rows)
			rc = morph_buf_putc(out, '\n');
		if (!rc && widget->styled)
			rc = morph_buf_puts(out, ANSI_DIM);
		if (!rc)
			rc = morph_tui_widget_tree_prefix(out, NULL, 0, 1, rows != 0, 2);
		if (!rc)
			rc = morph_buf_append(out, line, (size_t)(limit - line));
		if (!rc && clipped)
			rc = morph_buf_puts(out, "…");
		if (!rc && widget->styled)
			rc = morph_buf_puts(out, ANSI_RESET);
		rows++;
		start = newline ? newline + 1 : end;
	}
	return rc;
}

static int cli_tool_call_render(struct cli_tool_call *widget, int width)
{
	if (widget->columns == width)
		return 0;
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int columns = width + (int)CLI_CONTENT_RIGHT_PADDING;
	int rc = cli_transcript_tool_append(&document, widget->tool, columns, widget->styled,
					    widget->details);
	if (rc)
		return rc;
	while (document.len && document.data[document.len - 1] == '\n')
		document.data[--document.len] = '\0';
	char *newline = document.data ? strchr(document.data, '\n') : NULL;
	const char *body = "";
	if (newline) {
		*newline = '\0';
		body = newline + 1;
	}
	morph_buf_t live MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (!widget->details && cli_transcript_tool_is_running(widget->tool)) {
		rc = cli_tool_call_live_body(widget, &live, columns);
		if (!rc)
			body = morph_buf_cstr_borrow(&live);
	}
	if (!rc)
		rc = cli_messages_set_rendered_view(
			widget->heading, morph_buf_cstr_borrow(&document), widget->styled);
	if (!rc)
		rc = cli_messages_set_rendered_view(widget->body, body, widget->styled);
	if (!rc)
		rc = morph_tui_view_set_hidden(widget->body, !*body);
	if (!rc)
		rc = morph_tui_view_set_hidden(widget->stack, !document.len);
	if (!rc)
		widget->columns = width;
	return rc;
}

static int cli_tool_call_measure(struct morph_tui_view *view, int width,
				 struct morph_tui_view_size *size)
{
	struct cli_tool_call *widget = (struct cli_tool_call *)view;
	int rc = cli_tool_call_render(widget, width);
	return rc ? rc : morph_tui_view_measure(widget->stack, width, size);
}

static int cli_tool_call_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_tool_call *)view)->stack,
				      morph_tui_view_content_rect(view));
}

static void cli_tool_call_destroy(struct morph_tui_view *view)
{
	cli_tool_call_snapshot_destroy(((struct cli_tool_call *)view)->tool);
}

static const struct morph_tui_view_ops cli_tool_call_ops = {
	.measure = cli_tool_call_measure,
	.layout = cli_tool_call_layout,
	.destroy = cli_tool_call_destroy,
};

int cli_tool_call_update(struct morph_tui_view *view, const struct cli_transcript_tool *tool,
			 int styled, int details)
{
	if (!view || view->ops != &cli_tool_call_ops || !tool)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct cli_tool_call *widget = (struct cli_tool_call *)view;
	struct cli_transcript_tool *copy = NULL;
	int rc = cli_tool_call_snapshot_copy(tool, &copy);
	if (rc)
		return rc;
	cli_tool_call_snapshot_destroy(widget->tool);
	widget->tool = copy;
	widget->styled = styled;
	widget->details = details;
	widget->columns = -1;
	morph_tui_view_invalidate(view, 1);
	return morph_tui_view_set_hidden(view,
		!details && cli_agent_tools_analytics_only(tool->name));
}

struct morph_tui_view *cli_tool_call_add_borrow(struct morph_tui_builder *builder,
						struct morph_tui_view *parent,
						const struct cli_transcript_tool *tool, int styled,
						int details)
{
	const struct morph_tui_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_tool_call *widget = (struct cli_tool_call *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*widget), &cli_tool_call_ops, &options);
	if (!widget)
		return NULL;
	struct morph_tui_view *view = &widget->base.view;
	widget->stack = morph_tui_widget_stack_add_borrow(builder, view,
						  MORPH_TUI_WIDGET_STACK_VERTICAL, 0, NULL);
	widget->heading = morph_tui_widget_text_view_add_borrow(builder, widget->stack, "", NULL);
	widget->body = morph_tui_widget_text_view_add_borrow(builder, widget->stack, "", NULL);
	struct morph_tui_view *documents[] = {widget->heading, widget->body};
	for (size_t i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
		if (documents[i]) {
			morph_tui_builder_fail(builder,
				  morph_tui_widget_text_view_set_content_sizing(documents[i], 1));
			morph_tui_view_set_focusable(documents[i], 0);
		}
	}
	if (!morph_tui_builder_error(builder))
		morph_tui_builder_fail(builder, cli_tool_call_update(view, tool, styled, details));
	return view;
}
