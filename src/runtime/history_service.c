#include "runtime/runtime_internal.h"
#include "util/strmap.h"
#include "util/arena.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int history_lock(struct runtime *runtime)
{
	if (!runtime)
		MORPH_RETURN(-EINVAL);
	int rc = pthread_mutex_trylock(&runtime->context.execution_lock);

	MORPH_RETURN(rc == 0 ? 0 : -rc);
}

static int history_coverage(struct runtime_context *ctx, morph_strmap_t *covered)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(ctx->database.handle,
		"WITH RECURSIVE covered(id) AS ("
		"SELECT s.source_item_id FROM history_summary_sources s "
		"JOIN model_history_items m ON m.id=s.summary_item_id "
		"WHERE m.session_id=? AND m.active=1 AND m.kind='compaction_summary' "
		"UNION SELECT s.source_item_id FROM history_summary_sources s "
		"JOIN covered c ON s.summary_item_id=c.id) SELECT id FROM covered",
		-1, &stmt, NULL);
	int error = 0;

	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, ctx->current_session.id);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		char key[32];

		snprintf(key, sizeof(key), "%lld", (long long)sqlite3_column_int64(stmt, 0));
		error = morph_strmap_set(covered, key, covered);
		if (error != 0)
			break;
	}
	sqlite3_finalize(stmt);
	MORPH_RETURN(error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB);
}

static cJSON *history_entry(const struct model_history_item *item,
	const morph_strmap_t *covered)
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
	} else if (!item->active && morph_strmap_contains(covered, key)) {
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
		morph_strmap_t empty = {0};
		cJSON *entry;

		item.sequence_no = msg->id;
		item.content = msg->content;
		item.token_count = msg->token_count;
		snprintf(item.kind, sizeof(item.kind), "%s", strcmp(msg->role, "user") == 0 ?
			"user_message" : "assistant_message");
		snprintf(item.role, sizeof(item.role), "%s", msg->role);
		entry = history_entry(&item, &empty);
		if (!entry || !cJSON_SetValuestring(cJSON_GetObjectItem(entry, "status"), "LEGACY") ||
		    !cJSON_SetValuestring(cJSON_GetObjectItem(entry, "reason"),
			"Display-only legacy history; migrated and validated before the next turn.")) {
			cJSON_Delete(entry);
			rc = -ENOMEM;
		} else if (!cJSON_AddItemToArray(entries, entry)) {
			cJSON_Delete(entry);
			rc = -ENOMEM;
		}
	}
	message_free_list(messages);
	MORPH_RETURN(rc);
}

