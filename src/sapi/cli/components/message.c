#include "sapi/cli/components/message.h"
#include <nb/err.h>
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/components/rich_document.h"
#include "sapi/cli/components/report.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/text_view.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_message {
	struct nt_view base;
	struct nt_view *marker, *body;
	struct nt_component_state *text, *ultra;
	enum cli_message_kind kind;
	nb_buf rendered;
	struct nt_view_size size;
	int measured_width, published_width, styled;
};

static int cli_message_format(struct cli_message *message, int columns)
{
	if (message->measured_width == columns)
		return 0;

	const char *text = nt_component_state_get_string(message->text);
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;
	if (message->kind == CLI_MESSAGE_ASSISTANT)
		rc = cli_markdown_render_retained(&rendered, text, columns, message->styled);
	else {
		struct cli_message_style style = {.columns = columns, .styled = message->styled};
		rc = cli_messages_append(&rendered, CLI_MESSAGE_REASONING, text, &style);
	}

	if (!rc)
		rc = cli_report_measure_document(&rendered, columns, &message->size);

	if (!rc) {
		nb_buf_move(&message->rendered, &rendered);
		message->measured_width = columns;
	}

	return rc;
}

static int cli_message_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	struct cli_message *message = (struct cli_message *)view;
	int prefix = message->marker ? 2 : 0;
	int columns = width > prefix ? width - prefix : 0;
	int rc;
	if (message->kind == CLI_MESSAGE_USER)
		rc = nt_view_measure(message->body, columns, size);
	else {
		rc = cli_message_format(message, columns);
		*size = message->size;
	}

	if (!rc) {
		size->width += prefix;
		if (message->marker && size->height < 1)
			size->height = 1;
	}

	return rc;
}

static int cli_message_layout(struct nt_view *view)
{
	struct cli_message *message = (struct cli_message *)view;
	struct nt_view_rect rect = nt_view_content_rect(view);
	int prefix = message->marker ? (rect.width > 2 ? 2 : rect.width) : 0;
	if (message->marker) {
		struct nt_view_rect marker = {rect.x, rect.y, prefix, rect.height ? 1 : 0};
		int rc = nt_view_arrange(message->marker, marker);
		if (rc)
			return rc;
	}

	rect.x += prefix;
	rect.width -= prefix;
	if (message->kind != CLI_MESSAGE_USER && rect.width != message->published_width) {
		int rc = cli_message_format(message, rect.width);
		if (rc)
			return rc;

		const char *rendered = nb_buf_cstr(&message->rendered);
		rc = message->kind == CLI_MESSAGE_ASSISTANT
			     ? cli_rich_document_set_rendered_copy(message->body, rendered,
								   message->styled)
			     : cli_messages_set_rendered_view(message->body, rendered,
							      message->styled);
		if (rc)
			return rc;

		if (message->kind == CLI_MESSAGE_ASSISTANT) {
			rc = cli_rich_document_bind_markdown(message->body,
				nt_component_state_get_string(message->text));
			if (rc)
				return rc;
		}

		message->published_width = rect.width;
	}

	return nt_view_arrange(message->body, rect);
}

static int cli_message_text(void *data, struct nt_value *result)
{
	return nt_value_set_string(result, nt_component_state_get_string(data));
}

static int cli_message_marker(void *data, struct nt_value *result)
{
	struct cli_message *message = data;
	return nt_value_set_string(result, nt_component_state_get_bool(message->ultra) ? "» "
					   : message->styled			       ? "› "
										       : "> ");
}

static int cli_message_background(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_message *message = (struct cli_message *)view;

	return message->styled && message->kind == CLI_MESSAGE_USER
		       ? nt_canvas_fill_styled(canvas, &canvas->style)
		       : 0;
}

static int cli_message_padding(struct nt_view *view, enum nt_view_edge edge,
			       struct nt_canvas *canvas)
{
	(void)edge;
	return cli_message_background(view, canvas);
}

static void cli_message_destroy(struct nt_view *view)
{
	nb_buf_cleanup(&((struct cli_message *)view)->rendered);
}

static int cli_message_hit_test_overlay(struct nt_view *view, int x, int y)
{
	struct cli_message *message = (struct cli_message *)view;
	if (message->kind == CLI_MESSAGE_ASSISTANT &&
	    cli_rich_document_has_link_at(message->body, x, y))
		return 0;

	/* Read-only messages use window selection while the composer keeps focus. */
	return 1;
}

static const struct nt_view_class cli_message_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct cli_message),
	.kind = "cli_message",
	.measure = cli_message_measure,
	.layout = cli_message_layout,
	.deinit = cli_message_destroy,
	.draw = cli_message_background,
	.draw_padding = cli_message_padding,
	.hit_test_overlay = cli_message_hit_test_overlay,
};

