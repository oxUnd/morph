#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/presentation/transcript_details.h"
#include <nb/err.h>
#include "sapi/cli/presentation/feed_block.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/shell_style.h"
#include <nothing/tui/terminal/size.h>
#include <nb/utf8.h>
#include <nb/str.h>
#include <nothing/tui/terminal/text.h>
#include <nb/cleanup.h>
#include "cJSON.h"
#include "sapi/cli/presentation/tree.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cli_transcript_columns(void)
{
	return nt_terminal_columns(STDOUT_FILENO, 100);
}

static const char *cli_transcript_json_string(const cJSON *data, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

/* Reasoning stays separate from tools, with its original position in the turn. */

static void cli_transcript_append_clipped_text(nb_buf *buf, const char *text, int width)
{
	char *safe = nt_sanitize_dup(text, strlen(text),
						 NT_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe || width < 1) {
		free(safe);
		return;
	}

	end = nt_terminal_advance_display_width(safe, (size_t)width);
	if (*end) {
		end = nt_terminal_advance_display_width(safe, (size_t)(width - 1));
		(void)nb_buf_append(buf, safe, (size_t)(end - safe));
		(void)nb_buf_append_cstr(buf, "…");
	} else {
		(void)nb_buf_append(buf, safe, (size_t)(end - safe));
	}

	free(safe);
}

static const char *cli_transcript_tool_state_color(const struct cli_transcript_tool *tool)
{
	if (cli_transcript_tool_is_pending(tool) || tool->interrupted)
		return ANSI_DIM;

	return tool->timed_out || tool->state < 0 ? CLI_FEED_FAILED_STYLE
	       : cli_transcript_tool_is_running(tool) ? ANSI_CYAN : ANSI_GREEN;
}

static const char *cli_transcript_tool_state_marker(const struct cli_transcript_tool *tool)
{
	if (cli_transcript_tool_is_pending(tool))
		return CLI_FEED_PENDING;

	if (tool->interrupted)
		return CLI_FEED_CANCELLED;

	return tool->timed_out || tool->state < 0 ? CLI_FEED_FAILED
	       : cli_transcript_tool_is_running(tool) ? CLI_FEED_RUNNING : CLI_FEED_DONE;
}

static void cli_transcript_tool_row(const struct cli_transcript_tool *tool, nb_buf *row,
				    int available_columns, int styled)
{
	if (cli_agent_tools_row(tool, row, available_columns, styled))
		return;

	nb_buf action NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf subject NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf meta NB_CLEANUP(nb_buf_cleanup) = {0};
	int columns = available_columns - (int)CLI_CONTENT_RIGHT_PADDING;
	const char *label = !strcmp(tool->name, "process") ? "process [deprecated]"
			    : !strcmp(tool->name, "exec") ? "Exec" : tool->name;
	int name_width = (int)nb_utf8_width_cstr(label);
	const char *target = tool->subject;
	const char *color = cli_transcript_tool_state_color(tool);
	char cwd[PATH_MAX];
	double elapsed = tool->ended - tool->started;

	if (columns < 24) {
		if ((nb_buf_init(&action), nb_buf_reserve(&action, 64) ? 0 : NB_ERR_NOMEM) != 0)
			return;

		(void)nb_buf_printf(&action, "%s %s%s", cli_transcript_tool_state_marker(tool),
				    tool->interrupted ? "CANCELLED "
				    : tool->timed_out ? "TIMEOUT "
				    : tool->state < 0 ? "FAILED "
						      : "",
				    label);
		if (styled)
			(void)nb_buf_append_cstr(row, color);

		cli_transcript_append_clipped_text(row, nb_buf_cstr(&action), columns);
		if (styled)
			(void)nb_buf_append_cstr(row, ANSI_RESET);

		nb_buf_cleanup(&action);
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

	if ((nb_buf_init(&action), nb_buf_reserve(&action, 64) ? 0 : NB_ERR_NOMEM) != 0)
		return;

	if ((nb_buf_init(&subject), nb_buf_reserve(&subject, 128) ? 0 : NB_ERR_NOMEM) != 0) {
		nb_buf_cleanup(&action);
		return;
	}

	if ((nb_buf_init(&meta), nb_buf_reserve(&meta, 64) ? 0 : NB_ERR_NOMEM) != 0) {
		nb_buf_cleanup(&action);
		nb_buf_cleanup(&subject);
		return;
	}

	if (tool->historical && !tool->state)
		(void)nb_buf_append_cstr(&meta, "no recorded result");
	else if (tool->interrupted)
		(void)nb_buf_append_cstr(&meta, "CANCELLED");
	else if (tool->timed_out)
		(void)nb_buf_append_cstr(&meta, "TIMEOUT");
	else if (tool->state < 0)
		(void)nb_buf_append_cstr(&meta, "FAILED");
	else if (tool->state && cli_transcript_tool_is_running(tool))
		(void)nb_buf_append_cstr(&meta, "RUNNING");

	if (tool->state && (!tool->historical || elapsed > 0)) {
		if (meta.len)
			(void)nb_buf_append_cstr(&meta, " · ");

		(void)nb_buf_printf(&meta, "%.1fs", elapsed);
	}

	int available = columns - 5 - (int)nb_utf8_width_cstr(meta.ptr);

	if (name_width > available)
		name_width = available > 0 ? available : 1;

	cli_transcript_append_clipped_text(&action, label, name_width);
	cli_transcript_append_clipped_text(
		&subject, target, columns - name_width - 5 - (int)nb_utf8_width_cstr(meta.ptr));
	(void)nb_buf_printf(row, "%s%s%s %s%s%s ", styled ? color : "",
			    cli_transcript_tool_state_marker(tool), styled ? ANSI_RESET : "",
			    styled ? ANSI_BOLD : "", action.ptr, styled ? ANSI_RESET : "");
	if (styled && tool->shell)
		(void)cli_shell_style(row, subject.ptr, 0);
	else
		(void)nb_buf_printf(row, "%s%s%s", styled ? ANSI_CYAN : "", subject.ptr,
				    styled ? ANSI_RESET : "");

	if (meta.len) {
		(void)nb_buf_printf(row, "  %s%s%s",
				    styled ? (tool->state < 0 ? color : ANSI_DIM) : "", meta.ptr,
				    styled ? ANSI_RESET : "");
	} else if (!tool->historical) {
		(void)nb_buf_append_cstr(row, "…");
	}

	nb_buf_cleanup(&meta);
	nb_buf_cleanup(&subject);
	nb_buf_cleanup(&action);
}

int cli_transcript_live_text(const struct cli_transcript_model *model, nb_buf *text, int columns,
			     int styled)
{
	const struct cli_transcript_tool *tool = cli_transcript_model_active_borrow(model);

	if (!tool || cli_agent_tools_analytics_only(tool->name))
		return 0;

	cli_transcript_tool_row(tool, text, columns, styled);
	return 1;
}

/* The command is in the header; the active body contains only progress. */
int cli_transcript_live_body(const struct cli_transcript_model *model, nb_buf *body, int columns,
			     int styled)
{
	return cli_transcript_tool_live_body(cli_transcript_model_active_borrow(model), body,
					     columns, styled);
}

int cli_transcript_tool_live_body(const struct cli_transcript_tool *tool, nb_buf *body, int columns,
				  int styled)
{
	const char *start;
	const char *end;
	int rows = 0;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - 4;

	if (!tool || cli_agent_tools_matches(tool->name))
		return 0;

	const char *stream = nb_buf_cstr(&tool->stream);
	end = stream + tool->stream.len;
	while (end > stream && end[-1] == '\n')
		end--;

	if (end == stream) {
		(void)nb_buf_printf(body, "%s    Running…%s", styled ? ANSI_DIM : "",
				    styled ? ANSI_RESET : "");
		return 1;
	}

	/* Inspect only the tail. Retain the complete sanitized stream for details. */
	start = end;
	while (start > stream) {
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
			(void)nb_buf_append_char(body, '\n');

		(void)nb_buf_append_cstr(body, styled ? ANSI_DIM : "");
		(void)cli_tree_prefix(body, NULL, 0, 1, rows != 0, 2);
		cli_transcript_append_clipped_text(body, line, width);
		(void)nb_buf_append_cstr(body, styled ? ANSI_RESET : "");
		free(line);
		rows++;
		start = newline ? newline + 1 : end;
	}

	return rows;
}

static void cli_transcript_print_inline(struct cli_application *ctx, const char *text, int columns)
{
	char *safe = nt_sanitize_dup(text, strlen(text),
						 NT_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe || columns < 1) {
		free(safe);
		return;
	}

	end = nt_terminal_advance_display_width(safe, (size_t)columns);
	if (*end)
		end = nt_terminal_advance_display_width(safe, (size_t)(columns - 1));

	cli_output_printf(ctx, "%.*s%s", (int)(end - safe), safe, *end ? "…" : "");
	free(safe);
}

/* Compact feed lines share one width budget and never interpret shell text. */
static void cli_transcript_append_feed_line(nb_buf *out, const char *prefix, const char *text,
					    const char *color, int columns, int styled, int wrap)
{
	if (columns < 8)
		return;

	if (wrap) {
		size_t indent = nb_utf8_width_cstr(prefix);
		int available = columns - (int)CLI_CONTENT_RIGHT_PADDING - (int)indent;
		size_t width = available > 0 ? (size_t)available : 1;
		nb_buf continuation NB_CLEANUP(nb_buf_cleanup) = {0};
		for (size_t i = 0; i < indent; i++)
			(void)nb_buf_append_char(&continuation, ' ');

		const char *cursor = text;
		do {
			const char *end = nt_terminal_advance_display_width(cursor, width);
			if (end == cursor && *cursor)
				end += nb_utf8_next_codepoint_len(cursor, strnlen(cursor, 4));

			(void)nb_buf_printf(out, "%s%s%.*s%s\n", styled ? color : "", prefix,
				(int)(end - cursor), cursor, styled ? ANSI_RESET : "");
			cursor = end;
			prefix = nb_buf_cstr(&continuation);
		} while (*cursor);

		return;
	}

	(void)nb_buf_printf(out, "%s%s", styled ? color : "", prefix);
	cli_transcript_append_clipped_text(out, text,
					   columns - (int)CLI_CONTENT_RIGHT_PADDING -
						   (int)nb_utf8_width_cstr(prefix));
	(void)nb_buf_printf(out, "%s\n", styled ? ANSI_RESET : "");
}

#define CLI_TRANSCRIPT_TOOL_PREVIEW_LINES 3

struct cli_transcript_patch_position {
	size_t old_line, new_line;
	int old_known, new_known, in_hunk;
};

static void cli_transcript_patch_numbers(struct cli_transcript_patch_position *position,
	const char *line, nb_buf *prefix)
{
	size_t old_line = 0, new_line = 0, old_count = 0, new_count = 0;
	int end = 0;
	if (sscanf(line, "@@ -%zu,%zu +%zu,%zu @@%n", &old_line, &old_count,
		&new_line, &new_count, &end) == 4 && end) {
		*position = (struct cli_transcript_patch_position){old_line, new_line, 1, 1, 1};
		return;
	}

	if (!strncmp(line, "*** Add File: ", 14)) {
		*position = (struct cli_transcript_patch_position){0, 1, 0, 1, 1};
		return;
	}

	if (!strncmp(line, "*** ", 4) || !strncmp(line, "@@", 2)) {
		*position = (struct cli_transcript_patch_position){.in_hunk = line[0] == '@'};
		return;
	}

	if (!position->in_hunk || (*line != '-' && *line != '+' && *line != ' '))
		return;

	char old_number[32] = "", new_number[32] = "";
	if (*line != '+') {
		if (position->old_known)
			(void)snprintf(old_number, sizeof(old_number), "%zu", position->old_line);
		else
			(void)snprintf(old_number, sizeof(old_number), "?");

		if (position->old_line < SIZE_MAX)
			position->old_line++;
		else
			position->old_known = 0;
	}

	if (*line != '-') {
		if (position->new_known)
			(void)snprintf(new_number, sizeof(new_number), "%zu", position->new_line);
		else
			(void)snprintf(new_number, sizeof(new_number), "?");

		if (position->new_line < SIZE_MAX)
			position->new_line++;
		else
			position->new_known = 0;
	}

	if (prefix)
		(void)nb_buf_printf(prefix, "%5s %5s ", old_number, new_number);
}

static void cli_transcript_append_patch_input(nb_buf *text, const struct cli_transcript_tool *tool)
{
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->args);
	const char *line = cli_transcript_json_string(args, "input");
	while (*line) {
		size_t length = strcspn(line, "\r\n");
		nb_str value = nb_str_from_parts(line, length);
		if (!nb_str_eq(value, NB_STR_LIT("*** Begin Patch")) &&
		    !nb_str_eq(value, NB_STR_LIT("*** End Patch")) &&
		    !nb_str_eq(value, NB_STR_LIT("*** End of File"))) {
			(void)nb_buf_append(text, line, length);
			(void)nb_buf_append_char(text, '\n');
		}

		line += length;
		if (*line == '\r')
			line++;

		if (*line == '\n')
			line++;
	}
}

/* Prefer human-readable payloads. The complete original stays in turn details. */
static void cli_transcript_tool_preview_text(const struct cli_transcript_tool *tool,
					     const cJSON *data, nb_buf *text, int inline_output)
{
	const char *value = tool->error;
	const cJSON *content;

	if (inline_output && !value[0] && strcmp(tool->name, "apply_patch") == 0) {
		if (tool->patch_preview[0])
			(void)nb_buf_append_cstr(text, tool->patch_preview);
		else
			cli_transcript_append_patch_input(text, tool);

		if (text->len)
			return;
	}

	if (inline_output && tool->shell) {
		const char *output = cli_transcript_json_string(data, "stdout");
		const char *error = cli_transcript_json_string(data, "stderr");
		if (*output || *error) {
			(void)nb_buf_append_cstr(text, output);
			if (*error && text->len && text->ptr[text->len - 1] != '\n')
				(void)nb_buf_append_char(text, '\n');

			(void)nb_buf_append_cstr(text, error);
			return;
		}
	}

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
		(void)nb_buf_append_cstr(text, value);
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
				(void)nb_buf_append_char(text, '\n');

			(void)nb_buf_append_cstr(text, value);
		}
	}

	if (!text->len && tool->stream.len)
		(void)nb_buf_append_cstr(text, nb_buf_cstr(&tool->stream));

	if (!text->len) {
		value = !data && tool->result[0] ? tool->result
			: tool->interrupted	 ? "Tool interrupted"
			: tool->timed_out	 ? "Command timed out"
			: tool->state < 0	 ? "Tool failed"
			: tool->summary[0] && strcmp(tool->summary, "exit 0") != 0 ? tool->summary
										   : "Completed";
		(void)nb_buf_append_cstr(text, value);
	}
}

