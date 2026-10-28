#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"
#include "agent/patch.h"
#include <sys/ioctl.h>

#define CLI_STREAM_NONE      0
#define CLI_STREAM_THOUGHT   1
#define CLI_STREAM_REASONING 2
#define CLI_STREAM_FINAL     3
#define CLI_STREAM_CONTENT   4
#define CLI_EVENT_TEXT_MAX   2000
#define CLI_EVENT_ARGS_MAX   180
#define CLI_TREE_VALUE_MAX   2000
#define CLI_TREE_DEPTH_MAX   6
#define CLI_TREE_ITEMS_MAX   12
#define CLI_TREE_RESULT_DEPTH 2
#define CLI_TREE_EMBEDDED_JSON_DEPTH 1
#define CLI_PATCH_BYTES_MAX 16384
#define CLI_PATCH_LINES_MAX 240

static char *presentation_safe_dup(const char *text, size_t max_bytes,
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

static void presentation_print_safe_inline(const char *text,
					    size_t max_bytes)
{
	char *safe = presentation_safe_dup(text ? text : "", max_bytes,
		UTF8_TERMINAL_TEXT_SINGLE_LINE);

	if (!safe)
		return;
	printf("%s", safe);
	free(safe);
}

/* Keep submitted local commands visible without committing an editable bubble. */
void cli_presentation_command_input(struct cli_context *ctx, const char *text)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE ||
	    !text || text[0] != '/')
		return;
	if (cli_color_enabled())
		printf(ANSI_RESET "\n" CLI_COMMAND_COLOR);
	else
		putchar('\n');
	presentation_print_safe_inline(text, SIZE_MAX);
	if (cli_color_enabled())
		printf(ANSI_RESET);
	printf("\n\n");
}

/* Submitted user messages remain ordinary scrollback, separate from tools. */
static void presentation_user_padding(void)
{
	printf(ANSI_RESET CLI_INPUT_BACKGROUND);
	(void)fwrite("\033[K", 1, 3, stdout);
	printf(ANSI_RESET "\n");
}

static void presentation_user_row(const char *text, size_t length, int first)
{
	int color = cli_color_enabled();

	/* Erase with the active background, without padding scrollback with spaces. */
	if (color) {
		printf(ANSI_RESET CLI_INPUT_BACKGROUND);
		/* Trusted layout control bypasses the text output sanitizer. */
		(void)fwrite("\033[K", 1, 3, stdout);
	}
	if (first) {
		if (color)
			printf(CLI_INPUT_MARKER_COLOR "› " ANSI_RESET CLI_INPUT_BACKGROUND);
		else
			printf("> ");
	} else {
		printf("  ");
	}
	printf("%.*s", (int)length, text);
	if (color)
		printf(ANSI_RESET);
	putchar('\n');
}

void cli_presentation_user_input(const char *text)
{
	char *safe;
	char *line;
	struct winsize size = {0};
	int width = cli_list_columns() - 1;
	int first = 1;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
		width = size.ws_col - 1;

	if (!text || !*text)
		return;
	safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return;
	/* Tabs affect terminal cursor position; normalize only the display copy. */
	for (char *p = safe; *p; p++) {
		if (*p == '\t')
			*p = ' ';
	}
	if (width < 8)
		width = 8;
	putchar('\n');
	if (cli_color_enabled())
		presentation_user_padding();
	line = safe;
	while (line) {
		char *newline = strchr(line, '\n');
		const char *cursor = line;

		if (newline)
			*newline = '\0';
		do {
			const char *end = utf8_advance_display_width(cursor,
				(size_t)(width - 2));

			presentation_user_row(cursor, (size_t)(end - cursor), first);
			first = 0;
			cursor = end;
		} while (*cursor);
		line = newline ? newline + 1 : NULL;
	}
	if (cli_color_enabled())
		presentation_user_padding();
	free(safe);
}

static void presentation_clear_status(struct cli_context *ctx)
{
	cli_terminal_live_clear(ctx);
}

static void presentation_status(struct cli_context *ctx, const char *text)
{
	if (!ctx || !text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("status: ");
		presentation_print_safe_inline(text, CLI_EVENT_TEXT_MAX);
		printf("\n");
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	cli_terminal_live_set(ctx, text);
}

static void presentation_model_wait(struct cli_context *ctx)
{
	presentation_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		presentation_status(ctx, "Thinking…");
}

static int presentation_pause_status(struct cli_context *ctx)
{
	if (ctx && ctx->turn_active && cli_terminal_live_active(ctx)) {
		cli_terminal_history_begin(ctx);
		return 1;
	}
	presentation_clear_status(ctx);
	return 0;
}

static void presentation_resume_status(struct cli_context *ctx, int paused)
{
	if (paused)
		cli_terminal_history_end(ctx);
}

void cli_presentation_prepare_prompt(struct cli_context *ctx)
{
	presentation_clear_status(ctx);
}

static const char *event_string(const struct morph_event *ev,
				const char *name)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int event_error_code(const struct morph_event *ev)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, "error_code");
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static int event_int(const struct morph_event *ev, const char *name)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, name);
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static char *event_args_json(const struct morph_event *ev)
{
	cJSON *args;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	args = cJSON_GetObjectItemCaseSensitive(ev->data, "args");
	if (!args)
		return NULL;
	return cJSON_PrintUnformatted(args);
}

static cJSON *event_item(const struct morph_event *ev, const char *name)
{
	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	return cJSON_GetObjectItemCaseSensitive(ev->data, name);
}

static void print_tree_prefix(const int *ancestors_last, int depth,
			      int is_last)
{
	cli_tree_prefix(ancestors_last, depth, is_last, 0);
}

static void print_tree_continuation_prefix(const int *ancestors_last,
					   int depth, int is_last,
					   size_t label_width)
{
	cli_tree_prefix(ancestors_last, depth, is_last, 1);
	for (size_t i = 0; i < label_width + 2; i++)
		putchar(' ');
}

