#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"
#include "sapi/cli/shell_style.h"
#include "agent/patch.h"
#include <sys/ioctl.h>

struct transcript_tool {
	const char *id;
	const char *name;
	const char *args;
	const char *subject;
	const char *result;
	const char *error;
	const char *summary;
	const char *patch_preview;
	morph_buf_t stream;
	struct utf8_terminal_sanitizer sanitizer;
	int state;
	int timed_out;
	double started;
	double ended;
};

struct transcript_reasoning {
	size_t after_tools;
	morph_buf_t text;
};

struct cli_transcript {
	struct arena *arena;
	morph_array_t tools;
	morph_array_t reasoning;
	int finished;
	int allocation_failed;
	int view_follow;
	size_t view_offset;
	morph_buf_t deferred;
	morph_buf_t previous_frame;
	int frame_rows;
	int frame_columns;
};

static double transcript_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int transcript_columns(void)
{
	struct winsize size = {0};
	const char *configured = getenv("COLUMNS");

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
		return size.ws_col;
	if (configured && configured[0])
		return cli_terminal_columns(NULL);
	return cli_list_columns();
}

static const char *transcript_workdir(struct cli_context *ctx)
{
	if (!ctx)
		return NULL;
	if (ctx->workdir[0])
		return ctx->workdir;
	return runtime_workdir_get(ctx->runtime);
}

static const char *json_string(const cJSON *data, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

static const char *save_text(struct cli_transcript *tr, const char *text)
{
	char *copy = arena_strdup(tr->arena, text ? text : "");

	if (!copy)
		tr->allocation_failed = 1;
	return copy ? copy : "";
}

static int transcript_init(struct cli_context *ctx)
{
	struct cli_transcript *tr = calloc(1, sizeof(*tr));
	struct transcript_tool tool;
	struct transcript_reasoning reasoning;

	if (!tr)
		MORPH_RETURN(-ENOMEM);
	tr->arena = arena_create(BUFSIZ);
	if (!tr->arena) {
		free(tr);
		MORPH_RETURN(-ENOMEM);
	}
	/* Arena-backed arrays have fixed capacity; event/tool counts are unbounded. */
	if (morph_array_init(&tr->tools,
		MORPH_ARRAY_INIT_CAP, sizeof(tool)) != 0) {
		morph_array_cleanup(&tr->tools);
		arena_destroy(tr->arena);
		free(tr);
		MORPH_RETURN(-ENOMEM);
	}
	ctx->transcript = tr;
	if (morph_array_init(&tr->reasoning, MORPH_ARRAY_INIT_CAP,
			     sizeof(reasoning)) != 0) {
		cli_transcript_reset(ctx);
		MORPH_RETURN(-ENOMEM);
	}
	if (morph_buf_init(&tr->deferred, BUFSIZ) != 0 ||
	    morph_buf_init(&tr->previous_frame, BUFSIZ) != 0) {
		cli_transcript_reset(ctx);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

void cli_transcript_reset(struct cli_context *ctx)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_reasoning *reasoning;

	if (!tr)
		return;

	morph_array_foreach(reasoning, &tr->reasoning, struct transcript_reasoning)
		morph_buf_cleanup(&reasoning->text);
	morph_array_cleanup(&tr->reasoning);
	morph_array_cleanup(&tr->tools);
	morph_buf_cleanup(&tr->deferred);
	morph_buf_cleanup(&tr->previous_frame);
	arena_destroy(tr->arena);
	free(tr);
	ctx->transcript = NULL;
}

/* Reasoning stays separate from tools, with its original position in the turn. */
int cli_transcript_reasoning_append(struct cli_context *ctx, const char *text,
				    size_t len, int begin)
{
	struct cli_transcript *tr;
	struct transcript_reasoning *reasoning;
	int rc;

	if (!ctx->transcript && transcript_init(ctx) != 0)
		MORPH_RETURN(-ENOMEM);
	tr = ctx->transcript;
	if (begin || !tr->reasoning.nelts) {
		reasoning = morph_array_push(&tr->reasoning);
		if (!reasoning)
			MORPH_RETURN(-ENOMEM);
		memset(reasoning, 0, sizeof(*reasoning));
		reasoning->after_tools = tr->tools.nelts;
		rc = morph_buf_init(&reasoning->text, BUFSIZ);
		if (rc != 0) {
			(void)morph_array_pop(&tr->reasoning);
			return rc;
		}
	} else {
		reasoning = morph_array_get(&tr->reasoning, tr->reasoning.nelts - 1);
	}
	return morph_buf_append(&reasoning->text, text, len);
}

int cli_transcript_capture_begin(struct cli_context *ctx)
{
	if (!ctx->transcript)
		MORPH_RETURN(-EINVAL);
	return cli_command_capture_styled_begin(&ctx->transcript->deferred);
}


static struct transcript_tool *find_tool(struct cli_transcript *tr,
					 const cJSON *data)
{
	const char *id = json_string(data, "tool_call_id");
	const char *name = json_string(data, "tool");

	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct transcript_tool *tool = morph_array_get(&tr->tools, i - 1);

		if (id[0] ? strcmp(id, tool->id) == 0 :
		    (!name[0] || strcmp(name, tool->name) == 0))
			return tool;
	}
	return NULL;
}

static void append_clipped_text(morph_buf_t *buf, const char *text, int width)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe || width < 1) {
		free(safe);
		return;
	}
	end = utf8_advance_display_width(safe, (size_t)width);
	if (*end) {
		end = utf8_advance_display_width(safe, (size_t)(width - 1));
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
		(void)morph_buf_puts(buf, "…");
	} else {
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
	}
	free(safe);
}

