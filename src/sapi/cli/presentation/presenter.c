#include "sapi/cli/presentation/presenter.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/application.h"
#include "sapi/cli/models/agents.h"
#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/presentation/turn_details.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/output.h"
#include "agent/plan.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/feed_block.h"
#include "sapi/cli/widgets/plan.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/services/transcript.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/jobs.h"
#include "sapi/cli/widgets/list.h"
#include "agent/patch.h"
#include <morph/tui/view.h>

#define CLI_PRESENTER_STREAM_NONE 0
#define CLI_PRESENTER_STREAM_THOUGHT 1
#define CLI_PRESENTER_STREAM_REASONING 2
#define CLI_PRESENTER_STREAM_FINAL 3
#define CLI_PRESENTER_STREAM_CONTENT 4
#define CLI_PRESENTER_EVENT_TEXT_MAX 2000
#define CLI_PRESENTER_EVENT_ARGS_MAX 180
#define CLI_PRESENTER_TREE_VALUE_MAX 2000
#define CLI_PRESENTER_TREE_DEPTH_MAX 6
#define CLI_PRESENTER_TREE_ITEMS_MAX 12
#define CLI_PRESENTER_TREE_RESULT_DEPTH 2
#define CLI_PRESENTER_TREE_EMBEDDED_JSON_DEPTH 1
#define CLI_PRESENTER_PATCH_BYTES_MAX 16384
#define CLI_PRESENTER_PATCH_LINES_MAX 240

static char *cli_presenter_safe_dup(const char *text, size_t max_bytes,
				    enum morph_terminal_text_mode mode)
{
	char *safe;
	char *limited;

	if (!text)
		return NULL;
	safe = morph_terminal_sanitize_dup(text, strlen(text), mode, NULL);
	if (!safe)
		return NULL;
	limited = nb_utf8_ellipsize_bytes(safe, max_bytes);
	free(safe);
	return limited;
}

static void cli_presenter_print_safe_inline(struct cli_application *ctx, const char *text,
					    size_t max_bytes)
{
	char *safe = cli_presenter_safe_dup(text ? text : "", max_bytes,
					    MORPH_TERMINAL_TEXT_SINGLE_LINE);

	if (!safe)
		return;
	cli_output_printf(ctx, "%s", safe);
	free(safe);
}

/* Keep submitted local commands visible without committing an editable bubble. */
void cli_presentation_command_input(struct cli_application *ctx, const char *text)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE || !text || text[0] != '/')
		return;
	if (cli_chat_active(ctx)) {
		(void)cli_chat_command_input(ctx, text);
		ctx->output.command_result_pending = 1;
		return;
	}
	if (cli_output_color_enabled(ctx))
		cli_output_printf(ctx, ANSI_RESET CLI_COMMAND_COLOR);
	cli_presenter_print_safe_inline(ctx, text, SIZE_MAX);
	if (cli_output_color_enabled(ctx))
		cli_output_printf(ctx, ANSI_RESET);
	cli_output_putchar(ctx, '\n');
	ctx->output.command_result_pending = 1;
}

/* Keep command results clear of the editable composer's background. */
void cli_presentation_command_end(struct cli_application *ctx)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	ctx->output.command_result_pending = 0;
	if (cli_chat_active(ctx)) {
		cli_chat_command_end(ctx);
		return;
	}
	if (cli_output_color_enabled(ctx))
		cli_output_printf(ctx, ANSI_RESET);
	cli_output_putchar(ctx, '\n');
}

/* Submitted users share the same stateless layout as child transcripts. */
void cli_presentation_user_input(struct cli_application *ctx, const char *text)
{
	if (!ctx || !text || !*text)
		return;
	if (cli_chat_active(ctx)) {
		(void)cli_chat_message(ctx, CLI_MESSAGE_USER, text);
		return;
	}
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_message_style style = {
		.columns = cli_list_columns(),
		.styled = cli_output_color_enabled(ctx),
		.ultra = ctx->composer.ultra,
		.trailing_newlines = ctx->output.trailing_newlines,
	};
	if ((nb_buf_init(&document), nb_buf_reserve(&document, 256) ? 0 : NB_ERR_NOMEM) == 0 &&
	    cli_messages_append(&document, CLI_MESSAGE_USER, text, &style) == 0)
		cli_output_printf(ctx, "%s", nb_buf_cstr(&document));
}

static void cli_presenter_clear_status(struct cli_application *ctx)
{
	cli_feed_live_clear(ctx);
}

static void cli_presenter_block_gap(struct cli_application *ctx)
{
	struct morph_tui_view_insets margin = {.top = 1};

	for (int i = cli_output_block_gap_margin(margin, ctx->output.trailing_newlines); i > 0; i--)
		cli_output_putchar(ctx, '\n');
}

static void cli_presenter_status(struct cli_application *ctx, const char *text)
{
	if (!ctx || !text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "status: ");
		cli_presenter_print_safe_inline(ctx, text, CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, "\n");
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	cli_feed_live_set(ctx, text);
}

static void cli_presenter_model_wait(struct cli_application *ctx)
{
	cli_presenter_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		cli_presenter_status(ctx, "Thinking…");
}

static int cli_presenter_pause_status(struct cli_application *ctx)
{
	if (ctx && ctx->turn_active && cli_feed_live_active(ctx)) {
		cli_feed_history_begin(ctx);
		return 1;
	}
	cli_presenter_clear_status(ctx);
	return 0;
}

static void cli_presenter_resume_status(struct cli_application *ctx, int paused)
{
	if (paused)
		cli_feed_history_end(ctx);
}

void cli_presentation_prepare_prompt(struct cli_application *ctx)
{
	cli_presenter_clear_status(ctx);
}

static const char *cli_presenter_event_string(const struct morph_event *ev, const char *name)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int cli_presenter_event_error_code(const struct morph_event *ev)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, "error_code");
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static int cli_presenter_event_int(const struct morph_event *ev, const char *name)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, name);
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static char *cli_presenter_event_args_json(const struct morph_event *ev)
{
	cJSON *args;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	args = cJSON_GetObjectItemCaseSensitive(ev->data, "args");
	if (!args)
		return NULL;
	return cJSON_PrintUnformatted(args);
}

static cJSON *cli_presenter_event_item(const struct morph_event *ev, const char *name)
{
	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	return cJSON_GetObjectItemCaseSensitive(ev->data, name);
}

static void cli_presenter_print_tree_prefix(struct cli_application *ctx, const int *ancestors_last,
					    int depth, int is_last)
{
	cli_list_tree_prefix(ctx, ancestors_last, depth, is_last, 0);
}

static void cli_presenter_print_tree_continuation_prefix(struct cli_application *ctx,
							 const int *ancestors_last, int depth,
							 int is_last, size_t label_width)
{
	cli_list_tree_prefix(ctx, ancestors_last, depth, is_last, 1);
	for (size_t i = 0; i < label_width + 2; i++)
		cli_output_putchar(ctx, ' ');
}

static const char *cli_presenter_tree_wrap_end(const char *text, size_t width, int *has_break)
{
	const char *end;
	const char *space = NULL;

	end = morph_terminal_advance_display_width_borrow(text, width);
	if (!*end) {
		*has_break = 0;
		return end;
	}
	for (const char *p = text; p < end; p++) {
		if (*p == ' ')
			space = p;
	}
	*has_break = space && space > text;
	return *has_break ? space : end;
}