static const char *tree_wrap_end(const char *text, size_t width,
				 int *has_break)
{
	const char *end;
	const char *space = NULL;

	end = utf8_advance_display_width(text, width);
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

static void print_tree_string(const char *text, const char *label,
			      int depth, int is_last,
			      const int *ancestors_last)
{
	char *display;
	const char *line;
	const char *end;
	size_t label_width;
	int available;
	int has_break;
	int first = 1;

	display = presentation_safe_dup(text ? text : "",
		CLI_TREE_VALUE_MAX, UTF8_TERMINAL_TEXT_SINGLE_LINE);
	if (!display)
		return;
	for (char *p = display; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	}
	label_width = label ? utf8_display_width(label) : 0;
	available = cli_list_columns() - 4 - depth * 2 -
		(int)label_width - 2;
	if (available < 8)
		available = 8;
	line = display;
	while (*line) {
		end = tree_wrap_end(line, (size_t)available, &has_break);
		if (first) {
			print_tree_prefix(ancestors_last, depth, is_last);
			if (label && label[0]) {
				printf(ANSI_DIM);
				presentation_print_safe_inline(label,
					CLI_TREE_VALUE_MAX);
				printf(":" ANSI_RESET " ");
			}
			first = 0;
		} else {
			print_tree_continuation_prefix(
				ancestors_last, depth, is_last, label_width);
		}
		if (*end && !has_break)
			end = utf8_advance_display_width(
				line, (size_t)(available - 1));
		printf("%.*s", (int)(end - line), line);
		if (*end && !has_break) {
			printf("…\n");
			break;
		}
		printf("\n");
		line = end;
		while (*line == ' ')
			line++;
	}
	if (first) {
		print_tree_prefix(ancestors_last, depth, is_last);
		if (label && label[0]) {
			printf(ANSI_DIM);
			presentation_print_safe_inline(label, CLI_TREE_VALUE_MAX);
			printf(":" ANSI_RESET " ");
		}
		printf("\n");
	}
	free(display);
}

static void print_tree_scalar(const cJSON *item)
{
	char *value;

	value = cJSON_PrintUnformatted(item);
	if (value) {
		printf("%s", value);
		free(value);
	}
}

static int json_child_count(const cJSON *item)
{
	return cJSON_IsArray(item) ? cJSON_GetArraySize(item) :
		(cJSON_IsObject(item) ? cJSON_GetArraySize(item) : 0);
}

static void print_tree_container_summary(const cJSON *item)
{
	int count = json_child_count(item);

	printf("%s %d item%s",
	       cJSON_IsArray(item) ? "[...]" : "{...}",
	       count, count == 1 ? "" : "s");
}

static void print_json_tree_node(const cJSON *item, const char *label,
				 int depth, int is_last,
				 int *ancestors_last, int collapse_depth,
				 int embedded_json_depth)
{
	int count;
	int shown;
	int index = 0;
	cJSON *child;
	cJSON *embedded = NULL;

	if (!cJSON_IsArray(item) && !cJSON_IsObject(item)) {
		if (cJSON_IsString(item) &&
		    depth == embedded_json_depth)
			embedded = cJSON_Parse(item->valuestring);
		if (cJSON_IsArray(embedded) || cJSON_IsObject(embedded)) {
			print_json_tree_node(embedded, label, depth, is_last,
					     ancestors_last, collapse_depth, -1);
			cJSON_Delete(embedded);
			return;
		}
		cJSON_Delete(embedded);
		if (cJSON_IsString(item)) {
			print_tree_string(item->valuestring, label, depth,
					  is_last, ancestors_last);
			return;
		}
		print_tree_prefix(ancestors_last, depth, is_last);
		if (label && label[0]) {
			printf(ANSI_DIM);
			presentation_print_safe_inline(label, CLI_TREE_VALUE_MAX);
			printf(":" ANSI_RESET " ");
		}
		print_tree_scalar(item);
		printf("\n");
		return;
	}
	print_tree_prefix(ancestors_last, depth, is_last);
	if (label && label[0]) {
		printf(ANSI_DIM);
		presentation_print_safe_inline(label, CLI_TREE_VALUE_MAX);
		printf(":" ANSI_RESET);
	}
	count = json_child_count(item);
	if (count == 0) {
		printf(" %s\n", cJSON_IsArray(item) ? "[]" : "{}");
		return;
	}
	if (depth >= collapse_depth) {
		printf(" ");
		print_tree_container_summary(item);
		printf("\n");
		return;
	}
	printf("\n");
	ancestors_last[depth] = is_last;
	if (depth + 1 >= CLI_TREE_DEPTH_MAX) {
		print_tree_prefix(ancestors_last, depth + 1, 1);
		printf(ANSI_DIM "… nested data" ANSI_RESET "\n");
		return;
	}
	shown = count < CLI_TREE_ITEMS_MAX ? count : CLI_TREE_ITEMS_MAX;
	cJSON_ArrayForEach(child, item) {
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
		print_json_tree_node(child, child_label, depth + 1,
				     child_last, ancestors_last,
				     collapse_depth, embedded_json_depth);
		index++;
	}
	if (shown < count) {
		print_tree_prefix(ancestors_last, depth + 1, 1);
		printf(ANSI_DIM "… %d more" ANSI_RESET "\n", count - shown);
	}
}

static void print_json_tree_children(const cJSON *item)
{
	int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};
	int count;
	int index = 0;
	cJSON *child;

	if (!cJSON_IsArray(item) && !cJSON_IsObject(item))
		return;
	count = json_child_count(item);
	cJSON_ArrayForEach(child, item) {
		char array_label[32];
		const char *label;
		int is_last;

		if (index >= CLI_TREE_ITEMS_MAX)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(array_label, sizeof(array_label), "[%d]", index);
			label = array_label;
		} else {
			label = child->string ? child->string : "item";
		}
		is_last = index == count - 1 &&
			count <= CLI_TREE_ITEMS_MAX;
		print_json_tree_node(child, label, 0, is_last,
				     ancestors_last, CLI_TREE_DEPTH_MAX, -1);
		index++;
	}
	if (count > CLI_TREE_ITEMS_MAX) {
		print_tree_prefix(ancestors_last, 0, 1);
		printf(ANSI_DIM "… %d more" ANSI_RESET "\n",
		       count - CLI_TREE_ITEMS_MAX);
	}
}

static void print_indented(const char *prefix, const char *content)
{
	char *display;
	const char *line;

	if (!content || !content[0])
		return;
	display = presentation_safe_dup(content, CLI_EVENT_TEXT_MAX,
		UTF8_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	line = display;
	while (line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);

		printf("%s%.*s\n", prefix, (int)len, line);
		line = end ? end + 1 : NULL;
	}
	free(display);
}

static void print_plain_labeled(const char *label, const char *content)
{
	char *display;

	if (!content || !content[0])
		return;
	display = presentation_safe_dup(content, CLI_EVENT_TEXT_MAX,
		UTF8_TERMINAL_TEXT_MULTILINE);
	if (!display)
		return;
	if (strchr(display, '\n'))
		printf("%s:\n%s\n", label, display);
	else
		printf("%s: %s\n", label, display);
	free(display);
}

static void presentation_reasoning_flush(struct cli_context *ctx, int finish);