static int shell_tool(const struct transcript_tool *tool)
{
	return strcmp(tool->name, "exec") == 0 || strcmp(tool->name, "process") == 0;
}

static int tool_is_running(const struct transcript_tool *tool)
{
	return tool->state >= 0 &&
		(!tool->state || strcmp(tool->summary, "running") == 0);
}

static const char *tool_state_color(const struct transcript_tool *tool)
{
	return tool->timed_out ? ANSI_YELLOW : tool->state < 0 ? ANSI_RED :
		tool_is_running(tool) ? ANSI_CYAN : ANSI_GREEN;
}

static const char *tool_state_marker(const struct transcript_tool *tool)
{
	return tool->state < 0 && !tool->timed_out ? CLI_FEED_FAILED :
		tool_is_running(tool) ? CLI_FEED_RUNNING : "●";
}

static void tool_row(const struct transcript_tool *tool, morph_buf_t *row,
		     int styled)
{
	morph_buf_t action;
	morph_buf_t subject;
	morph_buf_t meta;
	int columns = transcript_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	const char *label = strcmp(tool->name, "exec") == 0 ? "Exec" : tool->name;
	int name_width = (int)utf8_display_width(label);
	const char *target = tool->subject;
	const char *color = tool_state_color(tool);
	char cwd[PATH_MAX];
	double elapsed = tool->ended - tool->started;

	if (columns < 24) {
		if (morph_buf_init(&action, 64) != 0)
			return;
		(void)morph_buf_printf(&action, "%s %s%s", tool_state_marker(tool),
			tool->timed_out ? "TIMEOUT " : tool->state < 0 ? "FAILED " : "", label);
		if (styled)
			(void)morph_buf_puts(row, color);
		append_clipped_text(row, morph_buf_cstr(&action), columns);
		if (styled)
			(void)morph_buf_puts(row, ANSI_RESET);
		morph_buf_cleanup(&action);
		return;
	}
	if (shell_tool(tool) && getcwd(cwd, sizeof(cwd)))
		target = cli_shell_summary(target, cwd);
	/* Shorten only a whole path prefix, never shell source or sibling paths. */
	if (!shell_tool(tool) && target[0] == '/' && getcwd(cwd, sizeof(cwd))) {
		size_t len = strlen(cwd);

		if (strncmp(target, cwd, len) == 0 &&
		    (target[len] == '/' || target[len] == '\0'))
			target = target[len] ? target + len + 1 : ".";
	}
	if (morph_buf_init(&action, 64) != 0)
		return;
	if (morph_buf_init(&subject, 128) != 0) {
		morph_buf_cleanup(&action);
		return;
	}
	if (morph_buf_init(&meta, 64) != 0) {
		morph_buf_cleanup(&action);
		morph_buf_cleanup(&subject);
		return;
	}
	if (tool->timed_out)
		(void)morph_buf_puts(&meta, "TIMEOUT · ");
	else if (tool->state < 0)
		(void)morph_buf_puts(&meta, "FAILED · ");
	else if (tool->state && tool_is_running(tool))
		(void)morph_buf_puts(&meta, "RUNNING · ");
	if (tool->state)
		(void)morph_buf_printf(&meta, "%.1fs", elapsed);
	int available = columns - 5 - (int)utf8_display_width(meta.data);

	if (name_width > available)
		name_width = available > 0 ? available : 1;
	append_clipped_text(&action, label, name_width);
	append_clipped_text(&subject, target,
		columns - name_width - 5 - (int)utf8_display_width(meta.data));
	(void)morph_buf_printf(row, "%s%s%s %s%s%s ", styled ? color : "",
		tool_state_marker(tool), styled ? ANSI_RESET : "",
		styled ? ANSI_BOLD : "", action.data, styled ? ANSI_RESET : "");
	if (styled && shell_tool(tool))
		(void)cli_shell_style(row, subject.data, 0);
	else
		(void)morph_buf_printf(row, "%s%s%s", styled ? ANSI_CYAN : "",
			subject.data, styled ? ANSI_RESET : "");
	if (tool->state) {
		(void)morph_buf_printf(row, "  %s%s%s", styled ?
			(tool->state < 0 ? color : ANSI_DIM) : "",
			meta.data, styled ? ANSI_RESET : "");
	} else {
		(void)morph_buf_puts(row, "…");
	}
	morph_buf_cleanup(&meta);
	morph_buf_cleanup(&subject);
	morph_buf_cleanup(&action);
}

