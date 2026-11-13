#include "sapi/cli/models/transcript.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "cJSON.h"
/* Database model-history API (src/session_history.h); the CLI restore service
 * lives in services/session_restore.h and no longer shares this name. */
#include "session_history.h"
#include "cJSON.h"
#include <string.h>

#include <stdlib.h>
#include <stdio.h>

static void cli_transcript_tool_cleanup(struct cli_transcript_tool *tool)
{
	free(tool->id);
	free(tool->name);
	free(tool->agent_id);
	free(tool->args);
	free(tool->subject);
	free(tool->result);
	free(tool->error);
	free(tool->summary);
	free(tool->patch_preview);
	nb_buf_cleanup(&tool->stream);
	memset(tool, 0, sizeof(*tool));
}

static int cli_transcript_tool_push_take(struct cli_transcript_model *model,
					 struct cli_transcript_tool *source)
{
	struct cli_transcript_tool *slot = nb_vec_push(&model->tools);

	if (!slot)
		return NB_ERR_NOMEM;
	*slot = *source;
	memset(source, 0, sizeof(*source));
	model->revision++;
	return 0;
}

int cli_transcript_model_create(struct cli_transcript_model **out)
{
	struct cli_transcript_model *model;
	struct cli_transcript_tool tool;
	struct cli_transcript_reasoning reasoning;
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	model = calloc(1, sizeof(*model));
	if (!model)
		return NB_ERR_NOMEM;
	rc = (nb_vec_init(&model->tools, sizeof(tool)),
	      nb_vec_reserve(&model->tools, 16) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_vec_init(&model->reasoning, sizeof(reasoning)),
		      nb_vec_reserve(&model->reasoning, 16) ? 0 : NB_ERR_NOMEM);
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
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&model->reasoning) &&
	     (reasoning = (struct cli_transcript_reasoning *)nb_vec_at(&model->reasoning, _i_));
	     _i_++)
		nb_buf_cleanup(&reasoning->text);
	nb_vec_cleanup(&model->reasoning);
	for (size_t i = 0; i < model->tools.len; i++)
		cli_transcript_tool_cleanup(nb_vec_at(&model->tools, i));
	nb_vec_cleanup(&model->tools);
	free(model);
}

