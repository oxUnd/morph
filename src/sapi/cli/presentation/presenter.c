#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/pages/subagents.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/output.h"
#include "agent/plan.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/services/transcript.h"
#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/pages/jobs.h"
#include "sapi/cli/services/agents.h"
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
				    enum utf8_terminal_text_mode mode)
{
	char *safe;
	char *limited;

	if (!text)
		return NULL;
	safe = utf8_terminal_sanitize_dup(text, strlen(text), mode, NULL);
	if (!safe)
		return NULL;
	limited = utf8_dup_clamped(safe, max_bytes);
	free(safe);
	return limited;
}

static void cli_presenter_print_safe_inline(struct cli_application *ctx, const char *text,
					    size_t max_bytes)
{
	char *safe =
		cli_presenter_safe_dup(text ? text : "", max_bytes, UTF8_TERMINAL_TEXT_SINGLE_LINE);

	if (!safe)
		return;
	printf(ctx, "%s", safe);
	free(safe);
}

/* Keep submitted local commands visible without committing an editable bubble. */
void cli_presentation_command_input(struct cli_application *ctx, const char *text)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE || !text || text[0] != '/')
		return;
	if (cli_output_color_enabled(ctx))
		printf(ctx, ANSI_RESET CLI_COMMAND_COLOR);
	cli_presenter_print_safe_inline(ctx, text, SIZE_MAX);
	if (cli_output_color_enabled(ctx))
		printf(ctx, ANSI_RESET);
	putchar(ctx, '\n');
	ctx->output.command_result_pending = 1;
}

/* Keep command results clear of the editable composer's background. */
void cli_presentation_command_end(struct cli_application *ctx)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	ctx->output.command_result_pending = 0;
	if (cli_output_color_enabled(ctx))
		printf(ctx, ANSI_RESET);
	putchar(ctx, '\n');
}

/* Submitted user messages remain ordinary scrollback, separate from tools. */
static void cli_presenter_user_padding(struct cli_application *ctx)
{
	printf(ctx, ANSI_RESET CLI_INPUT_BACKGROUND);
	(void)fwrite("\033[K", 1, 3, stdout);
	printf(ctx, ANSI_RESET "\n");
}

static void cli_presenter_user_row(struct cli_application *ctx, const char *text, size_t length,
				   int first)
{
	int color = cli_output_color_enabled(ctx);

	/* Erase with the active background, without padding scrollback with spaces. */
	if (color) {
		printf(ctx, ANSI_RESET CLI_INPUT_BACKGROUND);
		/* Trusted layout control bypasses the text output sanitizer. */
		(void)fwrite("\033[K", 1, 3, stdout);
	}
	if (first) {
		if (color)
			printf(ctx, "%s%s" ANSI_RESET CLI_INPUT_BACKGROUND,
			       ctx->composer.ultra ? CLI_INPUT_ULTRA_COLOR : CLI_INPUT_MARKER_COLOR,
			       ctx->composer.ultra ? "›› " : "› ");
		else
			printf(ctx, "%s", ctx->composer.ultra ? "›› " : "> ");
	} else {
		printf(ctx, "%*s", cli_composer_model_prompt_width(&ctx->composer), "");
	}
	printf(ctx, "%.*s", (int)length, text);
	if (color)
		printf(ctx, ANSI_RESET);
	putchar(ctx, '\n');
}

void cli_presentation_user_input(struct cli_application *ctx, const char *text)
{
	const struct morph_tui_view_options box = {
		.margin = {.top = 1, .bottom = 1},
		.padding = {.top = 1, .bottom = 1},
	};
	char *safe;
	char *line;
	int width = cli_list_columns() - 1;
	int first = 1;

	if (!ctx || !text || !*text)
		return;
	safe = utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return;
	/* Tabs affect terminal cursor position; normalize only the display copy. */
	for (char *p = safe; *p; p++) {
		if (*p == '\t')
			*p = ' ';
	}
	if (width < 8)
		width = 8;
	/* Share a preceding block's separator, keeping outer margins unpainted. */
	for (int row = ctx->output.trailing_newlines >= 2 ? 1 : 0; row < box.margin.top; row++)
		putchar(ctx, '\n');
	if (cli_output_color_enabled(ctx))
		for (int row = 0; row < box.padding.top; row++)
			cli_presenter_user_padding(ctx);
	line = safe;
	while (line) {
		char *newline = strchr(line, '\n');
		const char *cursor = line;

		if (newline)
			*newline = '\0';
		do {
			const char *end =
				utf8_advance_display_width_borrow(cursor, (size_t)(width - cli_composer_model_prompt_width(&ctx->composer)));

			cli_presenter_user_row(ctx, cursor, (size_t)(end - cursor), first);
			first = 0;
			cursor = end;
		} while (*cursor);
		line = newline ? newline + 1 : NULL;
	}
	if (cli_output_color_enabled(ctx))
		for (int row = 0; row < box.padding.bottom; row++)
			cli_presenter_user_padding(ctx);
	for (int row = 0; row < box.margin.bottom; row++)
		putchar(ctx, '\n');
	free(safe);
}