static struct transcript_tool *active_tool(struct cli_context *ctx)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;

	if (!tr)
		return NULL;
	for (size_t i = tr->tools.nelts; i > 0; i--) {
		tool = morph_array_get(&tr->tools, i - 1);
		if (!tool->state)
			return tool;
	}
	return NULL;
}

int cli_transcript_live_text(struct cli_context *ctx, morph_buf_t *text, int styled)
{
	struct transcript_tool *tool = active_tool(ctx);

	if (!tool)
		return 0;
	tool_row(tool, text, styled);
	return 1;
}

/* The command is in the header; the active body contains only progress. */
int cli_transcript_live_body(struct cli_context *ctx, morph_buf_t *body, int styled)
{
	struct transcript_tool *tool = active_tool(ctx);
	const char *start;
	const char *end;
	int rows = 0;
	int width = transcript_columns() - (int)CLI_CONTENT_RIGHT_PADDING - 4;

	if (!tool)
		return 0;
	end = morph_buf_cstr(&tool->stream) + tool->stream.len;
	while (end > tool->stream.data && end[-1] == '\n')
		end--;
	if (end == tool->stream.data) {
		(void)morph_buf_printf(body, "%s    Running…%s",
			styled ? ANSI_DIM : "", styled ? ANSI_RESET : "");
		return 1;
	}
	/* Inspect only the tail. Retain the complete sanitized stream for details. */
	start = end;
	while (start > tool->stream.data) {
		if (start[-1] == '\n' && ++rows == 3)
			break;
		start--;
	}
	rows = 0;
	while (start < end) {
		const char *newline = memchr(start, '\n', (size_t)(end - start));
		char *line = strndup(start, (size_t)((newline ? newline : end) - start));

		if (!line)
			break;
		if (rows)
			(void)morph_buf_putc(body, '\n');
		(void)morph_buf_puts(body, styled ? ANSI_DIM : "");
		(void)morph_tui_tree_prefix(body, NULL, 0, 1, rows != 0, 2);
		append_clipped_text(body, line, width);
		(void)morph_buf_puts(body, styled ? ANSI_RESET : "");
		free(line);
		rows++;
		start = newline ? newline + 1 : end;
	}
	return rows;
}

static void restore_live(struct cli_context *ctx)
{
	morph_buf_t live;

	if (morph_buf_init(&live, 128) != 0)
		return;
	if (cli_transcript_live_text(ctx, &live, 0))
		cli_terminal_live_set(ctx, morph_buf_cstr(&live));
	morph_buf_cleanup(&live);
}

static void print_inline(const char *text, int columns)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe || columns < 1) {
		free(safe);
		return;
	}
	end = utf8_advance_display_width(safe, (size_t)columns);
	if (*end)
		end = utf8_advance_display_width(safe, (size_t)(columns - 1));
	printf("%.*s%s", (int)(end - safe), safe, *end ? "…" : "");
	free(safe);
}

/* Compact feed lines share one width budget and never interpret shell text. */
static void print_feed_line(const char *prefix, const char *text, const char *color)
{
	if (transcript_columns() < 8)
		return;
	printf("%s%s", color, prefix);
	print_inline(text, transcript_columns() - (int)CLI_CONTENT_RIGHT_PADDING -
		(int)utf8_display_width(prefix));
	printf(ANSI_RESET "\n");
}

#define TOOL_PREVIEW_LINES 3

/* Prefer human-readable payloads. The complete original stays in turn details. */
static void tool_preview_text(const struct transcript_tool *tool, const cJSON *data,
			       morph_buf_t *text)
{
	const char *value = tool->error;
	const cJSON *content;

	if (!value[0] && cJSON_IsString(data))
		value = data->valuestring;

	if (!value[0] && shell_tool(tool)) {
		value = json_string(data, tool->state < 0 ? "stderr" : "stdout");
		if (!value[0])
			value = json_string(data, "stderr");
	} else if (!value[0] && strcmp(tool->name, "apply_patch") == 0) {
		value = tool->patch_preview;
	} else if (!value[0]) {
		value = json_string(data, "content");
		if (!value[0])
			value = json_string(data, "text");
		if (!value[0])
			value = json_string(data, "preview");
	}
	if (value[0]) {
		(void)morph_buf_puts(text, value);
		return;
	}
	content = cJSON_GetObjectItemCaseSensitive(data, "content");
	if (cJSON_IsArray(content)) {
		const cJSON *block;

		cJSON_ArrayForEach(block, content) {
			value = json_string(block, "text");
			if (!value[0])
				continue;
			if (text->len)
				(void)morph_buf_putc(text, '\n');
			(void)morph_buf_puts(text, value);
		}
	}
	if (!text->len && tool->stream.len)
		(void)morph_buf_puts(text, morph_buf_cstr(&tool->stream));
	if (!text->len) {
		value = !data && tool->result[0] ? tool->result :
			tool->timed_out ? "Command timed out" :
			tool->state < 0 ? "Tool failed" :
			tool->summary[0] && strcmp(tool->summary, "exit 0") != 0 ?
			tool->summary : "Completed";
		(void)morph_buf_puts(text, value);
	}
}

