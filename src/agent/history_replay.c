#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/utf8.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http/client.h"

static struct chat_message *history_push_message(nb_vec *messages)
{
	struct chat_message *message = nb_vec_push(messages);

	if (message)
		memset(message, 0, sizeof(*message));
	return message;
}

static int history_add_text_message(nb_vec *messages, const char *role, const char *content)
{
	struct chat_message *message = history_push_message(messages);

	if (!message)
		return NB_ERR_NOMEM;
	message->role = strdup(role ? role : "user");
	message->content = strdup(content ? content : "");
	if (!message->role || !message->content)
		return NB_ERR_NOMEM;
	return 0;
}

static int history_tool_arguments_valid(const cJSON *arguments)
{
	cJSON *parsed;
	int valid;

	if (!cJSON_IsString(arguments) || !arguments->valuestring)
		return 0;
	parsed = cJSON_ParseWithOpts(arguments->valuestring, NULL, 1);
	valid = cJSON_IsObject(parsed);
	cJSON_Delete(parsed);
	return valid;
}

int history_tool_input_borrow(const cJSON *call, enum tool_input_kind *kind, const char **input)
{
	cJSON *kind_item;
	cJSON *value;

	if (!cJSON_IsObject(call) || !kind || !input)
		return 0;
	kind_item = cJSON_GetObjectItem(call, "input_kind");
	if (cJSON_IsString(kind_item) && kind_item->valuestring &&
	    strcmp(kind_item->valuestring, "text") == 0) {
		value = cJSON_GetObjectItem(call, "input");
		if (!cJSON_IsString(value) || !value->valuestring)
			return 0;
		*kind = TOOL_INPUT_TEXT;
		*input = value->valuestring;
		return 1;
	}
	value = cJSON_GetObjectItem(call, "arguments");
	if (!history_tool_arguments_valid(value))
		return 0;
	*kind = TOOL_INPUT_JSON;
	*input = value->valuestring;
	return 1;
}

int agent_history_tool_arguments_copy(const char *arguments, char **normalized)
{
	const char *raw = arguments ? arguments : "{}";
	cJSON *parsed;
	cJSON *fallback;
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = NULL;

	if (!normalized)
		return NB_ERR_INVAL;
	if (*normalized)
		return NB_ERR_STATE;
	parsed = cJSON_ParseWithOpts(raw, NULL, 1);
	if (cJSON_IsObject(parsed)) {
		cJSON_Delete(parsed);
		*normalized = strdup(raw);
		if (!*normalized)
			return NB_ERR_NOMEM;
		return 0;
	}
	cJSON_Delete(parsed);
	fallback = cJSON_CreateObject();
	if (!fallback)
		return NB_ERR_NOMEM;
	if (!cJSON_AddStringToObject(fallback, "_morph_invalid_arguments", raw) ||
	    !cJSON_AddStringToObject(fallback, "_morph_recovery",
				     "Reissue this tool call with a valid JSON object.")) {
		cJSON_Delete(fallback);
		return NB_ERR_NOMEM;
	}
	json = cJSON_PrintUnformatted(fallback);
	cJSON_Delete(fallback);
	if (!json)
		return NB_ERR_NOMEM;
	*normalized = strdup(json);
	return *normalized ? 0 : NB_ERR_NOMEM;
}

int agent_history_migrate_legacy_tool_call(struct tool_call *call)
{
	cJSON *arguments;
	char *normalized NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	char *owned NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!call || !call->name || strcmp(call->name, "bash_exec") != 0)
		return 0;
	char *name NB_CLEANUP(nb_charp_cleanup) = strdup("exec");
	if (!name)
		return NB_ERR_NOMEM;
	free(call->name);
	call->name = nb_charp_steal(&name);
	if (call->input_kind != TOOL_INPUT_JSON || !call->arguments)
		return 0;
	arguments = cJSON_Parse(call->arguments);
	if (!cJSON_IsObject(arguments)) {
		cJSON_Delete(arguments);
		return 0;
	}
	cJSON_DeleteItemFromObject(arguments, "timeout_seconds");
	cJSON_DeleteItemFromObject(arguments, "cwd");
	cJSON_DeleteItemFromObject(arguments, "write_paths");
	normalized = cJSON_PrintUnformatted(arguments);
	cJSON_Delete(arguments);
	if (!normalized)
		return NB_ERR_NOMEM;
	owned = strdup(normalized);
	if (!owned)
		return NB_ERR_NOMEM;
	free(call->arguments);
	call->arguments = nb_charp_steal(&owned);
	return 0;
}

