#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include "util/utf8.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http/client.h"

static const char *history_compaction_text(const struct model_history_item *item, char **rendered)
{
	*rendered = NULL;
	if (!agent_history_item_replayable(item))
		return NULL;
	if (strcmp(item->kind, "user_message") == 0) {
		*rendered = history_user_content(item);
		return *rendered;
	}
	if (strcmp(item->kind, "assistant_tool_calls") == 0 && item->payload_json) {
		cJSON *payload = cJSON_ParseWithOpts(item->payload_json, NULL, 1);
		if (!payload)
			return item->payload_json;
		/* Reasoning stays on the replay path; checkpoints consume facts only. */
		cJSON_DeleteItemFromObjectCaseSensitive(payload, "reasoning_content");
		if (item->content && item->content[0] &&
		    !cJSON_AddStringToObject(payload, "content", item->content)) {
			cJSON_Delete(payload);
			return NULL;
		}
		*rendered = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		return *rendered;
	}
	return item->content;
}

static int history_retained(const morph_array_t *preserved, const struct model_history_item *item)
{
	for (size_t i = 0; preserved && i < preserved->nelts; i++) {
		const int64_t *id = morph_array_get(preserved, i);
		if (*id == item->id)
			return 1;
	}
	return 0;
}

/* Steering can interleave a call and its output; remove both from candidates. */
static int history_result_in_omitted_group(const struct model_history_item *items,
					   const struct model_history_item *result, int64_t cutoff)
{
	for (const struct model_history_item *item = items; item; item = item->next) {
		if (item->sequence_no > cutoff || strcmp(item->kind, "assistant_tool_calls"))
			continue;
		cJSON *payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		cJSON *call;
		int found = 0;
		cJSON_ArrayForEach (call, cJSON_GetObjectItem(payload, "calls")) {
			const char *local =
				cJSON_GetStringValue(cJSON_GetObjectItem(call, "tool_call_id"));
			const char *provider =
				cJSON_GetStringValue(cJSON_GetObjectItem(call, "provider_call_id"));
			if ((local && result->tool_call_id &&
			     !strcmp(local, result->tool_call_id)) ||
			    (provider && result->provider_call_id &&
			     !strcmp(provider, result->provider_call_id))) {
				found = 1;
				break;
			}
		}
		cJSON_Delete(payload);
		if (found)
			return 1;
	}
	return 0;
}

static int history_compaction_input(const struct model_history_item *items, morph_buf_t *buf,
				    int64_t omitted_through, const morph_array_t *preserved)
{
	const struct model_history_item *item = items;
	if (omitted_through &&
	    morph_buf_puts(buf, "Earlier tool groups were omitted to fit the summary request; do "
				"not infer their results.\n") != 0)
		return -ENOMEM;

	while (item) {
		/* Keep user requests as objective references; exclude retained exchanges. */
		if (strcmp(item->kind, "user_message") && history_retained(preserved, item)) {
			item = item->next;
			continue;
		}
		if ((!strcmp(item->kind, "assistant_tool_calls") &&
		     item->sequence_no <= omitted_through) ||
		    (!strcmp(item->kind, "tool_result") && omitted_through &&
		     history_result_in_omitted_group(items, item, omitted_through))) {
			item = item->next;
			continue;
		}
		const char *label = item->role[0] ? item->role : item->kind;
		char *rendered = NULL;
		const char *text = history_compaction_text(item, &rendered);
		int rc = 0;

		if (!text && agent_history_item_replayable(item) &&
		    (item->content || item->payload_json))
			return -ENOMEM;
		if (text && text[0] &&
		    morph_buf_printf(buf, "[%s/%s]\n%s\n\n", label,
				     history_retained(preserved, item)
					     ? "retained_request_reference"
					     : item->kind,
				     text) != 0)
			rc = -ENOMEM;
		free(rendered);
		if (rc != 0)
			return -ENOMEM;
		item = item->next;
	}
	return 0;
}

