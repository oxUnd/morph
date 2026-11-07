#include "runtime/runtime.h"
#include "runtime/runtime_internal.h"
#include "persistence/history_store.h"
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
		return -EINVAL;
	int rc = pthread_mutex_trylock(&runtime->context.execution_lock);

	return rc == 0 ? 0 : -rc;
}

static int history_coverage(struct runtime_context *ctx, morph_strmap_t *covered)
{
	morph_array_t ids = {0};
	int rc;

	rc = morph_array_init(&ids, 16, sizeof(int64_t));
	if (rc == 0)
		rc = history_store_summary_sources(&ctx->database, ctx->current_session.id, &ids);
	for (size_t i = 0; rc == 0 && i < ids.nelts; i++) {
		const int64_t *id = morph_array_get(&ids, i);
		char key[32];

		snprintf(key, sizeof(key), "%lld", (long long)*id);
		rc = morph_strmap_set(covered, key, covered);
	}
	morph_array_cleanup(&ids);
	return rc;
}

static struct json *history_entry(const struct model_history_item *item,
				  const morph_strmap_t *covered)
{
	struct json *entry = json_object();
	char key[32];
	const char *status = "OUT";
	const char *reason = "Inactive history; summary coverage is unavailable or no longer active.";

	snprintf(key, sizeof(key), "%lld", (long long)item->id);
	if (strcmp(item->kind, "background_receipt") == 0 &&
	    !agent_history_item_replayable(item)) {
		reason = "Internal memory-maintenance status; excluded from model input.";
	} else if (item->active && agent_history_item_replayable(item)) {
		status = item->truncated ? "IN / TRIMMED" : "IN";
		struct json *payload = item->payload_json
					       ? json_parse_prefix(item->payload_json, NULL, NULL)
					       : NULL;
		int has_original =
			json_is_string_value(json_member_ci(payload, "original_content"));
		reason = !item->truncated ?
			"Included in the current replay window. New input may trigger repair or compaction." :
			has_original ? "Model output was shortened; a redacted original is retained in metadata." :
			"Active replay item. Output was shortened before persistence; the original is unavailable.";
		json_free(payload);
	} else if (!item->active && morph_strmap_contains(covered, key)) {
		status = "VIA SUMMARY";
		reason = "Used to produce a summary represented in the active summary chain; original not sent.";
	} else if (item->active) {
		reason = "This record kind is not replayed to the model.";
	}
	if (!entry || !json_add_number(entry, "sequence_no", (double)item->sequence_no) ||
	    !json_add_string(entry, "kind", item->kind) ||
	    !json_add_string(entry, "role", item->role) ||
	    !json_add_string(entry, "content", item->content ? item->content : "") ||
	    !json_add_string(entry, "payload_json", item->payload_json ? item->payload_json : "") ||
	    !json_add_string(entry, "tool_name", item->tool_name) ||
	    !json_add_string(entry, "tool_call_id", item->tool_call_id ? item->tool_call_id : "") ||
	    !json_add_string(entry, "turn_id", item->turn_id ? item->turn_id : "") ||
	    !json_add_number(entry, "token_count", item->token_count) ||
	    !json_add_bool(entry, "active", item->active) ||
	    !json_add_bool(entry, "truncated", item->truncated) ||
	    !json_add_string(entry, "status", status) ||
	    !json_add_string(entry, "reason", reason)) {
		json_free(entry);
		return NULL;
	}
	return entry;
}

