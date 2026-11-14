#include "history_internal.h"
#include "persistence/history_store.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "nb/cleanup.h"
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

static int history_result_exists(const struct model_history_item *items, const char *local_id,
				 const char *provider_id)
{
	for (const struct model_history_item *item = items; item; item = item->next) {
		if (!item->active || strcmp(item->kind, "tool_result") != 0)
			continue;
		if (local_id && local_id[0] && item->tool_call_id && item->tool_call_id[0]) {
			if (strcmp(local_id, item->tool_call_id) == 0)
				return 1;
			continue;
		}
		if (provider_id && item->provider_call_id &&
		    strcmp(provider_id, item->provider_call_id) == 0)
			return 1;
	}
	return 0;
}

int agent_history_repair_interrupted(struct db *db, int64_t session_id)
{
	const char *interrupted = "tool error: previous execution was interrupted before a result "
				  "was recorded";
	struct model_history_item *items;
	struct model_history_item *item;
	int count = 0;
	int rc = 0;

	if (!db || session_id <= 0)
		return NB_ERR_INVAL;
	items = model_history_list(db, session_id, 1, &count);
	for (item = items; item && rc == 0; item = item->next) {
		cJSON *root;
		cJSON *calls;

		if (strcmp(item->kind, "assistant_tool_calls") != 0 || !item->payload_json)
			continue;
		root = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
		calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;
		if (!cJSON_IsArray(calls)) {
			cJSON_Delete(root);
			continue;
		}
		for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
			cJSON *call = cJSON_GetArrayItem(calls, i);
			cJSON *local = cJSON_GetObjectItem(call, "tool_call_id");
			cJSON *provider = cJSON_GetObjectItem(call, "provider_call_id");
			cJSON *name = cJSON_GetObjectItem(call, "name");
			struct model_history_insert repair = {0};
			const char *input;
			enum tool_input_kind input_kind;

			if (!cJSON_IsString(local) || !cJSON_IsString(provider) ||
			    !cJSON_IsString(name) ||
			    !history_tool_input_borrow(call, &input_kind, &input) ||
			    history_result_exists(items, local->valuestring, provider->valuestring))
				continue;
			repair.session_id = session_id;
			repair.turn_id = item->turn_id;
			repair.kind = "tool_result";
			repair.role = "tool";
			repair.content = interrupted;
			repair.payload_json = "{\"status\":\"interrupted\"}";
			repair.tool_call_id = local->valuestring;
			repair.provider_call_id = provider->valuestring;
			repair.tool_name = name->valuestring;
			{
				char repair_key[256];

				snprintf(repair_key, sizeof(repair_key), "result:%s",
					 local->valuestring);
				repair.idempotency_key = repair_key;
				repair.token_count = tokenizer_estimate_tokens(interrupted);
				repair.active = 1;
				rc = model_history_add(db, &repair, NULL);
			}
			if (rc != 0)
				break;
		}
		cJSON_Delete(root);
	}
	model_history_free_list(items);
	return rc;
}

static int history_call_exists(const struct model_history_item *items, const char *local_id,
			       const char *provider_id)
{
	for (const struct model_history_item *item = items; item; item = item->next) {
		cJSON *root;
		cJSON *calls;
		int found = 0;

		if (!item->active || strcmp(item->kind, "assistant_tool_calls") != 0 ||
		    !item->payload_json)
			continue;
		root = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
		calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;
		if (cJSON_IsArray(calls)) {
			for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
				cJSON *call = cJSON_GetArrayItem(calls, i);
				cJSON *local = cJSON_GetObjectItem(call, "tool_call_id");
				cJSON *provider = cJSON_GetObjectItem(call, "provider_call_id");
				cJSON *name = cJSON_GetObjectItem(call, "name");
				const char *input;
				enum tool_input_kind input_kind;

				if (!cJSON_IsString(local) || !cJSON_IsString(provider) ||
				    !cJSON_IsString(name) ||
				    !history_tool_input_borrow(call, &input_kind, &input))
					continue;
				if (local_id && local_id[0] && local->valuestring[0]) {
					if (strcmp(local_id, local->valuestring) == 0)
						found = 1;
				} else if (provider_id &&
					   strcmp(provider_id, provider->valuestring) == 0) {
					found = 1;
				}
				if (found)
					break;
			}
		}
		cJSON_Delete(root);
		if (found)
			return 1;
	}
	return 0;
}

int agent_history_diagnose(const struct model_history_item *items, struct tokenizer *tokenizer,
			   struct agent_history_diagnostic *diagnostic)
{
	const struct model_history_item *item;