static void presentation_print_stream(struct cli_context *ctx)
{
	const char *content;
	const char *label;
	int already_streamed;
	int reasoning_streamed;

	if (!ctx || ctx->event_stream_kind == CLI_STREAM_NONE)
		return;
	reasoning_streamed =
		ctx->event_stream_kind == CLI_STREAM_REASONING &&
		ctx->presentation_mode == CLI_PRESENT_INTERACTIVE;
	already_streamed = ctx->markdown_stream_visible &&
		ctx->markdown_stream_kind == ctx->event_stream_kind;
	if (already_streamed)
		cli_markdown_stream_reset(ctx, 1);
	presentation_clear_status(ctx);
	(void)utf8_terminal_sanitize_feed(&ctx->event_stream_sanitizer,
		&ctx->event_stream, NULL, 0, 1);
	content = morph_buf_cstr(&ctx->event_stream);
	if (!content || !content[0])
		goto reset;
	if (reasoning_streamed) {
		presentation_reasoning_flush(ctx, 1);
		goto reset;
	}
	if (already_streamed)
		goto reset;
	label = ctx->event_stream_kind == CLI_STREAM_REASONING ?
		"reasoning" : (ctx->event_stream_kind == CLI_STREAM_CONTENT ?
		"content" : "thought");
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		print_plain_labeled(label, content);
	} else {
		printf("\n" ANSI_BOLD "●" ANSI_RESET " ");
		cli_markdown_render_ansi_with_media_indented(content, 2, media_callback, ctx);
	}
reset:
	if (ctx->event_stream_kind == CLI_STREAM_CONTENT)
		ctx->native_content_visible = 1;
	morph_buf_reset(&ctx->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->event_stream_sanitizer);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
	ctx->reasoning_stream_offset = 0;
}

static void presentation_discard_stream(struct cli_context *ctx)
{
	if (!ctx)
		return;
	cli_markdown_stream_reset(ctx, 1);
	morph_buf_reset(&ctx->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->event_stream_sanitizer);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
	ctx->reasoning_stream_offset = 0;
}

static void presentation_plain_content(struct cli_context *ctx, const char *text)
{
	char *safe;

	if (ctx->presentation_mode != CLI_PRESENT_ONCE_PLAIN)
		return;
	if (!text || !*text)
		text = morph_buf_cstr(&ctx->native_content);
	if (text && *text) {
		safe = presentation_safe_dup(text, SIZE_MAX,
			UTF8_TERMINAL_TEXT_MULTILINE);
		if (safe) {
			if (strchr(safe, '\n'))
				printf("content:\n%s\n", safe);
			else
				printf("content: %s\n", safe);
			free(safe);
		}
	}
	morph_buf_reset(&ctx->native_content);
}

void cli_presentation_flush_stream(struct cli_context *ctx)
{
	presentation_plain_content(ctx, NULL);
	presentation_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
}

static int presentation_append_stream(struct cli_context *ctx, int kind,
				      const char *text, int is_delta,
				      int is_complete,
				      const char **appended_text)
{
	size_t offset;
	int rc;

	if (!ctx || !text)
		return 0;
	if (appended_text)
		*appended_text = "";
	if (ctx->event_stream_kind != CLI_STREAM_NONE &&
	    ctx->event_stream_kind != kind)
		presentation_print_stream(ctx);
	if (ctx->event_stream_kind == CLI_STREAM_NONE) {
		ctx->event_stream_kind = kind;
		utf8_terminal_sanitizer_init(&ctx->event_stream_sanitizer,
			UTF8_TERMINAL_TEXT_MULTILINE);
	}
	if (!is_delta && ctx->event_stream_has_delta) {
		rc = utf8_terminal_sanitize_feed(&ctx->event_stream_sanitizer,
			&ctx->event_stream, NULL, 0, 1);
		if (rc != 0)
			return rc;
		ctx->event_stream_complete = is_complete;
		return 0;
	}
	offset = ctx->event_stream.len;
	rc = utf8_terminal_sanitize_feed(&ctx->event_stream_sanitizer,
		&ctx->event_stream, text, strlen(text), !is_delta);
	if (rc != 0)
		return rc;
	if (appended_text)
		*appended_text = morph_buf_cstr(&ctx->event_stream) + offset;
	if (is_delta)
		ctx->event_stream_has_delta = 1;
	if (is_complete)
		ctx->event_stream_complete = 1;
	return 0;
}

/* The terminal owns the live row; commit its preview only when the stream ends. */
static void presentation_reasoning_flush(struct cli_context *ctx, int finish)
{
	const char *content = morph_buf_cstr(&ctx->event_stream);
	size_t offset = ctx->reasoning_stream_offset;
	const char *start;
	const char *end;
	const char *newline;
	morph_buf_t preview;
	int columns = cli_terminal_columns(ctx);
	size_t width = columns > 4 ? (size_t)(columns - 4) : 1u;
	int rc;

	if (offset < ctx->event_stream.len) {
		rc = cli_transcript_reasoning_append(ctx, content + offset,
			ctx->event_stream.len - offset, !ctx->event_stream_visible);
		if (rc != 0) {
			log_warn("Could not retain reasoning details: %s", morph_strerror(rc));
			return;
		}
		ctx->reasoning_stream_offset = ctx->event_stream.len;
		ctx->event_stream_visible = 1;
	}
	start = content + strspn(content, " \t\r\n");
	if (!*start || morph_buf_init(&preview, 128) != 0)
		return;
	newline = strchr(start, '\n');
	end = utf8_advance_display_width(start, width - 1u);
	if (newline && newline < end)
		end = newline;
	(void)morph_buf_append(&preview, start, (size_t)(end - start));
	if (*end)
		(void)morph_buf_puts(&preview, "…");
	if (finish) {
		presentation_clear_status(ctx);
		/* Commit one quiet narration line between tool blocks. */
		printf("\n" ANSI_DIM "\033[3m%s" ANSI_RESET "\n",
			morph_buf_cstr(&preview));
	} else if (isatty(STDOUT_FILENO)) {
		cli_terminal_reasoning_set(ctx, morph_buf_cstr(&preview));
	}
	morph_buf_cleanup(&preview);
}

static const char *presentation_workdir(struct cli_context *ctx)
{
	if (!ctx)
		return NULL;
	if (ctx->workdir[0])
		return ctx->workdir;
	return runtime_workdir_get(ctx->runtime);
}

static const char *presentation_patch_style(const char *line)
{
	if (strncmp(line, "*** Add File: ", 14) == 0 ||
	    strncmp(line, "*** Update File: ", 17) == 0 ||
	    strncmp(line, "*** Delete File: ", 17) == 0 ||
	    strncmp(line, "*** Move to: ", 13) == 0)
		return ANSI_BOLD ANSI_CYAN;
	if (strncmp(line, "@@", 2) == 0)
		return ANSI_CYAN;
	if (line[0] == '+')
		return ANSI_GREEN;
	if (line[0] == '-')
		return ANSI_RED;
	if (strncmp(line, "*** Begin Patch", 15) == 0 ||
	    strncmp(line, "*** End Patch", 13) == 0)
		return ANSI_DIM;
	return "";
}