static const char *cli_transcript_json_string(const cJSON *data, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

static void cli_transcript_set_text(struct cli_transcript_model *tr, char **field, const char *text)
{
	char *copy = strdup(text ? text : "");

	if (!copy) {
		tr->allocation_failed = 1;
		return;
	}
	free(*field);
	*field = copy;
	tr->revision++;
}

static struct cli_transcript_tool *cli_transcript_find_tool(struct cli_transcript_model *tr,
							    const cJSON *data)
{
	const char *id = cli_transcript_json_string(data, "tool_call_id");
	const char *name = cli_transcript_json_string(data, "tool");

	for (size_t i = tr->tools.len; i > 0; i--) {
		struct cli_transcript_tool *tool = nb_vec_at(&tr->tools, i - 1);

		if (id[0] ? strcmp(id, tool->id) == 0 : (!name[0] || strcmp(name, tool->name) == 0))
			return tool;
	}
	return NULL;
}

static const char *cli_transcript_find_process_command(struct cli_transcript_model *tr,
						       const char *session_id)
{
	for (size_t i = tr->tools.len; i > 0; i--) {
		struct cli_transcript_tool *candidate = nb_vec_at(&tr->tools, i - 1);
		cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		const cJSON *data;
		const char *candidate_id;

		if (strcmp(candidate->name, "exec") != 0 || !candidate->result[0])
			continue;
		json = cJSON_Parse(candidate->result);
		data = cJSON_GetObjectItemCaseSensitive(json, "data");
		if (!cJSON_IsObject(data))
			data = json;
		candidate_id = cli_transcript_json_string(data, "session_id");
		if (strcmp(candidate_id, session_id) == 0) {
			return candidate->subject;
		}
	}
	return "";
}

int cli_transcript_model_reasoning_append(struct cli_transcript_model *tr, const char *text,
					  size_t len, int begin)
{
	struct cli_transcript_reasoning *reasoning;
	int rc;

	if (!tr || (!text && len))
		return NB_ERR_INVAL;
	if (begin || !tr->reasoning.len) {
		reasoning = nb_vec_push(&tr->reasoning);
		if (!reasoning)
			return NB_ERR_NOMEM;
		memset(reasoning, 0, sizeof(*reasoning));
		reasoning->after_tools = tr->tools.len;
		rc = (nb_buf_init(&reasoning->text),
		      nb_buf_reserve(&reasoning->text, BUFSIZ) ? 0 : NB_ERR_NOMEM);
		if (rc != 0) {
			(void)nb_vec_pop(&tr->reasoning);
			return rc;
		}
	} else {
		reasoning = nb_vec_at(&tr->reasoning, tr->reasoning.len - 1);
	}
	if (!nb_buf_append(&reasoning->text, text, len))
		return NB_ERR_NOMEM;
	tr->revision++;
	return 0;
}

static int cli_transcript_add_tool(struct cli_transcript_model *tr, const cJSON *data, double now)
{
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(data, "args");
	struct cli_transcript_tool pending NB_CLEANUP(cli_transcript_tool_cleanup) = {0};
	struct cli_transcript_tool *tool = &pending;
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	const char *subject = "";
	static const char *const keys[] = {"command", "file_path", "dir_path", "path",
					   "query",   "pattern",   "url"};

	cli_transcript_set_text(tr, &tool->id, cli_transcript_json_string(data, "tool_call_id"));
	cli_transcript_set_text(tr, &tool->name, cli_transcript_json_string(data, "tool"));
	cli_transcript_set_text(tr, &tool->agent_id, cli_transcript_json_string(data, "agent_id"));
	json = args ? cJSON_PrintUnformatted(args) : NULL;
	cli_transcript_set_text(tr, &tool->args, json ? json : "{}");
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		subject = cli_transcript_json_string(args, keys[i]);
		if (subject[0])
			break;
	}
	/* Older transcripts may contain the removed exec parameter alias. */
	if (!subject[0] && strcmp(tool->name, "exec") == 0)
		subject = cli_transcript_json_string(args, "cmd");
	cli_transcript_set_text(tr, &tool->subject, subject);
	cli_transcript_set_text(tr, &tool->result, "");
	cli_transcript_set_text(tr, &tool->error, "");
	cli_transcript_set_text(tr, &tool->summary, "");
	cli_transcript_set_text(tr, &tool->patch_preview, "");
	if (tr->allocation_failed)
		return NB_ERR_NOMEM;
	if (strcmp(tool->name, "activate_skill") == 0) {
		cli_transcript_set_text(tr, &tool->subject,
					cli_transcript_json_string(args, "name"));
	}
	/* Deprecated process name is display-only compatibility, never a tool alias. */
	int background = strcmp(tool->name, "background") == 0 ||
			 strcmp(tool->name, "process") == 0;
	tool->shell = strcmp(tool->name, "exec") == 0 || background;
	if (background) {
		tool->process_poll =
			strcmp(cli_transcript_json_string(args, "action"), "poll") == 0;
		const char *session_id = cli_transcript_json_string(args, "session_id");
		const char *command = cli_transcript_find_process_command(tr, session_id);
		nb_buf fallback NB_CLEANUP(nb_buf_cleanup) = {0};

		if (command[0]) {
			cli_transcript_set_text(tr, &tool->subject, command);
		} else if ((nb_buf_init(&fallback),
			    nb_buf_reserve(&fallback, 64) ? 0 : NB_ERR_NOMEM) == 0) {
			(void)nb_buf_printf(&fallback, "session %s", session_id);
			cli_transcript_set_text(tr, &tool->subject, nb_buf_cstr(&fallback));
		}
	}
	if (strcmp(tool->name, "apply_patch") == 0) {
		const char *input = cli_transcript_json_string(args, "input");
		const char *path = strstr(input, " File: ");
		nb_buf patch NB_CLEANUP(nb_buf_cleanup) = {0};
		size_t added = 0;
		size_t removed = 0;

		if ((nb_buf_init(&patch), nb_buf_reserve(&patch, 128) ? 0 : NB_ERR_NOMEM) == 0) {
			if (path) {
				path += strlen(" File: ");
				(void)nb_buf_append(&patch, path, strcspn(path, "\r\n"));
				cli_transcript_set_text(tr, &tool->subject, nb_buf_cstr(&patch));
			}
			for (const char *p = input; *p; p++) {
				if (p == input || p[-1] == '\n') {
					added += *p == '+';
					removed += *p == '-';
				}
			}
			nb_buf_clear(&patch);
			(void)nb_buf_printf(&patch, "+%zu -%zu", added, removed);
			cli_transcript_set_text(tr, &tool->summary, nb_buf_cstr(&patch));
		}
	}
	tool->started = now;
	morph_terminal_sanitizer_init(&tool->sanitizer, MORPH_TERMINAL_TEXT_MULTILINE);
	if ((nb_buf_init(&tool->stream), nb_buf_reserve(&tool->stream, 128) ? 0 : NB_ERR_NOMEM) !=
	    0)
		return NB_ERR_NOMEM;
	if (tr->allocation_failed)
		return NB_ERR_NOMEM;
	return cli_transcript_tool_push_take(tr, &pending);
}