static void cli_transcript_append_tool_preview(nb_buf *out, const struct cli_transcript_tool *tool,
					       int columns, int styled, int expanded,
					       struct cli_transcript_tool_preview *preview)
{
	cJSON *json = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	char *safe;
	char *line;
	size_t total = 0;
	size_t shown = 0;
	const char *color = !tool->interrupted && (tool->timed_out || tool->state < 0)
		? CLI_FEED_FAILED_STYLE : ANSI_DIM;

	if (!cJSON_IsObject(data))
		data = json;

	if ((nb_buf_init(&text), nb_buf_reserve(&text, 128) ? 0 : NB_ERR_NOMEM) != 0) {
		cJSON_Delete(json);
		return;
	}

	int live = preview && cli_transcript_tool_is_running(tool);
	if (live)
		(void)nb_buf_append_str(&text, nb_buf_str(&tool->stream));
	else if (!cli_agent_tools_preview(tool, &text))
		cli_transcript_tool_preview_text(tool, data, &text, preview != NULL);

	safe = nt_sanitize_dup(nb_buf_cstr(&text), text.len,
					   NT_TEXT_MULTILINE, NULL);
	nb_buf_cleanup(&text);
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

	if (preview)
		preview->hidden_lines = total > CLI_TRANSCRIPT_TOOL_PREVIEW_LINES
			? total - CLI_TRANSCRIPT_TOOL_PREVIEW_LINES : 0;