static void cli_presenter_clear_status(struct cli_application *ctx)
{
	cli_feed_live_clear(ctx);
}

static void cli_presenter_status(struct cli_application *ctx, const char *text)
{
	if (!ctx || !text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf(ctx, "status: ");
		cli_presenter_print_safe_inline(ctx, text, CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, "\n");
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
		putchar(ctx, ' ');
}

static const char *cli_presenter_tree_wrap_end(const char *text, size_t width, int *has_break)
{
	const char *end;
	const char *space = NULL;

	end = utf8_advance_display_width_borrow(text, width);
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
					 UTF8_TERMINAL_TEXT_SINGLE_LINE);
	if (!display)
		return;
	for (char *p = display; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	}
	label_width = label ? utf8_display_width(label) : 0;
	available = cli_list_columns() - 4 - depth * 2 - (int)label_width - 2;
	if (available < 8)
		available = 8;
	line = display;
	while (*line) {
		end = cli_presenter_tree_wrap_end(line, (size_t)available, &has_break);
		if (first) {
			cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
			if (label && label[0]) {
				printf(ctx, ANSI_DIM);
				cli_presenter_print_safe_inline(ctx, label,
								CLI_PRESENTER_TREE_VALUE_MAX);
				printf(ctx, ":" ANSI_RESET " ");
			}
			first = 0;
		} else {
			cli_presenter_print_tree_continuation_prefix(ctx, ancestors_last, depth,
								     is_last, label_width);
		}
		if (*end && !has_break)
			end = utf8_advance_display_width_borrow(line, (size_t)(available - 1));
		printf(ctx, "%.*s", (int)(end - line), line);
		if (*end && !has_break) {
			printf(ctx, "…\n");
			break;
		}
		printf(ctx, "\n");
		line = end;
		while (*line == ' ')
			line++;
	}
	if (first) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
		if (label && label[0]) {
			printf(ctx, ANSI_DIM);
			cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
			printf(ctx, ":" ANSI_RESET " ");
		}
		printf(ctx, "\n");
	}
	free(display);
}

static void cli_presenter_print_tree_scalar(struct cli_application *ctx, const cJSON *item)
{
	char *value;

	value = cJSON_PrintUnformatted(item);
	if (value) {
		printf(ctx, "%s", value);
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

	printf(ctx, "%s %d item%s", cJSON_IsArray(item) ? "[...]" : "{...}", count,
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
			printf(ctx, ANSI_DIM);
			cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
			printf(ctx, ":" ANSI_RESET " ");
		}
		cli_presenter_print_tree_scalar(ctx, item);
		printf(ctx, "\n");
		return;
	}
	cli_presenter_print_tree_prefix(ctx, ancestors_last, depth, is_last);
	if (label && label[0]) {
		printf(ctx, ANSI_DIM);
		cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_TREE_VALUE_MAX);
		printf(ctx, ":" ANSI_RESET);
	}
	count = cli_presenter_json_child_count(item);
	if (count == 0) {
		printf(ctx, " %s\n", cJSON_IsArray(item) ? "[]" : "{}");
		return;
	}
	if (depth >= collapse_depth) {
		printf(ctx, " ");
		cli_presenter_print_tree_container_summary(ctx, item);
		printf(ctx, "\n");
		return;
	}
	printf(ctx, "\n");
	ancestors_last[depth] = is_last;
	if (depth + 1 >= CLI_PRESENTER_TREE_DEPTH_MAX) {
		cli_presenter_print_tree_prefix(ctx, ancestors_last, depth + 1, 1);
		printf(ctx, ANSI_DIM "… nested data" ANSI_RESET "\n");
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
		printf(ctx, ANSI_DIM "… %d more" ANSI_RESET "\n", count - shown);
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
		printf(ctx, ANSI_DIM "… %d more" ANSI_RESET "\n",
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
					 UTF8_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	line = display;
	while (line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);

		printf(ctx, "%s%.*s\n", prefix, (int)len, line);
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
					 UTF8_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	if (strchr(display, '\n'))
		printf(ctx, "%s:\n%s\n", label, display);
	else
		printf(ctx, "%s: %s\n", label, display);
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
	(void)utf8_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
					  &ctx->presenter->event_stream, NULL, 0, 1);
	content = morph_buf_cstr_borrow(&ctx->presenter->event_stream);
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
			printf(ctx, "%s" ANSI_BOLD "●" ANSI_RESET " ",
			       ctx->output.trailing_newlines < 2 ? "\n" : "");
			cli_markdown_render_ansi_with_media_indented(
				ctx, content, 2, cli_renderer_media, ctx);
		}
	}
	if (ctx->presenter->event_stream_kind == CLI_PRESENTER_STREAM_CONTENT)
		ctx->presenter->native_content_visible = 1;
	morph_buf_reset(&ctx->presenter->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
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
	morph_buf_reset(&ctx->presenter->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
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
		text = morph_buf_cstr_borrow(&ctx->presenter->native_content);
	if (text && *text) {
		safe = cli_presenter_safe_dup(text, SIZE_MAX, UTF8_TERMINAL_TEXT_MULTILINE);
		if (safe) {
			if (strchr(safe, '\n'))
				printf(ctx, "content:\n%s\n", safe);
			else
				printf(ctx, "content: %s\n", safe);
			free(safe);
		}
	}
	morph_buf_reset(&ctx->presenter->native_content);
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
		printf(ctx, "\n" ANSI_DIM "  Stop hook requested continuation\n" ANSI_RESET);
		cli_presenter_print_indented(ctx, "  ", text);
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
		utf8_terminal_sanitizer_init(&ctx->presenter->event_stream_sanitizer,
					     UTF8_TERMINAL_TEXT_MULTILINE);
	}
	if (!is_delta && ctx->presenter->event_stream_has_delta) {
		rc = utf8_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
						 &ctx->presenter->event_stream, NULL, 0, 1);
		if (rc != 0)
			return rc;
		ctx->presenter->event_stream_complete = is_complete;
		return 0;
	}
	offset = ctx->presenter->event_stream.len;
	rc = utf8_terminal_sanitize_feed(&ctx->presenter->event_stream_sanitizer,
					 &ctx->presenter->event_stream, text, strlen(text),
					 !is_delta);
	if (rc != 0)
		return rc;
	if (appended_text)
		*appended_text = morph_buf_cstr_borrow(&ctx->presenter->event_stream) + offset;
	if (is_delta)
		ctx->presenter->event_stream_has_delta = 1;
	if (is_complete)
		ctx->presenter->event_stream_complete = 1;
	return 0;
}