static int presentation_patch_range(const char *line, size_t *old_line,
				    size_t *new_line)
{
	size_t old_count;
	size_t new_count;

	return sscanf(line, "@@ -%zu,%zu +%zu,%zu @@",
		      old_line, &old_count, new_line, &new_count) == 4;
}

static void presentation_print_patch_line(const char *line, size_t len,
					  const char *style,
					  int in_hunk,
					  size_t *old_line,
					  size_t *new_line)
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
		printf("  │ " ANSI_DIM "%5s " ANSI_RESET "%s%.*s"
		       ANSI_RESET "\n",
		       number, style, (int)len, line);
		return;
	}
	printf("  │ %s%.*s" ANSI_RESET "\n", style, (int)len, line);
}

void cli_presentation_patch_diff(const char *workdir, const char *input)
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
				text = morph_buf_cstr(&preview);
				preview_ready = 1;
			} else {
				morph_buf_cleanup(&preview);
			}
		}
	}
	input_len = strlen(text);
	display = malloc(CLI_PATCH_BYTES_MAX + 1);
	if (!display) {
		if (preview_ready)
			morph_buf_cleanup(&preview);
		return;
	}
	{
		char *safe = presentation_safe_dup(text, CLI_PATCH_BYTES_MAX,
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
	truncated = input_len > CLI_PATCH_BYTES_MAX;
	for (char *p = display; *p; p++) {
		unsigned char ch = (unsigned char)*p;

		if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f)
			*p = '?';
	}
	line = display;
	while (*line && lines < CLI_PATCH_LINES_MAX) {
		char *end = strchr(line, '\n');
		const char *style = presentation_patch_style(line);
		size_t len = end ? (size_t)(end - line) : strlen(line);

		if (presentation_patch_range(line, &old_line, &new_line))
			in_hunk = 1;
		else if (strncmp(line, "*** ", 4) == 0)
			in_hunk = 0;
		presentation_print_patch_line(line, len, style, in_hunk,
			&old_line, &new_line);
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
		printf("  %s" ANSI_DIM "… patch display truncated" ANSI_RESET
		       "\n", tui_tree_branch(1, 0));
	free(display);
	if (preview_ready)
		morph_buf_cleanup(&preview);
}

static void presentation_tool_call(struct cli_context *ctx,
				   const struct morph_event *ev)
{
	const char *tool = event_string(ev, "tool");
	const char *title = event_string(ev, "toolTitle");
	cJSON *args_item = event_item(ev, "args");
	cJSON *input_item = NULL;
	const char *patch_input = NULL;
	char *args = NULL;
	char *safe_tool;
	char *safe_title;
	char display[512];

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!tool || !tool[0])
		tool = "tool";
	if (!title || !title[0])
		title = tool;
	safe_tool = presentation_safe_dup(tool, TOOL_NAME_MAX,
		UTF8_TERMINAL_TEXT_SINGLE_LINE);
	safe_title = presentation_safe_dup(title, CLI_EVENT_TEXT_MAX,
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
		args = event_args_json(ev);
	display[0] = '\0';
	if (args && strcmp(args, "{}") != 0) {
		utf8_copy_sanitized_display_width(display, sizeof(display),
						  args,
						  CLI_EVENT_ARGS_MAX);
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("tool: %s", tool);
		if (display[0])
			printf(" %s", display);
		printf("\n");
		if (patch_input)
			cli_presentation_patch_diff(presentation_workdir(ctx),
						   patch_input);
	} else {
		printf("\n" ANSI_YELLOW "◦" ANSI_RESET " "
		       ANSI_BOLD "%s" ANSI_RESET "\n", title);
		if (patch_input)
			cli_presentation_patch_diff(presentation_workdir(ctx),
						   patch_input);
		else
			print_json_tree_children(args_item);
		presentation_status(ctx, "Running tool…");
	}
	free(args);
	free(safe_tool);
	free(safe_title);
}

static void presentation_tool_end(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *title = event_string(ev, "toolTitle");
	const char *tool = event_string(ev, "tool");
	const char *error = event_string(ev, "error");
	int failed;

	presentation_clear_status(ctx);
	if (!title || !title[0])
		title = tool && tool[0] ? tool : "Tool";
	failed = !ev->name || strcmp(ev->name, "tool.result") != 0;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("status: ");
		presentation_print_safe_inline(tool ? tool : "tool",
			TOOL_NAME_MAX);
		printf(" %s\n", failed ? "failed" : "completed");
		return;
	}
	if (failed) {
		printf("  " ANSI_RED CLI_FEED_FAILED " ");
		presentation_print_safe_inline(title, CLI_EVENT_TEXT_MAX);
		printf(" failed" ANSI_RESET);
		if (error && error[0]) {
			printf(ANSI_DIM " · ");
			presentation_print_safe_inline(error, CLI_EVENT_TEXT_MAX);
			printf(ANSI_RESET);
		}
		printf("\n");
	} else {
		printf("  " ANSI_GREEN CLI_FEED_DONE " ");
		presentation_print_safe_inline(title, CLI_EVENT_TEXT_MAX);
		printf(" completed" ANSI_RESET "\n");
	}
}

static const char *plan_mark(const char *status, int active)
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

static int presentation_plan(const struct morph_event *ev)
{
	cJSON *result;
	cJSON *plans;
	cJSON *plan;
	int printed = 0;

	if (!ev || !cJSON_IsObject(ev->data) ||
	    !event_string(ev, "tool") ||
	    strcmp(event_string(ev, "tool"), "plan") != 0)
		return 0;
	result = cJSON_GetObjectItemCaseSensitive(ev->data, "data");
	plans = cJSON_IsObject(result) ?
		cJSON_GetObjectItemCaseSensitive(result, "plans") : NULL;
	if (!cJSON_IsArray(plans))
		return 0;
	cJSON_ArrayForEach(plan, plans) {
		cJSON *steps;
		cJSON *step;
		int completed = 0;
		int count;

		if (!cJSON_IsObject(plan))
			continue;
		steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
		if (!cJSON_IsArray(steps))
			continue;
		count = cJSON_GetArraySize(steps);
		cJSON_ArrayForEach(step, steps) {
			const cJSON *status = cJSON_GetObjectItemCaseSensitive(step, "status");

			if (cJSON_IsString(status) && strcmp(status->valuestring, "completed") == 0)
				completed++;
		}
		printf("  " ANSI_DIM "Plan · %d/%d completed" ANSI_RESET "\n",
			completed, count);
		printed = 1;
		cJSON_ArrayForEach(step, steps) {
			cJSON *description;
			cJSON *status;
			cJSON *active;
			const char *mark;
			const char *color;

			if (!cJSON_IsObject(step))
				continue;
			description = cJSON_GetObjectItemCaseSensitive(
				step, "description");
			status = cJSON_GetObjectItemCaseSensitive(step, "status");
			active = cJSON_GetObjectItemCaseSensitive(step, "active");
			if (!cJSON_IsString(description))
				continue;
			mark = plan_mark(cJSON_IsString(status) ? status->valuestring : NULL,
				cJSON_IsTrue(active));
			color = strcmp(mark, CLI_FEED_DONE) == 0 ? ANSI_GREEN :
				strcmp(mark, CLI_FEED_FAILED) == 0 ? ANSI_RED :
				strcmp(mark, CLI_FEED_RUNNING) == 0 ? ANSI_YELLOW : ANSI_DIM;
			printf("  %s%s" ANSI_RESET " ", color, mark);
			presentation_print_safe_inline(description->valuestring,
				CLI_EVENT_TEXT_MAX);
			printf("\n");
		}
	}
	return printed;
}