static void cli_presenter_print_tree_string(struct cli_application *ctx, const char *text,
					    const char *label, int depth, int is_last,
					    const int *ancestors_last)
{
	char *display;
	const char *line;
	const char *end;
	size_t label_width;
	int available;
	int has_break;
	int first = 1;

	display = cli_presenter_safe_dup(text ? text : "", CLI_PRESENTER_TREE_VALUE_MAX,
					 MORPH_TERMINAL_TEXT_SINGLE_LINE);
	if (!display)
		return;
	for (char *p = display; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	}
	label_width = label ? nb_utf8_width_cstr(label) : 0;
	available = cli_list_columns() - 4 - depth * 2 - (int)label_width - 2;
	if (available < 8)
		available = 8;
	line = display;
	while (*line) {
		end = cli_presenter_tree_wrap_end(line, (size_t)available, &has_break);
		if (first) {
			cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
			if (label && label[0]) {
				cli_output_printf(ctx, ANSI_DIM);
				cli_presenter_print_safe_inline(ctx, label,
								CLI_PRESENTER_TREE_VALUE_MAX);
				cli_output_printf(ctx, ":" ANSI_RESET " ");
			}
			first = 0;
		} else {
			cli_presenter_print_tree_continuation_prefix(ctx, ancestors_last, depth,
								     is_last, label_width);
		}
		if (*end && !has_break)
			end = morph_terminal_advance_display_width_borrow(line,
									  (size_t)(available - 1));
		cli_output_printf(ctx, "%.*s", (int)(end - line), line);
		if (*end && !has_break) {
			cli_output_printf(ctx, "…\n");
			break;
		}
		cli_output_printf(ctx, "\n");
		line = end;
		while (*line == ' ')
			line++;
	}
	if (first) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
		if (label && label[0]) {
			cli_output_printf(ctx, ANSI_DIM);
			cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
			cli_output_printf(ctx, ":" ANSI_RESET " ");
		}
		cli_output_printf(ctx, "\n");
	}
	free(display);
}

static void cli_presenter_print_tree_scalar(struct cli_application *ctx, const cJSON *item)
{
	char *value;

	value = cJSON_PrintUnformatted(item);
	if (value) {
		cli_output_printf(ctx, "%s", value);
		free(value);
	}
}

static int cli_presenter_json_child_count(const cJSON *item)
{
	return cJSON_IsArray(item) ? cJSON_GetArraySize(item)
				   : (cJSON_IsObject(item) ? cJSON_GetArraySize(item) : 0);
}

static void cli_presenter_print_tree_container_summary(struct cli_application *ctx,
						       const cJSON *item)
{
	int count = cli_presenter_json_child_count(item);

	cli_output_printf(ctx, "%s %d item%s", cJSON_IsArray(item) ? "[...]" : "{...}", count,
	       count == 1 ? "" : "s");
}

static void cli_presenter_print_json_tree_node(struct cli_application *ctx, const cJSON *item,
					       const char *label, int depth, int is_last,
					       int *ancestors_last, int collapse_depth,
					       int embedded_json_depth)
{
	int count;
	int shown;
	int index = 0;
	cJSON *child;
	cJSON *embedded = NULL;

	if (!cJSON_IsArray(item) && !cJSON_IsObject(item)) {
		if (cJSON_IsString(item) && depth == embedded_json_depth)
			embedded = cJSON_Parse(item->valuestring);
		if (cJSON_IsArray(embedded) || cJSON_IsObject(embedded)) {
			cli_presenter_print_json_tree_node(ctx, embedded, label, depth, is_last,
							   ancestors_last, collapse_depth, -1);
			cJSON_Delete(embedded);
			return;
		}
		cJSON_Delete(embedded);
		if (cJSON_IsString(item)) {
			cli_presenter_print_tree_string(ctx, item->valuestring, label, depth,
							is_last, ancestors_last);
			return;
		}
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
		if (label && label[0]) {
			cli_output_printf(ctx, ANSI_DIM);
			cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
			cli_output_printf(ctx, ":" ANSI_RESET " ");
		}
		cli_presenter_print_tree_scalar(ctx, item);
		cli_output_printf(ctx, "\n");
		return;
	}
	cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
	if (label && label[0]) {
		cli_output_printf(ctx, ANSI_DIM);
		cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
		cli_output_printf(ctx, ":" ANSI_RESET);
	}
	count = cli_presenter_json_child_count(item);
	if (count == 0) {
		cli_output_printf(ctx, " %s\n", cJSON_IsArray(item) ? "[]" : "{}");
		return;
	}
	if (depth >= collapse_depth) {
		cli_output_printf(ctx, " ");
		cli_presenter_print_tree_container_summary(ctx, item);
		cli_output_printf(ctx, "\n");
		return;
	}
	cli_output_printf(ctx, "\n");
	ancestors_last[depth] = is_last;
	if (depth + 1 >= CLI_PRESENTER_TREE_DEPTH_MAX) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth + 1, 1);
		cli_output_printf(ctx, ANSI_DIM "… nested data" ANSI_RESET "\n");
		return;
	}
	shown = count < CLI_PRESENTER_TREE_ITEMS_MAX ? count : CLI_PRESENTER_TREE_ITEMS_MAX;
	cJSON_ArrayForEach (child, item) {
		char array_label[32];
		const char *child_label;
		int child_last;

		if (index >= shown)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(array_label, sizeof(array_label), "[%d]", index);
			child_label = array_label;
		} else {
			child_label = child->string ? child->string : "item";
		}
		child_last = index == shown - 1 && shown == count;
		cli_presenter_print_json_tree_node(ctx, child, child_label, depth + 1, child_last,
						   ancestors_last, collapse_depth,
						   embedded_json_depth);
		index++;
	}
	if (shown < count) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth + 1, 1);
		cli_output_printf(ctx, ANSI_DIM "… %d more" ANSI_RESET "\n", count - shown);
	}
}

static void cli_presenter_print_json_tree_children(struct cli_application *ctx, const cJSON *item)
{
	int ancestors_last[CLI_PRESENTER_TREE_DEPTH_MAX] = {0};
	int count;
	int index = 0;
	cJSON *child;

	if (!cJSON_IsArray(item) && !cJSON_IsObject(item))
		return;
	count = cli_presenter_json_child_count(item);
	cJSON_ArrayForEach (child, item) {
		char array_label[32];
		const char *label;
		int is_last;

		if (index >= CLI_PRESENTER_TREE_ITEMS_MAX)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(array_label, sizeof(array_label), "[%d]", index);
			label = array_label;
		} else {
			label = child->string ? child->string : "item";
		}
		is_last = index == count - 1 && count <= CLI_PRESENTER_TREE_ITEMS_MAX;
		cli_presenter_print_json_tree_node(ctx, child, label, 0, is_last, ancestors_last,
						   CLI_PRESENTER_TREE_DEPTH_MAX, -1);
		index++;
	}
	if (count > CLI_PRESENTER_TREE_ITEMS_MAX) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, 0, 1);
		cli_output_printf(ctx, ANSI_DIM "… %d more" ANSI_RESET "\n",
		       count - CLI_PRESENTER_TREE_ITEMS_MAX);
	}
}

static void cli_presenter_print_indented(struct cli_application *ctx, const char *prefix,
					 const char *content)
{
	char *display;
	const char *line;

	if (!content || !content[0])
		return;
	display = cli_presenter_safe_dup(content, CLI_PRESENTER_EVENT_TEXT_MAX,
					 MORPH_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	line = display;
	while (line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);

		cli_output_printf(ctx, "%s%.*s\n", prefix, (int)len, line);
		line = end ? end + 1 : NULL;
	}
	free(display);
}

static void cli_presenter_print_plain_labeled(struct cli_application *ctx, const char *label,
					      const char *content)
{
	char *display;

	if (!content || !content[0])
		return;
	display = cli_presenter_safe_dup(content, CLI_PRESENTER_EVENT_TEXT_MAX,
					 MORPH_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	if (strchr(display, '\n'))
		cli_output_printf(ctx, "%s:\n%s\n", label, display);
	else
		cli_output_printf(ctx, "%s: %s\n", label, display);
	free(display);
}

static void cli_presenter_reasoning_flush(struct cli_application *ctx, int finish);

static void cli_presenter_print_stream(struct cli_application *ctx)
{
	const char *content;
	const char *label;
	int already_streamed;
	int reasoning_streamed;

	if (!ctx || ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_NONE)
		return;
	reasoning_streamed = ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_REASONING &&
			     ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
	already_streamed =
		ctx->presenter->markdown_stream_visible &&
		ctx->presenter->markdown_stream_kind == ctx->presenter->event_stream_kind;
	if (already_streamed)
		cli_markdown_stream_reset(ctx, 1);
	cli_presenter_clear_status(ctx);
	(void)morph_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
					   &ctx->presenter->event_stream, NULL, 0, 1);
	content = nb_buf_cstr(&ctx->presenter->event_stream);
	if (content && content[0] && reasoning_streamed) {
		cli_presenter_reasoning_flush(ctx, 1);
	} else if (content && content[0] && !already_streamed) {
		label = ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_REASONING
				? "reasoning"
				: (ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_CONTENT
					   ? "content"
					   : "thought");
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
			cli_presenter_print_plain_labeled(ctx, label, content);
		} else {
			cli_presenter_block_gap(ctx);
			cli_output_printf(ctx, ANSI_BOLD CLI_FEED_MESSAGE ANSI_RESET " ");
			cli_markdown_render_ansi_with_media_indented(
				ctx, content, 2, cli_renderer_media, ctx);
		}
	}
	if (ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_CONTENT)
		ctx->presenter->native_content_visible = 1;
	nb_buf_clear(&ctx->presenter->event_stream);
	morph_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
	ctx->presenter->event_stream_kind = CLI_PRESENTER_STREAM_NONE;
	ctx->presenter->event_stream_has_delta = 0;
	ctx->presenter->event_stream_complete = 0;
	ctx->presenter->event_stream_visible = 0;
	ctx->presenter->reasoning_stream_offset = 0;
}