	if (live && !expanded && total > CLI_TRANSCRIPT_TOOL_PREVIEW_LINES) {
		for (size_t i = 0; i < total - CLI_TRANSCRIPT_TOOL_PREVIEW_LINES; i++)
			line = strchr(line, '\n') + 1;
	}

	int patch = preview && tool->state > 0 && !tool->error[0] &&
		!strcmp(tool->name, "apply_patch");
	struct cli_transcript_patch_position position = {0};
	if (patch) {
		size_t skipped = 0;
		for (char *cursor = line; cursor && *cursor;) {
			if (*cursor == '+' || *cursor == '-') {
				size_t remaining = total - skipped;
				preview->hidden_lines = total -
					(remaining < CLI_TRANSCRIPT_TOOL_PREVIEW_LINES
						? remaining : CLI_TRANSCRIPT_TOOL_PREVIEW_LINES);
				if (!expanded)
					line = cursor;

				break;
			}

			char *newline = strchr(cursor, '\n');
			if (!expanded)
				cli_transcript_patch_numbers(&position, cursor, NULL);

			cursor = newline ? newline + 1 : NULL;
			skipped++;
		}
	}

	while (*line && (expanded || shown < CLI_TRANSCRIPT_TOOL_PREVIEW_LINES)) {
		char *newline = strchr(line, '\n');

		if (newline)
			*newline = '\0';

		nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
		if ((nb_buf_init(&prefix), nb_buf_reserve(&prefix, 16) ? 0 : NB_ERR_NOMEM) != 0)
			break;

		cli_tree_prefix(&prefix, NULL, 0, 1, shown != 0, 2);
		if (patch)
			cli_transcript_patch_numbers(&position, line, &prefix);

		int width = columns - (int)CLI_CONTENT_RIGHT_PADDING -
			(int)nb_utf8_width_cstr(nb_buf_cstr(&prefix));
		if (preview && shown < CLI_TRANSCRIPT_TOOL_PREVIEW_LINES &&
		    (width < 1 || *nt_terminal_advance_display_width(line, (size_t)width)))
			preview->clipped = 1;

		const char *style = patch && *line == '+' ? ANSI_GREEN
			: patch && *line == '-' ? ANSI_RED : color;
		cli_transcript_append_feed_line(out, nb_buf_cstr(&prefix), line, style, columns,
						styled, expanded);
		nb_buf_cleanup(&prefix);
		shown++;
		if (!newline)
			break;

		line = newline + 1;
	}