static void presentation_observation(struct cli_context *ctx,
				     const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");
	cJSON *structured = event_item(ev, "data");
	cJSON *parsed = NULL;

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		print_plain_labeled("observation", text);
	} else if (presentation_plan(ev)) {
		return;
	} else if (event_error_code(ev) < 0) {
		morph_buf_t prefix = {0};
		if (morph_buf_init(&prefix, 64) == 0) {
			tui_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
			morph_buf_puts(&prefix, ANSI_RED "Error: " ANSI_RESET);
			print_indented(morph_buf_cstr(&prefix), text);
			morph_buf_cleanup(&prefix);
		}
	} else {
		if (!cJSON_IsArray(structured) &&
		    !cJSON_IsObject(structured))
			parsed = cJSON_Parse(text);
		if (cJSON_IsArray(structured) ||
		    cJSON_IsObject(structured)) {
			int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};

			print_json_tree_node(structured, "result", 0, 1,
					     ancestors_last,
					     CLI_TREE_RESULT_DEPTH,
					     CLI_TREE_EMBEDDED_JSON_DEPTH);
		} else if (parsed) {
			int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};

			print_json_tree_node(parsed, "result", 0, 1,
					     ancestors_last,
					     CLI_TREE_RESULT_DEPTH,
					     CLI_TREE_EMBEDDED_JSON_DEPTH);
		} else {
			morph_buf_t prefix = {0};
			if (morph_buf_init(&prefix, 32) == 0) {
				tui_tree_prefix(&prefix, NULL, 0, 1, 0, 2);
				print_indented(morph_buf_cstr(&prefix), text);
				morph_buf_cleanup(&prefix);
			}
		}
		cJSON_Delete(parsed);
	}
}

static void presentation_reflection(struct cli_context *ctx,
				    const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		print_plain_labeled("guardrail", text);
	else if (text && text[0])
		print_indented(ANSI_YELLOW CLI_FEED_SYSTEM " Guardrail · " ANSI_RESET, text);
}

static void presentation_token_label(int tokens, char *label, size_t size)
{
	if (tokens >= 1000000)
		snprintf(label, size, "%.1fm", (double)tokens / 1000000.0);
	else if (tokens >= 1000)
		snprintf(label, size, "%.0fk", (double)tokens / 1000.0);
	else
		snprintf(label, size, "%d", tokens);
}

static void presentation_compaction(struct cli_context *ctx,
				    const struct morph_event *ev)
{
	int before = event_int(ev, "before_tokens");
	int after = event_int(ev, "after_tokens");
	const char *error = event_string(ev, "error");
	int failed = ev->name && strcmp(ev->name,
		"react.compaction.failed") == 0;
	int64_t elapsed = cli_terminal_activity_elapsed_ms(ctx);

	if (ev->name && strcmp(ev->name, "react.compaction.begin") == 0) {
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
			presentation_status(ctx, "Compacting context…");
		} else if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
			presentation_clear_status(ctx);
			presentation_print_stream(ctx);
			printf("\n");
			cli_terminal_activity_set(ctx, "Compacting context…");
		}
		return;
	}
	presentation_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		if (failed) {
			printf("context: compression failed: ");
			presentation_print_safe_inline(error ? error : "unknown error",
				CLI_EVENT_TEXT_MAX);
			printf("\n");
		} else {
			printf("context: compacted %d -> %d tokens\n", before, after);
		}
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (elapsed < 0)
		printf("\n");
	if (failed) {
		printf(ANSI_DIM ANSI_RED "◇ Context compression failed · ");
		presentation_print_safe_inline(error ? error : "unknown error",
			CLI_EVENT_TEXT_MAX);
	} else {
		char before_label[32];
		char after_label[32];
		char line[BUFSIZ];
		char clipped[BUFSIZ];
		int columns = cli_terminal_columns(ctx) - 1;

		presentation_token_label(before, before_label, sizeof(before_label));
		presentation_token_label(after, after_label, sizeof(after_label));
		if (elapsed >= 0)
			snprintf(line, sizeof(line), "◇ Context compacted  %s → %s · %llds",
				before_label, after_label, (long long)(elapsed / 1000));
		else
			snprintf(line, sizeof(line), "◇ Context compacted  %s → %s",
				before_label, after_label);
		(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), line,
			(size_t)(columns > 0 ? columns : 1), 0);
		printf(ANSI_DIM "%s", clipped);
	}
	if (failed && elapsed >= 0)
		printf(" · %llds", (long long)(elapsed / 1000));
	printf(ANSI_RESET "\n\n");
	fflush(stdout);
	if (!failed && ctx->turn_active)
		presentation_model_wait(ctx);
}

static void presentation_final(struct cli_context *ctx,
			       const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");
	int had_stream = ctx->markdown_stream_visible &&
		(ctx->markdown_stream_kind == CLI_STREAM_THOUGHT ||
		 ctx->markdown_stream_kind == CLI_STREAM_CONTENT ||
		 ctx->markdown_stream_kind == CLI_STREAM_FINAL);
	had_stream = had_stream || ctx->native_content_visible;
	morph_buf_reset(&ctx->native_content);
	ctx->native_content_started = 0;
	ctx->native_content_visible = 0;

	presentation_clear_status(ctx);
	if ((ctx->event_stream_kind == CLI_STREAM_THOUGHT ||
	     ctx->event_stream_kind == CLI_STREAM_CONTENT) &&
	    !ctx->event_stream_complete)
		presentation_discard_stream(ctx);
	else
		presentation_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("final:\n");
		if (text && text[0]) {
			char *safe = presentation_safe_dup(text, SIZE_MAX,
				UTF8_TERMINAL_TEXT_MULTILINE);

			if (safe) {
				printf("%s\n", safe);
				free(safe);
			}
		}
	} else {
		if (!had_stream)
			printf("\n" ANSI_BOLD "●" ANSI_RESET " ");
		if (!had_stream && text && text[0]) {
			cli_markdown_render_final(ctx, text);
		} else if (!had_stream) {
			printf("\n");
			ctx->markdown_output.trailing_newlines = 1;
		}
		cli_markdown_finish_final(ctx);
	}
	ctx->final_rendered = 1;
}