static void cli_presenter_discard_stream(struct cli_application *ctx)
{
	if (!ctx)
		return;
	cli_markdown_stream_reset(ctx, 1);
	nb_buf_clear(&ctx->presenter->event_stream);
	morph_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
	ctx->presenter->event_stream_kind = CLI_PRESENTER_STREAM_NONE;
	ctx->presenter->event_stream_has_delta = 0;
	ctx->presenter->event_stream_complete = 0;
	ctx->presenter->event_stream_visible = 0;
	ctx->presenter->reasoning_stream_offset = 0;
}

static void cli_presenter_plain_content(struct cli_application *ctx, const char *text)
{
	char *safe;

	if (ctx->presentation_mode != CLI_PRESENT_ONCE_PLAIN)
		return;
	if (!text || !*text)
		text = nb_buf_cstr(&ctx->presenter->native_content);
	if (text && *text) {
		safe = cli_presenter_safe_dup(text, SIZE_MAX, MORPH_TERMINAL_TEXT_MULTILINE);
		if (safe) {
			if (strchr(safe, '\n'))
				cli_output_printf(ctx, "content:\n%s\n", safe);
			else
				cli_output_printf(ctx, "content: %s\n", safe);
			free(safe);
		}
	}
	nb_buf_clear(&ctx->presenter->native_content);
}

void cli_presentation_flush_stream(struct cli_application *ctx)
{
	cli_presenter_plain_content(ctx, NULL);
	cli_presenter_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
}

void cli_presentation_hook_prompt(struct cli_application *ctx, const char *text)
{
	cli_presenter_clear_status(ctx);
	cli_presentation_flush_stream(ctx);
	ctx->presenter->final_rendered = 0;
	ctx->presenter->native_content_started = 0;
	ctx->presenter->native_content_visible = 0;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		cli_presenter_print_plain_labeled(ctx, "hook continuation", text);
	else {
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_RUNNING,
			"Stop hook requested continuation", text, NULL);
	}
}

static int cli_presenter_append_stream(struct cli_application *ctx, int kind, const char *text,
				       int is_delta, int is_complete, const char **appended_text)
{
	size_t offset;
	int rc;

	if (!ctx || !text)
		return 0;
	if (appended_text)
		*appended_text = "";
	if (ctx->presenter->event_stream_kind != CLI_PRESENTER_STREAM_NONE &&
	    ctx->presenter->event_stream_kind != kind)
		cli_presenter_print_stream(ctx);
	if (ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_NONE) {
		ctx->presenter->event_stream_kind = kind;
		morph_terminal_sanitizer_init(&ctx->presenter->event_stream_sanitizer,
					      MORPH_TERMINAL_TEXT_MULTILINE);
	}
	if (!is_delta && ctx->presenter->event_stream_has_delta) {
		rc = morph_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
						  &ctx->presenter->event_stream, NULL, 0, 1);
		if (rc != 0)
			return rc;
		ctx->presenter->event_stream_complete = is_complete;
		return 0;
	}
	offset = ctx->presenter->event_stream.len;
	rc = morph_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
					  &ctx->presenter->event_stream, text, strlen(text),
					  !is_delta);
	if (rc != 0)
		return rc;
	if (appended_text)
		*appended_text = nb_buf_cstr(&ctx->presenter->event_stream) + offset;
	if (is_delta)
		ctx->presenter->event_stream_has_delta = 1;
	if (is_complete)
		ctx->presenter->event_stream_complete = 1;
	return 0;
}