static void cli_transcript_summarize_result(struct cli_transcript_model *tr,
					    struct cli_transcript_tool *tool)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
	const cJSON *value;
	nb_buf summary NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!json)
		return;
	if (!cJSON_IsObject(data))
		data = json;
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) || cJSON_IsObject(error) ||
	    cJSON_IsString(error))
		tool->state = -1;
	if (cJSON_IsObject(error)) {
		const char *message = cli_transcript_json_string(error, "message");

		if (message[0])
			cli_transcript_set_text(tr, &tool->error, message);
	} else if (cJSON_IsString(error)) {
		cli_transcript_set_text(tr, &tool->error, error->valuestring);
	}
	if ((nb_buf_init(&summary), nb_buf_reserve(&summary, 64) ? 0 : NB_ERR_NOMEM) != 0) {
		return;
	}
	value = cJSON_GetObjectItemCaseSensitive(data, "duration_ms");
	if (tool->shell && cJSON_IsNumber(value) && value->valuedouble >= 0)
		tool->started = tool->ended - value->valuedouble / 1000.0;
	tool->process_running = tool->process_poll &&
				strcmp(cli_transcript_json_string(data, "status"), "running") == 0;
	tool->timed_out = strcmp(cli_transcript_json_string(data, "status"), "timed_out") == 0 ||
			  cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "timed_out"));
	if (tool->timed_out)
		tool->state = -1;
	const char *status = cli_transcript_json_string(data, "status");
	if (!strcmp(status, "killed") || !strcmp(status, "cancelled") ||
	    !strcmp(status, "interrupted")) {
		tool->interrupted = 1;
		tool->state = -1;
	} else if (!strcmp(status, "signaled") || !strcmp(status, "failed")) {
		tool->state = -1;
	}
	value = cJSON_GetObjectItemCaseSensitive(data, "exit_code");
	if (cJSON_IsNumber(value)) {
		(void)nb_buf_printf(&summary, "exit %d", value->valueint);
		if (value->valueint != 0) {
			tool->state = -1;
			cli_transcript_set_text(tr, &tool->error,
						cli_transcript_json_string(data, "stderr"));
			if (!tool->error[0])
				cli_transcript_set_text(tr, &tool->error, nb_buf_cstr(&summary));
		}
	} else if (cJSON_IsString(value = cJSON_GetObjectItemCaseSensitive(data, "status"))) {
		(void)nb_buf_append_cstr(&summary, value->valuestring);
	} else if (cJSON_IsNumber(
			   value = cJSON_GetObjectItemCaseSensitive(data, "returned_lines"))) {
		(void)nb_buf_printf(&summary, "%d lines", value->valueint);
	} else if (cJSON_IsArray(value = cJSON_GetObjectItemCaseSensitive(data, "entries"))) {
		(void)nb_buf_printf(&summary, "%d entries", cJSON_GetArraySize(value));
	}
	if (tool->state > 0 && strcmp(tool->name, "activate_skill") == 0)
		(void)nb_buf_append_cstr(&summary, "loaded");
	if (strcmp(tool->name, "tasks") == 0) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id");
		const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
		const cJSON *task = cJSON_GetArrayItem(tasks, 0);
		const char *title = cli_transcript_json_string(task, "title");
		nb_buf subject NB_CLEANUP(nb_buf_cleanup) = {0};

		if (cJSON_IsNumber(id) && id->valuedouble > 0 &&
		    (nb_buf_init(&subject), (nb_buf_reserve(&subject, 64) ? 0 : NB_ERR_NOMEM)) ==
			    0) {
			(void)nb_buf_printf(&subject, "#%lld%s%s", (long long)id->valuedouble,
					    title[0] ? " " : "", title);
			cli_transcript_set_text(tr, &tool->subject, nb_buf_cstr(&subject));
		}
	}
	if (summary.len)
		cli_transcript_set_text(tr, &tool->summary, nb_buf_cstr(&summary));
}