int agent_history_reload(struct react_context *ctx)
{
	struct model_history_item *items;
	int count = 0;

	if (!ctx || !ctx->history_enabled || !ctx->history_db || ctx->history_session_id <= 0)
		return -EINVAL;
	items = model_history_list(ctx->history_db, ctx->history_session_id, 1, &count);
	if (!items && count != 0)
		return MORPH_ERR_DB;
	for (struct model_history_item *item = items; item; item = item->next) {
		cJSON *payload;
		const cJSON *original;
		char *prepared;
		int truncated;
		int rc;

		if (strcmp(item->kind, "tool_result") != 0)
			continue;
		payload = item->payload_json ? cJSON_Parse(item->payload_json) : NULL;
		original = cJSON_GetObjectItemCaseSensitive(payload, "original_content");
		rc = agent_history_prepare_tool_content(
			ctx, cJSON_IsString(original) ? original->valuestring : item->content,
			&prepared, &truncated);
		cJSON_Delete(payload);
		if (rc != 0) {
			model_history_free_list(items);
			return rc;
		}
		free(item->content);
		item->content = prepared;
		item->truncated = truncated;
		item->token_count = tokenizer_count(ctx->tokenizer, prepared);
	}
	model_history_free_list(ctx->history_items);
	ctx->history_items = items;
	return 0;
}

static int history_validate_replay(struct morph_log *logger, const struct model_history_item *items)
{
	struct arena *arena = arena_create(0);
	morph_array_t messages = {0};
	int rc;

	if (!arena)
		return -ENOMEM;
	rc = morph_array_init(&messages, 64, sizeof(struct chat_message));
	if (rc == 0)
		rc = agent_history_build_chat_messages(logger, items, &messages, arena);
	if (rc == 0) {
		struct agent_history_diagnostic diagnostic;

		rc = agent_history_diagnose(items, NULL, &diagnostic);
		if (rc == 0 && (diagnostic.invalid_payloads || diagnostic.dangling_calls ||
				diagnostic.orphan_results))
			rc = MORPH_ERR_PROTOCOL;
	}
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	return rc;
}

/* Retained rows may have been counted by an older tokenizer or budget policy. */
static int history_refresh_retention_tokens(struct react_context *ctx)
{
	const char *sql = "SELECT a.id,a.kind,a.content,a.payload_json "
			  "FROM model_history_items a WHERE a.session_id=? AND (a.active=1 OR "
			  "(a.kind='assistant_message' AND EXISTS (SELECT 1 "
			  "FROM model_history_items u WHERE u.session_id=a.session_id "
			  "AND u.turn_id=a.turn_id AND u.active=1 AND u.kind='user_message')))";
	const char *update_sql = "UPDATE model_history_items SET token_count=? WHERE id=?";
	sqlite3_stmt *select = NULL;
	sqlite3_stmt *update = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(ctx->history_db->handle, sql, -1, &select, NULL);

	if (rc == SQLITE_OK)
		rc = sqlite3_prepare_v2(ctx->history_db->handle, update_sql, -1, &update, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_finalize(select);
		sqlite3_finalize(update);
		return MORPH_ERR_DB;
	}
	sqlite3_bind_int64(select, 1, ctx->history_session_id);
	while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
		struct model_history_item item = {0};

		strncpy(item.kind, (const char *)sqlite3_column_text(select, 1),
			sizeof(item.kind) - 1);
		item.content = (char *)sqlite3_column_text(select, 2);
		item.payload_json = (char *)sqlite3_column_text(select, 3);
		const char *text = strcmp(item.kind, "assistant_tool_calls") == 0
					   ? item.payload_json
					   : item.content;
		int tokens = history_item_tokens(&item, ctx->tokenizer, text);

		if (tokens < 0) {
			error = (tokens);
			break;
		}

		sqlite3_bind_int(update, 1, tokens);
		sqlite3_bind_int64(update, 2, sqlite3_column_int64(select, 0));
		if (sqlite3_step(update) != SQLITE_DONE) {
			error = (MORPH_ERR_DB);
			break;
		}
		sqlite3_reset(update);
		sqlite3_clear_bindings(update);
	}
	sqlite3_finalize(select);
	sqlite3_finalize(update);
	if (error)
		return error;
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