/* The terminal owns the live row; commit its preview only when the stream ends. */
static void cli_presenter_reasoning_flush(struct cli_application *ctx, int finish)
{
	const char *content = nb_buf_cstr(&ctx->presenter->event_stream);
	size_t offset = ctx->presenter->reasoning_stream_offset;
	const char *start;
	const char *end;
	const char *newline;
	nb_buf preview NB_CLEANUP(nb_buf_cleanup) = {0};
	int columns = cli_feed_columns(ctx);
	size_t width = columns > 4 ? (size_t)(columns - 4) : 1u;
	int rc;

	if (offset < ctx->presenter->event_stream.len) {
		rc = cli_transcript_reasoning_append(ctx, content + offset,
						     ctx->presenter->event_stream.len - offset,
						     !ctx->presenter->event_stream_visible);
		if (rc != 0) {
			nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_WARN,
				     "Could not retain reasoning details: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
			return;
		}
		ctx->presenter->reasoning_stream_offset = ctx->presenter->event_stream.len;
		ctx->presenter->event_stream_visible = 1;
	}
	start = content + strspn(content, " \t\r\n");
	if (!*start ||
	    (nb_buf_init(&preview), nb_buf_reserve(&preview, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return;
	newline = strchr(start, '\n');
	end = morph_terminal_advance_display_width_borrow(start, width - 1u);
	if (newline && newline < end)
		end = newline;
	(void)nb_buf_append(&preview, start, (size_t)(end - start));
	if (*end)
		(void)nb_buf_append_cstr(&preview, "…");
	if (finish) {
		cli_presenter_clear_status(ctx);
		/* Commit one quiet narration line between tool blocks. */
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_REASONING,
			.content.reasoning = nb_buf_cstr(&preview),
		};
		(void)cli_feed_block_write(ctx, &block);
	} else if (isatty(STDOUT_FILENO)) {
		cli_feed_reasoning_set(ctx, nb_buf_cstr(&preview));
	}
	nb_buf_cleanup(&preview);
}

static const char *cli_presenter_workdir(struct cli_application *ctx)
{
	if (!ctx)
		return NULL;
	if (ctx->workdir[0])
		return ctx->workdir;
	return runtime_workdir_borrow(ctx->runtime);
}

static const char *cli_presenter_patch_style(const char *line)
{
	if (strncmp(line, "*** Add File: ", 14) == 0 ||
	    strncmp(line, "*** Update File: ", 17) == 0 ||
	    strncmp(line, "*** Delete File: ", 17) == 0 || strncmp(line, "*** Move to: ", 13) == 0)
		return ANSI_BOLD ANSI_CYAN;
	if (strncmp(line, "@@", 2) == 0)
		return ANSI_CYAN;
	if (line[0] == '+')
		return ANSI_GREEN;
	if (line[0] == '-')
		return ANSI_RED;
	if (strncmp(line, "*** Begin Patch", 15) == 0 || strncmp(line, "*** End Patch", 13) == 0)
		return ANSI_DIM;
	return "";
}

static int cli_presenter_patch_range(const char *line, size_t *old_line, size_t *new_line)
{
	size_t old_count;
	size_t new_count;

	return sscanf(line, "@@ -%zu,%zu +%zu,%zu @@", old_line, &old_count, new_line,
		      &new_count) == 4;
}

static void cli_presenter_print_patch_line(struct cli_application *ctx, const char *line,
					   size_t len, const char *style, int in_hunk,
					   size_t *old_line, size_t *new_line)
{
	char number[32] = "";

	if (in_hunk && len > 0) {
		if (line[0] == '-') {
			snprintf(number, sizeof(number), "%zu", *old_line);
			(*old_line)++;
		} else if (line[0] == '+') {
			snprintf(number, sizeof(number), "%zu", *new_line);
			(*new_line)++;
		} else if (line[0] == ' ') {
			snprintf(number, sizeof(number), "%zu", *new_line);
			(*old_line)++;
			(*new_line)++;
		}
	}
	if (number[0]) {
		cli_output_printf(ctx, "  │ " ANSI_DIM "%5s " ANSI_RESET "%s%.*s" ANSI_RESET "\n", number,
		       style, (int)len, line);
		return;
	}
	cli_output_printf(ctx, "  │ %s%.*s" ANSI_RESET "\n", style, (int)len, line);
}

void cli_presentation_patch_diff(struct cli_application *ctx, const char *workdir,
				 const char *input)
{
	nb_buf preview NB_CLEANUP(nb_buf_cleanup) = {0};
	char preview_error[256];
	const char *text = input;
	char *display;
	char *line;
	size_t input_len;
	size_t display_len;
	int truncated;
	int lines = 0;
	int preview_ready = 0;
	int in_hunk = 0;
	size_t old_line = 0;
	size_t new_line = 0;

	if (!input || !input[0])
		return;
	if (workdir && workdir[0]) {
		memset(&preview, 0, sizeof(preview));
		if ((nb_buf_init(&preview), nb_buf_reserve(&preview, 256) ? 0 : NB_ERR_NOMEM) ==
		    0) {
			if (patch_preview(workdir, input, &preview, preview_error,
					  sizeof(preview_error)) == 0) {
				text = nb_buf_cstr(&preview);
				preview_ready = 1;
			} else {
				nb_buf_cleanup(&preview);
			}
		}
	}
	input_len = strlen(text);
	display = malloc(CLI_PRESENTER_PATCH_BYTES_MAX + 1);
	if (!display) {
		if (preview_ready)
			nb_buf_cleanup(&preview);
		return;
	}
	{
		char *safe = cli_presenter_safe_dup(text, CLI_PRESENTER_PATCH_BYTES_MAX,
						    MORPH_TERMINAL_TEXT_MULTILINE);

		if (!safe) {
			free(display);
			if (preview_ready)
				nb_buf_cleanup(&preview);
			return;
		}
		display_len = strlen(safe);
		memcpy(display, safe, display_len + 1);
		free(safe);
	}
	truncated = input_len > CLI_PRESENTER_PATCH_BYTES_MAX;
	for (char *p = display; *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f)
			*p = '?';
	}
	line = display;
	while (*line && lines < CLI_PRESENTER_PATCH_LINES_MAX) {
		char *end = strchr(line, '\n');
		const char *style = cli_presenter_patch_style(line);
		size_t len = end ? (size_t)(end - line) : strlen(line);

		if (cli_presenter_patch_range(line, &old_line, &new_line))
			in_hunk = 1;
		else if (strncmp(line, "*** ", 4) == 0)
			in_hunk = 0;
		cli_presenter_print_patch_line(ctx, line, len, style, in_hunk, &old_line,
					       &new_line);
		lines++;
		if (!end) {
			line += len;
			break;
		}
		line = end + 1;
	}
	if (*line)
		truncated = 1;
	if (truncated)
		cli_output_printf(ctx, "  %s" ANSI_DIM "… patch display truncated" ANSI_RESET "\n",
		       morph_tui_widget_tree_branch(1, 0));
	free(display);
	if (preview_ready)
		nb_buf_cleanup(&preview);
}

static void cli_presenter_tool_call(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *tool = cli_presenter_event_string(ev, "tool");
	const char *title = cli_presenter_event_string(ev, "toolTitle");
	cJSON *args_item = cli_presenter_event_item(ev, "args");
	cJSON *input_item = NULL;
	const char *patch_input = NULL;
	char *args = NULL;
	char *safe_tool;
	char *safe_title;
	char display[512];

	cli_presenter_clear_status(ctx);
	cli_presenter_print_stream(ctx);
	if (!tool || !tool[0])
		tool = "tool";
	if (!title || !title[0])
		title = tool;
	safe_tool = cli_presenter_safe_dup(tool, TOOL_NAME_MAX, MORPH_TERMINAL_TEXT_SINGLE_LINE);
	safe_title = cli_presenter_safe_dup(title, CLI_PRESENTER_EVENT_TEXT_MAX,
					    MORPH_TERMINAL_TEXT_SINGLE_LINE);
	if (safe_tool)
		tool = safe_tool;
	if (safe_title)
		title = safe_title;
	if (strcmp(tool, "apply_patch") == 0 && cJSON_IsObject(args_item)) {
		input_item = cJSON_GetObjectItemCaseSensitive(args_item, "input");
		if (cJSON_IsString(input_item))
			patch_input = input_item->valuestring;
	}
	if (!patch_input)
		args = cli_presenter_event_args_json(ev);
	display[0] = '\0';
	if (args && strcmp(args, "{}") != 0) {
		morph_terminal_copy_sanitized_display_width(display, sizeof(display), args,
							    CLI_PRESENTER_EVENT_ARGS_MAX);
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "tool: %s", tool);
		if (display[0])
			cli_output_printf(ctx, " %s", display);
		cli_output_printf(ctx, "\n");
		if (patch_input)
			cli_presentation_patch_diff(ctx, cli_presenter_workdir(ctx), patch_input);
	} else {
		cli_output_printf(ctx, "\n" ANSI_CYAN CLI_FEED_RUNNING ANSI_RESET
		       " " ANSI_BOLD "%s" ANSI_RESET "\n",
		       title);
		if (patch_input)
			cli_presentation_patch_diff(ctx, cli_presenter_workdir(ctx), patch_input);
		else
			cli_presenter_print_json_tree_children(ctx, args_item);
		cli_presenter_status(ctx, "Running tool…");
	}
	free(args);
	free(safe_tool);
	free(safe_title);
}

static void cli_presenter_tool_end(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *title = cli_presenter_event_string(ev, "toolTitle");
	const char *tool = cli_presenter_event_string(ev, "tool");
	const char *error = cli_presenter_event_string(ev, "error");
	int failed;

	cli_presenter_clear_status(ctx);
	if (!title || !title[0])
		title = tool && tool[0] ? tool : "Tool";
	failed = !ev->name || strcmp(ev->name, "tool.result") != 0;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "status: ");
		cli_presenter_print_safe_inline(ctx, tool ? tool : "tool", TOOL_NAME_MAX);
		cli_output_printf(ctx, " %s\n", failed ? "failed" : "completed");
		return;
	}
	if (failed) {
		cli_output_printf(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED " ");
		cli_presenter_print_safe_inline(ctx, title, CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, " failed" ANSI_RESET);
		if (error && error[0]) {
			cli_output_printf(ctx, ANSI_DIM " · ");
			cli_presenter_print_safe_inline(ctx, error, CLI_PRESENTER_EVENT_TEXT_MAX);
			cli_output_printf(ctx, ANSI_RESET);
		}
		cli_output_printf(ctx, "\n");
	} else {
		cli_output_printf(ctx, ANSI_GREEN CLI_FEED_DONE " ");
		cli_presenter_print_safe_inline(ctx, title, CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, " completed" ANSI_RESET "\n");
	}
}

static int cli_presenter_plan(struct cli_application *ctx, const struct morph_event *ev)
{
	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	const cJSON *state = ev->data;
	if (!ev->name || strcmp(ev->name, "plan.updated")) {
		const char *tool = cli_presenter_event_string(ev, "tool");
		if (!tool || strcmp(tool, "plan"))
			return 0;
		state = cJSON_GetObjectItemCaseSensitive(ev->data, "data");
	}
	if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(state, "plans")))
		return 0;
	if (!cli_plan_renders(state))
		return 0;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		nb_buf body NB_CLEANUP(nb_buf_cleanup) = {0};
		struct cli_plan_style style = {.columns = BUFSIZ};
		if (cli_plan_append(&body, state, &style) == 0 && body.len)
			cli_output_printf(ctx, "%s", nb_buf_cstr(&body));
	} else {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_PLAN, .content.plan = state,
		};
		(void)cli_feed_block_write(ctx, &block);
	}
	return 1;
}

