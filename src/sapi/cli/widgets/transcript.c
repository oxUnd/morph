#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/shell_style.h"
#include "util/terminal_size.h"
#include "util/utf8.h"
#include "cJSON.h"
#include <morph/tui/widgets/tree.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cli_transcript_columns(void)
{
	return morph_terminal_columns(STDOUT_FILENO, 100);
}

static const char *cli_transcript_json_string(const cJSON *data, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

/* Reasoning stays separate from tools, with its original position in the turn. */

static void cli_transcript_append_clipped_text(morph_buf_t *buf, const char *text, int width)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE,
						NULL);
	const char *end;

	if (!safe || width < 1) {
		free(safe);
		return;
	}
	end = utf8_advance_display_width_borrow(safe, (size_t)width);
	if (*end) {
		end = utf8_advance_display_width_borrow(safe, (size_t)(width - 1));
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
		(void)morph_buf_puts(buf, "…");
	} else {
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
	}
	free(safe);
}

static const char *cli_transcript_tool_state_color(const struct cli_transcript_tool *tool)
{
	if (tool->historical && !tool->state)
		return ANSI_DIM;
	return tool->timed_out			      ? ANSI_YELLOW
	       : tool->state < 0		      ? ANSI_RED
	       : cli_transcript_tool_is_running(tool) ? ANSI_CYAN
						      : ANSI_GREEN;
}

static const char *cli_transcript_tool_state_marker(const struct cli_transcript_tool *tool)
{
	if (tool->historical && !tool->state)
		return "-";
	return tool->state < 0 && !tool->timed_out    ? CLI_FEED_FAILED
	       : cli_transcript_tool_is_running(tool) ? CLI_FEED_RUNNING
						      : CLI_FEED_DONE;
}