static int history_record_summary_sources(struct react_context *ctx, int64_t summary_id,
					  const morph_array_t *preserved)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(
		ctx->history_db->handle,
		"INSERT INTO history_summary_sources(summary_item_id,source_item_id) "
		"VALUES(?,?)",
		-1, &stmt, NULL);
	int error = 0;

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	for (const struct model_history_item *item = ctx->history_items; item; item = item->next) {
		if (strcmp(item->kind, "user_message") && history_retained(preserved, item))
			continue;
		char *rendered = NULL;
		const char *text = history_compaction_text(item, &rendered);
		int has_text = text && text[0];

		if (strcmp(item->kind, "user_message") == 0 && !rendered)
			error = -ENOMEM;
		free(rendered);
		if (error)
			break;
		if (!has_text)
			continue;
		sqlite3_bind_int64(stmt, 1, summary_id);
		sqlite3_bind_int64(stmt, 2, item->id);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE)
			break;
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE || rc == SQLITE_OK ? 0 : MORPH_ERR_DB;
}

/* Keep the old window until the new database window can actually replay. */
static int history_compact_checked(struct react_context *ctx, const char *summary,
				   int summary_tokens, int input_tokens, const char *trigger_kind,
				   int record_sources, int *active_count,
				   history_compaction_validator validate, void *user_data,
				   const morph_array_t *preserved, int64_t cutoff)
{
	struct model_history_item *candidate = NULL;
	int64_t summary_id = 0;
	int count = 0;
	int rc = db_exec(ctx->history_db, "SAVEPOINT history_replay;");

	if (rc != 0)
		return rc;
	rc = history_refresh_retention_tokens(ctx);
	if (rc == 0)
		rc = model_history_compact_selected(
			ctx->history_db, ctx->history_session_id, ctx->turn_id, summary,
			summary_tokens, input_tokens, trigger_kind, &summary_id, preserved, cutoff);
	if (rc == 0 && record_sources)
		rc = history_record_summary_sources(ctx, summary_id, preserved);
	if (rc == 0) {
		candidate = model_history_list(ctx->history_db, ctx->history_session_id, 1, &count);
		rc = count < 0 ? MORPH_ERR_DB
			       : history_validate_replay(ctx ? ctx->logger : NULL, candidate);
	}
	if (rc == 0 && validate)
		rc = validate(candidate, user_data);
	if (rc == 0)
		rc = db_exec(ctx->history_db, "RELEASE history_replay;");
	if (rc != 0) {
		(void)db_exec(ctx->history_db,
			      "ROLLBACK TO history_replay; RELEASE history_replay;");
		model_history_free_list(candidate);
		return rc;
	}
	model_history_free_list(ctx->history_items);
	ctx->history_items = candidate;
	*active_count = count;
	return 0;
}

static char *history_finish_summary(struct react_context *ctx, const char *summary,
				    int pruned_groups)
{
	if (!pruned_groups)
		return history_redact_all(ctx, summary);
	morph_buf_t marked;
	if (morph_buf_init(&marked, 256) != 0)
		return NULL;
	cJSON *root = cJSON_ParseWithOpts(summary, NULL, 1);
	cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "working_state");
	int rc = morph_buf_printf(
		&marked, "%d older tool groups were omitted; their outcomes are unavailable.\n%s",
		pruned_groups, cJSON_IsString(state) ? state->valuestring : summary);
	char *text = NULL;
	if (rc == 0 && cJSON_IsString(state)) {
		if (cJSON_SetValuestring(state, morph_buf_cstr(&marked)))
			text = cJSON_PrintUnformatted(root);
	} else if (rc == 0) {
		text = morph_buf_detach(&marked);
	}
	cJSON_Delete(root);
	morph_buf_cleanup(&marked);
	char *redacted = text ? history_redact_all(ctx, text) : NULL;
	free(text);
	return redacted;
}

static int history_window_tokens(const struct react_context *ctx)
{
	int tokens = 0;
	for (const struct model_history_item *item = ctx->history_items; item; item = item->next) {
		if (agent_history_item_replayable(item) && item->token_count > 0)
			tokens = item->token_count > INT_MAX - tokens ? INT_MAX
								      : tokens + item->token_count;
	}
	return tokens;
}