/* The terminal owns the live row; commit its preview only when the stream ends. */
static void cli_presenter_reasoning_flush(struct cli_application *ctx, int finish)
{
	const char *content = morph_buf_cstr_borrow(&ctx->presenter->event_stream);
	size_t offset = ctx->presenter->reasoning_stream_offset;
	const char *start;
	const char *end;
	const char *newline;
	morph_buf_t preview;
	int columns = cli_feed_columns(ctx);
	size_t width = columns > 4 ? (size_t)(columns - 4) : 1u;
	int rc;

	if (offset < ctx->presenter->event_stream.len) {
		rc = cli_transcript_reasoning_append(ctx, content + offset,
						     ctx->presenter->event_stream.len - offset,
						     !ctx->presenter->event_stream_visible);
		if (rc != 0) {
			log_warn(ctx ? ctx->logger : NULL, "Could not retain reasoning details: %s",
				 morph_strerror(rc));
			return;
		}
		ctx->presenter->reasoning_stream_offset = ctx->presenter->event_stream.len;
		ctx->presenter->event_stream_visible = 1;
	}
	start = content + strspn(content, " \t\r\n");
	if (!*start || morph_buf_init(&preview, 128) != 0)
		return;
	newline = strchr(start, '\n');
	end = utf8_advance_display_width_borrow(start, width - 1u);
	if (newline && newline < end)
		end = newline;
	(void)morph_buf_append(&preview, start, (size_t)(end - start));
	if (*end)
		(void)morph_buf_puts(&preview, "…");
	if (finish) {
		cli_presenter_clear_status(ctx);
		/* Commit one quiet narration line between tool blocks. */
		printf(ctx, "%s" ANSI_DIM "\033[3m%s" ANSI_RESET "\n",
		       ctx->output.trailing_newlines < 2 ? "\n" : "",
		       morph_buf_cstr_borrow(&preview));
	} else if (isatty(STDOUT_FILENO)) {
		cli_feed_reasoning_set(ctx, morph_buf_cstr_borrow(&preview));
	}
	morph_buf_cleanup(&preview);
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
		printf(ctx, "  │ " ANSI_DIM "%5s " ANSI_RESET "%s%.*s" ANSI_RESET "\n", number,
		       style, (int)len, line);
		return;
	}
	printf(ctx, "  │ %s%.*s" ANSI_RESET "\n", style, (int)len, line);
}