static void cli_transcript_tool_row(const struct cli_transcript_tool *tool, morph_buf_t *row,
				    int available_columns, int styled)
{
	if (cli_agent_tools_row(tool, row, available_columns, styled))
		return;
	morph_buf_t action;
	morph_buf_t subject;
	morph_buf_t meta;
	int columns = available_columns - (int)CLI_CONTENT_RIGHT_PADDING;
	const char *label = strcmp(tool->name, "exec") == 0 ? "Exec" : tool->name;
	int name_width = (int)utf8_display_width(label);
	const char *target = tool->subject;
	const char *color = cli_transcript_tool_state_color(tool);
	char cwd[PATH_MAX];
	double elapsed = tool->ended - tool->started;

	if (columns < 24) {
		if (morph_buf_init(&action, 64) != 0)
			return;
		(void)morph_buf_printf(&action, "%s %s%s", cli_transcript_tool_state_marker(tool),
				       tool->timed_out	 ? "TIMEOUT "
				       : tool->state < 0 ? "FAILED "
							 : "",
				       label);
		if (styled)
			(void)morph_buf_puts(row, color);
		cli_transcript_append_clipped_text(row, morph_buf_cstr_borrow(&action), columns);
		if (styled)
			(void)morph_buf_puts(row, ANSI_RESET);
		morph_buf_cleanup(&action);
		return;
	}
	if (tool->shell && getcwd(cwd, sizeof(cwd)))
		target = cli_shell_summary_borrow(target, cwd);
	/* Shorten only a whole path prefix, never shell source or sibling paths. */
	if (!tool->shell && target[0] == '/' && getcwd(cwd, sizeof(cwd))) {
		size_t len = strlen(cwd);

		if (strncmp(target, cwd, len) == 0 && (target[len] == '/' || target[len] == '\0'))
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
	if (tool->historical && !tool->state)
		(void)morph_buf_puts(&meta, "no recorded result");
	else if (tool->timed_out)
		(void)morph_buf_puts(&meta, "TIMEOUT");
	else if (tool->state < 0)
		(void)morph_buf_puts(&meta, "FAILED");
	else if (tool->state && cli_transcript_tool_is_running(tool))
		(void)morph_buf_puts(&meta, "RUNNING");
	if (tool->state && (!tool->historical || elapsed > 0)) {
		if (meta.len)
			(void)morph_buf_puts(&meta, " · ");
		(void)morph_buf_printf(&meta, "%.1fs", elapsed);
	}
	int available = columns - 5 - (int)utf8_display_width(meta.data);

	if (name_width > available)
		name_width = available > 0 ? available : 1;
	cli_transcript_append_clipped_text(&action, label, name_width);
	cli_transcript_append_clipped_text(
		&subject, target, columns - name_width - 5 - (int)utf8_display_width(meta.data));
	(void)morph_buf_printf(row, "%s%s%s %s%s%s ", styled ? color : "",
			       cli_transcript_tool_state_marker(tool), styled ? ANSI_RESET : "",
			       styled ? ANSI_BOLD : "", action.data, styled ? ANSI_RESET : "");
	if (styled && tool->shell)
		(void)cli_shell_style(row, subject.data, 0);
	else
		(void)morph_buf_printf(row, "%s%s%s", styled ? ANSI_CYAN : "", subject.data,
				       styled ? ANSI_RESET : "");
	if (meta.len) {
		(void)morph_buf_printf(row, "  %s%s%s",
				       styled ? (tool->state < 0 ? color : ANSI_DIM) : "",
				       meta.data, styled ? ANSI_RESET : "");
	} else if (!tool->historical) {
		(void)morph_buf_puts(row, "…");
	}
	morph_buf_cleanup(&meta);
	morph_buf_cleanup(&subject);
	morph_buf_cleanup(&action);
}

int cli_transcript_live_text(const struct cli_transcript_model *model, morph_buf_t *text,
			     int columns, int styled)
{
	const struct cli_transcript_tool *tool = cli_transcript_model_active_borrow(model);

	if (!tool || cli_agent_tools_analytics_only(tool->name))
		return 0;
	cli_transcript_tool_row(tool, text, columns, styled);
	return 1;
}

/* The command is in the header; the active body contains only progress. */
int cli_transcript_live_body(const struct cli_transcript_model *model, morph_buf_t *body,
			     int columns, int styled)
{
	const struct cli_transcript_tool *tool = cli_transcript_model_active_borrow(model);
	const char *start;
	const char *end;
	int rows = 0;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - 4;

	if (!tool || cli_agent_tools_matches(tool->name))
		return 0;
	end = morph_buf_cstr_borrow(&tool->stream) + tool->stream.len;
	while (end > tool->stream.data && end[-1] == '\n')
		end--;
	if (end == tool->stream.data) {
		(void)morph_buf_printf(body, "%s    Running…%s", styled ? ANSI_DIM : "",
				       styled ? ANSI_RESET : "");
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
		(void)morph_tui_widget_tree_prefix(body, NULL, 0, 1, rows != 0, 2);
		cli_transcript_append_clipped_text(body, line, width);
		(void)morph_buf_puts(body, styled ? ANSI_RESET : "");
		free(line);
		rows++;
		start = newline ? newline + 1 : end;
	}
	return rows;
}

static void cli_transcript_print_inline(struct cli_application *ctx, const char *text, int columns)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE,
						NULL);
	const char *end;

	if (!safe || columns < 1) {
		free(safe);
		return;
	}
	end = utf8_advance_display_width_borrow(safe, (size_t)columns);
	if (*end)
		end = utf8_advance_display_width_borrow(safe, (size_t)(columns - 1));
	cli_output_printf(ctx, "%.*s%s", (int)(end - safe), safe, *end ? "…" : "");
	free(safe);
}

/* Compact feed lines share one width budget and never interpret shell text. */
static void cli_transcript_print_feed_line(struct cli_application *ctx, const char *prefix,
					   const char *text, const char *color)
{
	if (cli_transcript_columns() < 8)
		return;
	cli_output_printf(ctx, "%s%s", color, prefix);
	cli_transcript_print_inline(ctx, text,
				    cli_transcript_columns() - (int)CLI_CONTENT_RIGHT_PADDING -
					    (int)utf8_display_width(prefix));
	cli_output_printf(ctx, ANSI_RESET "\n");
}

#define CLI_TRANSCRIPT_TOOL_PREVIEW_LINES 3

