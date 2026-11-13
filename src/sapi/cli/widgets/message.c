#include "sapi/cli/widgets/message.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/utf8.h"
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
	int columns, styled;
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
			morph_buf_t rendered MORPH_CLEANUP(morph_buf_cleanup) = {0};
			rc = morph_buf_init(&rendered, 128);
			if (!rc)
				rc = cli_markdown_render_document(&rendered, message->text, columns,
								  0);
			/* The message owns outer spacing through its View options. */
			while (!rc && rendered.len && rendered.data[rendered.len - 1] == '\n')
				rendered.data[--rendered.len] = '\0';
			if (!rc)
				rc = cli_messages_set_rendered_view(
					message->body, morph_buf_cstr_borrow(&rendered),
					message->styled);
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

int cli_message_set_text_copy(struct morph_tui_view *view, const char *text)
{
	if (!view || view->ops != &cli_message_ops || !text)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct cli_message *message = (struct cli_message *)view;
	if (message->text && !strcmp(message->text, text))
		return 0;
	char *copy MORPH_CLEANUP(morph_charp_cleanup) =
		utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!copy)
		return -ENOMEM;
	free(message->text);
	message->text = morph_charp_steal(&copy);
	message->columns = -1;
	morph_tui_view_invalidate(view, 1);
	return 0;
}

struct morph_tui_view *cli_message_add_borrow(struct morph_tui_builder *builder,
					      struct morph_tui_view *parent,
					      enum cli_message_kind kind, const char *text,
					      int styled)
{
	struct morph_tui_view_options options = {0};
	if (kind == CLI_MESSAGE_USER) {
		options.padding = (struct morph_tui_view_insets){1, 1, 1, 0};
		if (styled)
			options.style.background =
				(struct morph_tui_color){MORPH_TUI_COLOR_RGB, 0x3a3a3a};
	} else if (kind == CLI_MESSAGE_REASONING && styled)
		options.style.attributes = MORPH_TUI_STYLE_DIM | MORPH_TUI_STYLE_ITALIC;
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
				(struct morph_tui_color){MORPH_TUI_COLOR_RGB, 0x8a8a8a};
		message->marker = morph_tui_widget_label_add_borrow(
			builder, view, kind == CLI_MESSAGE_USER ? "›" : CLI_FEED_MESSAGE, &marker);
	}
	message->body = morph_tui_widget_text_view_add_borrow(builder, view, "", NULL);
	if (message->body) {
		morph_tui_builder_fail(
			builder, morph_tui_widget_text_view_set_content_sizing(message->body, 1));
		morph_tui_view_set_focusable(message->body, 0);
	}
	morph_tui_builder_fail(builder, cli_message_set_text_copy(view, text));
	return view;
}