void cli_presentation_patch_diff(struct cli_application *ctx, const char *workdir,
				 const char *input)
{
	morph_buf_t preview;
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
		if (morph_buf_init(&preview, 256) == 0) {
			if (patch_preview(workdir, input, &preview, preview_error,
					  sizeof(preview_error)) == 0) {
				text = morph_buf_cstr_borrow(&preview);
				preview_ready = 1;
			} else {
				morph_buf_cleanup(&preview);
			}
		}
	}
	input_len = strlen(text);
	display = malloc(CLI_PRESENTER_PATCH_BYTES_MAX + 1);
	if (!display) {
		if (preview_ready)
			morph_buf_cleanup(&preview);
		return;
	}
	{
		char *safe = cli_presenter_safe_dup(text, CLI_PRESENTER_PATCH_BYTES_MAX,
						    UTF8_TERMINAL_TEXT_MULTILINE);

		if (!safe) {
			free(display);
			if (preview_ready)
				morph_buf_cleanup(&preview);
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
		printf(ctx, "  %s" ANSI_DIM "… patch display truncated" ANSI_RESET "\n",
		       morph_tui_widget_tree_branch(1, 0));
	free(display);
	if (preview_ready)
		morph_buf_cleanup(&preview);
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
	safe_tool = cli_presenter_safe_dup(tool, TOOL_NAME_MAX, UTF8_TERMINAL_TEXT_SINGLE_LINE);
	safe_title = cli_presenter_safe_dup(title, CLI_PRESENTER_EVENT_TEXT_MAX,
					    UTF8_TERMINAL_TEXT_SINGLE_LINE);
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
		utf8_copy_sanitized_display_width(display, sizeof(display), args,
						  CLI_PRESENTER_EVENT_ARGS_MAX);
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf(ctx, "tool: %s", tool);
		if (display[0])
			printf(ctx, " %s", display);
		printf(ctx, "\n");
		if (patch_input)
			cli_presentation_patch_diff(ctx, cli_presenter_workdir(ctx), patch_input);
	} else {
		printf(ctx, "\n" ANSI_YELLOW "◦" ANSI_RESET " " ANSI_BOLD "%s" ANSI_RESET "\n",
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
		printf(ctx, "status: ");
		cli_presenter_print_safe_inline(ctx, tool ? tool : "tool", TOOL_NAME_MAX);
		printf(ctx, " %s\n", failed ? "failed" : "completed");
		return;
	}
	if (failed) {
		printf(ctx, "  " ANSI_RED CLI_FEED_FAILED " ");
		cli_presenter_print_safe_inline(ctx, title, CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, " failed" ANSI_RESET);
		if (error && error[0]) {
			printf(ctx, ANSI_DIM " · ");
			cli_presenter_print_safe_inline(ctx, error, CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ANSI_RESET);
		}
		printf(ctx, "\n");
	} else {
		printf(ctx, "  " ANSI_GREEN CLI_FEED_DONE " ");
		cli_presenter_print_safe_inline(ctx, title, CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, " completed" ANSI_RESET "\n");
	}
}

static const char *cli_presenter_plan_mark(const char *status, int active)
{
	if (active)
		return CLI_FEED_RUNNING;
	if (status && strcmp(status, "completed") == 0)
		return CLI_FEED_DONE;
	if (status && strcmp(status, "failed") == 0)
		return CLI_FEED_FAILED;
	if (status && strcmp(status, "skipped") == 0)
		return "–";
	return CLI_FEED_PENDING;
}

static int cli_presenter_plan(struct cli_application *ctx, const struct morph_event *ev)
{
	cJSON *result;
	cJSON *plans;
	cJSON *plan;
	const char *selected;
	const char *selected_key = "id";
	int printed = 0;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	if (ev->name && !strcmp(ev->name, "plan.updated")) {
		result = ev->data;
	} else {
		const char *tool = cli_presenter_event_string(ev, "tool");
		if (!tool || strcmp(tool, "plan"))
			return 0;
		result = cJSON_GetObjectItemCaseSensitive(ev->data, "data");
	}
	plans = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "plans") : NULL;
	if (!cJSON_IsArray(plans))
		return 0;
	selected =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "selected_plan_id"));
	if (!selected) {
		selected = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(result, "selected_plan"));
		selected_key = "name";
	}
	cJSON_ArrayForEach (plan, plans) {
		cJSON *steps;
		cJSON *step;
		int completed = 0;
		int count;

		if (!cJSON_IsObject(plan))
			continue;
		if (selected) {
			const char *value = cJSON_GetStringValue(
				cJSON_GetObjectItemCaseSensitive(plan, selected_key));

			if (!value || strcmp(value, selected) != 0)
				continue;
		}
		steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
		if (!cJSON_IsArray(steps))
			continue;
		count = cJSON_GetArraySize(steps);
		cJSON_ArrayForEach (step, steps) {
			const cJSON *status = cJSON_GetObjectItemCaseSensitive(step, "status");

			if (cJSON_IsString(status) && strcmp(status->valuestring, "completed") == 0)
				completed++;
		}
		printf(ctx, "  " ANSI_DIM "Plan · %d/%d completed" ANSI_RESET "\n", completed,
		       count);
		printed = 1;
		cJSON_ArrayForEach (step, steps) {
			cJSON *description;
			cJSON *status;
			cJSON *active;
			const char *mark;
			const char *color;

			if (!cJSON_IsObject(step))
				continue;
			description = cJSON_GetObjectItemCaseSensitive(step, "description");
			status = cJSON_GetObjectItemCaseSensitive(step, "status");
			active = cJSON_GetObjectItemCaseSensitive(step, "active");
			if (!cJSON_IsString(description))
				continue;
			mark = cli_presenter_plan_mark(cJSON_IsString(status) ? status->valuestring
									      : NULL,
						       cJSON_IsTrue(active));
			color = strcmp(mark, CLI_FEED_DONE) == 0      ? ANSI_GREEN
				: strcmp(mark, CLI_FEED_FAILED) == 0  ? ANSI_RED
				: strcmp(mark, CLI_FEED_RUNNING) == 0 ? ANSI_YELLOW
								      : ANSI_DIM;
			printf(ctx, "  %s%s" ANSI_RESET " ", color, mark);
			cli_presenter_print_safe_inline(ctx, description->valuestring,
							CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, "\n");
		}
		if (selected)
			break;
	}
	return printed;
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
		morph_buf_t prefix = {0};
		if (morph_buf_init(&prefix, 64) == 0) {
			morph_tui_widget_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
			morph_buf_puts(&prefix, ANSI_RED "Error: " ANSI_RESET);
			cli_presenter_print_indented(ctx, morph_buf_cstr_borrow(&prefix), text);
			morph_buf_cleanup(&prefix);
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
			morph_buf_t prefix = {0};
			if (morph_buf_init(&prefix, 32) == 0) {
				morph_tui_widget_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
				cli_presenter_print_indented(ctx, morph_buf_cstr_borrow(&prefix),
							     text);
				morph_buf_cleanup(&prefix);
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
		cli_presenter_print_indented(
			ctx, ANSI_YELLOW CLI_FEED_SYSTEM " Guardrail · " ANSI_RESET, text);
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
			printf(ctx, "\n");
			cli_feed_activity_set(ctx, "Compacting context…");
		}
		return;
	}
	cli_presenter_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		if (failed) {
			printf(ctx, "context: compression failed: ");
			cli_presenter_print_safe_inline(ctx, error ? error : "unknown error",
							CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, "\n");
		} else {
			printf(ctx, "context: compacted %d -> %d tokens\n", before, after);
		}
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (elapsed < 0)
		printf(ctx, "\n");
	if (failed) {
		printf(ctx, ANSI_DIM ANSI_RED "◇ Context compression failed · ");
		cli_presenter_print_safe_inline(ctx, error ? error : "unknown error",
						CLI_PRESENTER_EVENT_TEXT_MAX);
	} else {
		char before_label[32];
		char after_label[32];
		char line[BUFSIZ];
		char clipped[BUFSIZ];
		int columns = cli_feed_columns(ctx) - 1;

		cli_presenter_token_label(before, before_label, sizeof(before_label));
		cli_presenter_token_label(after, after_label, sizeof(after_label));
		if (elapsed >= 0)
			snprintf(line, sizeof(line), "◇ Context compacted  %s → %s · %llds",
				 before_label, after_label, (long long)(elapsed / 1000));
		else
			snprintf(line, sizeof(line), "◇ Context compacted  %s → %s", before_label,
				 after_label);
		(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), line,
							 (size_t)(columns > 0 ? columns : 1), 0);
		printf(ctx, ANSI_DIM "%s", clipped);
	}
	if (failed && elapsed >= 0)
		printf(ctx, " · %llds", (long long)(elapsed / 1000));
	printf(ctx, ANSI_RESET "\n\n");
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
	morph_buf_reset(&ctx->presenter->native_content);
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
		printf(ctx, "final:\n");
		if (text && text[0]) {
			char *safe = cli_presenter_safe_dup(text, SIZE_MAX,
							    UTF8_TERMINAL_TEXT_MULTILINE);

			if (safe) {
				printf(ctx, "%s\n", safe);
				free(safe);
			}
		}
	} else {
		if (!had_stream)
			printf(ctx, "%s" ANSI_BOLD "●" ANSI_RESET " ",
			       ctx->output.trailing_newlines < 2 ? "\n" : "");
		if (!had_stream && text && text[0]) {
			cli_markdown_render_final(ctx, text);
		} else if (!had_stream) {
			printf(ctx, "\n");
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
			printf(ctx, "cancelled\n");
		else
			printf(ctx, "\n" ANSI_DIM CLI_FEED_SYSTEM " Cancelled" ANSI_RESET "\n");
		ctx->presenter->final_rendered = 1;
		return;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf(ctx, "error: ");
		cli_presenter_print_safe_inline(ctx,
						error ? error : (outcome ? outcome : "turn failed"),
						CLI_PRESENTER_EVENT_TEXT_MAX);
		if (detail && detail[0]) {
			printf(ctx, " (");
			cli_presenter_print_safe_inline(ctx, detail, CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ")");
		}
		printf(ctx, "\n");
	} else {
		printf(ctx, "\n" ANSI_BOLD ANSI_RED CLI_FEED_FAILED " Error" ANSI_RESET " ");
		cli_presenter_print_safe_inline(ctx,
						error ? error : (outcome ? outcome : "turn failed"),
						CLI_PRESENTER_EVENT_TEXT_MAX);
		if (detail && detail[0]) {
			printf(ctx, ANSI_DIM " (");
			cli_presenter_print_safe_inline(ctx, detail, CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ")" ANSI_RESET);
		}
		printf(ctx, "\n");
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
		printf(ctx, "error: authentication required for ");
		cli_presenter_print_safe_inline(ctx, backend, CLI_PRESENTER_EVENT_TEXT_MAX);
		if (tool && tool[0]) {
			printf(ctx, " tool ");
			cli_presenter_print_safe_inline(ctx, tool, TOOL_NAME_MAX);
		}
		if (env_name && env_name[0]) {
			printf(ctx, "; export ");
			cli_presenter_print_safe_inline(ctx, env_name,
							CLI_PRESENTER_EVENT_TEXT_MAX);
		}
		printf(ctx, "\n");
	} else {
		printf(ctx, "\n" ANSI_BOLD ANSI_YELLOW CLI_FEED_QUESTION
			    " Authentication required" ANSI_RESET " for ");
		cli_presenter_print_safe_inline(ctx, backend, CLI_PRESENTER_EVENT_TEXT_MAX);
		if (tool && tool[0]) {
			printf(ctx, " tool " ANSI_BOLD);
			cli_presenter_print_safe_inline(ctx, tool, TOOL_NAME_MAX);
			printf(ctx, ANSI_RESET);
		}
		if (env_name && env_name[0]) {
			printf(ctx, ANSI_DIM " · export ");
			cli_presenter_print_safe_inline(ctx, env_name,
							CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ANSI_RESET);
		}
		printf(ctx, "\n");
	}
}