static void print_tool_preview(const struct transcript_tool *tool)
{
	cJSON *json = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	morph_buf_t text;
	char *safe;
	char *line;
	size_t total = 0;
	size_t shown = 0;
	const char *color = tool->timed_out ? ANSI_YELLOW :
		tool->state < 0 ? ANSI_RED : ANSI_DIM;

	if (!cJSON_IsObject(data))
		data = json;
	if (morph_buf_init(&text, 128) != 0) {
		cJSON_Delete(json);
		return;
	}
	tool_preview_text(tool, data, &text);
	safe = utf8_terminal_sanitize_dup(morph_buf_cstr(&text), text.len,
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	morph_buf_cleanup(&text);
	cJSON_Delete(json);
	if (!safe)
		return;
	size_t len = strlen(safe);

	while (len && safe[len - 1] == '\n')
		safe[--len] = '\0';
	line = safe + strspn(safe, "\n");
	for (char *p = line; *p; p++)
		total += *p == '\n';
	if (*line)
		total++;
	while (*line && shown < TOOL_PREVIEW_LINES) {
		char *newline = strchr(line, '\n');

		if (newline)
			*newline = '\0';
		morph_buf_t prefix = {0};
		if (morph_buf_init(&prefix, 16) != 0)
			break;
		morph_tui_tree_prefix(&prefix, NULL, 0, 1, shown != 0, 2);
		print_feed_line(morph_buf_cstr(&prefix), line, color);
		morph_buf_cleanup(&prefix);
		shown++;
		if (!newline)
			break;
		line = newline + 1;
	}
	if (total > shown) {
		morph_buf_t hint;

		if (morph_buf_init(&hint, 64) == 0) {
			(void)morph_buf_printf(&hint, "+ %zu %s (ctrl+o to expand)",
				total - shown, total - shown == 1 ? "line" : "lines");
			print_feed_line("    ", morph_buf_cstr(&hint), ANSI_DIM);
			morph_buf_cleanup(&hint);
		}
	}
	free(safe);
}

static void print_tool(const struct transcript_tool *tool, int compact)
{
	morph_buf_t line;

	if (morph_buf_init(&line, 128) != 0)
		return;
	tool_row(tool, &line, 1);
	printf("\n%s\n", morph_buf_cstr(&line));
	if (compact && tool->state)
		print_tool_preview(tool);
	morph_buf_cleanup(&line);
}

/* Details are complete terminal-safe text, including multiline strings inside
 * JSON envelopes. No tree depth, item count, or byte truncation is applied. */
static void print_lines(const char *text)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	const char *line = safe;
	int width = transcript_columns() - 5;

	if (!safe)
		return;
	if (width < 1)
		width = 1;
	while (*line) {
		const char *end = strchr(line, '\n');
		const char *wrap = utf8_advance_display_width(line, (size_t)width);
		const char *style = line[0] == '+' ? ANSI_GREEN :
			(line[0] == '-' ? ANSI_RED : ANSI_DIM);

		if (!end || wrap < end) {
			end = wrap;
			if (*wrap && *wrap != '\n') {
				const char *space = wrap;

				while (space > line && space[-1] != ' ')
					space--;
				if (space > line)
					end = space;
			}
		}
		if (end == line && *line != '\n')
			end = line + utf8codepointcalcsize(line);
		printf("    %s%.*s" ANSI_RESET "\n", style, (int)(end - line), line);
		line = *end == '\n' ? end + 1 : end;
	}
	free(safe);
}

static void print_value(const cJSON *value, int depth)
{
	cJSON *child;
	char *json;

	if (cJSON_IsString(value)) {
		print_lines(value->valuestring);
		return;
	}
	if (depth < 32 && (cJSON_IsObject(value) || cJSON_IsArray(value))) {
		cJSON_ArrayForEach(child, value) {
			if (child->string) {
				printf("    " ANSI_DIM);
				print_inline(child->string, transcript_columns() - 10);
				printf(":" ANSI_RESET "\n");
			}
			print_value(child, depth + 1);
		}
		return;
	}
	json = cJSON_Print(value);
	if (json) {
		print_lines(json);
		free(json);
	}
}

static void print_payload(const char *text)
{
	cJSON *json = cJSON_Parse(text);

	if (json) {
		print_value(json, 0);
		cJSON_Delete(json);
	} else {
		print_lines(text);
	}
}