int cli_transcript_model_restore(struct cli_transcript_model *tr, const cJSON *call,
				 const struct model_history_item *result, size_t *tool_index)
{
	const char *name = cli_transcript_json_string(call, "name");
	const char *id = cli_transcript_json_string(call, "tool_call_id");
	const char *input = cli_transcript_json_string(call, "input");
	const char *arguments = cli_transcript_json_string(call, "arguments");
	cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	if (!tr || !tool_index)
		return NB_ERR_INVAL;
	*tool_index = SIZE_MAX;
	if (!name[0])
		return 0;
	data = cJSON_CreateObject();
	args = cJSON_Parse(arguments);
	if (!args) {
		args = cJSON_CreateObject();
		if (args && !cJSON_AddStringToObject(args, "input", input)) {
			cJSON_DeleteOwner(&args);
		}
	}
	if (!data || !args || !cJSON_AddStringToObject(data, "tool", name) ||
	    !cJSON_AddStringToObject(data, "tool_call_id", id)) {
		return NB_ERR_NOMEM;
	}
	if ((cJSON_AddItemToObject(data, "args", args) ? (args = NULL, NB_OK) : NB_ERR_NOMEM) !=
	    0) {
		return NB_ERR_NOMEM;
	}
	/* No patch previews against today's files, and no synthetic live timing. */
	rc = cli_transcript_add_tool(tr, data, 0);
	if (rc)
		return rc;
	struct cli_transcript_tool *tool = nb_vec_at(&tr->tools, tr->tools.len - 1);

	tool->historical = 1;
	tool->started = 0;
	tool->ended = 0;
	if (result) {
		cJSON *metadata NB_CLEANUP(cJSON_DeleteOwner) =
			result->payload_json ? cJSON_Parse(result->payload_json) : NULL;
		const cJSON *original =
			cJSON_GetObjectItemCaseSensitive(metadata, "original_content");
		const cJSON *code = cJSON_GetObjectItemCaseSensitive(metadata, "error_code");

		tool->state = 1;
		cli_transcript_set_text(tr, &tool->result,
					cJSON_IsString(original) ? original->valuestring
								 : result->content);
		cli_transcript_summarize_result(tr, tool);
		if (cJSON_IsNumber(code) && code->valueint < 0) {
			tool->state = -1;
			tool->timed_out = tool->timed_out || code->valueint == NB_ERR_TIMEOUT;
			tool->interrupted = tool->interrupted ||
					    code->valueint == NB_ERR_CANCELLED ||
					    code->valueint == NB_ERR_INTERRUPTED;
			if (!tool->error[0])
				cli_transcript_set_text(tr, &tool->error,
							nb_err_str(morph_error_descriptions(),
								   MORPH_ERROR_COUNT,
								   code->valueint));
		}
	}
	*tool_index = tr->tools.len - 1;
	return tr->allocation_failed ? NB_ERR_NOMEM : 0;
}