static void presentation_turn_end(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *error;
	const char *detail;
	const char *outcome;

	if (!ev->phase || strcmp(ev->phase, "end") == 0)
		return;
	if (ctx->final_rendered)
		return;
	presentation_plain_content(ctx, NULL);
	cli_markdown_stream_reset(ctx, 1);
	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	error = event_string(ev, "error");
	detail = event_string(ev, "detail");
	outcome = event_string(ev, "outcome");
	if ((outcome && strcmp(outcome, "cancelled") == 0) ||
	    strcmp(ev->phase, "cancelled") == 0) {
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
			printf("cancelled\n");
		else
			printf("\n" ANSI_DIM CLI_FEED_SYSTEM " Cancelled" ANSI_RESET "\n");
		ctx->final_rendered = 1;
		return;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("error: ");
		presentation_print_safe_inline(error ? error :
			(outcome ? outcome : "turn failed"), CLI_EVENT_TEXT_MAX);
		if (detail && detail[0]) {
			printf(" (");
			presentation_print_safe_inline(detail, CLI_EVENT_TEXT_MAX);
			printf(")");
		}
		printf("\n");
	} else {
		printf("\n" ANSI_BOLD ANSI_RED CLI_FEED_FAILED " Error" ANSI_RESET " ");
		presentation_print_safe_inline(error ? error :
			(outcome ? outcome : "turn failed"), CLI_EVENT_TEXT_MAX);
		if (detail && detail[0]) {
			printf(ANSI_DIM " (");
			presentation_print_safe_inline(detail, CLI_EVENT_TEXT_MAX);
			printf(")" ANSI_RESET);
		}
		printf("\n");
	}
	ctx->final_rendered = 1;
}

static void presentation_auth(struct cli_context *ctx,
			      const struct morph_event *ev)
{
	const char *backend = event_string(ev, "backend");
	const char *tool = event_string(ev, "tool");
	const char *env_name = event_string(ev, "env_name");
	const struct config *config;

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!backend)
		backend = "configured";
	config = ctx->runtime ? runtime_config_get(ctx->runtime) : NULL;
	if ((!env_name || !env_name[0]) && config) {
		if (strcmp(backend, "image") == 0)
			env_name = config->models.image.api_key_env;
		else if (strcmp(backend, "video") == 0)
			env_name = config->models.video.api_key_env;
		else
			env_name = config->models.text.api_key_env;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("error: authentication required for ");
		presentation_print_safe_inline(backend, CLI_EVENT_TEXT_MAX);
		if (tool && tool[0]) {
			printf(" tool ");
			presentation_print_safe_inline(tool, TOOL_NAME_MAX);
		}
		if (env_name && env_name[0]) {
			printf("; export ");
			presentation_print_safe_inline(env_name, CLI_EVENT_TEXT_MAX);
		}
		printf("\n");
	} else {
		printf("\n" ANSI_BOLD ANSI_YELLOW
		       CLI_FEED_QUESTION " Authentication required" ANSI_RESET " for ");
		presentation_print_safe_inline(backend, CLI_EVENT_TEXT_MAX);
		if (tool && tool[0]) {
			printf(" tool " ANSI_BOLD);
			presentation_print_safe_inline(tool, TOOL_NAME_MAX);
			printf(ANSI_RESET);
		}
		if (env_name && env_name[0]) {
			printf(ANSI_DIM " · export ");
			presentation_print_safe_inline(env_name, CLI_EVENT_TEXT_MAX);
			printf(ANSI_RESET);
		}
		printf("\n");
	}
}

static void presentation_artifact(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *path = event_string(ev, "path");
	const char *kind = event_string(ev, "kind");
	int status_paused;

	if (!ev->name || strcmp(ev->name, "artifact.ready") != 0 ||
	    !path || !path[0])
		return;
	if (morph_strmap_contains(&ctx->announced_artifacts, path))
		return;
	status_paused = presentation_pause_status(ctx);
	(void)morph_strmap_set(&ctx->announced_artifacts, path, (void *)1);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("artifact: ");
		presentation_print_safe_inline(kind ? kind : "file",
			CLI_EVENT_TEXT_MAX);
		printf(" ");
		presentation_print_safe_inline(path, CLI_EVENT_TEXT_MAX);
		printf("\n");
		presentation_resume_status(ctx, status_paused);
		return;
	}
	cli_tree_prefix(NULL, 0, 1, 0);
	printf(ANSI_DIM);
	presentation_print_safe_inline(kind ? kind : "artifact",
		CLI_EVENT_TEXT_MAX);
	printf(": ");
	presentation_print_safe_inline(path, CLI_EVENT_TEXT_MAX);
	printf(ANSI_RESET "\n");
	presentation_resume_status(ctx, status_paused);
}

static void presentation_mcp_tree_start(struct cli_context *ctx,
					const char *server)
{
	printf("\n" ANSI_DIM CLI_FEED_SYSTEM ANSI_RESET " "
	       ANSI_BOLD "MCP ");
	presentation_print_safe_inline(server, CLI_EVENT_TEXT_MAX);
	printf(ANSI_RESET "\n");
	strncpy(ctx->mcp_tree_server, server,
		sizeof(ctx->mcp_tree_server) - 1);
	ctx->mcp_tree_server[sizeof(ctx->mcp_tree_server) - 1] = '\0';
	ctx->mcp_tree_active = 1;
}

static void presentation_mcp_ready(const struct morph_event *ev)
{
	cJSON *tools = event_item(ev, "tools");
	cJSON *resources = event_item(ev, "resources");
	cJSON *prompts = event_item(ev, "prompts");

	printf(ANSI_GREEN CLI_FEED_DONE " Ready" ANSI_RESET);
	if (cJSON_IsNumber(tools) && cJSON_IsNumber(resources) &&
	    cJSON_IsNumber(prompts)) {
		printf(ANSI_DIM " · %d tools, %d resources, %d prompts"
		       ANSI_RESET, tools->valueint, resources->valueint,
		       prompts->valueint);
	}
}