static void print_details(const struct transcript_tool *tool, int args,
			  const char *workdir)
{
	if (args) {
		if (strcmp(tool->name, "apply_patch") == 0) {
			cJSON *json = cJSON_Parse(tool->args);

			if (tool->patch_preview[0])
				cli_presentation_patch_diff(NULL,
					tool->patch_preview);
			else
				cli_presentation_patch_diff(workdir,
					json_string(json, "input"));
			cJSON_Delete(json);
		} else if (strcmp(tool->name, "exec") == 0) {
			cJSON *json = cJSON_Parse(tool->args);
			morph_buf_t command;

			if (morph_buf_init(&command, 128) == 0) {
				(void)cli_shell_style(&command, json_string(json, "command"),
					transcript_columns() - 6);
				printf("    $ %s\n", morph_buf_cstr(&command));
				morph_buf_cleanup(&command);
			}
			cJSON_DeleteItemFromObjectCaseSensitive(json, "command");
			if (json && json->child)
				print_value(json, 0);
			cJSON_Delete(json);
		} else {
			print_payload(tool->args);
		}
	}
	if (tool->stream.len) {
		print_lines(morph_buf_cstr(&tool->stream));
	}
	if (tool->result[0]) {
		cJSON *json = cJSON_Parse(tool->result);
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");

		if (!cJSON_IsObject(data))
			data = json;
		if (strcmp(tool->name, "exec") == 0 &&
		    cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "stdout"))) {
			const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(data, "exit_code");

			printf("\n");
			print_lines(json_string(data, "stdout"));
			print_lines(json_string(data, "stderr"));
			if (cJSON_IsNumber(exit_code))
				printf(ANSI_DIM "    exit %d\n" ANSI_RESET, exit_code->valueint);
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "timed_out")))
				print_lines("Command timed out");
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stdout_truncated")) ||
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stderr_truncated")))
				print_lines("Output truncated by the tool");
		} else {
			print_payload(tool->result);
		}
		cJSON_Delete(json);
	}
	if (tool->error[0])
		print_lines(tool->error);
}

static void present_frame(struct cli_transcript *tr, morph_buf_t *frame,
			  int rows, int columns)
{
	const char *current = morph_buf_cstr(frame);
	const char *previous = morph_buf_cstr(&tr->previous_frame);
	int resized = rows != tr->frame_rows || columns != tr->frame_columns;
	int writing = 0;
	int row = 1;

	while (*current || *previous) {
		size_t len = strcspn(current, "\n");
		size_t old_len = strcspn(previous, "\n");

		if (resized || len != old_len || memcmp(current, previous, len) != 0) {
			if (!writing) {
				fprintf(stdout, "\033[?2026h");
				if (resized)
					fprintf(stdout, "\033[H\033[2J");
				writing = 1;
			}
			fprintf(stdout, "\033[%d;1H", row);
			printf("%.*s" ANSI_RESET, (int)len, current);
			fprintf(stdout, "\033[K");
		}
		current += len + (current[len] == '\n');
		previous += old_len + (previous[old_len] == '\n');
		row++;
	}
	if (writing) {
		fprintf(stdout, "\033[?2026l");
		fflush(stdout);
	}
	morph_buf_reset(&tr->previous_frame);
	(void)morph_buf_puts(&tr->previous_frame, morph_buf_cstr(frame));
	tr->frame_rows = rows;
	tr->frame_columns = columns;
}