int cli_transcript_model_reduce(struct cli_transcript_model *tr, const struct morph_event *event,
				double now, struct cli_transcript_change *change)
{
	struct cli_transcript_tool *tool;
	const char *name;
	int rc;

	if (!tr || !event || !change)
		return NB_ERR_INVAL;
	*change = (struct cli_transcript_change){0};
	name = event->name ? event->name : "";
	if (strcmp(name, "react.observation") == 0 &&
	    strcmp(cli_transcript_json_string(event->data, "tool"), "plan") != 0 &&
	    (tool = cli_transcript_find_tool(tr, event->data))) {
		change->kind = CLI_TRANSCRIPT_OBSERVATION;
		change->tool_index =
			(size_t)(tool - (struct cli_transcript_tool *)((void *)tr->tools.data));
		if (!tool->result[0]) {
			cli_transcript_set_text(tr, &tool->result,
						cli_transcript_json_string(event->data, "text"));
			change->observation_added = 1;
		}
		return tr->allocation_failed ? NB_ERR_NOMEM : 0;
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
	change->tool_index =
		(size_t)(tool - (struct cli_transcript_tool *)((void *)tr->tools.data));
	if (strcmp(name, "tool.call") == 0) {
		change->kind = CLI_TRANSCRIPT_TOOL_STARTED;
		return 0;
	}
	if (strcmp(name, "tool.running") == 0) {
		change->kind = CLI_TRANSCRIPT_TOOL_RUNNING;
		return 0;
	}
	if (strcmp(name, "tool.stream.delta") == 0) {
		const char *text = cli_transcript_json_string(event->data, "text");

		change->kind = CLI_TRANSCRIPT_TOOL_STREAMED;
		change->stream_offset = tool->stream.len;
		rc = morph_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, text,
						  strlen(text), 0);
		if (!rc)
			tr->revision++;
		return rc;
	}
	if (strcmp(name, "tool.result") != 0 && strcmp(name, "tool.failed") != 0 &&
	    strcmp(name, "tool.cancelled") != 0)
		return 0;
	change->kind = CLI_TRANSCRIPT_TOOL_FINISHED;
	tool->state = strcmp(name, "tool.result") == 0 ? 1 : -1;
	tool->interrupted = strcmp(name, "tool.cancelled") == 0;
	tool->ended = now;
	cli_transcript_set_text(tr, &tool->result,
				cli_transcript_json_string(event->data, "result"));
	cli_transcript_set_text(tr, &tool->error, cli_transcript_json_string(event->data, "error"));
	rc = morph_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, NULL, 0, 1);
	if (rc != 0)
		return rc;
	cli_transcript_summarize_result(tr, tool);
	return tr->allocation_failed ? NB_ERR_NOMEM : 0;
}

int cli_transcript_model_set_preview(struct cli_transcript_model *tr, size_t index,
				     const char *preview)
{
	struct cli_transcript_tool *tool;

	if (!tr || !preview)
		return NB_ERR_INVAL;
	tool = nb_vec_at(&tr->tools, index);
	if (!tool)
		return NB_ERR_NOT_FOUND;
	cli_transcript_set_text(tr, &tool->patch_preview, preview);
	return tr->allocation_failed ? NB_ERR_NOMEM : 0;
}

void cli_transcript_model_finish(struct cli_transcript_model *tr, double now)
{
	struct cli_transcript_tool *tool;

	if (!tr || tr->finished)
		return;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&tr->tools) &&
			     (tool = (struct cli_transcript_tool *)nb_vec_at(&tr->tools, _i_));
	     _i_++) {
		if (!tool->state && !tool->historical) {
			tool->state = -1;
			tool->interrupted = 1;
			tool->ended = now;
			cli_transcript_set_text(tr, &tool->error,
						"Interrupted before a result was received");
		}
	}
	tr->finished = 1;
	tr->revision++;
}

const struct cli_transcript_tool *
cli_transcript_model_active_borrow(const struct cli_transcript_model *tr)
{
	if (!tr)
		return NULL;
	for (size_t i = tr->tools.len; i > 0; i--) {
		const struct cli_transcript_tool *tool = nb_vec_at(&tr->tools, i - 1);

		if (!tool->state)
			return tool;
	}
	return NULL;
}

int cli_transcript_tool_is_running(const struct cli_transcript_tool *tool)
{
	return !tool->historical && tool->state >= 0 &&
	       (!tool->state || strcmp(tool->summary, "running") == 0 ||
		strcmp(tool->summary, "starting") == 0);
}

int cli_transcript_tool_is_pending(const struct cli_transcript_tool *tool)
{
	return (tool->historical && !tool->state) ||
	       (tool->state >= 0 && !tool->interrupted &&
		(!strcmp(tool->summary, "pending") || !strcmp(tool->summary, "queued")));
}
