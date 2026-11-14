#include "runtime/runtime.h"
#include "runtime/runtime_internal.h"
#include "persistence/history_store.h"
#include "nb/map.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int history_lock(struct runtime *runtime)
{
	if (!runtime)
		return NB_ERR_INVAL;
	int rc = nb_mutex_trylock(runtime->context.execution_lock);

	return rc;
}

static int history_coverage(struct runtime_context *ctx, nb_map *covered)
{
	nb_vec ids = {0};
	int rc;

	rc = (nb_vec_init(&ids, sizeof(int64_t)), nb_vec_reserve(&ids, 16) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = history_store_summary_sources(&ctx->database, ctx->current_session.id, &ids);
	for (size_t i = 0; rc == 0 && i < ids.len; i++) {
		const int64_t *id = nb_vec_at(&ids, i);
		char key[32];

		snprintf(key, sizeof(key), "%lld", (long long)*id);
		rc = nb_map_put_cstr(covered, key, covered, NULL) ? 0 : NB_ERR_NOMEM;
	}
	nb_vec_cleanup(&ids);
	return rc;
}

static cJSON *history_entry(const struct model_history_item *item, const nb_map *covered)
{
	cJSON *entry = cJSON_CreateObject();
	char key[32];
	const char *status = "OUT";
	const char *reason = "Inactive history; summary coverage is unavailable or no longer active.";

	snprintf(key, sizeof(key), "%lld", (long long)item->id);
	if (strcmp(item->kind, "background_receipt") == 0 &&
	    !agent_history_item_replayable(item)) {
		reason = "Internal memory-maintenance status; excluded from model input.";
	} else if (item->active && agent_history_item_replayable(item)) {
		status = item->truncated ? "IN / TRIMMED" : "IN";
		cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		int has_original = cJSON_IsString(cJSON_GetObjectItem(payload, "original_content"));
		reason = !item->truncated ?
			"Included in the current replay window. New input may trigger repair or compaction." :
			has_original ? "Model output was shortened; a redacted original is retained in metadata." :
			"Active replay item. Output was shortened before persistence; the original is unavailable.";
		cJSON_Delete(payload);
	} else if (!item->active && nb_map_has_cstr(covered, key)) {
		status = "VIA SUMMARY";
		reason = "Used to produce a summary represented in the active summary chain; original not sent.";
	} else if (item->active) {
		reason = "This record kind is not replayed to the model.";
	}
	if (!entry ||
	    !cJSON_AddNumberToObject(entry, "sequence_no", (double)item->sequence_no) ||
	    !cJSON_AddStringToObject(entry, "kind", item->kind) ||
	    !cJSON_AddStringToObject(entry, "role", item->role) ||
	    !cJSON_AddStringToObject(entry, "content", item->content ? item->content : "") ||
	    !cJSON_AddStringToObject(entry, "payload_json",
		item->payload_json ? item->payload_json : "") ||
	    !cJSON_AddStringToObject(entry, "tool_name", item->tool_name) ||
	    !cJSON_AddStringToObject(entry, "tool_call_id",
		item->tool_call_id ? item->tool_call_id : "") ||
	    !cJSON_AddStringToObject(entry, "turn_id", item->turn_id ? item->turn_id : "") ||
	    !cJSON_AddNumberToObject(entry, "token_count", item->token_count) ||
	    !cJSON_AddBoolToObject(entry, "active", item->active) ||
	    !cJSON_AddBoolToObject(entry, "truncated", item->truncated) ||
	    !cJSON_AddStringToObject(entry, "status", status) ||
	    !cJSON_AddStringToObject(entry, "reason", reason)) {
		cJSON_Delete(entry);
		return NULL;
	}
	return entry;
}

static int history_legacy_entries(struct runtime_context *ctx, cJSON *entries)
{
	int count = 0;
	struct message *messages = message_list(&ctx->database, ctx->current_session.id, &count);
	int rc = count < 0 ? MORPH_ERR_DB : 0;

	for (const struct message *msg = messages; msg && rc == 0; msg = msg->next) {
		struct model_history_item item = {0};
		cJSON *entry;

		item.sequence_no = msg->id;
		item.content = msg->content;
		item.token_count = msg->token_count;
		snprintf(item.kind, sizeof(item.kind), "%s", strcmp(msg->role, "user") == 0 ?
			"user_message" : "assistant_message");
		(void)nb_str_copy_to(
			nb_str_from_parts(msg->role,
					  nb_utf8_clamp_bytes(msg->role, sizeof(item.role) - 1)),
			item.role, sizeof(item.role));
		entry = history_entry(&item, NULL);
		if (!entry || !cJSON_SetValuestring(cJSON_GetObjectItem(entry, "status"), "LEGACY") ||
		    !cJSON_SetValuestring(cJSON_GetObjectItem(entry, "reason"),
			"Display-only legacy history; migrated and validated before the next turn.")) {
			cJSON_Delete(entry);
			rc = NB_ERR_NOMEM;
		} else if (!cJSON_AddItemToArray(entries, entry)) {
			cJSON_Delete(entry);
			rc = NB_ERR_NOMEM;
		}
	}
	message_free_list(messages);
	return rc;
}

int runtime_session_history_inspect_copy(struct runtime *runtime, cJSON **out)
{
	struct model_history_item *items = NULL;
	nb_map *covered NB_CLEANUP(nb_map_cleanup) = NULL;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *entries = NULL;
	int count = 0;
	int64_t tokens = 0;
	int64_t summary_tokens = 0;
	int active = 0;
	struct agent_history_diagnostic diagnostic = {0};
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	struct runtime_context *ctx = &runtime->context;

	covered = nb_map_new_with_cap(16);
	rc = covered ? 0 : NB_ERR_NOMEM;
	if (rc == 0 && ctx->current_session.id > 0)
		rc = history_coverage(ctx, covered);
	if (rc == 0 && ctx->current_session.id > 0) {
		items = model_history_list(&ctx->database, ctx->current_session.id, 0, &count);
		if (count < 0)
			rc = MORPH_ERR_DB;
	}
	root = cJSON_CreateObject();
	entries = root ? cJSON_AddArrayToObject(root, "entries") : NULL;
	if (rc == 0 && !entries)
		rc = NB_ERR_NOMEM;
	for (const struct model_history_item *item = items; item && rc == 0; item = item->next) {
		cJSON *entry = history_entry(item, covered);

		if (!entry || !cJSON_AddItemToArray(entries, entry)) {
			cJSON_Delete(entry);
			rc = NB_ERR_NOMEM;
			break;
		}
		if (item->active && agent_history_item_replayable(item)) {
			active++;
			tokens += item->token_count > 0 ? item->token_count : 0;
			if (strcmp(item->kind, "compaction_summary") == 0)
				summary_tokens += item->token_count > 0 ? item->token_count : 0;
		}
	}
	if (rc == 0 && count == 0 && ctx->current_session.id > 0)
		rc = history_legacy_entries(ctx, entries);
	if (rc == 0)
		rc = agent_history_diagnose(items, ctx->tokenizer, &diagnostic);
	if (rc == 0 &&
	    (!cJSON_AddNumberToObject(root, "active_items", active) ||
	     !cJSON_AddBoolToObject(root, "needs_repair",
				    diagnostic.dangling_calls || diagnostic.orphan_results ||
					    diagnostic.invalid_payloads) ||
	     !cJSON_AddNumberToObject(root, "history_tokens", (double)tokens) ||
	     !cJSON_AddNumberToObject(root, "summary_tokens", (double)summary_tokens) ||
	     !cJSON_AddNumberToObject(root, "context_limit",
				      ctx->tokenizer ? ctx->tokenizer->context_limit : 0) ||
	     !cJSON_AddStringToObject(root, "model", ctx->llm ? ctx->llm->model_id : "")))
		rc = NB_ERR_NOMEM;
	model_history_free_list(items);
	nb_mutex_unlock(ctx->execution_lock);
	if (rc != 0)
		return rc;
	*out = root;
	root = NULL;
	return 0;
}

int runtime_session_history_entry_input_copy(struct runtime *runtime, int64_t sequence, cJSON **out)
{
	struct model_history_item *items;
	struct model_history_item *item;
	nb_vec messages NB_CLEANUP(agent_chat_messages_cleanup) = {0};
	cJSON *snapshot NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int count = 0;
	int rc;

	if (!out || sequence <= 0)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	struct runtime_context *ctx = &runtime->context;

	items = model_history_list(&ctx->database, ctx->current_session.id, 0, &count);
	for (item = items; item && item->sequence_no != sequence; item = item->next) {}
	rc = count < 0 ? MORPH_ERR_DB : item ? 0 : NB_ERR_NOT_FOUND;
	if (rc == 0 && (!item->active || !agent_history_item_replayable(item)))
		rc = NB_ERR_NO_DATA;
	if (rc == 0) {
		struct chat_message message;

		rc = (nb_vec_init(&messages, sizeof(message)),
		      nb_vec_reserve(&messages, 4) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0) {
		struct model_history_item one = *item;

		one.next = NULL;
		rc = agent_history_messages_copy(runtime ? runtime->options.logger : NULL, &one,
						 &messages);
	}
	if (rc == 0)
		rc = model_input_snapshot_create(ctx->llm, NULL, ((void *)messages.data),
						 (int)messages.len, NULL, 0, &snapshot);
	model_history_free_list(items);
	nb_mutex_unlock(ctx->execution_lock);
	if (rc == 0) {
		*out = snapshot;
		snapshot = NULL;
	}
	return rc;
}

int runtime_session_last_request_copy(struct runtime *runtime, cJSON **out)
{
	struct history_request_snapshot snapshot NB_CLEANUP(history_request_snapshot_cleanup) = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	if (runtime->context.current_session.id <= 0) {
		nb_mutex_unlock(runtime->context.execution_lock);
		return NB_ERR_NO_DATA;
	}
	rc = history_store_request_snapshot(&runtime->context.database,
					    runtime->context.current_session.id, &snapshot);
	if (rc == 0) {
		root = cJSON_CreateObject();
		if (!root || !cJSON_AddStringToObject(root, "turn_id", snapshot.turn_id) ||
		    !cJSON_AddStringToObject(root, "provider", snapshot.provider) ||
		    !cJSON_AddStringToObject(root, "model", snapshot.model) ||
		    !cJSON_AddStringToObject(root, "input_json", snapshot.input_json) ||
		    !cJSON_AddNumberToObject(root, "created_at", (double)snapshot.created_at))
			rc = NB_ERR_NOMEM;
	}
	nb_mutex_unlock(runtime->context.execution_lock);
	if (rc != 0)
		return rc;
	*out = root;
	root = NULL;
	return 0;
}