static int history_legacy_entries(struct runtime_context *ctx, struct json *entries)
{
	int count = 0;
	struct message *messages = message_list(&ctx->database, ctx->current_session.id, &count);
	int rc = count < 0 ? MORPH_ERR_DB : 0;

	for (const struct message *msg = messages; msg && rc == 0; msg = msg->next) {
		struct model_history_item item = {0};
		morph_strmap_t empty = {0};
		struct json *entry;

		item.sequence_no = msg->id;
		item.content = msg->content;
		item.token_count = msg->token_count;
		snprintf(item.kind, sizeof(item.kind), "%s", strcmp(msg->role, "user") == 0 ?
			"user_message" : "assistant_message");
		snprintf(item.role, sizeof(item.role), "%s", msg->role);
		entry = history_entry(&item, &empty);
		if (!entry || !json_string_update(json_member_ci(entry, "status"), "LEGACY") ||
		    !json_string_update(json_member_ci(entry, "reason"),
					"Display-only legacy history; migrated and validated "
					"before the next turn.")) {
			json_free(entry);
			rc = -ENOMEM;
		} else if (json_array_add(entries, entry) != 0) {
			json_free(entry);
			rc = -ENOMEM;
		}
	}
	message_free_list(messages);
	return rc;
}

int runtime_session_history_inspect(struct runtime *runtime, struct json **out)
{
	struct model_history_item *items = NULL;
	morph_strmap_t covered = {0};
	struct json *root = NULL;
	struct json *entries = NULL;
	int count = 0;
	int64_t tokens = 0;
	int64_t summary_tokens = 0;
	int active = 0;
	struct agent_history_diagnostic diagnostic = {0};
	int rc;

	if (!out)
		return -EINVAL;
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
	root = json_object();
	entries = root ? json_add_array(root, "entries") : NULL;
	if (rc == 0 && !entries)
		rc = -ENOMEM;
	for (const struct model_history_item *item = items; item && rc == 0; item = item->next) {
		struct json *entry = history_entry(item, &covered);

		if (!entry || json_array_add(entries, entry) != 0) {
			json_free(entry);
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
	if (rc == 0 && (!json_add_number(root, "active_items", active) ||
			!json_add_bool(root, "needs_repair",
				       diagnostic.dangling_calls || diagnostic.orphan_results ||
					       diagnostic.invalid_payloads) ||
			!json_add_number(root, "history_tokens", (double)tokens) ||
			!json_add_number(root, "summary_tokens", (double)summary_tokens) ||
			!json_add_number(root, "context_limit",
					 ctx->tokenizer ? ctx->tokenizer->context_limit : 0) ||
			!json_add_string(root, "model", ctx->llm ? ctx->llm->model_id : "")))
		rc = -ENOMEM;
	model_history_free_list(items);
	morph_strmap_cleanup(&covered);
	pthread_mutex_unlock(&ctx->execution_lock);
	if (rc != 0) {
		json_free(root);
		return rc;
	}
	*out = root;
	return 0;
}

int runtime_session_history_entry_input(struct runtime *runtime, int64_t sequence,
					struct json **out)
{
	struct model_history_item *items;
	struct model_history_item *item;
	struct arena *arena = NULL;
	morph_array_t messages = {0};
	int count = 0;
	int rc;

	if (!out || sequence <= 0)
		return -EINVAL;
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
	return rc;
}

int runtime_session_last_request(struct runtime *runtime, struct json **out)
{
	struct history_request_snapshot snapshot;
	struct arena *arena;
	struct json *root = NULL;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	rc = history_lock(runtime);
	if (rc != 0)
		return rc;
	if (runtime->context.current_session.id <= 0) {
		pthread_mutex_unlock(&runtime->context.execution_lock);
		return -ENODATA;
	}
	arena = arena_create(0);
	rc = arena ? history_store_request_snapshot(&runtime->context.database,
						    runtime->context.current_session.id, arena,
						    &snapshot)
		   : -ENOMEM;
	if (rc == 0) {
		root = json_object();
		if (!root || !json_add_string(root, "turn_id", snapshot.turn_id) ||
		    !json_add_string(root, "provider", snapshot.provider) ||
		    !json_add_string(root, "model", snapshot.model) ||
		    !json_add_string(root, "input_json", snapshot.input_json) ||
		    !json_add_number(root, "created_at", (double)snapshot.created_at))
			rc = -ENOMEM;
	}
	arena_destroy(arena);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	if (rc != 0) {
		json_free(root);
		return rc;
	}
	*out = root;
	return 0;
}