static int history_compact_selected(struct react_context *ctx, int force,
				    const char *trigger_kind_in,
				    history_compaction_validator validate, void *user_data,
				    const morph_array_t *preserved, int64_t cutoff)
{
	morph_buf_t input;
	char *summary = NULL;
	int max_summary_tokens;
	int user_budget;
	int recent_budget;
	int target_budget;
	int input_tokens = 0;
	int threshold;
	int count = 0;
	int record_sources;
	int64_t omitted_through = 0;
	int pruned_groups = 0;
	int rc;
	const char *trigger_kind = trigger_kind_in && trigger_kind_in[0]
					   ? trigger_kind_in
					   : (force ? "manual" : "threshold");

	if (!ctx || !ctx->history_enabled || !ctx->history_db || !ctx->compress.summarize ||
	    ctx->compress.max_context_tokens <= 0)
		return 0;
	input_tokens = history_window_tokens(ctx);
	threshold = (int)((double)ctx->compress.max_context_tokens *
			  ctx->compress.summarize_threshold_ratio);
	if (!force && input_tokens < threshold)
		return 0;
	if (!ctx->history_items)
		return 0;
	rc = morph_buf_init(&input, 8192);
	if (rc != 0)
		return rc;
	rc = history_compaction_input(ctx->history_items, &input, omitted_through, preserved);
	if (rc == 0) {
		for (int attempt = 0; attempt < 3; attempt++) {
			struct timespec started = {0};
			struct timespec finished = {0};
			int timed = clock_gettime(CLOCK_MONOTONIC, &started) == 0;

			log_info(ctx->logger,
				 "history summary request: session=%lld turn=%s trigger=%s "
				 "attempt=%d/3 input_bytes=%zu history_tokens=%d "
				 "summary_max_tokens=%d omitted_groups=%d",
				 (long long)ctx->history_session_id, ctx->turn_id, trigger_kind,
				 attempt + 1, input.len, input_tokens,
				 ctx->compress.compaction_summary_max_tokens, pruned_groups);
			rc = ctx->compress.summarize(morph_buf_cstr(&input),
						     ctx->compress.summarize_user_data, &summary);
			if (rc == 0 && summary && summary[0]) {
				char *prepared =
					history_finish_summary(ctx, summary, pruned_groups);
				free(summary);
				summary = prepared;
				if (!summary)
					rc = (-ENOMEM);
			}
			double elapsed_ms =
				timed && clock_gettime(CLOCK_MONOTONIC, &finished) == 0
					? (double)(finished.tv_sec - started.tv_sec) * 1000.0 +
						  (double)(finished.tv_nsec - started.tv_nsec) /
							  1000000.0
					: 0.0;
			int empty = !summary || !summary[0];

			int limit = ctx->compress.compaction_summary_max_tokens;
			if (rc == 0 && summary && summary[0] && limit > 0 &&
			    tokenizer_count(ctx->tokenizer, summary) > limit) {
				log_warn(ctx->logger,
					 "history summary exceeds tokenizer budget; "
					 "retrying without truncating checkpoint fields");
				rc = MORPH_ERR_PROTOCOL;
			}
			log_write(ctx->logger, rc == 0 && !empty ? LOG_INFO : LOG_WARN,
				  "history summary response: session=%lld turn=%s trigger=%s "
				  "attempt=%d/3 elapsed_ms=%.0f result=%s summary_bytes=%zu "
				  "error=%s",
				  (long long)ctx->history_session_id, ctx->turn_id, trigger_kind,
				  attempt + 1, elapsed_ms,
				  rc != 0 ? "failed"
				  : empty ? "empty_summary"
					  : "ok",
				  summary ? strlen(summary) : 0, morph_strerror(rc));
			if (rc == 0 && summary && summary[0])
				break;
			if (rc == 0)
				rc = MORPH_ERR_PROTOCOL;
			free(summary);
			summary = NULL;
			if (rc == MORPH_ERR_CONTEXT_LIMIT && pruned_groups < 32) {
				const struct model_history_item *group = ctx->history_items;
				while (group && (group->sequence_no <= omitted_through ||
						 history_retained(preserved, group) ||
						 strcmp(group->kind, "assistant_tool_calls")))
					group = group->next;
				if (!group)
					break;
				omitted_through = group->sequence_no;
				for (group = group->next;
				     group && strcmp(group->kind, "assistant_tool_calls") &&
				     strcmp(group->kind, "user_message");
				     group = group->next)
					omitted_through = group->sequence_no;
				pruned_groups++;
				morph_buf_reset(&input);
				rc = history_compaction_input(ctx->history_items, &input,
							      omitted_through, preserved);
				if (rc != 0)
					break;
				log_warn(ctx ? ctx->logger : NULL,
					 "summary context recovery: omitted %d old tool groups",
					 pruned_groups);
				attempt--;
				continue;
			}
			if (attempt == 2 || (rc != MORPH_ERR_NETWORK && rc != MORPH_ERR_PROTOCOL &&
					     rc != -ETIMEDOUT))
				break;
			if (rc == MORPH_ERR_PROTOCOL) {
				int target = ctx->compress.compaction_summary_max_tokens;
				target = target > 0 ? target / (attempt + 2) : 1000;
				if (morph_buf_printf(
					    &input,
					    "\n\n[Checkpoint retry instruction: the previous "
					    "response "
					    "was unusable. Return a complete, concise factual "
					    "checkpoint in at most %d tokens. Preserve completed "
					    "work, exact paths and pending checks. Do not "
					    "continue the task or call tools.]\n",
					    target > 0 ? target : 1) != 0) {
					rc = (-ENOMEM);
					break;
				}
			}
			log_warn(ctx->logger,
				 "history summary retry scheduled: session=%lld turn=%s "
				 "next_attempt=%d/3 delay_ms=%u error=%s",
				 (long long)ctx->history_session_id, ctx->turn_id, attempt + 2,
				 (unsigned int)(attempt + 1) * 1000u, morph_strerror(rc));
			rc = http_wait_cancelable(
				&(struct http_control){.cancel_token = &ctx->cancel_token,
						       .cancel_flag = &ctx->cancelled},
				(unsigned int)(attempt + 1) * 1000u);
			if (rc != 0)
				break;
		}
	}
	morph_buf_cleanup(&input);
	record_sources = rc == 0 && summary && summary[0] && !pruned_groups;
	if (rc != 0 || !summary || !summary[0]) {
		int failure = rc != 0 ? rc : MORPH_ERR_PROTOCOL;

		free(summary);
		(void)model_history_compaction_attempt_add(
			ctx->history_db, ctx->history_session_id, ctx->turn_id, trigger_kind,
			"failed", input_tokens, 0, failure, morph_strerror(failure));
		return failure;
	}
	max_summary_tokens = tokenizer_count(ctx->tokenizer, summary);
	if (!summary[0]) {
		free(summary);
		return MORPH_ERR_PROTOCOL;
	}
	user_budget = (int)((double)ctx->compress.max_context_tokens *
			    ctx->compress.compress_target_ratio) -
		      max_summary_tokens;
	if (user_budget < 0)
		user_budget = 0;
	if (user_budget > ctx->compress.compaction_user_message_tokens)
		user_budget = ctx->compress.compaction_user_message_tokens;
	target_budget = (int)((double)ctx->compress.max_context_tokens *
			      ctx->compress.compress_target_ratio);
	recent_budget = target_budget - max_summary_tokens - user_budget;
	if (recent_budget < 0)
		recent_budget = 0;
	rc = history_compact_checked(ctx, summary, max_summary_tokens, input_tokens, trigger_kind,
				     record_sources, &count, validate, user_data, preserved,
				     cutoff);
	free(summary);
	if (rc != 0) {
		(void)model_history_compaction_attempt_add(ctx->history_db, ctx->history_session_id,
							   ctx->turn_id, trigger_kind, "failed",
							   input_tokens, 0, rc, morph_strerror(rc));
		return rc;
	}
	(void)model_history_compaction_attempt_add(ctx->history_db, ctx->history_session_id,
						   ctx->turn_id, trigger_kind, "completed",
						   input_tokens, max_summary_tokens, 0, NULL);
	log_info(ctx ? ctx->logger : NULL,
		 "model history compaction %s: session=%lld turn=%s trigger=%s "
		 "history_tokens=%d summary_tokens=%d active_items=%d "
		 "user_budget=%d recent_budget=%d omitted_groups=%d",
		 strncmp(trigger_kind, "in_turn_", 8) == 0 ? "prepared" : "committed",
		 (long long)ctx->history_session_id, ctx->turn_id, trigger_kind, input_tokens,
		 max_summary_tokens, count, user_budget, recent_budget, pruned_groups);
	if (ctx->compress.compaction_warning_count > 0) {
		int compactions =
			model_history_compaction_count(ctx->history_db, ctx->history_session_id);

		if (compactions >= ctx->compress.compaction_warning_count) {
			cJSON *data = cJSON_CreateObject();

			log_warn(ctx ? ctx->logger : NULL,
				 "model history has been compacted %d times; "
				 "consider starting a new session",
				 compactions);
			if (data) {
				cJSON_AddNumberToObject(data, "count", compactions);
				(void)morph_event_emit_simple(
					ctx->event_cb, ctx->event_user_data, MORPH_EVENT_BACKGROUND,
					"history.compaction.warning", "warning",
					"Consider starting a new session; repeated "
					"compaction loses detail.",
					data);
				cJSON_Delete(data);
			}
		}
	}
	return 1;
}

