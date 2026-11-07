#include "sapi/cli/models/transcript.h"
#include "util/error.h"
#include "session_history.h"
#include "util/json.h"
#include <string.h>

#include <stdlib.h>
#include <stdio.h>

int cli_transcript_model_create(struct cli_transcript_model **out)
{
	struct cli_transcript_model *model;
	struct cli_transcript_tool tool;
	struct cli_transcript_reasoning reasoning;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	model = calloc(1, sizeof(*model));
	if (!model)
		return -ENOMEM;
	model->arena = arena_create(BUFSIZ);
	if (!model->arena) {
		free(model);
		return -ENOMEM;
	}
	rc = morph_array_init(&model->tools, MORPH_ARRAY_INIT_CAP, sizeof(tool));
	if (!rc)
		rc = morph_array_init(&model->reasoning, MORPH_ARRAY_INIT_CAP, sizeof(reasoning));
	if (rc) {
		cli_transcript_model_destroy(model);
		return rc;
	}
	*out = model;
	return 0;
}

void cli_transcript_model_destroy(struct cli_transcript_model *model)
{
	struct cli_transcript_reasoning *reasoning;

	if (!model)
		return;
	morph_array_foreach (reasoning, &model->reasoning, struct cli_transcript_reasoning)
		morph_buf_cleanup(&reasoning->text);
	morph_array_cleanup(&model->reasoning);
	morph_array_cleanup(&model->tools);
	arena_destroy(model->arena);
	free(model);
}

static const char *cli_transcript_save_text(struct cli_transcript_model *tr, const char *text)
{
	char *copy = arena_strdup(tr->arena, text ? text : "");

	if (!copy)
		tr->allocation_failed = 1;
	return copy ? copy : "";
}

static struct cli_transcript_tool *cli_transcript_find_tool(struct cli_transcript_model *tr,
							    const struct json *data)
{
	const char *id = json_str_or(json_member(data, "tool_call_id"), NULL, "");
	const char *name = json_str_or(json_member(data, "tool"), NULL, "");

	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct cli_transcript_tool *tool = morph_array_get(&tr->tools, i - 1);

		if (id[0] ? strcmp(id, tool->id) == 0 : (!name[0] || strcmp(name, tool->name) == 0))
			return tool;
	}
	return NULL;
}

static const char *cli_transcript_find_process_command(struct cli_transcript_model *tr,
						       const char *session_id)
{
	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct cli_transcript_tool *candidate = morph_array_get(&tr->tools, i - 1);
		struct json *json;
		const struct json *data;
		const char *candidate_id;

		if (strcmp(candidate->name, "exec") != 0 || !candidate->result[0])
			continue;
		json = json_parse_prefix(candidate->result, NULL, NULL);
		data = json_member(json, "data");
		if (!json_is_object_value(data))
			data = json;
		candidate_id = json_str_or(json_member(data, "session_id"), NULL, "");
		if (strcmp(candidate_id, session_id) == 0) {
			json_free(json);
			return candidate->subject;
		}
		json_free(json);
	}
	return "";
}