	if (preview && !expanded)
		preview->hidden_lines = total - shown;

	if (total > shown && !preview) {
		nb_buf hint NB_CLEANUP(nb_buf_cleanup) = {0};

		if ((nb_buf_init(&hint), nb_buf_reserve(&hint, 64) ? 0 : NB_ERR_NOMEM) == 0) {
			(void)nb_buf_printf(&hint, "+ %zu %s (Ctrl+T to expand)", total - shown,
					    total - shown == 1 ? "line" : "lines");
			cli_transcript_append_feed_line(out, "    ", nb_buf_cstr(&hint), ANSI_DIM,
							columns, styled, 0);
			nb_buf_cleanup(&hint);
		}
	}

	free(safe);
}

static int cli_transcript_append_lines(nb_buf *out, const char *text, int columns, int styled)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = nt_sanitize_dup(
		text, strlen(text), NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	size_t width = columns > 6 ? (size_t)(columns - 6) : 1u;
	const char *cursor = safe;
	int rc = 0;
	while (*cursor && rc == 0) {
		const char *newline = strchr(cursor, '\n');
		const char *end = nt_terminal_advance_display_width(cursor, width);
		if (newline && newline < end)
			end = newline;

		if (end == cursor && *cursor != '\n')
			end += nb_utf8_next_codepoint_len(cursor, strnlen(cursor, 4));

		const char *color = *cursor == '+' ? ANSI_GREEN : *cursor == '-' ? ANSI_RED : ANSI_DIM;
		rc = (nb_buf_printf(out, "    %s%.*s%s\n", styled ? color : "", (int)(end - cursor),
				    cursor, styled ? ANSI_RESET : "")
			      ? 0
			      : NB_ERR_NOMEM);
		cursor = *end == '\n' ? end + 1 : end;
	}

	return rc;
}