static void cli_presenter_observation(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *text = cli_presenter_event_string(ev, "text");
	cJSON *structured = cli_presenter_event_item(ev, "data");
	cJSON *parsed = NULL;

	cli_presenter_clear_status(ctx);
	cli_presenter_print_stream(ctx);
	if (!text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_presenter_print_plain_labeled(ctx, "observation", text);
	} else if (cli_presenter_plan(ctx, ev)) {
		return;
	} else if (cli_presenter_event_error_code(ev) < 0) {
		nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
		if ((nb_buf_init(&prefix), nb_buf_reserve(&prefix, 64) ? 0 : NB_ERR_NOMEM) == 0) {
			morph_tui_widget_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
			nb_buf_append_cstr(&prefix, ANSI_RED "Error: " ANSI_RESET);
			cli_presenter_print_indented(ctx, nb_buf_cstr(&prefix), text);
			nb_buf_cleanup(&prefix);
		}
	} else {
		if (!cJSON_IsArray(structured) && !cJSON_IsObject(structured))
			parsed = cJSON_Parse(text);
		if (cJSON_IsArray(structured) || cJSON_IsObject(structured)) {
			int ancestors_last[CLI_PRESENTER_TREE_DEPTH_MAX] = {0};

			cli_presenter_print_json_tree_node(ctx, structured, "result", 0, 1,
							   ancestors_last,
							   CLI_PRESENTER_TREE_RESULT_DEPTH,
							   CLI_PRESENTER_TREE_EMBEDDED_JSON_DEPTH);
		} else if (parsed) {
			int ancestors_last[CLI_PRESENTER_TREE_DEPTH_MAX] = {0};

			cli_presenter_print_json_tree_node(ctx, parsed, "result", 0, 1,
							   ancestors_last,
							   CLI_PRESENTER_TREE_RESULT_DEPTH,
							   CLI_PRESENTER_TREE_EMBEDDED_JSON_DEPTH);
		} else {
			nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
			if ((nb_buf_init(&prefix),
			     nb_buf_reserve(&prefix, 32) ? 0 : NB_ERR_NOMEM) == 0) {
				morph_tui_widget_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
				cli_presenter_print_indented(ctx, nb_buf_cstr(&prefix), text);
				nb_buf_cleanup(&prefix);
			}
		}
		cJSON_Delete(parsed);
	}
}

static void cli_presenter_reflection(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *text = cli_presenter_event_string(ev, "text");

	cli_presenter_clear_status(ctx);
	cli_presenter_print_stream(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		cli_presenter_print_plain_labeled(ctx, "guardrail", text);
	else if (text && text[0])
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_WARNING, "Guardrail", text, NULL);
}

static void cli_presenter_token_label(int tokens, char *label, size_t size)
{
	if (tokens >= 1000000)
		snprintf(label, size, "%.1fm", (double)tokens / 1000000.0);
	else if (tokens >= 1000)
		snprintf(label, size, "%.0fk", (double)tokens / 1000.0);
	else
		snprintf(label, size, "%d", tokens);
}

static void cli_presenter_compaction(struct cli_application *ctx, const struct morph_event *ev)
{
	int before = cli_presenter_event_int(ev, "before_tokens");
	int after = cli_presenter_event_int(ev, "after_tokens");
	const char *error = cli_presenter_event_string(ev, "error");
	int failed = ev->name && strcmp(ev->name, "react.compaction.failed") == 0;
	int64_t elapsed = cli_feed_activity_elapsed_ms(ctx);

	if (ev->name && strcmp(ev->name, "react.compaction.begin") == 0) {
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
			cli_presenter_status(ctx, "Compacting context…");
		} else if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
			cli_presenter_clear_status(ctx);
			cli_presenter_print_stream(ctx);
			cli_feed_activity_set(ctx, "Compacting context…");
		}
		return;
	}
	cli_presenter_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		if (failed) {
			cli_output_printf(ctx, "context: compression failed: ");
			cli_presenter_print_safe_inline(ctx, error ? error : "unknown error",
							CLI_PRESENTER_EVENT_TEXT_MAX);
			cli_output_printf(ctx, "\n");
		} else {
			cli_output_printf(ctx, "context: compacted %d -> %d tokens\n", before, after);
		}
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	if (failed) {
		(void)nb_buf_printf(&title, "Context compression failed · %s",
				    error ? error : "unknown error");
	} else {
		char before_label[32];
		char after_label[32];
		cli_presenter_token_label(before, before_label, sizeof(before_label));
		cli_presenter_token_label(after, after_label, sizeof(after_label));
		(void)nb_buf_printf(&title, "Context compacted  %s → %s", before_label,
				    after_label);
	}
	if (elapsed >= 0)
		(void)nb_buf_printf(&title, " · %llds", (long long)(elapsed / 1000));
	(void)cli_feed_notice_write(ctx, failed ? CLI_NOTICE_FAILED : CLI_NOTICE_DONE,
				    nb_buf_cstr(&title), NULL, NULL);
	fflush(stdout);
	if (!failed && ctx->turn_active)
		cli_presenter_model_wait(ctx);
}

static void cli_presenter_final(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *text = cli_presenter_event_string(ev, "text");
	int had_stream = ctx->presenter->markdown_stream_visible &&
			 (ctx->presenter->markdown_stream_kind == CLI_PRESENTER_STREAM_THOUGHT ||
			  ctx->presenter->markdown_stream_kind == CLI_PRESENTER_STREAM_CONTENT ||
			  ctx->presenter->markdown_stream_kind == CLI_PRESENTER_STREAM_FINAL);
	had_stream = had_stream || ctx->presenter->native_content_visible;
	nb_buf_clear(&ctx->presenter->native_content);
	ctx->presenter->native_content_started = 0;
	ctx->presenter->native_content_visible = 0;

	cli_presenter_clear_status(ctx);
	if ((ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_THOUGHT ||
	     ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_CONTENT) &&
	    !ctx->presenter->event_stream_complete)
		cli_presenter_discard_stream(ctx);
	else
		cli_presenter_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "final:\n");
		if (text && text[0]) {
			char *safe = cli_presenter_safe_dup(text, SIZE_MAX,
							    MORPH_TERMINAL_TEXT_MULTILINE);

			if (safe) {
				cli_output_printf(ctx, "%s\n", safe);
				free(safe);
			}
		}
	} else {
		if (!had_stream) {
			cli_presenter_block_gap(ctx);
			cli_output_printf(ctx, ANSI_BOLD CLI_FEED_MESSAGE ANSI_RESET " ");
		}
		if (!had_stream && text && text[0]) {
			cli_markdown_render_final(ctx, text);
		} else if (!had_stream) {
			cli_output_printf(ctx, "\n");
			ctx->presenter->markdown_output.trailing_newlines = 1;
		}
		cli_markdown_finish_final(ctx);
	}
	ctx->presenter->final_rendered = 1;
}

static void cli_presenter_turn_end(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *error;
	const char *detail;
	const char *outcome;

	if (!ev->phase || strcmp(ev->phase, "end") == 0)
		return;
	if (ctx->presenter->final_rendered)
		return;
	cli_presenter_plain_content(ctx, NULL);
	cli_presenter_clear_status(ctx);
	cli_presenter_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
	error = cli_presenter_event_string(ev, "error");
	detail = cli_presenter_event_string(ev, "detail");
	outcome = cli_presenter_event_string(ev, "outcome");
	if ((outcome && strcmp(outcome, "cancelled") == 0) || strcmp(ev->phase, "cancelled") == 0) {
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
			cli_output_printf(ctx, "cancelled\n");
		else
			(void)cli_feed_notice_write(ctx, CLI_NOTICE_CANCELLED, "Cancelled", NULL, NULL);
		ctx->presenter->final_rendered = 1;
		return;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "error: ");
		cli_presenter_print_safe_inline(ctx,
						error ? error : (outcome ? outcome : "turn failed"),
						CLI_PRESENTER_EVENT_TEXT_MAX);
		if (detail && detail[0]) {
			cli_output_printf(ctx, " (");
			cli_presenter_print_safe_inline(ctx, detail, CLI_PRESENTER_EVENT_TEXT_MAX);
			cli_output_printf(ctx, ")");
		}
		cli_output_printf(ctx, "\n");
	} else {
		nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_printf(&title, "Error %s",
				    error ? error : (outcome ? outcome : "turn failed"));
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_FAILED, nb_buf_cstr(&title), detail,
					    NULL);
	}
	ctx->presenter->final_rendered = 1;
}