int cli_transcript_model_reasoning_append(struct cli_transcript_model *tr, const char *text,
					  size_t len, int begin)
{
	struct cli_transcript_reasoning *reasoning;
	int rc;

	if (!tr || (!text && len))
		return -EINVAL;
	if (begin || !tr->reasoning.nelts) {
		reasoning = morph_array_push(&tr->reasoning);
		if (!reasoning)
			return -ENOMEM;
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

static int cli_transcript_add_tool(struct cli_transcript_model *tr, const struct json *data,
				   double now)
{
	const struct json *args = json_member(data, "args");
	struct cli_transcript_tool *tool = morph_array_push(&tr->tools);
	char *json;
	const char *subject = "";
	static const char *const keys[] = {"command", "file_path", "dir_path", "path",
					   "query",   "pattern",   "url"};

	if (!tool)
		return -ENOMEM;
	memset(tool, 0, sizeof(*tool));
	tool->id = cli_transcript_save_text(
		tr, json_str_or(json_member(data, "tool_call_id"), NULL, ""));
	tool->name = cli_transcript_save_text(tr, json_str_or(json_member(data, "tool"), NULL, ""));
	json = args ? json_stringify(args) : NULL;
	tool->args = cli_transcript_save_text(tr, json ? json : "{}");
	free(json);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		subject = json_str_or(json_member(args, keys[i]), NULL, "");
		if (subject[0])
			break;
	}
	tool->subject = cli_transcript_save_text(tr, subject);
	tool->result = "";
	tool->error = "";
	tool->summary = "";
	tool->patch_preview = "";
	if (strcmp(tool->name, "activate_skill") == 0) {
		tool->subject = cli_transcript_save_text(
			tr, json_str_or(json_member(args, "name"), NULL, ""));
	}
	tool->shell = strcmp(tool->name, "exec") == 0 || strcmp(tool->name, "process") == 0;
	if (strcmp(tool->name, "process") == 0) {
		tool->process_poll =
			strcmp(json_str_or(json_member(args, "action"), NULL, ""), "poll") == 0;
		const char *session_id = json_str_or(json_member(args, "session_id"), NULL, "");
		const char *command = cli_transcript_find_process_command(tr, session_id);
		morph_buf_t fallback;

		if (command[0]) {
			tool->subject = command;
		} else if (morph_buf_init(&fallback, 64) == 0) {
			(void)morph_buf_printf(&fallback, "session %s", session_id);
			tool->subject = cli_transcript_save_text(tr, morph_buf_cstr(&fallback));
			morph_buf_cleanup(&fallback);
		}
	}
	if (strcmp(tool->name, "apply_patch") == 0) {
		const char *input = json_str_or(json_member(args, "input"), NULL, "");
		const char *path = strstr(input, " File: ");
		morph_buf_t patch;
		size_t added = 0;
		size_t removed = 0;

		if (morph_buf_init(&patch, 128) == 0) {
			if (path) {
				path += strlen(" File: ");
				(void)morph_buf_append(&patch, path, strcspn(path, "\r\n"));
				tool->subject =
					cli_transcript_save_text(tr, morph_buf_cstr(&patch));
			}
			for (const char *p = input; *p; p++) {
				if (p == input || p[-1] == '\n') {
					added += *p == '+';
					removed += *p == '-';
				}
			}
			morph_buf_reset(&patch);
			(void)morph_buf_printf(&patch, "+%zu -%zu", added, removed);
			tool->summary = cli_transcript_save_text(tr, morph_buf_cstr(&patch));
			morph_buf_cleanup(&patch);
		}
	}
	tool->started = now;
	utf8_terminal_sanitizer_init(&tool->sanitizer, UTF8_TERMINAL_TEXT_MULTILINE);
	if (morph_buf_init_arena(&tool->stream, tr->arena, 128) != 0)
		return -ENOMEM;
	if (tr->allocation_failed)
		return -ENOMEM;
	return 0;
}

static void cli_transcript_summarize_result(struct cli_transcript_model *tr,
					    struct cli_transcript_tool *tool)
{
	struct json *json = json_parse_prefix(tool->result, NULL, NULL);
	const struct json *data = json_member(json, "data");
	const struct json *error = json_member(json, "error");
	const struct json *value;
	morph_buf_t summary;

	if (!json)
		return;
	if (!json_is_object_value(data))
		data = json;
	if (json_is_false(json_member(json, "ok")) || json_is_object_value(error) ||
	    json_is_string_value(error))
		tool->state = -1;
	if (json_is_object_value(error)) {
		const char *message = json_str_or(json_member(error, "message"), NULL, "");

		if (message[0])
			tool->error = cli_transcript_save_text(tr, message);
	} else if (json_is_string_value(error)) {
		tool->error = cli_transcript_save_text(tr, json_string_value(error));
	}
	if (morph_buf_init(&summary, 64) != 0) {
		json_free(json);
		return;
	}
	value = json_member(data, "duration_ms");
	if (tool->shell && json_is_number_value(value) && json_number_value(value) >= 0)
		tool->started = tool->ended - json_number_value(value) / 1000.0;
	tool->process_running =
		tool->process_poll &&
		strcmp(json_str_or(json_member(data, "status"), NULL, ""), "running") == 0;
	tool->timed_out =
		strcmp(json_str_or(json_member(data, "status"), NULL, ""), "timed_out") == 0 ||
		json_is_true(json_member(data, "timed_out"));
	if (tool->timed_out)
		tool->state = -1;
	value = json_member(data, "exit_code");
	if (json_is_number_value(value)) {
		(void)morph_buf_printf(&summary, "exit %d", json_int_value(value));
		if (json_int_value(value) != 0) {
			tool->state = -1;
			tool->error = cli_transcript_save_text(
				tr, json_str_or(json_member(data, "stderr"), NULL, ""));
			if (!tool->error[0])
				tool->error =
					cli_transcript_save_text(tr, morph_buf_cstr(&summary));
		}
	} else if (json_is_string_value(value = json_member(data, "status"))) {
		(void)morph_buf_puts(&summary, json_string_value(value));
	} else if (json_is_number_value(value = json_member(data, "returned_lines"))) {
		(void)morph_buf_printf(&summary, "%d lines", json_int_value(value));
	} else if (json_is_array_value(value = json_member(data, "entries"))) {
		(void)morph_buf_printf(&summary, "%d entries", json_count(value));
	}
	if (tool->state > 0 && strcmp(tool->name, "activate_skill") == 0)
		(void)morph_buf_puts(&summary, "loaded");
	if (strcmp(tool->name, "tasks") == 0) {
		const struct json *id = json_member(data, "id");
		const struct json *tasks = json_member(data, "tasks");
		const struct json *task = json_at(tasks, 0);
		const char *title = json_str_or(json_member(task, "title"), NULL, "");
		morph_buf_t subject;

		if (json_is_number_value(id) && json_number_value(id) > 0 &&
		    morph_buf_init(&subject, 64) == 0) {
			(void)morph_buf_printf(&subject, "#%lld%s%s",
					       (long long)json_number_value(id),
					       title[0] ? " " : "", title);
			tool->subject = cli_transcript_save_text(tr, morph_buf_cstr(&subject));
			morph_buf_cleanup(&subject);
		}
	}
	if (summary.len)
		tool->summary = cli_transcript_save_text(tr, morph_buf_cstr(&summary));
	morph_buf_cleanup(&summary);
	json_free(json);
}

int cli_transcript_model_restore(struct cli_transcript_model *tr, const struct json *call,
				 const struct model_history_item *result, size_t *tool_index)
{
	const char *name = json_str_or(json_member(call, "name"), NULL, "");
	const char *id = json_str_or(json_member(call, "tool_call_id"), NULL, "");
	const char *input = json_str_or(json_member(call, "input"), NULL, "");
	const char *arguments = json_str_or(json_member(call, "arguments"), NULL, "");
	struct json *data;
	struct json *args;
	int rc;

	if (!tr || !tool_index)
		return -EINVAL;
	*tool_index = SIZE_MAX;
	if (!name[0])
		return 0;
	data = json_object();
	args = json_parse_prefix(arguments, NULL, NULL);
	if (!args) {
		args = json_object();
		if (args && !json_add_string(args, "input", input)) {
			json_free(args);
			args = NULL;
		}
	}
	if (!data || !args || !json_add_string(data, "tool", name) ||
	    !json_add_string(data, "tool_call_id", id)) {
		json_free(data);
		json_free(args);
		return -ENOMEM;
	}
	if (json_object_add(data, "args", args) != 0) {
		json_free(data);
		json_free(args);
		return -ENOMEM;
	}
	/* No patch previews against today's files, and no synthetic live timing. */
	rc = cli_transcript_add_tool(tr, data, 0);
	json_free(data);
	if (rc)
		return rc;
	struct cli_transcript_tool *tool = morph_array_get(&tr->tools, tr->tools.nelts - 1);

	tool->historical = 1;
	tool->started = 0;
	tool->ended = 0;
	if (result) {
		struct json *metadata =
			result->payload_json ? json_parse_prefix(result->payload_json, NULL, NULL)
					     : NULL;
		const struct json *original = json_member(metadata, "original_content");
		const struct json *code = json_member(metadata, "error_code");

		tool->state = 1;
		tool->result = cli_transcript_save_text(tr, json_is_string_value(original)
								    ? json_string_value(original)
								    : result->content);
		cli_transcript_summarize_result(tr, tool);
		if (json_is_number_value(code) && json_int_value(code) < 0) {
			tool->state = -1;
			tool->timed_out = tool->timed_out || json_int_value(code) == -ETIMEDOUT;
			if (!tool->error[0])
				tool->error = cli_transcript_save_text(
					tr, morph_strerror(json_int_value(code)));
		}
		json_free(metadata);
	}
	*tool_index = tr->tools.nelts - 1;
	return tr->allocation_failed ? -ENOMEM : 0;
}

int cli_transcript_model_reduce(struct cli_transcript_model *tr, const struct morph_event *event,
				double now, struct cli_transcript_change *change)
{
	struct cli_transcript_tool *tool;
	const char *name;
	int rc;

	if (!tr || !event || !change)
		return -EINVAL;
	*change = (struct cli_transcript_change){0};
	name = event->name ? event->name : "";
	if (strcmp(name, "react.observation") == 0 &&
	    strcmp(json_str_or(json_member(event->data, "tool"), NULL, ""), "plan") != 0 &&
	    (tool = cli_transcript_find_tool(tr, event->data))) {
		change->kind = CLI_TRANSCRIPT_OBSERVATION;
		change->tool_index = (size_t)(tool - (struct cli_transcript_tool *)tr->tools.elts);
		if (!tool->result[0]) {
			tool->result = cli_transcript_save_text(
				tr, json_str_or(json_member(event->data, "text"), NULL, ""));
			change->observation_added = 1;
		}
		return tr->allocation_failed ? -ENOMEM : 0;
	}
	if (event->type != MORPH_EVENT_TOOL)
		return 0;
	if (strcmp(name, "tool.call") == 0) {
		rc = cli_transcript_add_tool(tr, event->data, now);
		if (rc != 0)
			return rc;
	}
	tool = cli_transcript_find_tool(tr, event->data);
	if (!tool)
		return 0;
	change->tool_index = (size_t)(tool - (struct cli_transcript_tool *)tr->tools.elts);
	if (strcmp(name, "tool.call") == 0) {
		change->kind = CLI_TRANSCRIPT_TOOL_STARTED;
		return 0;
	}
	if (strcmp(name, "tool.running") == 0) {
		change->kind = CLI_TRANSCRIPT_TOOL_RUNNING;
		return 0;
	}
	if (strcmp(name, "tool.stream.delta") == 0) {
		const char *text = json_str_or(json_member(event->data, "text"), NULL, "");

		change->kind = CLI_TRANSCRIPT_TOOL_STREAMED;
		change->stream_offset = tool->stream.len;
		return utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, text,
						   strlen(text), 0);
	}
	if (strcmp(name, "tool.result") != 0 && strcmp(name, "tool.failed") != 0 &&
	    strcmp(name, "tool.cancelled") != 0)
		return 0;
	change->kind = CLI_TRANSCRIPT_TOOL_FINISHED;
	tool->state = strcmp(name, "tool.result") == 0 ? 1 : -1;
	tool->ended = now;
	tool->result = cli_transcript_save_text(
		tr, json_str_or(json_member(event->data, "result"), NULL, ""));
	tool->error = cli_transcript_save_text(
		tr, json_str_or(json_member(event->data, "error"), NULL, ""));
	rc = utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, NULL, 0, 1);
	if (rc != 0)
		return rc;
	cli_transcript_summarize_result(tr, tool);
	return tr->allocation_failed ? -ENOMEM : 0;
}