void cli_transcript_view_render(struct cli_context *ctx, int scroll)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;
	struct winsize size = {0};
	morph_buf_t page;
	morph_buf_t frame;
	const char *line;
	size_t lines = 0;
	size_t first;
	int rows = 24;
	int height;

	if (!ctx->details_visible || !tr || morph_buf_init(&page, BUFSIZ) != 0)
		return;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row)
		rows = size.ws_row;
	height = rows > 5 ? rows - 4 : 1;
	if (cli_command_capture_styled_begin(&page) != 0) {
		morph_buf_cleanup(&page);
		return;
	}
	size_t reasoning_index = 0;

	for (size_t i = 0; i <= tr->tools.nelts; i++) {
		while (reasoning_index < tr->reasoning.nelts) {
			struct transcript_reasoning *reasoning =
				morph_array_get(&tr->reasoning, reasoning_index);

			if (reasoning->after_tools > i)
				break;
			printf(ANSI_DIM "  " CLI_FEED_SYSTEM " Reasoning\n" ANSI_RESET);
			print_lines(morph_buf_cstr(&reasoning->text));
			printf("\n");
			reasoning_index++;
		}
		if (i < tr->tools.nelts) {
			tool = morph_array_get(&tr->tools, i);
			print_tool(tool, 0);
			print_details(tool, 1, transcript_workdir(ctx));
			printf("\n");
		}
	}
	if (!tr->tools.nelts && !tr->reasoning.nelts)
		printf("  No activity yet.\n");
	cli_command_capture_end();
	for (const char *p = morph_buf_cstr(&page); *p; p++)
		lines += *p == '\n';
	if (scroll == INT_MAX)
		tr->view_follow = 1;
	else if (scroll == INT_MIN) {
		tr->view_follow = 0;
		tr->view_offset = 0;
	} else if (scroll) {
		size_t step = (size_t)(scroll < 0 ? -scroll : scroll);

		tr->view_follow = 0;
		if (scroll < 0)
			tr->view_offset = step > tr->view_offset ? 0 :
				tr->view_offset - step;
		else
			tr->view_offset += step;
	}
	first = lines > (size_t)height ? lines - (size_t)height : 0;
	if (tr->view_follow || tr->view_offset > first)
		tr->view_offset = first;
	if (morph_buf_init(&frame, BUFSIZ) != 0) {
		morph_buf_cleanup(&page);
		return;
	}
	if (cli_command_capture_styled_begin(&frame) != 0) {
		morph_buf_cleanup(&frame);
		morph_buf_cleanup(&page);
		return;
	}
	printf(ANSI_BOLD "  Turn details" ANSI_RESET
	       ANSI_DIM "  · %zu calls\n\n" ANSI_RESET, tr->tools.nelts);
	line = morph_buf_cstr(&page);
	for (size_t i = 0; i < tr->view_offset && *line; i++) {
		const char *next = strchr(line, '\n');

		line = next ? next + 1 : line + strlen(line);
	}
	for (int i = 0; i < height; i++) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);
		const char *style = strncmp(line, "    +", 5) == 0 ? ANSI_GREEN :
			(strncmp(line, "    -", 5) == 0 ? ANSI_RED : ANSI_DIM);

		printf("%s%.*s" ANSI_RESET "\n", style, (int)len, line);
		line = next ? next + 1 : line + len;
	}
	printf("\n");
	print_inline("  ctrl+o / esc back · PgUp/PgDn scroll · End follow",
		     transcript_columns() - 1);
	cli_command_capture_end();
	present_frame(tr, &frame, rows, transcript_columns());
	morph_buf_cleanup(&frame);
	morph_buf_cleanup(&page);
}

void cli_transcript_view_suspend(struct cli_context *ctx)
{
	if (!ctx || !ctx->details_visible)
		return;
	fprintf(stdout, "\033[?1006l\033[?1000l\033[?1049l\033[?25h");
	if (ctx->transcript) {
		morph_buf_t *pending = &ctx->transcript->deferred;

		(void)fwrite(morph_buf_cstr(pending), 1, pending->len, stdout);
		morph_buf_reset(pending);
	}
	fflush(stdout);
	ctx->details_visible = 0;
}

void cli_transcript_view_resume(struct cli_context *ctx)
{
	if (!ctx || !ctx->details_open || ctx->details_visible)
		return;
	fprintf(stdout, "\033[?1049h\033[?25l\033[?1000h\033[?1006h");
	morph_buf_reset(&ctx->transcript->previous_frame);
	ctx->details_visible = 1;
	cli_transcript_view_render(ctx, 0);
}

void cli_transcript_toggle(struct cli_context *ctx)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (ctx->details_open) {
		cli_transcript_view_suspend(ctx);
		ctx->details_open = 0;
	} else {
		if (!ctx->transcript && transcript_init(ctx) != 0)
			return;
		ctx->transcript->view_follow = 1;
		ctx->details_open = 1;
		cli_transcript_view_resume(ctx);
	}
}

void cli_transcript_finish(struct cli_context *ctx)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;

	if (!tr || tr->finished)
		return;
	morph_array_foreach(tool, &tr->tools, struct transcript_tool) {
		if (!tool->state) {
			tool->state = -1;
			tool->ended = transcript_now();
			tool->error = "Interrupted before a result was received";
			print_tool(tool, !ctx->tool_details);
		}
	}
	tr->finished = 1;
}

static const char *find_process_command(struct cli_transcript *tr,
					const char *session_id)
{
	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct transcript_tool *candidate =
			morph_array_get(&tr->tools, i - 1);
		cJSON *json;
		const cJSON *data;
		const char *candidate_id;

		if (strcmp(candidate->name, "exec") != 0 ||
		    !candidate->result[0])
			continue;
		json = cJSON_Parse(candidate->result);
		data = cJSON_GetObjectItemCaseSensitive(json, "data");
		if (!cJSON_IsObject(data))
			data = json;
		candidate_id = json_string(data, "session_id");
		if (strcmp(candidate_id, session_id) == 0) {
			cJSON_Delete(json);
			return candidate->subject;
		}
		cJSON_Delete(json);
	}
	return "";
}

static int process_action_is(const struct transcript_tool *tool,
			     const char *action)
{
	cJSON *args;
	int matches;

	if (strcmp(tool->name, "process") != 0)
		return 0;
	args = cJSON_Parse(tool->args);
	matches = strcmp(json_string(args, "action"), action) == 0;
	cJSON_Delete(args);
	return matches;
}