static int history_add_tool_calls(struct nb_log *logger, const struct model_history_item *item,
				  nb_vec *messages)
{
	struct chat_message *message;
	cJSON *calls;
	cJSON *root;
	int count;

	root = item->payload_json ? cJSON_ParseWithOpts(item->payload_json, NULL, 1) : NULL;
	if (!root) {
		nb_log_write(logger, NB_LOG_ERROR,
			     "history replay: invalid tool payload at item %lld, sequence %lld",
			     (long long)item->id, (long long)item->sequence_no);
		return NB_ERR_PARSE;
	}
	calls = cJSON_GetObjectItem(root, "calls");
	count = cJSON_IsArray(calls) ? cJSON_GetArraySize(calls) : 0;
	if (count <= 0) {
		cJSON_Delete(root);
		return NB_ERR_PROTOCOL;
	}
	message = history_push_message(messages);
	if (!message) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
	}
	message->role = strdup("assistant");
	message->content = item->content && item->content[0] ? strdup(item->content) : NULL;
	cJSON *reasoning = cJSON_GetObjectItem(root, "reasoning_content");
	if (cJSON_IsString(reasoning) && reasoning->valuestring)
		message->reasoning_content = strdup(reasoning->valuestring);
	message->tool_calls = calloc(1, (size_t)count * sizeof(*message->tool_calls));
	if (!message->role || !message->tool_calls ||
	    (cJSON_IsString(reasoning) && !message->reasoning_content)) {
		cJSON_Delete(root);
		return NB_ERR_NOMEM;
	}
	memset(message->tool_calls, 0, (size_t)count * sizeof(*message->tool_calls));
	message->tool_call_count = count;
	for (int i = 0; i < count; i++) {
		cJSON *call = cJSON_GetArrayItem(calls, i);
		cJSON *provider_id = cJSON_GetObjectItem(call, "provider_call_id");
		cJSON *local_id = cJSON_GetObjectItem(call, "tool_call_id");
		cJSON *name = cJSON_GetObjectItem(call, "name");
		struct tool_call *tool_call = &message->tool_calls[i];
		const char *input;
		enum tool_input_kind input_kind;

		if (!cJSON_IsString(provider_id) || !cJSON_IsString(local_id) ||
		    !cJSON_IsString(name) ||
		    !history_tool_input_borrow(call, &input_kind, &input)) {
			cJSON_Delete(root);
			return NB_ERR_PROTOCOL;
		}
		/* Replayed IDs are provider-neutral Morph IDs. */
		tool_call->id = strdup(local_id->valuestring);
		tool_call->tool_call_id = strdup(local_id->valuestring);
		tool_call->name = strdup(name->valuestring);
		tool_call->input_kind = input_kind;
		tool_call->arguments = strdup(input);
		if (!tool_call->id || !tool_call->tool_call_id || !tool_call->name ||
		    !tool_call->arguments) {
			cJSON_Delete(root);
			return NB_ERR_NOMEM;
		}
		int migrate_rc = agent_history_migrate_legacy_tool_call(tool_call);

		if (migrate_rc != 0) {
			cJSON_Delete(root);
			return migrate_rc;
		}
	}
	cJSON_Delete(root);
	return 0;
}

static int history_add_tool_result(const struct model_history_item *item, nb_vec *messages)
{
	struct chat_message *message = history_push_message(messages);
	const char *call_id = item->tool_call_id ? item->tool_call_id : item->provider_call_id;

	if (!message)
		return NB_ERR_NOMEM;
	message->role = strdup("tool");
	message->content = strdup(item->content ? item->content : "");
	message->tool_call_id = strdup(call_id ? call_id : "");
	if (!message->role || !message->content || !message->tool_call_id)
		return NB_ERR_NOMEM;
	return 0;
}