int runtime_session_history_inspect(struct runtime *runtime, cJSON **out)
{
	struct model_history_item *items = NULL;
	morph_strmap_t covered = {0};
	cJSON *root = NULL;
	cJSON *entries = NULL;
	int count = 0;
	int64_t tokens = 0;
	int64_t summary_tokens = 0;
	int active = 0;
	struct agent_history_diagnostic diagnostic = {0};
	int rc;

	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	struct runtime_context *ctx = &runtime->context;

	rc = morph_strmap_init(&covered, MORPH_STRMAP_INIT_CAP);
	if (rc == 0 && ctx->current_session.id > 0)
		rc = history_coverage(ctx, &covered);
	if (rc == 0 && ctx->current_session.id > 0) {
		items = model_history_list(&ctx->database, ctx->current_session.id, 0, &count);
		if (count < 0)
			rc = MORPH_ERR_DB;
	}
	root = cJSON_CreateObject();
	entries = root ? cJSON_AddArrayToObject(root, "entries") : NULL;
	if (rc == 0 && !entries)
		rc = -ENOMEM;
	for (const struct model_history_item *item = items; item && rc == 0; item = item->next) {
		cJSON *entry = history_entry(item, &covered);

		if (!entry || !cJSON_AddItemToArray(entries, entry)) {
			cJSON_Delete(entry);
			rc = -ENOMEM;
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
	if (rc == 0 && (!cJSON_AddNumberToObject(root, "active_items", active) ||
	    !cJSON_AddBoolToObject(root, "needs_repair", diagnostic.dangling_calls ||
		 diagnostic.orphan_results || diagnostic.invalid_payloads) ||
	    !cJSON_AddNumberToObject(root, "history_tokens", (double)tokens) ||
	    !cJSON_AddNumberToObject(root, "summary_tokens", (double)summary_tokens) ||
	    !cJSON_AddNumberToObject(root, "context_limit",
		ctx->tokenizer ? ctx->tokenizer->context_limit : 0) ||
	    !cJSON_AddStringToObject(root, "model", ctx->llm ? ctx->llm->model_id : "")))
		rc = -ENOMEM;
	model_history_free_list(items);
	morph_strmap_cleanup(&covered);
	pthread_mutex_unlock(&ctx->execution_lock);
	if (rc != 0) {
		cJSON_Delete(root);
		MORPH_RETURN(rc);
	}
	*out = root;
	return 0;
}

int runtime_session_history_entry_input(struct runtime *runtime, int64_t sequence, cJSON **out)
{
	struct model_history_item *items;
	struct model_history_item *item;
	struct arena *arena = NULL;
	morph_array_t messages = {0};
	int count = 0;
	int rc;

	if (!out || sequence <= 0)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	struct runtime_context *ctx = &runtime->context;

	items = model_history_list(&ctx->database, ctx->current_session.id, 0, &count);
	for (item = items; item && item->sequence_no != sequence; item = item->next) {}
	rc = count < 0 ? MORPH_ERR_DB : item ? 0 : -ENOENT;
	if (rc == 0 && (!item->active || !agent_history_item_replayable(item)))
		rc = -ENODATA;
	if (rc == 0) {
		struct chat_message message;

		arena = arena_create(0);
		rc = arena ? morph_array_init(&messages, 4, sizeof(message)) : -ENOMEM;
	}
	if (rc == 0) {
		struct model_history_item one = *item;

		one.next = NULL;
		rc = agent_history_build_chat_messages(runtime ? runtime->options.logger : NULL,
						       &one, &messages, arena);
	}
	if (rc == 0)
		rc = model_input_snapshot(ctx->llm, NULL, messages.elts, (int)messages.nelts,
			NULL, 0, out);
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	model_history_free_list(items);
	pthread_mutex_unlock(&ctx->execution_lock);
	MORPH_RETURN(rc);
}

int runtime_session_last_request(struct runtime *runtime, cJSON **out)
{
	sqlite3_stmt *stmt = NULL;
	cJSON *root = NULL;
	int rc;

	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	struct runtime_context *ctx = &runtime->context;

	rc = sqlite3_prepare_v2(ctx->database.handle,
		"SELECT turn_id,provider,model,input_json,created_at FROM model_request_snapshots "
		"WHERE session_id=?", -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, ctx->current_session.id);
		rc = sqlite3_step(stmt);
	}
	if (rc == SQLITE_ROW) {
		root = cJSON_CreateObject();
		const char *names[] = {"turn_id", "provider", "model", "input_json"};

		for (int i = 0; i < 4; i++) {
			const char *value = (const char *)sqlite3_column_text(stmt, i);

			if (!root || !cJSON_AddStringToObject(root, names[i], value ? value : "")) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
		if (rc == SQLITE_ROW && !cJSON_AddNumberToObject(root, "created_at",
			(double)sqlite3_column_int64(stmt, 4)))
			rc = SQLITE_NOMEM;
	}
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&ctx->execution_lock);
	if (rc != SQLITE_ROW) {
		cJSON_Delete(root);
		MORPH_RETURN(rc == SQLITE_DONE ? -ENODATA : rc == SQLITE_NOMEM ? -ENOMEM : MORPH_ERR_DB);
	}
	*out = root;
	return 0;
}