/* Prefer human-readable payloads. The complete original stays in turn details. */
static void cli_transcript_tool_preview_text(const struct cli_transcript_tool *tool,
					     const cJSON *data, morph_buf_t *text)
{
	const char *value = tool->error;
	const cJSON *content;

	if (!value[0] && cJSON_IsString(data))
		value = data->valuestring;

	if (!value[0] && tool->shell) {
		value = cli_transcript_json_string(data, tool->state < 0 ? "stderr" : "stdout");
		if (!value[0])
			value = cli_transcript_json_string(data, "stderr");
	} else if (!value[0] && strcmp(tool->name, "apply_patch") == 0) {
		value = tool->patch_preview;
	} else if (!value[0]) {
		value = cli_transcript_json_string(data, "content");
		if (!value[0])
			value = cli_transcript_json_string(data, "text");
		if (!value[0])
			value = cli_transcript_json_string(data, "preview");
	}
	if (value[0]) {
		(void)morph_buf_puts(text, value);
		return;
	}
	content = cJSON_GetObjectItemCaseSensitive(data, "content");
	if (cJSON_IsArray(content)) {
		const cJSON *block;

		cJSON_ArrayForEach (block, content) {
			value = cli_transcript_json_string(block, "text");
			if (!value[0])
				continue;
			if (text->len)
				(void)morph_buf_putc(text, '\n');
			(void)morph_buf_puts(text, value);
		}
	}
	if (!text->len && tool->stream.len)
		(void)morph_buf_puts(text, morph_buf_cstr_borrow(&tool->stream));
	if (!text->len) {
		value = !data && tool->result[0] ? tool->result
			: tool->timed_out	 ? "Command timed out"
			: tool->state < 0	 ? "Tool failed"
			: tool->summary[0] && strcmp(tool->summary, "exit 0") != 0 ? tool->summary
										   : "Completed";
		(void)morph_buf_puts(text, value);
	}
}