static int history_add_assistant_message(const struct model_history_item *item, nb_vec *messages)
{
	int rc = history_add_text_message(messages, "assistant", item->content);
	cJSON *payload;
	const cJSON *reasoning;
	struct chat_message *message;

	if (rc != 0 || !item->payload_json || !item->payload_json[0])
		return rc;
	payload = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
	if (!payload)
		return NB_ERR_PARSE;
	reasoning = cJSON_GetObjectItemCaseSensitive(payload, "reasoning_content");
	message = nb_vec_at(messages, messages->len - 1);
	if (cJSON_IsString(reasoning)) {
		message->reasoning_content = strdup(reasoning->valuestring);
		if (!message->reasoning_content)
			rc = (NB_ERR_NOMEM);
	}
	cJSON_Delete(payload);
	return rc;
}

char *history_user_content_create(const struct model_history_item *item)
{
	cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
	const cJSON *source = cJSON_GetObjectItemCaseSensitive(payload, "source");
	const cJSON *run_id = cJSON_GetObjectItemCaseSensitive(payload, "hook_run_id");

	if (cJSON_IsString(source) && strcmp(source->valuestring, "hook") == 0 &&
	    cJSON_IsString(run_id) && run_id->valuestring[0]) {
		char *text = agent_history_hook_prompt_create(item->content, run_id->valuestring);

		cJSON_Delete(payload);
		return text;
	}
	const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(payload, "turn_outcome");
	const cJSON *status = cJSON_GetObjectItemCaseSensitive(outcome, "status");
	const cJSON *finished = cJSON_GetObjectItemCaseSensitive(outcome, "finished");
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 256) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_append_cstr(&text, item->content ? item->content : "") ? 0
										    : NB_ERR_NOMEM);
	if (rc == 0 && cJSON_IsString(status) && cJSON_IsTrue(finished)) {
		rc = (nb_buf_printf(&text, "\n\n[Historical turn execution ended: %s. ",
				    status->valuestring)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0 && strcmp(status->valuestring, "cancelled") == 0)
			rc = (nb_buf_append_cstr(&text, "Execution was stopped by the user; the "
							"request and follow-up "
							"instructions remain context for later "
							"turns. Tool side effects "
							"are not rolled back. Inspect current "
							"files or external state "
							"before repeating actions. ")
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(
				      &text, "This records execution status, not completion of a "
					     "broader objective. Later results supersede earlier "
					     "checkpoint "
					     "TODOs for this turn.]")
				      ? 0
				      : NB_ERR_NOMEM);
	}
	cJSON_Delete(payload);
	if (rc != 0) {
		nb_buf_cleanup(&text);
		return NULL;
	}
	return (nb_buf_ok(&text) ? strdup(nb_buf_cstr(&text)) : NULL);
}

int history_item_tokens(const struct model_history_item *item, struct tokenizer *tokenizer,
			const char *text)
{
	if (strcmp(item->kind, "user_message") == 0) {
		char *rendered = history_user_content_create(item);
		int tokens;

		if (!rendered)
			return NB_ERR_NOMEM;
		tokens = tokenizer_count(tokenizer, rendered);
		free(rendered);
		return tokens;
	}
	int64_t tokens = tokenizer_count(tokenizer, text ? text : "");

	if (strcmp(item->kind, "assistant_tool_calls") == 0)
		tokens += tokenizer_count(tokenizer, item->content);
	if (strcmp(item->kind, "assistant_message") == 0 && item->payload_json)
		tokens += tokenizer_count(tokenizer, item->payload_json);
	return tokens > INT_MAX ? INT_MAX : (int)tokens;
}

int agent_history_item_replayable(const struct model_history_item *item)
{
	if (!item)
		return 0;
	if (strcmp(item->kind, "background_receipt") == 0) {
		static const char prefix[] = "[Background receipt] memory_consolidation:";
		cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		const cJSON *task = cJSON_GetObjectItemCaseSensitive(payload, "task");
		int internal =
			(cJSON_IsString(task) &&
			 strcmp(task->valuestring, "memory_consolidation") == 0) ||
			(item->content && strncmp(item->content, prefix, sizeof(prefix) - 1) == 0);

		cJSON_Delete(payload);
		return !internal;
	}
	return strcmp(item->kind, "user_message") == 0 ||
	       strcmp(item->kind, "assistant_message") == 0 ||
	       strcmp(item->kind, "assistant_tool_calls") == 0 ||
	       strcmp(item->kind, "tool_result") == 0 ||
	       strcmp(item->kind, "compaction_summary") == 0;
}