static int process_poll_is_running(const struct transcript_tool *tool)
{
	cJSON *result;
	const cJSON *data;
	int running;

	if (!process_action_is(tool, "poll"))
		return 0;
	result = cJSON_Parse(tool->result);
	data = cJSON_GetObjectItemCaseSensitive(result, "data");
	if (!cJSON_IsObject(data))
		data = result;
	running = strcmp(json_string(data, "status"), "running") == 0;
	cJSON_Delete(result);
	return running;
}

static int add_tool(struct cli_transcript *tr, const cJSON *data,
		    const char *workdir)
{
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(data, "args");
	struct transcript_tool *tool = morph_array_push(&tr->tools);
	char *json;
	const char *subject = "";
	static const char *const keys[] = {
		"command", "file_path", "dir_path", "path", "query", "pattern", "url"
	};

	if (!tool)
		MORPH_RETURN(-ENOMEM);
	memset(tool, 0, sizeof(*tool));
	tool->id = save_text(tr, json_string(data, "tool_call_id"));
	tool->name = save_text(tr, json_string(data, "tool"));
	json = args ? cJSON_PrintUnformatted(args) : NULL;
	tool->args = save_text(tr, json ? json : "{}");
	free(json);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		subject = json_string(args, keys[i]);
		if (subject[0])
			break;
	}
	tool->subject = save_text(tr, subject);
	tool->result = "";
	tool->error = "";
	tool->summary = "";
	tool->patch_preview = "";
	if (strcmp(tool->name, "activate_skill") == 0) {
		tool->subject = save_text(tr, json_string(args, "name"));
	}
	if (strcmp(tool->name, "process") == 0) {
		const char *session_id = json_string(args, "session_id");
		const char *command = find_process_command(tr, session_id);
		morph_buf_t fallback;

		if (command[0]) {
			tool->subject = command;
		} else if (morph_buf_init(&fallback, 64) == 0) {
			(void)morph_buf_printf(&fallback, "session %s", session_id);
			tool->subject = save_text(tr, morph_buf_cstr(&fallback));
			morph_buf_cleanup(&fallback);
		}
	}
	if (strcmp(tool->name, "apply_patch") == 0) {
		const char *input = json_string(args, "input");
		const char *path = strstr(input, " File: ");
		morph_buf_t patch;
		morph_buf_t preview;
		char preview_error[256];
		size_t added = 0;
		size_t removed = 0;

		if (workdir && workdir[0] &&
		    morph_buf_init(&preview, 256) == 0) {
			if (patch_preview(workdir, input, &preview, preview_error,
				sizeof(preview_error)) == 0)
				tool->patch_preview = save_text(
					tr, morph_buf_cstr(&preview));
			morph_buf_cleanup(&preview);
		}
		if (morph_buf_init(&patch, 128) == 0) {
			if (path) {
				path += strlen(" File: ");
				(void)morph_buf_append(&patch, path, strcspn(path, "\r\n"));
				tool->subject = save_text(tr, morph_buf_cstr(&patch));
			}
			for (const char *p = input; *p; p++) {
				if (p == input || p[-1] == '\n') {
					added += *p == '+';
					removed += *p == '-';
				}
			}
			morph_buf_reset(&patch);
			(void)morph_buf_printf(&patch, "+%zu -%zu", added, removed);
			tool->summary = save_text(tr, morph_buf_cstr(&patch));
			morph_buf_cleanup(&patch);
		}
	}
	tool->started = transcript_now();
	utf8_terminal_sanitizer_init(&tool->sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	if (morph_buf_init_arena(&tool->stream, tr->arena, 128) != 0)
		MORPH_RETURN(-ENOMEM);
	if (tr->allocation_failed)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static void summarize_result(struct cli_transcript *tr, struct transcript_tool *tool)
{
	cJSON *json = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
	const cJSON *value;
	morph_buf_t summary;

	if (!json)
		return;
	if (!cJSON_IsObject(data))
		data = json;
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) ||
	    cJSON_IsObject(error) || cJSON_IsString(error))
		tool->state = -1;
	if (cJSON_IsObject(error)) {
		const char *message = json_string(error, "message");

		if (message[0])
			tool->error = save_text(tr, message);
	} else if (cJSON_IsString(error)) {
		tool->error = save_text(tr, error->valuestring);
	}
	if (morph_buf_init(&summary, 64) != 0) {
		cJSON_Delete(json);
		return;
	}
	value = cJSON_GetObjectItemCaseSensitive(data, "duration_ms");
	if (shell_tool(tool) && cJSON_IsNumber(value) &&
	    value->valuedouble >= 0)
		tool->started = tool->ended - value->valuedouble / 1000.0;
	tool->timed_out = strcmp(json_string(data, "status"), "timed_out") == 0 ||
		cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "timed_out"));
	if (tool->timed_out)
		tool->state = -1;
	value = cJSON_GetObjectItemCaseSensitive(data, "exit_code");
	if (cJSON_IsNumber(value)) {
		(void)morph_buf_printf(&summary, "exit %d", value->valueint);
		if (value->valueint != 0) {
			tool->state = -1;
			tool->error = save_text(tr, json_string(data, "stderr"));
			if (!tool->error[0])
				tool->error = save_text(tr, morph_buf_cstr(&summary));
		}
	} else if (cJSON_IsString(value =
		cJSON_GetObjectItemCaseSensitive(data, "status"))) {
		(void)morph_buf_puts(&summary, value->valuestring);
	} else if (cJSON_IsNumber(value =
		cJSON_GetObjectItemCaseSensitive(data, "returned_lines"))) {
		(void)morph_buf_printf(&summary, "%d lines", value->valueint);
	} else if (cJSON_IsArray(value =
		cJSON_GetObjectItemCaseSensitive(data, "entries"))) {
		(void)morph_buf_printf(&summary, "%d entries", cJSON_GetArraySize(value));
	}
	if (tool->state > 0 && strcmp(tool->name, "activate_skill") == 0)
		(void)morph_buf_puts(&summary, "loaded");
	if (strcmp(tool->name, "tasks") == 0) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id");
		const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
		const cJSON *task = cJSON_GetArrayItem(tasks, 0);
		const char *title = json_string(task, "title");
		morph_buf_t subject;

		if (cJSON_IsNumber(id) && id->valuedouble > 0 &&
		    morph_buf_init(&subject, 64) == 0) {
			(void)morph_buf_printf(&subject, "#%lld%s%s",
				(long long)id->valuedouble, title[0] ? " " : "", title);
			tool->subject = save_text(tr, morph_buf_cstr(&subject));
			morph_buf_cleanup(&subject);
		}
	}
	if (summary.len)
		tool->summary = save_text(tr, morph_buf_cstr(&summary));
	morph_buf_cleanup(&summary);
	cJSON_Delete(json);
}