static void cli_transcript_print_tool_preview(struct cli_application *ctx,
					      const struct cli_transcript_tool *tool)
{
	cJSON *json = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	morph_buf_t text;
	char *safe;
	char *line;
	size_t total = 0;
	size_t shown = 0;
	const char *color = tool->timed_out ? ANSI_YELLOW : tool->state < 0 ? ANSI_RED : ANSI_DIM;

	if (!cJSON_IsObject(data))
		data = json;
	if (morph_buf_init(&text, 128) != 0) {
		cJSON_Delete(json);
		return;
	}
	if (!cli_agent_tools_preview(tool, &text))
		cli_transcript_tool_preview_text(tool, data, &text);
	safe = utf8_terminal_sanitize_dup(morph_buf_cstr_borrow(&text), text.len,
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
	while (*line && shown < CLI_TRANSCRIPT_TOOL_PREVIEW_LINES) {
		char *newline = strchr(line, '\n');

		if (newline)
			*newline = '\0';
		morph_buf_t prefix = {0};
		if (morph_buf_init(&prefix, 16) != 0)
			break;
		morph_tui_widget_tree_prefix(&prefix, NULL, 0, 1, shown != 0, 2);
		cli_transcript_print_feed_line(ctx, morph_buf_cstr_borrow(&prefix), line, color);
		morph_buf_cleanup(&prefix);
		shown++;
		if (!newline)
			break;
		line = newline + 1;
	}
	if (total > shown) {
		morph_buf_t hint;

		if (morph_buf_init(&hint, 64) == 0) {
			(void)morph_buf_printf(&hint, "+ %zu %s (ctrl+o to expand)", total - shown,
					       total - shown == 1 ? "line" : "lines");
			cli_transcript_print_feed_line(ctx, "    ", morph_buf_cstr_borrow(&hint),
						       ANSI_DIM);
			morph_buf_cleanup(&hint);
		}
	}
	free(safe);
}

void cli_transcript_render_tool(struct cli_application *ctx, const struct cli_transcript_tool *tool,
				int compact)
{
	if (compact && cli_agent_tools_analytics_only(tool->name))
		return;
	morph_buf_t line;

	if (morph_buf_init(&line, 128) != 0)
		return;
	cli_transcript_tool_row(tool, &line, cli_transcript_columns(), 1);
	int gap = ctx->output.trailing_newlines < 2 ||
		  (ctx->output.capture && !ctx->output.capture_history);
	cli_output_printf(ctx, "%s%s\n", gap ? "\n" : "", morph_buf_cstr_borrow(&line));
	if (compact && tool->state)
		cli_transcript_print_tool_preview(ctx, tool);
	morph_buf_cleanup(&line);
}

/* Details are complete terminal-safe text, including multiline strings inside
 * JSON envelopes. No tree depth, item count, or byte truncation is applied. */
static void cli_transcript_print_lines(struct cli_application *ctx, const char *text)
{
	char *safe =
		utf8_terminal_sanitize_dup(text, strlen(text), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	const char *line = safe;
	int width = cli_transcript_columns() - 5;

	if (!safe)
		return;
	if (width < 1)
		width = 1;
	while (*line) {
		const char *end = strchr(line, '\n');
		const char *wrap = utf8_advance_display_width_borrow(line, (size_t)width);
		const char *style =
			line[0] == '+' ? ANSI_GREEN : (line[0] == '-' ? ANSI_RED : ANSI_DIM);

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
			end = line + utf8_next_codepoint_len(line, strnlen(line, 4));
		cli_output_printf(ctx, "    %s%.*s" ANSI_RESET "\n", style, (int)(end - line),
				  line);
		line = *end == '\n' ? end + 1 : end;
	}
	free(safe);
}

static void cli_transcript_print_value(struct cli_application *ctx, const cJSON *value, int depth)
{
	cJSON *child;
	char *json;

	if (cJSON_IsString(value)) {
		cli_transcript_print_lines(ctx, value->valuestring);
		return;
	}
	if (depth < 32 && (cJSON_IsObject(value) || cJSON_IsArray(value))) {
		cJSON_ArrayForEach (child, value) {
			if (child->string) {
				cli_output_printf(ctx, "    " ANSI_DIM);
				cli_transcript_print_inline(ctx, child->string,
							    cli_transcript_columns() - 10);
				cli_output_printf(ctx, ":" ANSI_RESET "\n");
			}
			cli_transcript_print_value(ctx, child, depth + 1);
		}
		return;
	}
	json = cJSON_Print(value);
	if (json) {
		cli_transcript_print_lines(ctx, json);
		free(json);
	}
}

void cli_transcript_render_payload(struct cli_application *ctx, const char *text)
{
	cJSON *json = cJSON_Parse(text);

	if (json) {
		cli_transcript_print_value(ctx, json, 0);
		cJSON_Delete(json);
	} else {
		cli_transcript_print_lines(ctx, text);
	}
}

void cli_transcript_render_tool_details(struct cli_application *ctx,
					const struct cli_transcript_tool *tool, int args)
{
	if (args) {
		if (strcmp(tool->name, "apply_patch") == 0) {
			cJSON *json = cJSON_Parse(tool->args);

			if (tool->patch_preview[0])
				cli_presentation_patch_diff(ctx, NULL, tool->patch_preview);
			else
				cli_presentation_patch_diff(
					ctx, NULL, cli_transcript_json_string(json, "input"));
			cJSON_Delete(json);
		} else if (strcmp(tool->name, "exec") == 0) {
			cJSON *json = cJSON_Parse(tool->args);
			morph_buf_t command;

			if (morph_buf_init(&command, 128) == 0) {
				(void)cli_shell_style(&command,
						      cli_transcript_json_string(json, "command"),
						      cli_transcript_columns() - 6);
				cli_output_printf(ctx, "    $ %s\n",
						  morph_buf_cstr_borrow(&command));
				morph_buf_cleanup(&command);
			}
			cJSON_DeleteItemFromObjectCaseSensitive(json, "command");
			if (json && json->child)
				cli_transcript_print_value(ctx, json, 0);
			cJSON_Delete(json);
		} else {
			cli_transcript_render_payload(ctx, tool->args);
		}
	}
	if (tool->stream.len) {
		cli_transcript_print_lines(ctx, morph_buf_cstr_borrow(&tool->stream));
	}
	if (tool->result[0]) {
		cJSON *json = cJSON_Parse(tool->result);
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");

		if (!cJSON_IsObject(data))
			data = json;
		if (strcmp(tool->name, "exec") == 0 &&
		    cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "stdout"))) {
			const cJSON *exit_code =
				cJSON_GetObjectItemCaseSensitive(data, "exit_code");

			cli_output_printf(ctx, "\n");
			cli_transcript_print_lines(ctx, cli_transcript_json_string(data, "stdout"));
			cli_transcript_print_lines(ctx, cli_transcript_json_string(data, "stderr"));
			if (cJSON_IsNumber(exit_code))
				cli_output_printf(ctx, ANSI_DIM "    exit %d\n" ANSI_RESET,
						  exit_code->valueint);
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "timed_out")))
				cli_transcript_print_lines(ctx, "Command timed out");
			if (cJSON_IsTrue(
				    cJSON_GetObjectItemCaseSensitive(data, "stdout_truncated")) ||
			    cJSON_IsTrue(
				    cJSON_GetObjectItemCaseSensitive(data, "stderr_truncated")))
				cli_transcript_print_lines(ctx, "Output truncated by the tool");
		} else if (strcmp(tool->name, "file_read") == 0 &&
			   cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "content"))) {
			const cJSON *count =
				cJSON_GetObjectItemCaseSensitive(data, "returned_lines");
			const cJSON *next =
				cJSON_GetObjectItemCaseSensitive(data, "next_byte_offset");

			cli_output_printf(ctx, "\n");
			if (cJSON_IsNumber(count))
				cli_output_printf(ctx,
						  ANSI_DIM "    %.0f returned lines\n" ANSI_RESET,
						  count->valuedouble);
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "truncated")) &&
			    cJSON_IsNumber(next))
				cli_output_printf(
					ctx,
					ANSI_DIM
					"    More content at byte offset %.0f\n" ANSI_RESET,
					next->valuedouble);
			cli_transcript_print_lines(ctx,
						   cli_transcript_json_string(data, "content"));
		} else {
			cli_transcript_render_payload(ctx, tool->result);
		}
		cJSON_Delete(json);
	}
	if (tool->error[0])
		cli_transcript_print_lines(ctx, tool->error);
}