int cli_transcript_model_set_preview(struct cli_transcript_model *tr, size_t index,
				     const char *preview)
{
	struct cli_transcript_tool *tool;

	if (!tr || !preview)
		return -EINVAL;
	tool = morph_array_get(&tr->tools, index);
	if (!tool)
		return -ENOENT;
	tool->patch_preview = cli_transcript_save_text(tr, preview);
	return tr->allocation_failed ? -ENOMEM : 0;
}

void cli_transcript_model_finish(struct cli_transcript_model *tr, double now)
{
	struct cli_transcript_tool *tool;

	if (!tr || tr->finished)
		return;
	morph_array_foreach (tool, &tr->tools, struct cli_transcript_tool) {
		if (!tool->state && !tool->historical) {
			tool->state = -1;
			tool->interrupted = 1;
			tool->ended = now;
			tool->error = "Interrupted before a result was received";
		}
	}
	tr->finished = 1;
}

const struct cli_transcript_tool *cli_transcript_model_active(const struct cli_transcript_model *tr)
{
	if (!tr)
		return NULL;
	for (size_t i = tr->tools.nelts; i > 0; i--) {
		const struct cli_transcript_tool *tool = morph_array_get(&tr->tools, i - 1);

		if (!tool->state)
			return tool;
	}
	return NULL;
}

int cli_transcript_tool_is_running(const struct cli_transcript_tool *tool)
{
	return !tool->historical && tool->state >= 0 &&
	       (!tool->state || strcmp(tool->summary, "running") == 0);
}
