#include "sapi/cli/presentation/transcript_details.h"

#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/shell_style.h"
#include "sapi/cli/presentation/transcript.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <string.h>

static const char *cli_transcript_details_string(const cJSON *value, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
	return text ? text : "";
}

static int cli_transcript_details_body(nb_buf *out, const char *text, int columns,
	int styled, const char *style)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(text, strlen(text), NT_TEXT_MULTILINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	size_t width = columns > 4 ? (size_t)(columns - 4) : 1;
	const char *cursor = safe;
	while (*cursor) {
		const char *newline = strchr(cursor, '\n');
		const char *end = nt_terminal_advance_display_width(cursor, width);
		if (newline && newline < end)
			end = newline;

		if (end == cursor && *cursor != '\n')
			end += nb_utf8_next_codepoint_len(cursor, strnlen(cursor, 4));

		if (!nb_buf_printf(out, "    %s%.*s%s\n", styled ? style : "",
			(int)(end - cursor), cursor, styled ? ANSI_RESET : ""))
			return nb_buf_status(out);

		cursor = *end == '\n' ? end + 1 : end;
	}

	return nb_buf_status(out);
}

static int cli_transcript_details_heading(nb_buf *out, const char *title, int styled)
{
	return nb_buf_printf(out, "  %s%s%s\n", styled ? ANSI_BOLD : "", title,
		styled ? ANSI_RESET : "") ? NB_OK : nb_buf_status(out);
}

static int cli_transcript_details_json(nb_buf *out, const cJSON *value, const char *fallback,
	int columns, int styled)
{
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = value ? cJSON_Print(value) : NULL;
	if (value && !json)
		return NB_ERR_NOMEM;

	nb_buf expanded NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *text = json ? json : fallback;
	for (const char *p = text; *p; p++) {
		if (*p == '\t')
			(void)nb_buf_append_cstr(&expanded, "  ");
		else
			(void)nb_buf_append_char(&expanded, *p);
	}

	int rc = nb_buf_status(&expanded);
	return rc ? rc : cli_transcript_details_body(out, nb_buf_cstr(&expanded), columns,
		styled, "");
}

static int cli_transcript_details_section(nb_buf *out, const char *title, const char *text,
	int columns, int styled, const char *style)
{
	int rc = cli_transcript_details_heading(out, title, styled);
	if (!rc)
		rc = cli_transcript_details_body(out, text, columns, styled, style);

	return rc ? rc : (nb_buf_append_char(out, '\n') ? NB_OK : nb_buf_status(out));
}

static int cli_transcript_details_arguments(nb_buf *out, cJSON *args, const char *fallback,
	int shell, int columns, int styled)
{
	int rc = cli_transcript_details_heading(out, "Arguments", styled);
	const char *command = shell ? cli_transcript_details_string(args, "command") : "";
	if (shell && !*command)
		command = cli_transcript_details_string(args, "cmd");

	if (!rc && *command) {
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_append_cstr(&rendered, "    $ ");
		rc = cli_shell_style(&rendered, command, columns > 6 ? columns - 6 : 1);
		(void)nb_buf_append_char(&rendered, '\n');
		if (!rc)
			rc = cli_messages_append_rendered(out, nb_buf_cstr(&rendered), rendered.len,
				columns, styled);

		cJSON_DeleteItemFromObjectCaseSensitive(args, "command");
		cJSON_DeleteItemFromObjectCaseSensitive(args, "cmd");
		if (!rc && args->child)
			rc = cli_transcript_details_json(out, args, fallback, columns, styled);
	} else if (!rc)
		rc = cli_transcript_details_json(out, args, fallback, columns, styled);

	return rc ? rc : (nb_buf_append_char(out, '\n') ? NB_OK : nb_buf_status(out));
}

static int cli_transcript_details_raw(nb_buf *out, const struct cli_transcript_tool *tool,
	const cJSON *args, const cJSON *result, int columns, int styled)
{
	int rc = cli_transcript_details_heading(out, "Arguments JSON", styled);
	if (!rc)
		rc = cli_transcript_details_json(out, args, tool->args, columns, styled);

	if (!rc)
		(void)nb_buf_append_char(out, '\n');

	if (!rc && tool->stream.len)
		rc = cli_transcript_details_section(out, "Stream", nb_buf_cstr(&tool->stream),
			columns, styled, "");

	if (!rc && tool->result[0]) {
		rc = cli_transcript_details_heading(out, "Result JSON", styled);
		if (!rc)
			rc = cli_transcript_details_json(out, result, tool->result,
				columns, styled);
	}

	if (!rc && tool->error[0])
		rc = cli_transcript_details_section(out, "Error", tool->error, columns,
			styled, CLI_FEED_FAILED_STYLE);

	return rc;
}

static int cli_transcript_details_patch(nb_buf *out, const struct cli_transcript_tool *tool,
	int columns, int styled)
{
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	struct cli_transcript_tool_preview preview = {0};
	int rc = cli_transcript_tool_output_append(&document, tool,
		columns + (int)CLI_CONTENT_RIGHT_PADDING, styled, 1, &preview);
	if (rc)
		return rc;

	const char *body = strchr(nb_buf_cstr(&document), '\n');
	if (body && !nb_buf_append_cstr(out, body + 1))
		return nb_buf_status(out);

	return NB_OK;
}