	if (!diagnostic)
		return NB_ERR_INVAL;
	memset(diagnostic, 0, sizeof(*diagnostic));
	for (item = items; item; item = item->next) {
		const char *token_text = item->content;

		if (!item->active)
			continue;
		diagnostic->active_items++;
		if (strcmp(item->kind, "assistant_tool_calls") == 0) {
			cJSON *root = item->payload_json
					      ? cJSON_ParseWithOpts(item->payload_json, NULL, 1)
					      : NULL;
			cJSON *calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;

			token_text = item->payload_json;
			if (!cJSON_IsArray(calls)) {
				diagnostic->invalid_payloads++;
			} else {
				for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
					cJSON *call = cJSON_GetArrayItem(calls, i);
					cJSON *local = cJSON_GetObjectItem(call, "tool_call_id");
					cJSON *provider =
						cJSON_GetObjectItem(call, "provider_call_id");
					cJSON *name = cJSON_GetObjectItem(call, "name");
					const char *input;
					enum tool_input_kind input_kind;

					if (!cJSON_IsString(local) || !cJSON_IsString(provider) ||
					    !cJSON_IsString(name) ||
					    !history_tool_input_borrow(call, &input_kind, &input)) {
						diagnostic->invalid_payloads++;
						continue;
					}
					if (!history_result_exists(items, local->valuestring,
								   provider->valuestring))
						diagnostic->dangling_calls++;
				}
			}
			cJSON_Delete(root);
		} else if (strcmp(item->kind, "tool_result") == 0 &&
			   !history_call_exists(items, item->tool_call_id,
						item->provider_call_id)) {
			diagnostic->orphan_results++;
		}
		int tokens = history_item_tokens(item, tokenizer, token_text);

		if (tokens < 0)
			return tokens;
		if (tokens != item->token_count)
			diagnostic->token_mismatches++;
	}
	return 0;
}

int agent_history_repair(struct db *db, int64_t session_id, struct tokenizer *tokenizer,
			 struct agent_history_diagnostic *before, int *changed)
{
	struct model_history_item *items;
	struct agent_history_diagnostic local;
	int count = 0;
	int changes = 0;
	int rc;

	if (!db || session_id <= 0)
		return NB_ERR_INVAL;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	rc = db_lock_acquire(db, &lock);
	if (rc == 0)
		rc = db_exec(db, "BEGIN IMMEDIATE;");
	if (rc != 0)
		return rc;
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return MORPH_ERR_DB;
	}
	rc = agent_history_diagnose(items, tokenizer, before ? before : &local);
	model_history_free_list(items);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	changes = before ? before->dangling_calls : local.dangling_calls;
	/* Legacy answers prove a response was delivered, not task success. */
	rc = history_store_turn_outcomes(db, session_id, NULL, "answered", 0, tokenizer, &changes);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	rc = agent_history_repair_interrupted(db, session_id);
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return MORPH_ERR_DB;
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		const char *token_text = item->content;
		int active = strcmp(item->kind, "background_receipt") == 0
				     ? agent_history_item_replayable(item)
				     : 1;
		int tokens;

		if (strcmp(item->kind, "assistant_tool_calls") == 0) {
			cJSON *root = item->payload_json
					      ? cJSON_ParseWithOpts(item->payload_json, NULL, 1)
					      : NULL;
			cJSON *calls = root ? cJSON_GetObjectItem(root, "calls") : NULL;

			token_text = item->payload_json;
			if (!cJSON_IsArray(calls) || cJSON_GetArraySize(calls) <= 0) {
				active = 0;
			} else {
				for (int i = 0; i < cJSON_GetArraySize(calls); i++) {
					cJSON *call = cJSON_GetArrayItem(calls, i);
					const char *input;
					enum tool_input_kind input_kind;
					int input_valid = history_tool_input_borrow(
						call, &input_kind, &input);

					if (!cJSON_IsString(
						    cJSON_GetObjectItem(call, "tool_call_id")) ||
					    !cJSON_IsString(cJSON_GetObjectItem(
						    call, "provider_call_id")) ||
					    !cJSON_IsString(cJSON_GetObjectItem(call, "name")) ||
					    !input_valid) {
						active = 0;
						break;
					}
				}
			}
			cJSON_Delete(root);
		} else if (strcmp(item->kind, "tool_result") == 0 &&
			   !history_call_exists(items, item->tool_call_id,
						item->provider_call_id)) {
			active = 0;
			nb_log_write(db ? db->logger : NULL, NB_LOG_WARN,
				     "deactivating orphan history tool result %lld",
				     (long long)item->id);
		}
		tokens = history_item_tokens(item, tokenizer, token_text);
		if (tokens < 0) {
			model_history_free_list(items);
			(void)db_exec(db, "ROLLBACK;");
			return tokens;
		}
		if (active != item->active || tokens != item->token_count) {
			rc = history_store_update_item(db, item->id, active, tokens);
			if (rc != 0) {
				model_history_free_list(items);
				(void)db_exec(db, "ROLLBACK;");
				return rc;
			}
			changes++;
		}
	}
	model_history_free_list(items);
	items = model_history_list(db, session_id, 1, &count);
	if (!items && count != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return MORPH_ERR_DB;
	}
	for (struct model_history_item *item = items; item; item = item->next) {
		if (strcmp(item->kind, "tool_result") != 0 ||
		    history_call_exists(items, item->tool_call_id, item->provider_call_id))
			continue;
		nb_log_write(db ? db->logger : NULL, NB_LOG_WARN,
			     "deactivating orphan history tool result %lld", (long long)item->id);
		rc = history_store_update_item(db, item->id, 0, item->token_count);
		if (rc != 0) {
			model_history_free_list(items);
			(void)db_exec(db, "ROLLBACK;");
			return rc;
		}
		changes++;
	}
	model_history_free_list(items);
	rc = db_exec(db, "COMMIT;");
	if (rc != 0) {
		(void)db_exec(db, "ROLLBACK;");
		return rc;
	}
	if (changed)
		*changed = changes;
	return 0;
}