static void cli_presenter_artifact(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *path = cli_presenter_event_string(ev, "path");
	const char *kind = cli_presenter_event_string(ev, "kind");
	int status_paused;

	if (!ev->name || strcmp(ev->name, "artifact.ready") != 0 || !path || !path[0])
		return;
	if (morph_strmap_contains(&ctx->presenter->announced_artifacts, path))
		return;
	status_paused = cli_presenter_pause_status(ctx);
	(void)morph_strmap_set_borrow(&ctx->presenter->announced_artifacts, path, (void *)1);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf(ctx, "artifact: ");
		cli_presenter_print_safe_inline(ctx, kind ? kind : "file",
						CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, " ");
		cli_presenter_print_safe_inline(ctx, path, CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, "\n");
		cli_presenter_resume_status(ctx, status_paused);
		return;
	}
	cli_list_tree_prefix(ctx, NULL, 0, 1, 0);
	printf(ctx, ANSI_DIM);
	cli_presenter_print_safe_inline(ctx, kind ? kind : "artifact",
					CLI_PRESENTER_EVENT_TEXT_MAX);
	printf(ctx, ": ");
	cli_presenter_print_safe_inline(ctx, path, CLI_PRESENTER_EVENT_TEXT_MAX);
	printf(ctx, ANSI_RESET "\n");
	cli_presenter_resume_status(ctx, status_paused);
}

