#include "sapi/cli/widgets/message.h"
#include "nb/err.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/rich_document.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_message {
	struct morph_tui_widget base;
	struct morph_tui_view *marker, *body;
	char *text;
	enum cli_message_kind kind;
	int columns, styled, reasoning_expanded;
};

static int cli_message_measure(struct morph_tui_view *view, int width,
			       struct morph_tui_view_size *size)
{
	struct cli_message *message = (struct cli_message *)view;
	int prefix = message->marker ? 2 : 0;
	int columns = width > prefix ? width - prefix : 0;
	if (message->columns != columns) {
		int rc;
		if (message->kind == CLI_MESSAGE_ASSISTANT) {
			nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
			rc = (nb_buf_init(&rendered),
			      nb_buf_reserve(&rendered, 128) ? 0 : NB_ERR_NOMEM);
			if (!rc)
				rc = cli_markdown_render_retained(&rendered, message->text, columns,
					message->styled);
			/* The message owns outer spacing through its View options. */
			while (!rc && rendered.len && rendered.ptr[rendered.len - 1] == '\n')
				rendered.ptr[--rendered.len] = '\0';
			if (!rc)
				rc = cli_rich_document_set_rendered_copy(
					message->body, nb_buf_cstr(&rendered), message->styled);
		} else if (message->kind == CLI_MESSAGE_REASONING &&
			   !message->reasoning_expanded) {
			nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
			struct cli_message_style style = {
				.columns = columns, .styled = message->styled};
			rc = (nb_buf_init(&rendered),
			      nb_buf_reserve(&rendered, 128) ? 0 : NB_ERR_NOMEM);
			if (!rc)
				rc = cli_messages_append(&rendered, CLI_MESSAGE_REASONING,
					message->text, &style);
			if (!rc)
				rc = cli_messages_set_rendered_view(
					message->body, nb_buf_cstr(&rendered), message->styled);
		} else
			rc = morph_tui_widget_text_view_set_text_copy(message->body, message->text);
		if (rc)
			return rc;
		message->columns = columns;
	}
	int rc = morph_tui_view_measure(message->body, columns, size);
	if (!rc) {
		size->width += prefix;
		if (message->marker && size->height < 1)
			size->height = 1;
	}
	return rc;
}

static int cli_message_layout(struct morph_tui_view *view)
{
	struct cli_message *message = (struct cli_message *)view;
	struct morph_tui_view_rect rect = morph_tui_view_content_rect(view);
	int prefix = message->marker ? (rect.width > 2 ? 2 : rect.width) : 0;
	if (message->marker) {
		struct morph_tui_view_rect marker = {rect.x, rect.y, prefix, rect.height ? 1 : 0};
		int rc = morph_tui_view_arrange(message->marker, marker);
		if (rc)
			return rc;
	}
	rect.x += prefix;
	rect.width -= prefix;
	return morph_tui_view_arrange(message->body, rect);
}

static void cli_message_destroy(struct morph_tui_view *view)
{
	free(((struct cli_message *)view)->text);
}

static const struct morph_tui_view_ops cli_message_ops = {
	.measure = cli_message_measure,
	.layout = cli_message_layout,
	.destroy = cli_message_destroy,
};

int cli_message_set_reasoning_expanded(struct morph_tui_view *view, int expanded)
{
	if (!view || view->ops != &cli_message_ops)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
	struct cli_message *message = (struct cli_message *)view;
	if (message->reasoning_expanded != !!expanded) {
		message->reasoning_expanded = !!expanded;
		message->columns = -1;
		morph_tui_view_invalidate(view, 1);
	}
	return 0;
}

int cli_message_set_ultra(struct morph_tui_view *view, int ultra)
{
	if (!view || view->ops != &cli_message_ops)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
	struct cli_message *message = (struct cli_message *)view;
	if (message->kind != CLI_MESSAGE_USER)
		return 0;
	int rc = morph_tui_widget_label_set_text_copy(message->marker, ultra		 ? "» "
								       : message->styled ? "› "
											 : "> ");
	struct morph_tui_text_style style = {0};
	if (message->styled) {
		style.foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED,
			ultra ? CLI_INPUT_ULTRA_INDEX : CLI_INPUT_MARKER_INDEX};
		style.attributes = ultra ? MORPH_TUI_STYLE_BOLD : 0;
	}
	return rc ? rc : morph_tui_view_set_style(message->marker, style);
}

int cli_message_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || view->ops != &cli_message_ops || !text)
		return NB_ERR_INVAL;
	if (!morph_tui_view_is_owner(view))
		return NB_ERR_NOT_PERMITTED;
	struct cli_message *message = (struct cli_message *)view;
	if (message->text && !strcmp(message->text, text))
		return 0;
	char *copy NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!copy)
		return NB_ERR_NOMEM;
	free(message->text);
	message->text = nb_charp_steal(&copy);
	message->columns = -1;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

struct morph_tui_view *cli_message_add_borrow(struct morph_tui_builder *builder,
					      struct morph_tui_view *parent,
					      enum cli_message_kind kind, const char *text,
					      int styled)
{
	struct morph_tui_view_options options = {.margin = {.top = 1}};
	if (kind == CLI_MESSAGE_USER) {
		options.padding = (struct morph_tui_view_insets){styled ? 1 : 0, 1,
			styled ? 1 : 0, 0};
		if (styled)
			options.style.background =
				(struct morph_tui_color){MORPH_TUI_COLOR_INDEXED,
						       CLI_INPUT_BACKGROUND_INDEX};
	} else if (kind == CLI_MESSAGE_REASONING && styled)
		options.style.attributes = MORPH_TUI_STYLE_DIM | MORPH_TUI_STYLE_ITALIC;
	if (kind == CLI_MESSAGE_ASSISTANT)
		options.padding.right = (int)CLI_CONTENT_RIGHT_PADDING;
	struct cli_message *message = (struct cli_message *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*message), &cli_message_ops, &options);
	if (!message)
		return NULL;
	struct morph_tui_view *view = &message->base.view;
	message->kind = kind;
	message->styled = styled;
	message->columns = -1;
	if (kind != CLI_MESSAGE_REASONING) {
		struct morph_tui_view_options marker = {0};
		if (styled && kind == CLI_MESSAGE_USER)
			marker.style.foreground =
				(struct morph_tui_color){MORPH_TUI_COLOR_INDEXED,
						       CLI_INPUT_MARKER_INDEX};
		if (styled && kind == CLI_MESSAGE_ASSISTANT)
			marker.style.attributes = MORPH_TUI_STYLE_BOLD;
		message->marker = morph_tui_widget_label_add_borrow(
			builder, view,
			kind == CLI_MESSAGE_USER ? (styled ? "› " : "> ") : CLI_FEED_MESSAGE,
			&marker);
	}
	message->body = kind == CLI_MESSAGE_ASSISTANT
		? cli_rich_document_add_borrow(builder, view)
		: morph_tui_widget_text_view_add_borrow(builder, view, "", NULL);
	if (message->body && kind != CLI_MESSAGE_ASSISTANT) {
		morph_tui_builder_fail(
			builder, morph_tui_widget_text_view_set_content_sizing(message->body, 1));
		morph_tui_view_set_focusable(message->body, 0);
	}
	morph_tui_builder_fail(builder, cli_message_set_text_copy(view, text));
	return view;
}