int cli_transcript_render_details(struct cli_application *ctx,
				  const struct cli_transcript_model *tr, morph_buf_t *page)
{
	const struct cli_transcript_tool *tool;
	int rc;

	if (!tr)
		return -ENOENT;
	rc = cli_output_color_enabled(ctx) ? cli_output_capture_styled_begin(ctx, page)
					   : cli_output_capture_begin(ctx, page);
	if (rc)
		return rc;
	size_t reasoning_index = 0;

	for (size_t i = 0; i <= tr->tools.nelts; i++) {
		while (reasoning_index < tr->reasoning.nelts) {
			const struct cli_transcript_reasoning *reasoning =
				morph_array_borrow(&tr->reasoning, reasoning_index);

			if (reasoning->after_tools > i)
				break;
			cli_output_printf(ctx,
					  ANSI_DIM "  " CLI_FEED_SYSTEM " Reasoning\n" ANSI_RESET);
			cli_transcript_print_lines(ctx, morph_buf_cstr_borrow(&reasoning->text));
			cli_output_printf(ctx, "\n");
			reasoning_index++;
		}
		if (i < tr->tools.nelts) {
			tool = morph_array_borrow(&tr->tools, i);
			cli_transcript_render_tool(ctx, tool, 0);
			cli_transcript_render_tool_details(ctx, tool, 1);
			cli_output_printf(ctx, "\n");
		}
	}
	if (!tr->tools.nelts && !tr->reasoning.nelts)
		cli_output_printf(ctx, "  No activity yet.\n");
	cli_output_capture_end(ctx);
	return page->failed;
}

/* Restore presentation state only; historical calls never enter the tool runtime. */

/* Return one for events fully presented here, zero for the usual presenter. */

void cli_transcript_render_stream(struct cli_application *ctx,
				  const struct cli_transcript_tool *tool, size_t offset)
{
	if (offset <= tool->stream.len)
		cli_transcript_print_lines(ctx, morph_buf_cstr_borrow(&tool->stream) + offset);
}