int agent_history_compact_validated(struct react_context *ctx, int force, const char *trigger_kind,
				    history_compaction_validator validate, void *user_data)
{
	morph_array_t preserved = {0};
	sqlite3_stmt *stmt = NULL;
	int64_t cutoff = 0;
	int rc;

	if (!ctx || !ctx->history_enabled || !ctx->history_db || !ctx->history_items ||
	    !ctx->compress.summarize || ctx->compress.max_context_tokens <= 0)
		return 0;
	int threshold = (int)((double)ctx->compress.max_context_tokens *
			      ctx->compress.summarize_threshold_ratio);
	if (!force && history_window_tokens(ctx) < threshold)
		return 0;
	const char *hook_trigger =
		force && (!trigger_kind || strcmp(trigger_kind, "manual") == 0) ? "manual" : "auto";
	rc = agent_hook_compaction(ctx, "PreCompact", hook_trigger);
	if (rc != 0)
		return rc;
	int target = (int)((double)ctx->compress.max_context_tokens *
			   ctx->compress.compress_target_ratio);
	int reserve = ctx->compress.compaction_summary_max_tokens;
	if (reserve <= 0)
		reserve = target / 4 < 4096 ? target / 4 : 4096;
	int available = target > reserve ? target - reserve : 0;
	int users = ctx->compress.compaction_user_message_tokens;
	users = users < available ? users : available;
	if (users < 0)
		users = 0;
	rc = db_exec(ctx->history_db, "SAVEPOINT history_selection;");
	if (rc != 0)
		return rc;
	rc = history_refresh_retention_tokens(ctx);
	if (rc == 0)
		rc = model_history_select_retained(ctx->history_db, ctx->history_session_id, users,
						   available - users,
						   ctx->compress.max_history_rounds, &preserved);
	if (rc == 0) {
		rc = sqlite3_prepare_v2(
			ctx->history_db->handle,
			"SELECT COALESCE(MAX(sequence_no),0) FROM model_history_items "
			"WHERE session_id=?",
			-1, &stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, ctx->history_session_id);
			rc = sqlite3_step(stmt);
		}
		if (rc == SQLITE_ROW) {
			cutoff = sqlite3_column_int64(stmt, 0);
			rc = 0;
		} else {
			rc = (MORPH_ERR_DB);
		}
		sqlite3_finalize(stmt);
	}
	if (rc == 0)
		rc = db_exec(ctx->history_db, "RELEASE history_selection;");
	if (rc != 0) {
		(void)db_exec(ctx->history_db,
			      "ROLLBACK TO history_selection; RELEASE history_selection;");
	} else {
		rc = history_compact_selected(ctx, force, trigger_kind, validate, user_data,
					      &preserved, cutoff);
	}
	morph_array_cleanup(&preserved);
	if (rc > 0) {
		int hook_rc = agent_hook_compaction(ctx, "PostCompact", hook_trigger);

		if (hook_rc == 0)
			hook_rc = agent_hook_compaction(ctx, "SessionStart", "compact");
		if (hook_rc != 0)
			return hook_rc;
	}
	return rc;
}

int agent_history_compact_with_trigger(struct react_context *ctx, int force,
				       const char *trigger_kind)
{
	return agent_history_compact_validated(ctx, force, trigger_kind, NULL, NULL);
}

int agent_history_compact(struct react_context *ctx, int force)
{
	return agent_history_compact_with_trigger(ctx, force, NULL);
}

int agent_history_maybe_compact(struct react_context *ctx)
{
	return agent_history_compact(ctx, 0);
}