static void presentation_mcp(struct cli_context *ctx,
			     const struct morph_event *ev)
{
	const char *server = event_string(ev, "server");
	const char *error = event_string(ev, "error");
	const char *label;
	int is_last;
	int failed;
	int status_paused;

	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (!server || !server[0])
		server = "server";
	status_paused = presentation_pause_status(ctx);
	if (!ctx->mcp_tree_active ||
	    strcmp(ctx->mcp_tree_server, server) != 0)
		presentation_mcp_tree_start(ctx, server);
	failed = ev->name && strcmp(ev->name, "mcp.failed") == 0;
	is_last = failed ||
		(ev->name && (strcmp(ev->name, "mcp.ready") == 0 ||
			     strcmp(ev->name, "mcp.disconnected") == 0));
	cli_tree_prefix(NULL, 0, is_last, 0);
	if (failed) {
		printf(ANSI_RED CLI_FEED_FAILED " Failed" ANSI_RESET);
		if (error && error[0]) {
			printf(ANSI_DIM " · ");
			presentation_print_safe_inline(error, CLI_EVENT_TEXT_MAX);
			printf(ANSI_RESET);
		}
	} else if (ev->name && strcmp(ev->name, "mcp.ready") == 0) {
		presentation_mcp_ready(ev);
	} else {
		if (ev->name && strcmp(ev->name, "mcp.connecting") == 0)
			label = "Connecting";
		else if (ev->name &&
			 strcmp(ev->name, "mcp.connected") == 0)
			label = "Connected";
		else if (ev->name &&
			 strcmp(ev->name, "mcp.discovering") == 0)
			label = "Discovering capabilities";
		else if (ev->name &&
			 strcmp(ev->name, "mcp.disconnected") == 0)
			label = "Disconnected";
		else
			label = ev->message ? ev->message : "Updated";
		presentation_print_safe_inline(label, CLI_EVENT_TEXT_MAX);
	}
	printf("\n");
	if (is_last) {
		ctx->mcp_tree_active = 0;
		ctx->mcp_tree_server[0] = '\0';
	}
	presentation_resume_status(ctx, status_paused);
}