static void cli_presenter_mcp_tree_start(struct cli_application *ctx, const char *server)
{
	printf(ctx, "\n" ANSI_DIM CLI_FEED_SYSTEM ANSI_RESET " " ANSI_BOLD "MCP ");
	cli_presenter_print_safe_inline(ctx, server, CLI_PRESENTER_EVENT_TEXT_MAX);
	printf(ctx, ANSI_RESET "\n");
	strncpy(ctx->presenter->mcp_tree_server, server,
		sizeof(ctx->presenter->mcp_tree_server) - 1);
	ctx->presenter->mcp_tree_server[sizeof(ctx->presenter->mcp_tree_server) - 1] = '\0';
	ctx->presenter->mcp_tree_active = 1;
}

static void cli_presenter_mcp_ready(struct cli_application *ctx, const struct morph_event *ev)
{
	cJSON *tools = cli_presenter_event_item(ev, "tools");
	cJSON *resources = cli_presenter_event_item(ev, "resources");
	cJSON *prompts = cli_presenter_event_item(ev, "prompts");

	printf(ctx, ANSI_GREEN CLI_FEED_DONE " Ready" ANSI_RESET);
	if (cJSON_IsNumber(tools) && cJSON_IsNumber(resources) && cJSON_IsNumber(prompts)) {
		printf(ctx, ANSI_DIM " · %d tools, %d resources, %d prompts" ANSI_RESET,
		       tools->valueint, resources->valueint, prompts->valueint);
	}
}