static int cli_transcript_append_value(nb_buf *out, const cJSON *value, int depth, int columns,
				       int styled)
{
	if (cJSON_IsString(value))
		return cli_transcript_append_lines(out, value->valuestring, columns, styled);

	if (depth < 32 && (cJSON_IsObject(value) || cJSON_IsArray(value))) {
		const cJSON *child;
		cJSON_ArrayForEach (child, value) {
			int rc = child->string ? cli_transcript_append_lines(out, child->string,
				columns, styled) : 0;
			if (rc == 0)
				rc = cli_transcript_append_value(out, child, depth + 1, columns, styled);

			if (rc != 0)
				return rc;
		}

		return 0;
	}

	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_Print(value);
	return json ? cli_transcript_append_lines(out, json, columns, styled) : NB_ERR_NOMEM;
}

static int cli_transcript_append_payload(nb_buf *out, const char *text, int columns, int styled)
{
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(text);
	return value ? cli_transcript_append_value(out, value, 0, columns, styled) :
		cli_transcript_append_lines(out, text, columns, styled);
}

int cli_transcript_tool_header_append(nb_buf *out, const struct cli_transcript_tool *tool,
	int columns, int styled)
{
	if (!out || !tool)
		return NB_ERR_INVAL;

	cli_transcript_tool_row(tool, out, columns, styled);
	return nb_buf_status(out);
}