/* Return one for events fully presented here, zero for the usual presenter. */
int cli_transcript_event(struct cli_context *ctx, const struct morph_event *ev)
{
	struct cli_transcript *tr;
	struct transcript_tool *tool;
	const char *name = ev->name ? ev->name : "";
	int rc;

	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	if (!ctx->transcript && transcript_init(ctx) != 0)
		MORPH_RETURN(-ENOMEM);
	tr = ctx->transcript;
	if (strcmp(name, "react.observation") == 0 &&
	    strcmp(json_string(ev->data, "tool"), "plan") != 0 &&
	    (tool = find_tool(tr, ev->data))) {
		if (!tool->result[0]) {
			tool->result = save_text(tr, json_string(ev->data, "text"));
			if (ctx->tool_details)
				print_payload(tool->result);
		}
		return 1;
	}
	if (ev->type != MORPH_EVENT_TOOL)
		return 0;
	if (strcmp(name, "tool.call") == 0) {
		rc = add_tool(tr, ev->data, transcript_workdir(ctx));
		if (rc != 0)
			MORPH_RETURN(rc);
	}
	tool = find_tool(tr, ev->data);
	if (!tool)
		return 0;
	if (strcmp(name, "tool.call") == 0) {
		int quiet_poll = !ctx->tool_details &&
			process_action_is(tool, "poll");

		cli_terminal_live_clear(ctx);
		cli_presentation_flush_stream(ctx);
		if (!quiet_poll &&
		    (ctx->tool_details || !isatty(STDOUT_FILENO)))
			print_tool(tool, !ctx->tool_details);
		if (ctx->tool_details)
			print_details(tool, 1, transcript_workdir(ctx));
		restore_live(ctx);
		return 1;
	}
	if (strcmp(name, "tool.running") == 0)
		return 1;
	if (strcmp(name, "tool.stream.delta") == 0) {
		const char *text = json_string(ev->data, "text");
		size_t offset = tool->stream.len;

		rc = utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream,
			text, strlen(text), 0);
		if (rc != 0)
			MORPH_RETURN(rc);
		if (ctx->tool_details) {
			cli_terminal_live_clear(ctx);
			print_lines(morph_buf_cstr(&tool->stream) + offset);
		}
		restore_live(ctx);
		return 1;
	}
	if (strcmp(name, "tool.result") != 0 && strcmp(name, "tool.failed") != 0 &&
	    strcmp(name, "tool.cancelled") != 0)
		return 0;
	tool->state = strcmp(name, "tool.result") == 0 ? 1 : -1;
	tool->ended = transcript_now();
	tool->result = save_text(tr, json_string(ev->data, "result"));
	tool->error = save_text(tr, json_string(ev->data, "error"));
	(void)utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, NULL, 0, 1);
	summarize_result(tr, tool);
	cli_terminal_live_clear(ctx);
	if (ctx->tool_details || !process_poll_is_running(tool))
		print_tool(tool, !ctx->tool_details);
	if (ctx->tool_details) {
		print_details(tool, 0, transcript_workdir(ctx));
	}

	restore_live(ctx);
	return 1;
}