static void cli_presenter_mcp(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *server = cli_presenter_event_string(ev, "server");
	const char *error = cli_presenter_event_string(ev, "error");
	const char *label;
	int is_last;
	int failed;
	int status_paused;

	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (!server || !server[0])
		server = "server";
	status_paused = cli_presenter_pause_status(ctx);
	if (!ctx->presenter->mcp_tree_active ||
	    strcmp(ctx->presenter->mcp_tree_server, server) != 0)
		cli_presenter_mcp_tree_start(ctx, server);
	failed = ev->name && strcmp(ev->name, "mcp.failed") == 0;
	is_last = failed || (ev->name && (strcmp(ev->name, "mcp.ready") == 0 ||
					  strcmp(ev->name, "mcp.disconnected") == 0));
	cli_list_tree_prefix(ctx, NULL, 0, is_last, 0);
	if (failed) {
		printf(ctx, ANSI_RED CLI_FEED_FAILED " Failed" ANSI_RESET);
		if (error && error[0]) {
			printf(ctx, ANSI_DIM " · ");
			cli_presenter_print_safe_inline(ctx, error, CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ANSI_RESET);
		}
	} else if (ev->name && strcmp(ev->name, "mcp.ready") == 0) {
		cli_presenter_mcp_ready(ctx, ev);
	} else {
		if (ev->name && strcmp(ev->name, "mcp.connecting") == 0)
			label = "Connecting";
		else if (ev->name && strcmp(ev->name, "mcp.connected") == 0)
			label = "Connected";
		else if (ev->name && strcmp(ev->name, "mcp.discovering") == 0)
			label = "Discovering capabilities";
		else if (ev->name && strcmp(ev->name, "mcp.disconnected") == 0)
			label = "Disconnected";
		else
			label = ev->message ? ev->message : "Updated";
		cli_presenter_print_safe_inline(ctx, label, CLI_PRESENTER_EVENT_TEXT_MAX);
	}
	printf(ctx, "\n");
	if (is_last) {
		ctx->presenter->mcp_tree_active = 0;
		ctx->presenter->mcp_tree_server[0] = '\0';
	}
	cli_presenter_resume_status(ctx, status_paused);
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
			printf(ctx, "\n" ANSI_RED CLI_FEED_FAILED " Background · ");
			cli_presenter_print_safe_inline(
				ctx, ev->message ? ev->message : (ev->name ? ev->name : "failed"),
				CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ANSI_RESET "\n");
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
			printf(ctx, "\n" ANSI_RED CLI_FEED_FAILED ANSI_RESET " MCP ");
			cli_presenter_print_safe_inline(ctx, server ? server : "server",
							CLI_PRESENTER_EVENT_TEXT_MAX);
			printf(ctx, ANSI_DIM " · failed" ANSI_RESET "\n");
			if (error && error[0]) {
				printf(ctx, "  ");
				cli_presenter_print_safe_inline(ctx, error,
								CLI_PRESENTER_EVENT_TEXT_MAX);
				printf(ctx, "\n");
			}
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
	printf(ctx, "\n%s%s" ANSI_RESET " %s · ",
	       ev->type == MORPH_EVENT_ERROR || cli_presenter_event_error_code(ev) < 0 ? ANSI_RED
										       : ANSI_DIM,
	       ev->type == MORPH_EVENT_ERROR || cli_presenter_event_error_code(ev) < 0
		       ? CLI_FEED_FAILED
		       : CLI_FEED_SYSTEM,
	       prefix);
	cli_presenter_print_safe_inline(ctx, ev->message ? ev->message : (ev->name ? ev->name : ""),
					CLI_PRESENTER_EVENT_TEXT_MAX);
	printf(ctx, ANSI_RESET "\n");
	cli_presenter_resume_status(ctx, status_paused);
}

int cli_presentation_init(struct cli_application *ctx)
{
	int rc;

	if (!ctx)
		return -EINVAL;
	if (ctx->presenter)
		return -EALREADY;
	ctx->presenter = calloc(1, sizeof(*ctx->presenter));
	if (!ctx->presenter)
		return -ENOMEM;
	rc = morph_buf_init(&ctx->presenter->event_stream, BUFSIZ);
	if (!rc)
		rc = morph_buf_init(&ctx->presenter->agent_receipts, 256);
	if (!rc)
		rc = morph_buf_init(&ctx->presenter->markdown_stream_text, BUFSIZ);
	if (!rc)
		rc = morph_buf_init(&ctx->presenter->native_content, BUFSIZ);
	if (!rc)
		rc = morph_strmap_init(&ctx->presenter->rendered_artifacts, MORPH_STRMAP_INIT_CAP);
	if (!rc)
		rc = morph_strmap_init(&ctx->presenter->announced_artifacts, MORPH_STRMAP_INIT_CAP);
	if (rc) {
		cli_presentation_cleanup(ctx);
		return rc;
	}
	utf8_terminal_sanitizer_init(&ctx->presenter->event_stream_sanitizer,
				     UTF8_TERMINAL_TEXT_MULTILINE);
	utf8_terminal_sanitizer_init(&ctx->presenter->markdown_stream_sanitizer,
				     UTF8_TERMINAL_TEXT_MULTILINE);
	return 0;
}