static int cli_transcript_details_output(nb_buf *out, const struct cli_transcript_tool *tool,
	const cJSON *result, int columns, int styled)
{
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(result, "data");
	if (!data)
		data = result;

	const char *output = cli_transcript_details_string(data, "stdout");
	const char *error = cli_transcript_details_string(data, "stderr");
	const char *content = cli_transcript_details_string(data, "content");
	int rc = NB_OK;
	if (tool->state > 0 && !strcmp(tool->name, "apply_patch")) {
		rc = cli_transcript_details_heading(out, "Changes", styled);
		if (!rc)
			rc = cli_transcript_details_patch(out, tool, columns, styled);
	} else if (tool->shell) {
		if (*output)
			rc = cli_transcript_details_section(out, "Standard output", output,
				columns, styled, "");

		if (!rc && *error)
			rc = cli_transcript_details_section(out, "Standard error", error,
				columns, styled, ANSI_RED);

		const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(data, "exit_code");
		if (!rc && cJSON_IsNumber(exit_code))
			(void)nb_buf_printf(out, "  Exit %d\n", exit_code->valueint);
		else if (!rc && !*output && !*error && tool->result[0]) {
			rc = cli_transcript_details_heading(out, "Result", styled);
			if (!rc && cJSON_IsString(data))
				rc = cli_transcript_details_body(out, data->valuestring,
					columns, styled, "");
			else if (!rc)
				rc = cli_transcript_details_json(out, data, tool->result,
					columns, styled);
		}
	} else if (!strcmp(tool->name, "file_read") &&
		   cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "content"))) {
		nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)nb_buf_append_cstr(&title, "Content");
		const cJSON *count = cJSON_GetObjectItemCaseSensitive(data, "returned_lines");
		if (cJSON_IsNumber(count))
			(void)nb_buf_printf(&title, " · %.0f returned %s", count->valuedouble,
				count->valueint == 1 ? "line" : "lines");

		rc = nb_buf_status(&title);
		if (!rc)
			rc = cli_transcript_details_section(out, nb_buf_cstr(&title), content,
				columns, styled, "");

		const cJSON *next = cJSON_GetObjectItemCaseSensitive(data, "next_byte_offset");
		if (!rc && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "truncated")) &&
		    cJSON_IsNumber(next))
			(void)nb_buf_printf(out, "  More content at byte offset %.0f\n",
				next->valuedouble);
	} else if (tool->result[0] && (!tool->error[0] || data != result)) {
		rc = cli_transcript_details_heading(out, "Result", styled);
		if (!rc && cJSON_IsString(data))
			rc = cli_transcript_details_body(out, data->valuestring,
				columns, styled, "");
		else if (!rc)
			rc = cli_transcript_details_json(out, data, tool->result, columns, styled);
	}

	if (!rc && !tool->result[0] && tool->stream.len)
		rc = cli_transcript_details_section(out, "Live output", nb_buf_cstr(&tool->stream),
			columns, styled, "");

	if (!rc && tool->error[0] && strcmp(tool->error, error))
		rc = cli_transcript_details_section(out, "Error", tool->error, columns,
			styled, CLI_FEED_FAILED_STYLE);

	if (!rc && (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stdout_truncated")) ||
		    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stderr_truncated")) ||
		    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "truncated"))))
		rc = cli_transcript_details_body(out,
			"Output truncated by the tool. See raw JSON for details.", columns,
			styled, ANSI_DIM);

	return rc ? rc : nb_buf_status(out);
}

static int cli_transcript_details_tool(nb_buf *out, const struct cli_transcript_tool *tool,
	int columns, int styled, int raw)
{
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->args);
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->result);
	int rc = cli_transcript_tool_header_append(out, tool,
		columns + (int)CLI_CONTENT_RIGHT_PADDING, styled);
	if (!rc)
		(void)nb_buf_append_char(out, '\n');

	if (!rc && raw)
		return cli_transcript_details_raw(out, tool, args, result, columns, styled);

	if (!rc && strcmp(tool->name, "apply_patch"))
		rc = cli_transcript_details_arguments(out, args, tool->args, tool->shell,
			columns, styled);

	return rc ? rc : cli_transcript_details_output(out, tool, result, columns, styled);
}

int cli_transcript_details_append(nb_buf *out, const struct cli_transcript_model *model,
	int columns, int styled, int raw)
{
	if (!out || !model || columns < 1)
		return NB_ERR_INVAL;

	size_t reasoning_index = 0;
	int rc = NB_OK;
	for (size_t i = 0; !rc && i <= model->tools.len; i++) {
		while (!rc && reasoning_index < model->reasoning.len) {
			const struct cli_transcript_reasoning *reasoning =
				nb_vec_at(&model->reasoning, reasoning_index);
			if (reasoning->after_tools > i)
				break;

			(void)nb_buf_printf(out, "%s%s Reasoning%s\n", styled ? ANSI_DIM : "",
				CLI_FEED_SYSTEM, styled ? ANSI_RESET : "");
			rc = cli_transcript_details_body(out, nb_buf_cstr(&reasoning->text),
				columns, styled, ANSI_DIM);
			(void)nb_buf_append_char(out, '\n');
			reasoning_index++;
		}

		if (!rc && i < model->tools.len) {
			rc = cli_transcript_details_tool(out, nb_vec_at(&model->tools, i), columns,
				styled, raw);
			(void)nb_buf_append_char(out, '\n');
		}
	}

	if (!model->tools.len && !model->reasoning.len)
		(void)nb_buf_append_cstr(out, "No activity yet.\n");

	return rc ? rc : nb_buf_status(out);
}