static int history_build_chat_messages(struct nb_log *logger,
				       const struct model_history_item *items, nb_vec *messages)
{
	const struct model_history_item *item = items;
	int rc;

	if (!messages)
		return NB_ERR_INVAL;
	while (item) {
		if (strcmp(item->kind, "compaction_summary") == 0) {
			nb_buf snapshot NB_CLEANUP(nb_buf_cleanup) = {0};

			rc = (nb_buf_init(&snapshot),
			      nb_buf_reserve(&snapshot, 256) ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = (nb_buf_printf(
					      &snapshot,
					      "Snapshot through history sequence %lld. Later "
					      "messages and turn outcomes supersede this snapshot; "
					      "its pending work may already be resolved below.\n%s",
					      (long long)(item->sequence_no > 0
								  ? item->sequence_no - 1
								  : 0),
					      item->content ? item->content : "")
					      ? 0
					      : NB_ERR_NOMEM);
			if (rc != 0)
				return rc;
			char *reference NB_CLEANUP(nb_charp_cleanup) =
				prompt_reference_create(nb_buf_cstr(&snapshot));

			if (!reference)
				return NB_ERR_NOMEM;
			rc = history_add_text_message(messages, "assistant", reference);
			if (rc != 0)
				return rc;
			struct chat_message *message = nb_vec_at(messages, messages->len - 1);

			message->is_context_summary = 1;
		}
		item = item->next;
	}
	item = items;
	while (item) {
		if (!agent_history_item_replayable(item)) {
			item = item->next;
			continue;
		}
		if (strcmp(item->kind, "assistant_tool_calls") == 0)
			rc = history_add_tool_calls(logger, item, messages);
		else if (strcmp(item->kind, "tool_result") == 0)
			rc = history_add_tool_result(item, messages);
		else if (strcmp(item->kind, "compaction_summary") == 0)
			rc = 0;
		else if (strcmp(item->kind, "user_message") == 0) {
			char *content = history_user_content_create(item);

			if (!content)
				return NB_ERR_NOMEM;
			rc = history_add_text_message(messages, "user", content);
			free(content);
		} else if (strcmp(item->kind, "assistant_message") == 0)
			rc = history_add_assistant_message(item, messages);
		else if (strcmp(item->kind, "background_receipt") == 0)
			rc = history_add_text_message(messages, "system", item->content);
		else
			rc = 0;
		if (rc != 0)
			return rc;
		item = item->next;
	}
	return 0;
}

void agent_chat_messages_cleanup(nb_vec *messages)
{
	if (!messages)
		return;
	for (size_t i = 0; i < messages->len; i++)
		chat_message_cleanup(nb_vec_at(messages, i));
	nb_vec_cleanup(messages);
}

int agent_history_messages_copy(struct nb_log *logger, const struct model_history_item *items,
				nb_vec *messages)
{
	nb_vec candidate NB_CLEANUP(agent_chat_messages_cleanup) = {0};
	int rc;

	if (!messages)
		return NB_ERR_INVAL;
	if (messages->len)
		return NB_ERR_STATE;
	if (((void *)messages->data) && messages->elem_size != sizeof(struct chat_message))
		return NB_ERR_INVAL;
	rc = (nb_vec_init(&candidate, sizeof(struct chat_message)),
	      nb_vec_reserve(&candidate, messages->cap ? messages->cap : 64) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = history_build_chat_messages(logger, items, &candidate);
	if (rc != 0)
		return rc;
	agent_chat_messages_move(messages, &candidate);
	return 0;
}

void agent_chat_messages_move(nb_vec *out, nb_vec *source)
{
	if (!out || !source || out == source)
		return;
	agent_chat_messages_cleanup(out);
	*out = *source;
	memset(source, 0, sizeof(*source));
}