void cli_presentation_reset(struct cli_application *ctx)
{
	if (!ctx || !ctx->presenter)
		return;
	cli_transcript_reset(ctx);
	morph_buf_reset(&ctx->presenter->native_content);
	ctx->presenter->native_content_started = 0;
	ctx->presenter->native_content_visible = 0;
	morph_buf_reset(&ctx->presenter->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->presenter->event_stream_sanitizer);
	morph_strmap_clear(&ctx->presenter->rendered_artifacts);
	morph_strmap_clear(&ctx->presenter->announced_artifacts);
	cli_markdown_stream_reset(ctx, 1);
	cli_agents_flush_receipts(ctx);
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
	if (ctx && ctx->presenter && ctx->presenter->agent_receipts.len) {
		cli_presenter_print_stream(ctx);
		cli_markdown_stream_reset(ctx, 1);
		cli_agents_flush_receipts(ctx);
	}
}

void cli_presentation_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->presenter)
		return;
	if (cli_turn_details_is_open(ctx))
		cli_turn_details_toggle(ctx);
	cli_subagents_cleanup(ctx);
	cli_jobs_cleanup(ctx);
	cli_turn_details_cleanup(ctx);
	cli_transcript_reset(ctx);
	cli_presenter_clear_status(ctx);
	cli_markdown_stream_reset(ctx, 0);
	morph_buf_cleanup(&ctx->presenter->native_content);
	morph_buf_cleanup(&ctx->presenter->markdown_stream_text);
	morph_buf_cleanup(&ctx->presenter->event_stream);
	morph_buf_cleanup(&ctx->presenter->agent_receipts);
	morph_strmap_cleanup(&ctx->presenter->rendered_artifacts);
	morph_strmap_cleanup(&ctx->presenter->announced_artifacts);
	free(ctx->presenter);
	ctx->presenter = NULL;
}

int cli_presentation_event(struct cli_application *ctx, const struct morph_event *ev)
{
	const char *text;

	if (!ctx || !ev)
		return -EINVAL;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		if (ev->type == MORPH_EVENT_BACKGROUND && ev->name &&
		    strcmp(ev->name, "session.recap") == 0)
			cli_recap_event(ctx, ev);
		char *json = morph_event_json_text_copy(ev);

		if (!json)
			return -ENOMEM;
		int rc = cli_output_write_json(json);

		free(json);
		return rc;
	}
	if (!ctx->presentation_ready)
		return 0;
	int subagent_rc = cli_subagents_event(ctx, ev);
	if (subagent_rc)
		return subagent_rc < 0 ? subagent_rc : 0;
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

				printf(ctx, "\n" ANSI_YELLOW "  Hook%s%s · ", boundary ? " " : "",
				       boundary ? boundary : "");
				cli_presenter_print_safe_inline(
					ctx, ev->message ? ev->message : ev->name,
					CLI_PRESENTER_EVENT_TEXT_MAX);
				const char *error = cli_presenter_event_string(ev, "error");
				if (error && *error) {
					printf(ctx, ": ");
					cli_presenter_print_safe_inline(
						ctx, error, CLI_PRESENTER_EVENT_TEXT_MAX);
				}
				printf(ctx, ANSI_RESET "\n");
				cli_presenter_resume_status(ctx, paused);
			}
		}
		return 0;
	}
	if (ev->type == MORPH_EVENT_BACKGROUND && ev->phase && strcmp(ev->phase, "warning") == 0) {
		int paused = cli_presenter_pause_status(ctx);

		printf(ctx, "\n" ANSI_YELLOW CLI_FEED_SYSTEM " Warning · ");
		cli_presenter_print_safe_inline(ctx, ev->message ? ev->message : ev->name,
						CLI_PRESENTER_EVENT_TEXT_MAX);
		printf(ctx, ANSI_RESET "\n");
		cli_presenter_resume_status(ctx, paused);
		return 0;
	}
	int agents_rc = cli_agents_event(ctx, ev);
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

	if (ev->type == MORPH_EVENT_REACT && ev->name) {
		text = cli_presenter_event_string(ev, "text");
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
			morph_buf_reset(&ctx->presenter->native_content);
			ctx->presenter->native_content_started = 0;
			ctx->presenter->native_content_visible = 0;
			cli_presenter_clear_status(ctx);
			cli_presenter_discard_stream(ctx);
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				const char *display =
					cli_presenter_event_string(ev, "display_text");

				cli_presentation_user_input(ctx, display ? display : text);
			} else {
				printf(ctx, ANSI_DIM "  Requirement applied" ANSI_RESET "\n");
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
				return text ? morph_buf_puts(&ctx->presenter->native_content, text)
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
			cli_agents_flush_receipts(ctx);
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
			if (rc == 0 && ctx->presenter->agent_receipts.len) {
				cli_presenter_print_stream(ctx);
				cli_markdown_stream_reset(ctx, 1);
				cli_agents_flush_receipts(ctx);
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
			cli_agents_flush_receipts(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.end") == 0) {
			cli_presenter_turn_end(ctx, ev);
			cli_agents_flush_receipts(ctx);
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