const char *cli_message_user_text_borrow(const struct nt_view *view)
{
	if (!view || view->type != &cli_message_class)
		return NULL;

	const struct cli_message *message = (const struct cli_message *)view;
	return message->kind == CLI_MESSAGE_USER
		? nt_component_state_get_string(message->text) : NULL;
}

int cli_message_user_is_ultra(const struct nt_view *view)
{
	if (!view || view->type != &cli_message_class)
		return 0;

	const struct cli_message *message = (const struct cli_message *)view;
	return message->kind == CLI_MESSAGE_USER && nt_component_state_get_bool(message->ultra);
}

int cli_message_set_ultra(struct nt_view *view, int ultra)
{
	if (!view || view->type != &cli_message_class)
		return NB_ERR_INVAL;

	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	struct cli_message *message = (struct cli_message *)view;
	if (message->kind != CLI_MESSAGE_USER)
		return 0;

	int rc = nt_component_state_set_bool(message->ultra, ultra != 0);
	struct nt_text_style style = {0};
	if (message->styled) {
		style.foreground = (struct nt_color){
			NT_COLOR_INDEXED, ultra ? CLI_INPUT_ULTRA_INDEX : CLI_INPUT_MARKER_INDEX};
		style.attributes = ultra ? NT_STYLE_BOLD : 0;
	}

	return rc ? rc : nt_view_set_style(message->marker, style);
}

int cli_message_set_text_copy(struct nt_view *view, const char *text)
{
	if (!view || view->type != &cli_message_class || !text)
		return NB_ERR_INVAL;

	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	struct cli_message *message = (struct cli_message *)view;
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	int rc = nt_component_state_set_string(message->text, safe);
	if (!rc) {
		message->measured_width = message->published_width = -1;
		nt_view_invalidate(view, 1);
	}

	return rc;
}

struct nt_view *cli_message_add_borrow(struct nt_builder *builder, struct nt_view *parent,
				       enum cli_message_kind kind, const char *text, int styled)
{
	struct nt_view_options options = {.width = NT_FILL, .margin = {.top = 1}};
	if (kind == CLI_MESSAGE_USER) {
		options.padding = (struct nt_view_insets){styled ? 1 : 0, 1, styled ? 1 : 0, 0};
		if (styled)
			options.style.background =
				(struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_BACKGROUND_INDEX};
	} else if (kind == CLI_MESSAGE_REASONING && styled)
		options.style.attributes = NT_STYLE_DIM | NT_STYLE_ITALIC;

	if (kind == CLI_MESSAGE_ASSISTANT)
		options.padding.right = (int)CLI_CONTENT_RIGHT_PADDING;

	struct cli_message *message =
		(struct cli_message *)nt_view_add(builder, parent, &cli_message_class, &options);
	if (!message)
		return NULL;

	struct nt_view *view = &message->base;
	message->kind = kind;
	message->styled = styled;
	message->measured_width = message->published_width = -1;
	if (kind != CLI_MESSAGE_REASONING) {
		struct nt_view_options marker = {0};
		if (styled && kind == CLI_MESSAGE_USER)
			marker.style.foreground =
				(struct nt_color){NT_COLOR_INDEXED, CLI_INPUT_MARKER_INDEX};

		if (styled && kind == CLI_MESSAGE_ASSISTANT)
			marker.style.attributes = NT_STYLE_BOLD;

		message->marker = nt_label_add(builder, view,
					       kind == CLI_MESSAGE_USER ? (styled ? "› " : "> ")
									: CLI_FEED_MESSAGE,
					       &marker);
	}

	const struct nt_view_options body = {.width = NT_FILL};
	message->body = kind == CLI_MESSAGE_ASSISTANT ? cli_rich_document_add_borrow(builder, view)
						      : cli_report_text_add(builder, view, &body);
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		text ? nt_sanitize_dup(text, strlen(text), NT_TEXT_MULTILINE, NULL) : NULL;
	int rc = safe	? nt_view_use_state_string(view, safe, &message->text)
		 : text ? NB_ERR_NOMEM
			: NB_ERR_INVAL;
	if (!rc && kind == CLI_MESSAGE_USER)
		rc = nt_view_use_state_bool(view, false, &message->ultra);

	if (!rc && kind == CLI_MESSAGE_USER)
		rc = nt_view_bind_compute(message->body, NT_PROP_TEXT, cli_message_text,
					  message->text);

	if (!rc && kind == CLI_MESSAGE_USER)
		rc = nt_view_bind_compute(message->marker, NT_PROP_TEXT, cli_message_marker,
					  message);

	nt_builder_fail(builder, rc);
	return view;
}