static void cli_presenter_auth(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *backend = cli_presenter_event_string(ev, "backend");
	const char *tool = cli_presenter_event_string(ev, "tool");
	const char *env_name = cli_presenter_event_string(ev, "env_name");
	const struct config *config;

	cli_presenter_clear_status(ctx);
	cli_presenter_print_stream(ctx);
	if (!backend)
		backend = "configured";
	config = ctx->runtime ? runtime_config_borrow(ctx->runtime) : NULL;
	if ((!env_name || !env_name[0]) && config) {
		if (strcmp(backend, "image") == 0)
			env_name = config->models.image.api_key_env;
		else if (strcmp(backend, "video") == 0)
			env_name = config->models.video.api_key_env;
		else
			env_name = config->models.text.api_key_env;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "error: authentication required for ");
		cli_presenter_print_safe_inline(ctx, backend, CLI_PRESENTER_EVENT_TEXT_MAX);
		if (tool && tool[0]) {
			cli_output_printf(ctx, " tool ");
			cli_presenter_print_safe_inline(ctx, tool, TOOL_NAME_MAX);
		}
		if (env_name && env_name[0]) {
			cli_output_printf(ctx, "; export ");
			cli_presenter_print_safe_inline(ctx, env_name,
							CLI_PRESENTER_EVENT_TEXT_MAX);
		}
		cli_output_printf(ctx, "\n");
	} else {
		nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf detail NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_printf(&title, "Authentication required for %s", backend);
		if (tool && *tool)
			(void)nb_buf_printf(&title, " tool %s", tool);
		if (env_name && *env_name)
			(void)nb_buf_printf(&detail, "export %s", env_name);
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_QUESTION, nb_buf_cstr(&title),
					    nb_buf_cstr(&detail), NULL);
	}
}

static void cli_presenter_artifact(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *path = cli_presenter_event_string(ev, "path");
	const char *kind = cli_presenter_event_string(ev, "kind");
	int status_paused;

	if (!ev->name || strcmp(ev->name, "artifact.ready") != 0 || !path || !path[0])
		return;
	if (nb_map_has_cstr(ctx->presenter->announced_artifacts, path))
		return;
	status_paused = cli_presenter_pause_status(ctx);
	(void)nb_map_put_cstr(ctx->presenter->announced_artifacts, path, (void *)1, NULL);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		cli_output_printf(ctx, "artifact: ");
		cli_presenter_print_safe_inline(ctx, kind ? kind : "file",
						CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, " ");
		cli_presenter_print_safe_inline(ctx, path, CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, "\n");
		cli_presenter_resume_status(ctx, status_paused);
		return;
	}
	cli_list_tree_prefix(ctx, NULL, 0, 1, 0);
	cli_output_printf(ctx, ANSI_DIM);
	cli_presenter_print_safe_inline(ctx, kind ? kind : "artifact",
					CLI_PRESENTER_EVENT_TEXT_MAX);
	cli_output_printf(ctx, ": ");
	cli_presenter_print_safe_inline(ctx, path, CLI_PRESENTER_EVENT_TEXT_MAX);
	cli_output_printf(ctx, ANSI_RESET "\n");
	cli_presenter_resume_status(ctx, status_paused);
}

static void cli_presenter_mcp(struct cli_application *ctx, const struct morph_event *ev)
{
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	const char *server = cli_presenter_event_string(ev, "server");
	const char *error = cli_presenter_event_string(ev, "error");
	if (!server || !*server)
		server = "server";
	int paused = cli_presenter_pause_status(ctx);
	if (!ctx->presenter->mcp_tree_active || strcmp(ctx->presenter->mcp_tree_server, server)) {
		nb_buf root NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_printf(&root, "MCP %s", server);
		(void)cli_feed_notice_write(ctx, CLI_NOTICE_SYSTEM, nb_buf_cstr(&root), NULL, NULL);
		strncpy(ctx->presenter->mcp_tree_server, server,
			sizeof(ctx->presenter->mcp_tree_server) - 1);
		ctx->presenter->mcp_tree_server[sizeof(ctx->presenter->mcp_tree_server) - 1] = '\0';
		ctx->presenter->mcp_tree_active = 1;
	}
	int failed = ev->name && !strcmp(ev->name, "mcp.failed");
	int ready = ev->name && !strcmp(ev->name, "mcp.ready");
	int disconnected = ev->name && !strcmp(ev->name, "mcp.disconnected");
	int connected = ev->name && !strcmp(ev->name, "mcp.connected");
	int last = failed || ready || disconnected;
	const char *label = failed ? "Failed" : ready ? "Ready" : disconnected ? "Disconnected"
		: connected ? "Connected" : ev->name && !strcmp(ev->name, "mcp.connecting")
		? "Connecting" : ev->name && !strcmp(ev->name, "mcp.discovering")
		? "Discovering capabilities" : ev->message ? ev->message : "Updated";
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	(void)nb_buf_append_cstr(&title, label);
	if (failed && error && *error)
		(void)nb_buf_printf(&title, " · %s", error);
	if (ready && cJSON_IsNumber(cli_presenter_event_item(ev, "tools")) &&
	    cJSON_IsNumber(cli_presenter_event_item(ev, "resources")) &&
	    cJSON_IsNumber(cli_presenter_event_item(ev, "prompts")))
		(void)nb_buf_printf(&title, " · %d tools, %d resources, %d prompts",
				    cli_presenter_event_int(ev, "tools"),
				    cli_presenter_event_int(ev, "resources"),
				    cli_presenter_event_int(ev, "prompts"));
	struct cli_notice notice = {
		.status = failed	       ? CLI_NOTICE_FAILED
			  : disconnected       ? CLI_NOTICE_CANCELLED
			  : ready || connected ? CLI_NOTICE_DONE
					       : CLI_NOTICE_RUNNING,
		.title = nb_buf_cstr(&title),
		.tree_branch = last ? 2 : 1,
	};
	struct cli_feed_block block = {
		.kind = CLI_FEED_BLOCK_NOTICE, .join_previous = 1, .content.notice = &notice,
	};
	(void)cli_feed_block_write(ctx, &block);
	if (last) {
		ctx->presenter->mcp_tree_active = 0;
		ctx->presenter->mcp_tree_server[0] = '\0';
	}
	cli_presenter_resume_status(ctx, paused);
}

