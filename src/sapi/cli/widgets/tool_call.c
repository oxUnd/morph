#include "sapi/cli/widgets/tool_call.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
#include <string.h>

struct cli_tool_call {
	struct morph_tui_widget base;
	struct morph_tui_view *stack, *heading, *body;
};

static int cli_tool_call_measure(struct morph_tui_view *view, int width,
				 struct morph_tui_view_size *size)
{
	return morph_tui_view_measure(((struct cli_tool_call *)view)->stack, width, size);
}

static int cli_tool_call_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_tool_call *)view)->stack,
				      morph_tui_view_content_rect(view));
}

static const struct morph_tui_view_ops cli_tool_call_ops = {
	.measure = cli_tool_call_measure,
	.layout = cli_tool_call_layout,
};

int cli_tool_call_update(struct morph_tui_view *view, const struct cli_transcript_tool *tool,
			 int styled, int details)
{
	if (!view || view->ops != &cli_tool_call_ops || !tool)
		return -EINVAL;
	struct cli_tool_call *widget = (struct cli_tool_call *)view;
	int running = cli_transcript_tool_is_running(tool);
	const char *marker = tool->interrupted			  ? CLI_FEED_CANCELLED
			     : tool->timed_out || tool->state < 0 ? CLI_FEED_FAILED
			     : running				  ? CLI_FEED_RUNNING
			     : tool->state			  ? CLI_FEED_DONE
								  : CLI_FEED_PENDING;
	unsigned color = tool->interrupted		      ? 8u
			 : tool->timed_out || tool->state < 0 ? 1u
			 : running			      ? 6u
			 : tool->state			      ? 2u
							      : 8u;
	struct morph_tui_text_style style = {0};
	if (styled) {
		style.foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, color};
		if (tool->timed_out || tool->state < 0)
			style.attributes = MORPH_TUI_STYLE_BOLD;
	}
	morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t body MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&title, "%s %s  %s%s", marker, tool->name,
				  tool->subject ? tool->subject : "",
				  tool->timed_out     ? " · TIMEOUT"
				  : tool->interrupted ? " · CANCELLED"
				  : tool->state < 0   ? " · FAILED"
						      : "");
	if (!rc && tool->ended > tool->started)
		rc = morph_buf_printf(&title, " · %.1fs", tool->ended - tool->started);
	if (!rc)
		rc = morph_tui_widget_label_set_text_copy(widget->heading,
							  morph_buf_cstr_borrow(&title));
	if (!rc)
		rc = morph_tui_view_set_style(widget->heading, style);
	if (!rc && details && tool->args && *tool->args)
		rc = morph_buf_printf(&body, "Arguments\n%s\n", tool->args);
	const char *stream = tool->stream.data ? morph_buf_cstr_borrow(&tool->stream) : "";
	if (!details) {
		const char *end = stream + strlen(stream);
		int lines = 0;
		while (end > stream && end[-1] == '\n')
			end--;
		const char *start = end;
		while (start > stream) {
			if (start[-1] == '\n' && ++lines == 3)
				break;
			start--;
		}
		if (!rc)
			rc = morph_buf_append(&body, start, (size_t)(end - start));
	} else if (!rc)
		rc = morph_buf_puts(&body, stream);
	if (!rc && details && tool->result && *tool->result)
		rc = morph_buf_printf(&body, "\nResult\n%s", tool->result);
	if (!rc && tool->error && *tool->error)
		rc = morph_buf_printf(&body, "\n%s", tool->error);
	if (!rc && !body.len && tool->summary)
		rc = morph_buf_puts(&body, tool->summary);
	if (!rc)
		rc = morph_tui_widget_text_view_set_text_copy(
			widget->body, body.data ? morph_buf_cstr_borrow(&body) : "");
	if (!rc)
		rc = morph_tui_view_set_hidden(widget->body, !body.len);
	return rc;
}

struct morph_tui_view *cli_tool_call_add_borrow(struct morph_tui_builder *builder,
						struct morph_tui_view *parent,
						const struct cli_transcript_tool *tool, int styled,
						int details)
{
	const struct morph_tui_view_options options = {0};
	struct cli_tool_call *widget = (struct cli_tool_call *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*widget), &cli_tool_call_ops, &options);
	if (!widget)
		return NULL;
	struct morph_tui_view *view = &widget->base.view;
	widget->stack = morph_tui_widget_stack_add_borrow(builder, view,
							  MORPH_TUI_WIDGET_STACK_VERTICAL, 0, NULL);
	widget->heading = morph_tui_widget_label_add_borrow(builder, widget->stack, "", NULL);
	const struct morph_tui_view_options body = {.padding = {.left = 2}};
	widget->body = morph_tui_widget_text_view_add_borrow(builder, widget->stack, "", &body);
	if (widget->body) {
		morph_tui_builder_fail(
			builder, morph_tui_widget_text_view_set_content_sizing(widget->body, 1));
		morph_tui_view_set_focusable(widget->body, 0);
	}
	if (!morph_tui_builder_error(builder))
		morph_tui_builder_fail(builder, cli_tool_call_update(view, tool, styled, details));
	return view;
}