int cli_transcript_tool_append(nb_buf *out, const struct cli_transcript_tool *tool, int columns,
			       int styled, int details)
{
	if (!out || !tool)
		return NB_ERR_INVAL;

	if (!details && cli_agent_tools_analytics_only(tool->name))
		return 0;

	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&document), nb_buf_reserve(&document, 256) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;

	cli_transcript_tool_row(tool, &document, columns, styled);
	rc = (nb_buf_append_char(&document, '\n') ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && !details && tool->state)
		cli_transcript_append_tool_preview(&document, tool, columns, styled, 0, NULL);

	if (rc == 0 && details) {
		if (*tool->args) {
			rc = (nb_buf_append_cstr(&document, "    Arguments\n") ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = cli_transcript_append_payload(&document, tool->args, columns, styled);
		}

		if (rc == 0 && tool->stream.len)
			rc = cli_transcript_append_lines(&document, nb_buf_cstr(&tool->stream),
							 columns, styled);

		if (rc == 0 && *tool->result) {
			rc = (nb_buf_append_cstr(&document, "    Result\n") ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = cli_transcript_append_payload(&document, tool->result, columns, styled);
		}

		if (rc == 0 && *tool->error)
			rc = cli_transcript_append_lines(&document, tool->error, columns, styled);
	}

	if (rc == 0)
		rc = nb_buf_status(&document);

	if (rc == 0)
		rc = cli_messages_append_rendered(out, nb_buf_cstr(&document), document.len,
						  columns > 1 ? columns - 1 : 1, styled);

	return rc;
}

int cli_transcript_tool_output_append(nb_buf *out, const struct cli_transcript_tool *tool,
	int columns, int styled, int expanded, struct cli_transcript_tool_preview *preview)
{
	if (!out || !tool || !preview)
		return NB_ERR_INVAL;

	*preview = (struct cli_transcript_tool_preview){0};
	if (cli_agent_tools_analytics_only(tool->name))
		return 0;

	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	cli_transcript_tool_row(tool, &document, columns, styled);
	(void)nb_buf_append_char(&document, '\n');
	if (cli_transcript_tool_is_running(tool)) {
		if (!tool->stream.len || cli_agent_tools_matches(tool->name))
			(void)cli_transcript_tool_live_body(tool, &document, columns, styled);
		else
			cli_transcript_append_tool_preview(&document, tool, columns, styled,
				expanded, preview);
	} else if (tool->state)
		cli_transcript_append_tool_preview(&document, tool, columns, styled, expanded,
			preview);

	int rc = nb_buf_status(&document);
	return rc ? rc : cli_messages_append_rendered(out, nb_buf_cstr(&document), document.len,
		columns > 1 ? columns - 1 : 1, styled);
}

void cli_transcript_render_tool(struct cli_application *ctx, const struct cli_transcript_tool *tool,
				int compact)
{
	if (compact && cli_agent_tools_analytics_only(tool->name))
		return;

	if (compact) {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_TOOL, .content.tool = tool,
		};
		(void)cli_feed_block_write(ctx, &block);
		return;
	}

	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};

	if ((nb_buf_init(&line), nb_buf_reserve(&line, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return;

	cli_transcript_tool_row(tool, &line, cli_transcript_columns(), 1);
	(void)nb_buf_append_char(&line, '\n');
	struct nt_view_insets margin = {.top = 1};
	int gap = cli_output_block_gap_margin(margin, ctx->output.trailing_newlines);
	if (ctx->output.capture && !ctx->output.capture_history)
		gap = 1;

	for (int i = 0; i < gap; i++)
		cli_output_putchar(ctx, '\n');

	cli_output_printf(ctx, "%s", nb_buf_cstr(&line));
	nb_buf_cleanup(&line);
}

/* Details are complete terminal-safe text, including multiline strings inside
 * JSON envelopes. No tree depth, item count, or byte truncation is applied. */
static void cli_transcript_print_lines(struct cli_application *ctx, const char *text)
{
	char *safe = nt_sanitize_dup(text, strlen(text), NT_TEXT_MULTILINE,
						 NULL);
	const char *line = safe;
	int width = cli_transcript_columns() - 5;

	if (!safe)
		return;

	if (width < 1)
		width = 1;

	while (*line) {
		const char *end = strchr(line, '\n');
		const char *wrap = nt_terminal_advance_display_width(line, (size_t)width);
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
			end = line + nb_utf8_next_codepoint_len(line, strnlen(line, 4));

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
			nb_buf command NB_CLEANUP(nb_buf_cleanup) = {0};

			if ((nb_buf_init(&command),
			     nb_buf_reserve(&command, 128) ? 0 : NB_ERR_NOMEM) == 0) {
				(void)cli_shell_style(&command,
						      cli_transcript_json_string(json, "command"),
						      cli_transcript_columns() - 6);
				cli_output_printf(ctx, "    $ %s\n", nb_buf_cstr(&command));
				nb_buf_cleanup(&command);
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
		cli_transcript_print_lines(ctx, nb_buf_cstr(&tool->stream));
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
				  const struct cli_transcript_model *tr, nb_buf *page)
{
	if (!tr)
		return NB_ERR_NOT_FOUND;

	return cli_transcript_details_append(page, tr, cli_transcript_columns(),
		cli_output_color_enabled(ctx), 0);
}

/* Restore presentation state only; historical calls never enter the tool runtime. */

/* Return one for events fully presented here, zero for the usual presenter. */

void cli_transcript_render_stream(struct cli_application *ctx,
				  const struct cli_transcript_tool *tool, size_t offset)
{
	if (offset <= tool->stream.len)
		cli_transcript_print_lines(ctx, nb_buf_cstr(&tool->stream) + offset);
}