static void cli_presenter_auxiliary(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *prefix;
	int status_paused;

	if (!ctx->presentation_ready || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (ev->type == MORPH_EVENT_TASK && ev->name) {
		if (strcmp(ev->name, "task.notification") == 0 ||
		    (ctx->input_status.enabled &&
		     (strcmp(ev->name, "task.started") == 0 ||
		      strcmp(ev->name, "task.claimed") == 0 ||
		      strcmp(ev->name, "task.rescheduled") == 0 ||
		      cJSON_IsNumber(cli_presenter_event_item(ev, "notification_id")))))
			return;
	}
	/* Foreground task mutations are already reported by the tool result. */
	if (ev->type == MORPH_EVENT_TASK && ctx->turn_active && ev->name &&
	    cli_presenter_event_error_code(ev) >= 0 &&
	    (strcmp(ev->name, "task.created") == 0 || strcmp(ev->name, "task.updated") == 0 ||
	     strcmp(ev->name, "task.cancelled") == 0))
		return;
	if (ev->type == MORPH_EVENT_BACKGROUND) {
		if ((ev->name && strstr(ev->name, "failed")) ||
		    cli_presenter_event_error_code(ev) < 0) {
			status_paused = cli_presenter_pause_status(ctx);
			nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
			(void)nb_buf_printf(&title, "Background · %s",
					    ev->message ? ev->message
							: (ev->name ? ev->name : "failed"));
			(void)cli_feed_notice_write(ctx, CLI_NOTICE_FAILED, nb_buf_cstr(&title),
						    NULL, NULL);
			cli_presenter_resume_status(ctx, status_paused);
		} else if (ev->phase && (strcmp(ev->phase, "begin") == 0 ||
					 strcmp(ev->phase, "progress") == 0)) {
			if (!ctx->turn_active)
				cli_presenter_status(ctx, ev->message ? ev->message : "Working…");
		} else if (!ctx->turn_active) {
			cli_presenter_clear_status(ctx);
		}
		return;
	}
	if (ev->type == MORPH_EVENT_MCP) {
		if (!cJSON_IsTrue(cli_presenter_event_item(ev, "background")) ||
		    !ctx->input_status.enabled) {
			cli_presenter_mcp(ctx, ev);
		} else if (ev->name && strcmp(ev->name, "mcp.failed") == 0) {
			const char *server = cli_presenter_event_string(ev, "server");
			const char *error = cli_presenter_event_string(ev, "error");

			status_paused = cli_presenter_pause_status(ctx);
			nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
			(void)nb_buf_printf(&title, "MCP %s · failed", server ? server : "server");
			(void)cli_feed_notice_write(ctx, CLI_NOTICE_FAILED, nb_buf_cstr(&title),
						    error, NULL);
			cli_presenter_resume_status(ctx, status_paused);
		}
		return;
	}
	if (ev->type == MORPH_EVENT_TASK)
		prefix = "Task";
	else if (ev->type == MORPH_EVENT_ERROR)
		prefix = "Error";
	else
		return;
	status_paused = cli_presenter_pause_status(ctx);
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	(void)nb_buf_printf(&title, "%s · %s", prefix,
			    ev->message ? ev->message : (ev->name ? ev->name : ""));
	enum cli_notice_status status = ev->type == MORPH_EVENT_ERROR ||
		cli_presenter_event_error_code(ev) < 0 ? CLI_NOTICE_FAILED
		: ev->name && strstr(ev->name, "cancelled") ? CLI_NOTICE_CANCELLED
		: ev->phase && (!strcmp(ev->phase, "begin") || !strcmp(ev->phase, "progress"))
		? CLI_NOTICE_RUNNING : ev->phase && !strcmp(ev->phase, "end")
		? CLI_NOTICE_DONE : CLI_NOTICE_SYSTEM;
	(void)cli_feed_notice_write(ctx, status, nb_buf_cstr(&title), NULL, NULL);
	cli_presenter_resume_status(ctx, status_paused);
}

int cli_presentation_init(struct cli_application *ctx)
{
	int rc;

	if (!ctx)
		return NB_ERR_INVAL;
	if (ctx->presenter)
		return NB_ERR_STATE;
	ctx->presenter = calloc(1, sizeof(*ctx->presenter));
	if (!ctx->presenter)
		return NB_ERR_NOMEM;
	rc = cli_agents_model_init(&ctx->agents);
	if (!rc)
		rc = (nb_buf_init(&ctx->presenter->event_stream),
		      nb_buf_reserve(&ctx->presenter->event_stream, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_init(&ctx->presenter->markdown_stream_text),
		      nb_buf_reserve(&ctx->presenter->markdown_stream_text, BUFSIZ) ? 0
										    : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_init(&ctx->presenter->native_content),
		      nb_buf_reserve(&ctx->presenter->native_content, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (!rc) {
		ctx->presenter->rendered_artifacts = nb_map_new_with_cap(16);
		if (!ctx->presenter->rendered_artifacts)
			rc = NB_ERR_NOMEM;
	}
	if (!rc) {
		ctx->presenter->announced_artifacts = nb_map_new_with_cap(16);
		if (!ctx->presenter->announced_artifacts)
			rc = NB_ERR_NOMEM;
	}
	if (rc) {
		cli_presentation_cleanup(ctx);
		return rc;
	}
	morph_terminal_sanitizer_init(&ctx->presenter->event_stream_sanitizer,
				      MORPH_TERMINAL_TEXT_MULTILINE);
	morph_terminal_sanitizer_init(&ctx->presenter->markdown_stream_sanitizer,
				      MORPH_TERMINAL_TEXT_MULTILINE);
	cli_presentation_recap_attach(ctx);
	return 0;
}

void cli_presentation_reset(struct cli_application *ctx)
{
	if (!ctx || !ctx->presenter)
		return;
	cli_transcript_reset(ctx);
	nb_buf_clear(&ctx->presenter->native_content);
	ctx->presenter->native_content_started = 0;
	ctx->presenter->native_content_visible = 0;
	nb_buf_clear(&ctx->presenter->event_stream);
	morph_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
	nb_map_clear(ctx->presenter->rendered_artifacts);
	nb_map_clear(ctx->presenter->announced_artifacts);
	cli_markdown_stream_reset(ctx, 1);
	cli_presentation_agents_flush(ctx);
	ctx->presenter->event_stream_kind = CLI_PRESENTER_STREAM_NONE;
	ctx->presenter->event_stream_has_delta = 0;
	ctx->presenter->event_stream_complete = 0;
	ctx->presenter->event_stream_visible = 0;
	ctx->presenter->reasoning_stream_offset = 0;
	ctx->presenter->final_rendered = 0;
	cli_feed_live_clear(ctx);
	ctx->presenter->mcp_tree_active = 0;
	ctx->presenter->mcp_tree_server[0] = '\0';
}

void cli_presentation_finish(struct cli_application *ctx)
{
	cli_presenter_clear_status(ctx);
	cli_transcript_finish(ctx);
	if (ctx && ctx->presenter && ctx->agents.receipts.len) {
		cli_presenter_print_stream(ctx);
		cli_markdown_stream_reset(ctx, 1);
		cli_presentation_agents_flush(ctx);
	}
}

void cli_presentation_cleanup(struct cli_application *ctx)
{
	if (!ctx)
		return;
	cli_recap_set_renderer(ctx, NULL);
	if (!ctx->presenter)
		return;
	if (cli_turn_details_is_open(ctx))
		cli_turn_details_toggle(ctx);
	cli_turn_details_cleanup(ctx);
	cli_transcript_reset(ctx);
	cli_presenter_clear_status(ctx);
	cli_markdown_stream_reset(ctx, 0);
	nb_buf_cleanup(&ctx->presenter->native_content);
	nb_buf_cleanup(&ctx->presenter->markdown_stream_text);
	nb_buf_cleanup(&ctx->presenter->event_stream);
	nb_map_free(ctx->presenter->rendered_artifacts);
	nb_map_free(ctx->presenter->announced_artifacts);
	ctx->presenter->rendered_artifacts = NULL;
	ctx->presenter->announced_artifacts = NULL;
	cli_agents_model_cleanup(&ctx->agents);
	free(ctx->presenter);
	ctx->presenter = NULL;
}

int cli_presentation_event(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *text;

	if (!ctx || !ev)
		return NB_ERR_INVAL;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		if (ev->type == MORPH_EVENT_BACKGROUND && ev->name &&
		    strcmp(ev->name, "session.recap") == 0)
			cli_recap_event(ctx, ev);
		char *json = morph_event_json_text_copy(ev);

		if (!json)
			return NB_ERR_NOMEM;
		int rc = cli_output_write_json(json);

		free(json);
		return rc;
	}
	if (!ctx->presentation_ready)
		return 0;
	if (ev->name && !strcmp(ev->name, "plan.updated")) {
		int paused = cli_presenter_pause_status(ctx);
		cli_presenter_print_stream(ctx);
		(void)cli_presenter_plan(ctx, ev);
		cli_presenter_resume_status(ctx, paused);
		return 0;
	}
	if (ev->type == MORPH_EVENT_BACKGROUND && ev->name && strncmp(ev->name, "hook.", 5) == 0) {
		if (strcmp(ev->name, "hook.progress") == 0) {
			cli_presenter_status(ctx, ev->message ? ev->message : "Running hooks");
		} else if (strcmp(ev->name, "hook.started") != 0) {
			cli_presenter_clear_status(ctx);
			if (strcmp(ev->name, "hook.message") == 0 ||
			    strcmp(ev->name, "hook.failed") == 0 ||
			    strcmp(ev->name, "hook.blocked") == 0) {
				int paused = cli_presenter_pause_status(ctx);
				const char *boundary = cli_presenter_event_string(ev, "event");

				if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
					nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
					(void)nb_buf_printf(&title, "Hook%s%s · %s",
							    boundary ? " " : "",
							    boundary ? boundary : "",
							    ev->message ? ev->message : ev->name);
					const char *error = cli_presenter_event_string(ev, "error");
					enum cli_notice_status status = !strcmp(ev->name, "hook.failed")
						? CLI_NOTICE_FAILED : !strcmp(ev->name, "hook.blocked")
						? CLI_NOTICE_QUESTION : CLI_NOTICE_SYSTEM;
					(void)cli_feed_notice_write(
						ctx, status, nb_buf_cstr(&title), error, NULL);
				} else {
				cli_output_printf(ctx, "\n" ANSI_YELLOW "  Hook%s%s · ", boundary ? " " : "",
				       boundary ? boundary : "");
				cli_presenter_print_safe_inline(
					ctx, ev->message ? ev->message : ev->name,
					CLI_PRESENTER_EVENT_TEXT_MAX);
				const char *error = cli_presenter_event_string(ev, "error");
				if (error && *error) {
					cli_output_printf(ctx, ": ");
					cli_presenter_print_safe_inline(
						ctx, error, CLI_PRESENTER_EVENT_TEXT_MAX);
				}
				cli_output_printf(ctx, ANSI_RESET "\n");
				}
				cli_presenter_resume_status(ctx, paused);
			}
		}
		return 0;
	}
	if (ev->type == MORPH_EVENT_BACKGROUND && ev->phase && strcmp(ev->phase, "warning") == 0) {
		int paused = cli_presenter_pause_status(ctx);

		if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
			nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
			(void)nb_buf_printf(&title, "Warning · %s",
					    ev->message ? ev->message : ev->name);
			(void)cli_feed_notice_write(ctx, CLI_NOTICE_WARNING, nb_buf_cstr(&title),
						    NULL, NULL);
		} else {
		cli_output_printf(ctx, "\n" ANSI_YELLOW CLI_FEED_WARNING " Warning · ");
		cli_presenter_print_safe_inline(ctx, ev->message ? ev->message : ev->name,
						CLI_PRESENTER_EVENT_TEXT_MAX);
		cli_output_printf(ctx, ANSI_RESET "\n");
		}
		cli_presenter_resume_status(ctx, paused);
		return 0;
	}
	int agents_rc = cli_presentation_agents_event(ctx, ev);
	if (agents_rc)
		return agents_rc < 0 ? agents_rc : 0;
	int jobs_rc = cli_jobs_event(ctx, ev);
	if (jobs_rc)
		return jobs_rc < 0 ? jobs_rc : 0;
	if (ev->type == MORPH_EVENT_BACKGROUND && ev->name &&
	    strcmp(ev->name, "session.recap") == 0) {
		cli_recap_event(ctx, ev);
		return 0;
	}
	/* Background metadata belongs in the footer, never in the conversation. */
	if (ev->type == MORPH_EVENT_BACKGROUND && ev->name &&
	    strcmp(ev->name, "session.title") == 0) {
		if (ev->phase && strcmp(ev->phase, "begin") == 0)
			ctx->input_status.session_titles_pending++;
		else if (ev->phase && strcmp(ev->phase, "end") == 0 &&
			 ctx->input_status.session_titles_pending > 0)
			ctx->input_status.session_titles_pending--;
		return 0;
	}
	if (!ctx->turn_active && (ev->type == MORPH_EVENT_REACT || ev->type == MORPH_EVENT_TOOL ||
				  ev->type == MORPH_EVENT_HITL || ev->type == MORPH_EVENT_ARTIFACT))
		return 0;
	if (ctx->turn_active) {
		int handled = cli_transcript_event(ctx, ev);

		if (handled != 0)
			return handled < 0 ? handled : 0;
	}

	int chat_rc = cli_chat_react_event(ctx, ev);
	if (chat_rc)
		return chat_rc < 0 ? chat_rc : 0;
	if (ev->type == MORPH_EVENT_REACT && ev->name) {
		text = cli_presenter_event_string(ev, "text");
		if (strcmp(ev->name, "react.agents.wait.begin") == 0) {
			cli_presenter_status(ctx, "Waiting for agents");
			return 0;
		}
		if (strcmp(ev->name, "react.agents.wait.end") == 0) {
			cli_presenter_clear_status(ctx);
			return 0;
		}
		if (strncmp(ev->name, "react.compaction.", sizeof("react.compaction.") - 1) == 0) {
			cli_presenter_compaction(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.begin") == 0) {
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				cli_feed_render_frame(ctx, 1);
			return 0;
		}
		if (strcmp(ev->name, "react.hook.continue") == 0) {
			cli_presentation_hook_prompt(ctx, text);
			return 0;
		}
		if (strcmp(ev->name, "react.user.steer") == 0) {
			nb_buf_clear(&ctx->presenter->native_content);
			ctx->presenter->native_content_started = 0;
			ctx->presenter->native_content_visible = 0;
			cli_presenter_clear_status(ctx);
			cli_presenter_discard_stream(ctx);
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				const char *display =
					cli_presenter_event_string(ev, "display_text");

				cli_presentation_user_input(ctx, display ? display : text);
			} else {
				cli_output_printf(ctx, ANSI_DIM "  Requirement applied" ANSI_RESET "\n");
			}
			cli_presenter_model_wait(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.thinking") == 0) {
			cli_presenter_model_wait(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.content.delta") == 0) {
			ctx->presenter->native_content_started = 1;
			if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
				return text ? (nb_buf_append_cstr(&ctx->presenter->native_content,
								  text)
						       ? 0
						       : NB_ERR_NOMEM)
					    : 0;
			int rc = cli_presenter_append_stream(ctx, CLI_PRESENTER_STREAM_CONTENT,
							     text, 1, 0, NULL);
			if (rc == 0) {
				cli_presenter_clear_status(ctx);
				rc = cli_markdown_stream_append(ctx, text,
								CLI_PRESENTER_STREAM_CONTENT);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.content.end") == 0) {
			if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
				cli_presenter_plain_content(ctx, text);
			} else {
				if (!ctx->presenter->native_content_started ||
				    ctx->presenter->event_stream_kind ==
					    CLI_PRESENTER_STREAM_CONTENT) {
					int rc = cli_presenter_append_stream(
						ctx, CLI_PRESENTER_STREAM_CONTENT, text, 0, 1,
						NULL);
					if (rc != 0)
						return rc;
				}
				cli_presenter_print_stream(ctx);
			}
			ctx->presenter->native_content_started = 0;
			ctx->presenter->native_content_visible = 0;
			cli_presentation_agents_flush(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.thought.delta") == 0) {
			int rc = cli_presenter_append_stream(ctx, CLI_PRESENTER_STREAM_THOUGHT,
							     text, 1, 0, NULL);

			if (rc == 0 && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				cli_presenter_clear_status(ctx);
				rc = cli_markdown_stream_append(ctx, text,
								CLI_PRESENTER_STREAM_THOUGHT);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.thought.end") == 0) {
			int rc = cli_presenter_append_stream(ctx, CLI_PRESENTER_STREAM_THOUGHT, text,
				0, 1, NULL);
			if (rc == 0 && ctx->agents.receipts.len) {
				cli_presenter_print_stream(ctx);
				cli_markdown_stream_reset(ctx, 1);
				cli_presentation_agents_flush(ctx);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.reasoning.delta") == 0) {
			int rc = cli_presenter_append_stream(ctx, CLI_PRESENTER_STREAM_REASONING,
							     text, 1, 0, NULL);

			if (rc == 0 && ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				cli_presenter_reasoning_flush(ctx, 0);
			return rc;
		}
		if (strcmp(ev->name, "react.final.delta") == 0) {
			if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
				return 0;
			cli_presenter_clear_status(ctx);
			if (ctx->presenter->event_stream_kind != CLI_PRESENTER_STREAM_NONE)
				cli_presenter_print_stream(ctx);
			return cli_markdown_stream_append(ctx, text, CLI_PRESENTER_STREAM_FINAL);
		}
		if (strcmp(ev->name, "react.observation") == 0) {
			cli_presenter_observation(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.reflection") == 0) {
			cli_presenter_reflection(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.final") == 0) {
			cli_presenter_final(ctx, ev);
			cli_presentation_agents_flush(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.end") == 0) {
			cli_presenter_turn_end(ctx, ev);
			cli_presentation_agents_flush(ctx);
			return 0;
		}
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name && strcmp(ev->name, "tool.call") == 0) {
		cli_presenter_tool_call(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name && strcmp(ev->name, "tool.running") == 0) {
		cli_presenter_status(ctx, "Running tool…");
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    (strcmp(ev->name, "tool.result") == 0 || strcmp(ev->name, "tool.failed") == 0 ||
	     strcmp(ev->name, "tool.cancelled") == 0)) {
		cli_presenter_tool_end(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_HITL && ev->name && strcmp(ev->name, "auth.required") == 0) {
		cli_presenter_auth(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_ARTIFACT) {
		cli_presenter_artifact(ctx, ev);
		return 0;
	}
	cli_presenter_auxiliary(ctx, ev);
	fflush(stdout);
	return 0;
}

int cli_presentation_final_visible(const struct cli_application *ctx)
{
	return ctx && ctx->presenter && ctx->presenter->final_rendered;
}