static void presentation_auxiliary(struct cli_context *ctx,
				   const struct morph_event *ev)
{
	const char *prefix;
	int status_paused;

	if (!ctx->presentation_ready ||
	    ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (ev->type == MORPH_EVENT_TASK && ev->name) {
		if (strcmp(ev->name, "task.notification") == 0 ||
		    (ctx->input_status.enabled &&
		     (strcmp(ev->name, "task.started") == 0 ||
		      strcmp(ev->name, "task.claimed") == 0 ||
		      strcmp(ev->name, "task.rescheduled") == 0 ||
		      cJSON_IsNumber(event_item(ev, "notification_id")))))
			return;
	}
	/* Foreground task mutations are already reported by the tool result. */
	if (ev->type == MORPH_EVENT_TASK && ctx->turn_active && ev->name &&
	    event_error_code(ev) >= 0 &&
	    (strcmp(ev->name, "task.created") == 0 ||
	     strcmp(ev->name, "task.updated") == 0 ||
	     strcmp(ev->name, "task.cancelled") == 0))
		return;
	if (ev->type == MORPH_EVENT_BACKGROUND) {
		if ((ev->name && strstr(ev->name, "failed")) ||
		    event_error_code(ev) < 0) {
			status_paused = presentation_pause_status(ctx);
			printf("\n" ANSI_RED CLI_FEED_FAILED " Background · ");
			presentation_print_safe_inline(ev->message ? ev->message :
				(ev->name ? ev->name : "failed"),
				CLI_EVENT_TEXT_MAX);
			printf(ANSI_RESET "\n");
			presentation_resume_status(ctx, status_paused);
		} else if (ev->phase &&
			   (strcmp(ev->phase, "begin") == 0 ||
			    strcmp(ev->phase, "progress") == 0)) {
			if (!ctx->turn_active)
				presentation_status(ctx, ev->message ?
						    ev->message : "Working…");
		} else if (!ctx->turn_active) {
			presentation_clear_status(ctx);
		}
		return;
	}
	if (ev->type == MORPH_EVENT_MCP) {
		if (!cJSON_IsTrue(event_item(ev, "background")) ||
		    !ctx->input_status.enabled) {
			presentation_mcp(ctx, ev);
		} else if (ev->name && strcmp(ev->name, "mcp.failed") == 0) {
			const char *server = event_string(ev, "server");
			const char *error = event_string(ev, "error");

			status_paused = presentation_pause_status(ctx);
			printf("\n" ANSI_RED CLI_FEED_FAILED ANSI_RESET " MCP ");
			presentation_print_safe_inline(server ? server : "server",
				CLI_EVENT_TEXT_MAX);
			printf(ANSI_DIM " · failed" ANSI_RESET "\n");
			if (error && error[0]) {
				printf("  ");
				presentation_print_safe_inline(error, CLI_EVENT_TEXT_MAX);
				printf("\n");
			}
			presentation_resume_status(ctx, status_paused);
		}
		return;
	}
	if (ev->type == MORPH_EVENT_TASK)
		prefix = "Task";
	else if (ev->type == MORPH_EVENT_ERROR)
		prefix = "Error";
	else
		return;
	status_paused = presentation_pause_status(ctx);
	printf("\n%s%s" ANSI_RESET " %s · ",
		ev->type == MORPH_EVENT_ERROR || event_error_code(ev) < 0 ?
		ANSI_RED : ANSI_DIM,
		ev->type == MORPH_EVENT_ERROR || event_error_code(ev) < 0 ?
		CLI_FEED_FAILED : CLI_FEED_SYSTEM, prefix);
	presentation_print_safe_inline(ev->message ? ev->message :
		(ev->name ? ev->name : ""), CLI_EVENT_TEXT_MAX);
	printf(ANSI_RESET "\n");
	presentation_resume_status(ctx, status_paused);
}

int cli_presentation_init(struct cli_context *ctx)
{
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(&ctx->event_stream, BUFSIZ);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&ctx->markdown_stream_text, BUFSIZ);
	if (rc != 0) {
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	utf8_terminal_sanitizer_init(&ctx->event_stream_sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	utf8_terminal_sanitizer_init(&ctx->markdown_stream_sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	rc = morph_strmap_init(&ctx->rendered_artifacts,
			       MORPH_STRMAP_INIT_CAP);
	if (rc != 0) {
		morph_buf_cleanup(&ctx->markdown_stream_text);
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	rc = morph_strmap_init(&ctx->announced_artifacts,
			       MORPH_STRMAP_INIT_CAP);
	if (rc != 0) {
		morph_strmap_cleanup(&ctx->rendered_artifacts);
		morph_buf_cleanup(&ctx->markdown_stream_text);
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	rc = morph_buf_init(&ctx->native_content, BUFSIZ);
	if (rc != 0) {
		morph_strmap_cleanup(&ctx->announced_artifacts);
		morph_strmap_cleanup(&ctx->rendered_artifacts);
		morph_buf_cleanup(&ctx->markdown_stream_text);
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	return 0;
}

void cli_presentation_reset(struct cli_context *ctx)
{
	if (!ctx)
		return;
	cli_transcript_reset(ctx);
	morph_buf_reset(&ctx->native_content);
	ctx->native_content_started = 0;
	ctx->native_content_visible = 0;
	morph_buf_reset(&ctx->event_stream);
	utf8_terminal_sanitizer_reset(&ctx->event_stream_sanitizer);
	morph_strmap_clear(&ctx->rendered_artifacts);
	morph_strmap_clear(&ctx->announced_artifacts);
	cli_markdown_stream_reset(ctx, 1);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
	ctx->reasoning_stream_offset = 0;
	ctx->final_rendered = 0;
	cli_terminal_live_clear(ctx);
	ctx->mcp_tree_active = 0;
	ctx->mcp_tree_server[0] = '\0';
}

void cli_presentation_finish(struct cli_context *ctx)
{
	presentation_clear_status(ctx);
	cli_transcript_finish(ctx);
}

void cli_presentation_cleanup(struct cli_context *ctx)
{
	if (!ctx)
		return;
	if (ctx->details_open)
		cli_transcript_toggle(ctx);
	cli_transcript_reset(ctx);
	presentation_clear_status(ctx);
	cli_markdown_stream_reset(ctx, 0);
	morph_buf_cleanup(&ctx->native_content);
	morph_buf_cleanup(&ctx->markdown_stream_text);
	morph_buf_cleanup(&ctx->event_stream);
	morph_strmap_cleanup(&ctx->rendered_artifacts);
	morph_strmap_cleanup(&ctx->announced_artifacts);
}

int cli_presentation_event(struct cli_context *ctx,
			   const struct morph_event *ev)
{
	const char *text;

	if (!ctx || !ev)
		MORPH_RETURN(-EINVAL);
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		if (ev->type == MORPH_EVENT_BACKGROUND && ev->name &&
		    strcmp(ev->name, "session.recap") == 0)
			cli_recap_event(ctx, ev);
		char *json = morph_event_to_json_string(ev);

		if (!json)
			MORPH_RETURN(-ENOMEM);
		int rc = cli_write_ndjson(json);

		free(json);
		return rc;
	}
	if (!ctx->presentation_ready)
		return 0;
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
	if (!ctx->turn_active &&
	    (ev->type == MORPH_EVENT_REACT ||
	     ev->type == MORPH_EVENT_TOOL ||
	     ev->type == MORPH_EVENT_HITL ||
	     ev->type == MORPH_EVENT_ARTIFACT))
		return 0;
	if (ctx->turn_active) {
		int handled = cli_transcript_event(ctx, ev);

		if (handled != 0)
			return handled < 0 ? handled : 0;
	}

	if (ev->type == MORPH_EVENT_REACT && ev->name) {
		text = event_string(ev, "text");
		if (strncmp(ev->name, "react.compaction.",
		    sizeof("react.compaction.") - 1) == 0) {
			presentation_compaction(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.begin") == 0) {
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				cli_terminal_render_frame(ctx, 1);
			return 0;
		}
		if (strcmp(ev->name, "react.user.steer") == 0) {
			morph_buf_reset(&ctx->native_content);
			ctx->native_content_started = 0;
			ctx->native_content_visible = 0;
			presentation_clear_status(ctx);
			presentation_discard_stream(ctx);
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				const char *display = event_string(ev, "display_text");

				cli_presentation_user_input(display ? display : text);
			} else {
				printf(ANSI_DIM "  Requirement applied" ANSI_RESET "\n");
			}
			presentation_model_wait(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.thinking") == 0) {
			presentation_model_wait(ctx);
			return 0;
		}
		if (strcmp(ev->name, "react.content.delta") == 0) {
			ctx->native_content_started = 1;
			if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
				return text ? morph_buf_puts(&ctx->native_content, text) : 0;
			int rc = presentation_append_stream(
				ctx, CLI_STREAM_CONTENT, text, 1, 0, NULL);
			if (rc == 0) {
				presentation_clear_status(ctx);
				rc = cli_markdown_stream_append(ctx, text, CLI_STREAM_CONTENT);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.content.end") == 0) {
			if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
				presentation_plain_content(ctx, text);
			} else {
				if (!ctx->native_content_started ||
				    ctx->event_stream_kind == CLI_STREAM_CONTENT) {
					int rc = presentation_append_stream(ctx,
						CLI_STREAM_CONTENT, text, 0, 1, NULL);
					if (rc != 0)
						return rc;
				}
				presentation_print_stream(ctx);
			}
			ctx->native_content_started = 0;
			ctx->native_content_visible = 0;
			return 0;
		}
		if (strcmp(ev->name, "react.thought.delta") == 0) {
			int rc = presentation_append_stream(
				ctx, CLI_STREAM_THOUGHT, text, 1, 0, NULL);

			if (rc == 0 &&
			    ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				presentation_clear_status(ctx);
				rc = cli_markdown_stream_append(
					ctx, text, CLI_STREAM_THOUGHT);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.thought.end") == 0)
			return presentation_append_stream(
				ctx, CLI_STREAM_THOUGHT, text, 0, 1, NULL);
		if (strcmp(ev->name, "react.reasoning.delta") == 0) {
			int rc = presentation_append_stream(
				ctx, CLI_STREAM_REASONING, text, 1, 0, NULL);

			if (rc == 0 &&
			    ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				presentation_reasoning_flush(ctx, 0);
			return rc;
		}
		if (strcmp(ev->name, "react.final.delta") == 0) {
			if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
				return 0;
			presentation_clear_status(ctx);
			if (ctx->event_stream_kind != CLI_STREAM_NONE)
				presentation_print_stream(ctx);
			return cli_markdown_stream_append(
				ctx, text, CLI_STREAM_FINAL);
		}
		if (strcmp(ev->name, "react.observation") == 0) {
			presentation_observation(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.reflection") == 0) {
			presentation_reflection(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.final") == 0) {
			presentation_final(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.end") == 0) {
			presentation_turn_end(ctx, ev);
			return 0;
		}
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    strcmp(ev->name, "tool.call") == 0) {
		presentation_tool_call(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    strcmp(ev->name, "tool.running") == 0) {
		presentation_status(ctx, "Running tool…");
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    (strcmp(ev->name, "tool.result") == 0 ||
	     strcmp(ev->name, "tool.failed") == 0 ||
	     strcmp(ev->name, "tool.cancelled") == 0)) {
		presentation_tool_end(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_HITL && ev->name &&
	    strcmp(ev->name, "auth.required") == 0) {
		presentation_auth(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_ARTIFACT) {
		presentation_artifact(ctx, ev);
		return 0;
	}
	presentation_auxiliary(ctx, ev);
	fflush(stdout);
	return 0;
}
