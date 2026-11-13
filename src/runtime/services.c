#include "runtime/services.h"
#include "nb/err.h"

#include "agent/memory.h"
#include "agent/tools/skill_activate.h"

#include "runtime/runtime_internal.h"
#include "agent/compress.h"
#include "agent/history.h"
#include "agent/turn.h"
#include "nb/cleanup.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "cJSON.h"
#include "runtime/scheduler.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int runtime_string_copy(const char *source, char **out)
{
	if (!source)
		return 0;
	*out = strdup(source);
	return *out ? 0 : NB_ERR_NOMEM;
}

void runtime_turn_status_cleanup(struct runtime_turn_status *status)
{
	if (!status)
		return;
	for (int i = 0; status->steps && i < status->step_count; i++) {
		free(status->steps[i].content);
		free(status->steps[i].tool_name);
		free(status->steps[i].tool_args);
	}
	free(status->steps);
	free(status->outcome_reason);
	free(status->error_detail);
	free(status->final_answer);
	memset(status, 0, sizeof(*status));
}

static void runtime_turn_status_move(struct runtime_turn_status *destination,
				     struct runtime_turn_status *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

static int runtime_turn_status_copy_locked(const struct react_context *react,
					   struct runtime_turn_status *out)
{
	size_t count = 0;
	int rc;

	if (react->step_count < 0 || (size_t)react->step_count > SIZE_MAX / sizeof(*out->steps))
		return NB_ERR_RANGE;
	for (const struct react_step *step = react->steps; step; step = step->next) {
		if (count == INT_MAX || count >= SIZE_MAX / sizeof(*out->steps))
			return NB_ERR_RANGE;
		count++;
	}
	out->state = react->state;
	out->outcome = react->outcome;
	out->last_error_code = react->last_error_code;
	rc = runtime_string_copy(react->outcome_reason[0] ? react->outcome_reason : NULL,
				 &out->outcome_reason);
	if (rc == 0)
		rc = runtime_string_copy(react->last_error_detail, &out->error_detail);
	if (rc == 0)
		rc = runtime_string_copy(react->final_answer, &out->final_answer);
	if (rc != 0)
		return rc;
	if (count) {
		out->steps = calloc(count, sizeof(*out->steps));
		if (!out->steps)
			return NB_ERR_NOMEM;
	}
	out->step_count = (int)count;
	size_t index = 0;

	for (const struct react_step *step = react->steps; step; step = step->next, index++) {
		struct runtime_trace_step *copy = &out->steps[index];

		copy->type = step->type;
		rc = runtime_string_copy(step->content, &copy->content);
		if (rc == 0)
			rc = runtime_string_copy(step->tool_name, &copy->tool_name);
		if (rc == 0)
			rc = runtime_string_copy(step->tool_args, &copy->tool_args);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int runtime_turn_status_copy(struct runtime *runtime, struct runtime_turn_status *out)
{
	struct runtime_turn_status value NB_CLEANUP(runtime_turn_status_cleanup) = {0};
	int rc;

	if (!runtime || !out)
		return NB_ERR_INVAL;
	if (out->steps || out->outcome_reason || out->error_detail || out->final_answer)
		return NB_ERR_EXISTS;
	nb_mutex_lock(runtime->context.execution_lock);
	rc = runtime->context.react
		     ? runtime_turn_status_copy_locked(runtime->context.react, &value)
		     : NB_ERR_NOT_FOUND;
	nb_mutex_unlock(runtime->context.execution_lock);
	if (rc == 0)
		runtime_turn_status_move(out, &value);
	return rc;
}

char *runtime_turn_notification_body_copy(struct runtime *runtime)
{
	char *out;
	if (!runtime)
		return NULL;
	nb_mutex_lock(runtime->context.execution_lock);
	out = runtime_react_notification_body_copy(runtime->context.react);
	nb_mutex_unlock(runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_message_copy(struct runtime *runtime, int rc)
{
	char *out;
	if (!runtime)
		return NULL;
	nb_mutex_lock(runtime->context.execution_lock);
	out = runtime_react_error_message_copy(runtime->context.react, rc);
	nb_mutex_unlock(runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_json_copy(struct runtime *runtime)
{
	struct runtime_turn_status status NB_CLEANUP(runtime_turn_status_cleanup) = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	if (runtime_turn_status_copy(runtime, &status) != 0)
		return NULL;
	root = cJSON_CreateObject();
	if (!root)
		return NULL;
	rc = status.last_error_code < 0 ? status.last_error_code : 0;
	if (!cJSON_AddNumberToObject(root, "rc", rc) ||
	    !cJSON_AddStringToObject(root, "outcome", react_outcome_name(status.outcome)) ||
	    (status.outcome_reason &&
	     !cJSON_AddStringToObject(root, "reason", status.outcome_reason)) ||
	    (status.error_detail && !cJSON_AddStringToObject(root, "detail", status.error_detail)))
		return NULL;
	if (rc < 0) {
		if (!cJSON_AddNumberToObject(root, "error_code", rc) ||
		    !cJSON_AddStringToObject(
			    root, "error",
			    nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)))
			return NULL;
	}
	if (status.final_answer && !cJSON_AddStringToObject(root, "text", status.final_answer))
		return NULL;
	return cJSON_PrintUnformatted(root);
}

int runtime_session_list_query(struct runtime *runtime, struct session **out,
			       int *count, int limit, const char *filter)
{
	if (!runtime || !out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	return session_list(&runtime->context.database, out, count, limit, filter);
}

int runtime_session_count_all(struct runtime *runtime)
{
	return runtime ? session_count(&runtime->context.database) : 0;
}

int runtime_agent_budget(struct runtime *runtime, struct agent_budget *out)
{
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_budget_copy(runtime->context.agents, out);
}

int runtime_agent_list(struct runtime *runtime,
			   int64_t parent_session_id,
			   struct agent_info **out, int *count)
{
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_list_copy(runtime->context.agents, parent_session_id, out,
						count);
}

int runtime_agent_read(struct runtime *runtime, const char *id, struct agent_info **out)
{
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_read_copy(runtime->context.agents, id, out);
}

int runtime_agent_activity(struct runtime *runtime, int64_t parent_session_id,
	struct agent_info **out, int *count)
{
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_activity_copy(runtime->context.agents, parent_session_id, out, count);
}

int runtime_agent_subscribe(struct runtime *runtime, const char *id,
	char ***events, int *count, int64_t *sequence)
{
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_subscribe(runtime->context.agents, id, events, count, sequence);
}

static int runtime_agent_interaction(struct runtime *runtime, const char *operation,
	const char *id, const char *message)
{
	if (!runtime || !runtime->context.agents || !id || !*id)
		return NB_ERR_INVAL;
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!args || !cJSON_AddStringToObject(args, "target", id) ||
	    (message && !cJSON_AddStringToObject(args, "message", message)))
		return NB_ERR_NOMEM;
	struct runtime_agent_actor actor = {0};
	int rc = runtime_agent_actor_begin(runtime, &actor);
	if (rc != 0)
		return rc;
	rc = agent_control_call(runtime->context.agents, operation, args, &actor.context, &out);
	runtime_agent_actor_end(runtime);
	return rc;
}

int runtime_agent_followup(struct runtime *runtime, const char *id, const char *message)
{
	if (!message || !*message)
		return NB_ERR_INVAL;
	return runtime_agent_interaction(runtime, "followup_task", id, message);
}

int runtime_agent_interrupt(struct runtime *runtime, const char *id)
{
	return runtime_agent_interaction(runtime, "interrupt_agent", id, NULL);
}

void runtime_agent_list_free(struct agent_info *tasks,
				 int count)
{
	agent_control_free_list(tasks, count);
}

int runtime_agent_select(struct runtime *runtime, const char *task_id)
{
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_select(runtime->context.agents,
					     task_id);
}

int runtime_agent_events(struct runtime *runtime, const char *task_id,
			     char ***events, int *count)
{
	if (!events || !count)
		return NB_ERR_INVAL;
	if (*events)
		return NB_ERR_EXISTS;
	*count = 0;
	if (!runtime || !runtime->context.agents)
		return NB_ERR_NOT_FOUND;
	return agent_control_events_copy(runtime->context.agents, task_id, events,
						  count);
}

void runtime_agent_events_free(char **events, int count)
{
	agent_control_free_events(events, count);
}

int runtime_session_find_ref(struct runtime *runtime, const char *ref,
			     struct session *out)
{
	char *end;
	long long id;
	int rc;
	if (!runtime || !ref || !out)
		return NB_ERR_INVAL;
	rc = session_get_by_name(&runtime->context.database, ref, out);
	if (rc != 0)
		rc = session_get_by_display_id(&runtime->context.database, ref, out);
	if (rc == 0)
		return 0;
	errno = 0;
	id = strtoll(ref, &end, 10);
	if (*end != '\0' || errno != 0 || id <= 0)
		return NB_ERR_NOT_FOUND;
	return session_get_by_id(&runtime->context.database, (int64_t)id, out);
}

int runtime_session_select_existing(struct runtime *runtime, int64_t id,
				    struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;
	if (!ctx || id <= 0)
		return NB_ERR_INVAL;
	nb_mutex_lock(ctx->execution_lock);
	rc = runtime_agent_session_begin(runtime);
	if (rc != 0) {
		nb_mutex_unlock(ctx->execution_lock);
		return rc;
	}
	rc = session_get_by_id(&ctx->database, id, &session);
	if (rc == 0 && ctx->agents)
		rc = agent_control_set_parent_session(ctx->agents, session.id);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		/* Navigation defers model replay and repair until the next turn. */
		runtime_session_clear_history(ctx->react);
		if (out)
			*out = session;
	}
	runtime_agent_actor_end(runtime);
	nb_mutex_unlock(ctx->execution_lock);
	return rc;
}

int runtime_session_set_model(struct runtime *runtime, const char *model)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || !model || !model[0])
		return NB_ERR_INVAL;
	if (ctx->current_session.id > 0) {
		rc = session_update_model(&ctx->database,
					  ctx->current_session.id, model);
		if (rc != 0)
			return rc;
	}
	strncpy(ctx->current_session.model, model,
		sizeof(ctx->current_session.model) - 1);
	ctx->current_session.model[sizeof(ctx->current_session.model) - 1] = '\0';
	strncpy(ctx->config.models.text.model, model,
		sizeof(ctx->config.models.text.model) - 1);
	ctx->config.models.text.model[
		sizeof(ctx->config.models.text.model) - 1] = '\0';
	if (ctx->llm) {
		strncpy(ctx->llm->model_id, model, sizeof(ctx->llm->model_id) - 1);
		ctx->llm->model_id[sizeof(ctx->llm->model_id) - 1] = '\0';
	}
	return 0;
}

int runtime_turn_prepare_tools(struct runtime *runtime, int64_t now)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	if (!ctx)
		return NB_ERR_INVAL;
	(void)runtime_context_select_plan_session(ctx, ctx->current_session.id);
	(void)scheduled_tasks_tool_set_time_anchor(&ctx->tools, now);
	return scheduled_tasks_tool_set_source_session(&ctx->tools,
						       ctx->current_session.id);
}

int runtime_session_context_stats(struct runtime *runtime, int *messages,
			      int *tokens, int *limit)
{
	struct message *list;
	int count = 0;
	int total = 0;
	if (!runtime)
		return NB_ERR_INVAL;
	list = message_list(&runtime->context.database,
			    runtime->context.current_session.id, &count);
	for (struct message *item = list; item; item = item->next)
		total += item->token_count;
	message_free_list(list);
	if (messages)
		*messages = count;
	if (tokens)
		*tokens = total;
	if (limit)
		*limit = runtime->context.tokenizer
			? runtime->context.tokenizer->context_limit : 0;
	return 0;
}

static void runtime_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
	*statement = NULL;
}

int runtime_session_model_context_stats(struct runtime *runtime,
	int *active_items, int *tokens, int *tool_tokens,
	int *compactions, int *limit)
{
	sqlite3_stmt *statement NB_CLEANUP(runtime_statement_cleanup) = NULL;
	int64_t total = 0;
	int64_t tools = 0;
	int replay_items = 0;
	int compact_count = 0;

	if (!runtime)
		return NB_ERR_INVAL;
	/* Deferred sessions have no persisted history or compactions. */
	if (runtime->context.current_session.id > 0) {
		/* Only receipts need their body to decide replayability. Avoid copying
		 * full answers and tool results whenever the footer polls its totals. */
		const char *sql =
			"SELECT kind,token_count,"
			"CASE WHEN kind='background_receipt' THEN content END,"
			"CASE WHEN kind='background_receipt' THEN payload_json END "
			"FROM model_history_items WHERE session_id=? AND active=1";
		int rc = sqlite3_prepare_v2(runtime->context.database.handle, sql, -1,
			&statement, NULL);
		if (rc == SQLITE_OK)
			rc = sqlite3_bind_int64(statement, 1, runtime->context.current_session.id);
		if (rc != SQLITE_OK)
			return nb_err_trace(runtime->options.logger, MORPH_ERR_DB,
					    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					    __LINE__);
		while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
			struct model_history_item item = {0};
			const char *kind = (const char *)sqlite3_column_text(statement, 0);

			if (kind)
				snprintf(item.kind, sizeof(item.kind), "%s", kind);
			item.token_count = sqlite3_column_int(statement, 1);
			/* The predicate borrows these SQLite fields until the next step. */
			item.content = (char *)sqlite3_column_text(statement, 2);
			item.payload_json = (char *)sqlite3_column_text(statement, 3);
			if (!agent_history_item_replayable(&item))
				continue;
			if (replay_items == INT_MAX)
				return NB_ERR_RANGE;
			replay_items++;
			total += item.token_count;
			if (strcmp(item.kind, "tool_result") == 0)
				tools += item.token_count;
		}
		if (rc != SQLITE_DONE)
			return nb_err_trace(runtime->options.logger, MORPH_ERR_DB,
					    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
					    __LINE__);
	}
	if (total > INT_MAX || total < INT_MIN || tools > INT_MAX || tools < INT_MIN)
		return NB_ERR_RANGE;
	if (runtime->context.current_session.id > 0)
		compact_count = model_history_compaction_count(
			&runtime->context.database, runtime->context.current_session.id);
	if (compact_count < 0)
		return compact_count;
	if (active_items)
		*active_items = replay_items;
	if (tokens)
		*tokens = (int)total;
	if (tool_tokens)
		*tool_tokens = (int)tools;
	if (compactions)
		*compactions = compact_count;
	if (limit)
		*limit = runtime->context.tokenizer ?
			runtime->context.tokenizer->context_limit : 0;
	return 0;
}

int runtime_session_compaction_status(struct runtime *runtime,
	struct runtime_history_compaction_status *status)
{
	const char *sql =
		"SELECT trigger_kind,status,input_tokens,output_tokens,error_code "
		"FROM history_compaction_attempts WHERE session_id=? "
		"ORDER BY created_at DESC,id DESC LIMIT 1";
	sqlite3_stmt *stmt;
	int rc;

	if (!runtime || !status)
		return nb_err_trace(runtime ? runtime->options.logger : NULL, NB_ERR_INVAL,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	memset(status, 0, sizeof(*status));
	rc = sqlite3_prepare_v2(runtime->context.database.handle, sql, -1,
		&stmt, NULL);
	if (rc != SQLITE_OK)
		return nb_err_trace(runtime ? runtime->options.logger : NULL, MORPH_ERR_DB,
				    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				    __LINE__);
	sqlite3_bind_int64(stmt, 1, runtime->context.current_session.id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *trigger =
			(const char *)sqlite3_column_text(stmt, 0);
		const char *state =
			(const char *)sqlite3_column_text(stmt, 1);

		snprintf(status->trigger_kind, sizeof(status->trigger_kind),
			 "%s", trigger ? trigger : "");
		snprintf(status->status, sizeof(status->status), "%s",
			 state ? state : "");
		status->input_tokens = sqlite3_column_int(stmt, 2);
		status->output_tokens = sqlite3_column_int(stmt, 3);
		status->error_code = sqlite3_column_int(stmt, 4);
		rc = 0;
	} else if (rc == SQLITE_DONE) {
		rc = NB_ERR_NOT_FOUND;
	} else {
		rc = MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	return rc;
}

char *runtime_trace_latest_current_copy(struct runtime *runtime, int *round_no, int *aborted)
{
	if (!runtime)
		return NULL;
	return trace_load_latest(&runtime->context.database,
		runtime->context.current_session.id, round_no, aborted);
}

int runtime_session_compress(struct runtime *runtime,
				     int *trace_removed,
				     int *window_removed, int *kept)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct react_context *react;
	char previous_turn_id[sizeof(react->turn_id)];
	int previous_turn_id_user_set;
	int before;
	int after;
	int rc;

	if (!ctx)
		return NB_ERR_INVAL;
	react = ctx->react;
	if (!react)
		return NB_ERR_INVAL;
	rc = agent_session_load_history(&(struct agent_session_runtime) {
		.db = &ctx->database,
		.session_id = ctx->current_session.id,
		.react = react,
	});
	if (rc != 0)
		return rc;
	before = model_history_count(&ctx->database,
		ctx->current_session.id, 1);
	if (before < 0)
		return before;
	react->history_db = &ctx->database;
	react->history_session_id = ctx->current_session.id;
	react->history_enabled = 1;
	memcpy(previous_turn_id, react->turn_id, sizeof(previous_turn_id));
	previous_turn_id_user_set = react->turn_id_user_set;
	rc = ((file_status_1 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				: !(nb_buf_clear(&file_value_1),
				    nb_buf_append_cstr(&file_value_1, "compact_"))
					? NB_ERR_NOMEM
					: nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
		      ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), react->turn_id,
						sizeof(react->turn_id))) == NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : file_status_1);
	if (rc != 0) {
		memcpy(react->turn_id, previous_turn_id,
		       sizeof(react->turn_id));
		react->turn_id_user_set = previous_turn_id_user_set;
		react->history_db = NULL;
		react->history_session_id = 0;
		react->history_enabled = 0;
		return rc;
	}
	struct hook_manager *hooks = &ctx->hooks;
	int64_t previous_hook_session = hooks->session_id;

	rc = hook_manager_reload(hooks);
	if (rc == 0) {
		hooks->session_id = ctx->current_session.id;
		hooks->turn_id = react->turn_id;
		hooks->model = ctx->config.models.text.model;
		hooks->event_cb = react->event_cb;
		hooks->event_user = react->event_user_data;
		hooks->remaining_ms = 600000;
		hooks->context_bytes = 0;
		react->hook_fn = hook_manager_dispatch;
		react->hook_user = hooks;
		rc = agent_history_compact(react, 1);
		react->hook_fn = NULL;
		react->hook_user = NULL;
	}
	hooks->session_id = previous_hook_session;
	hooks->turn_id = NULL;
	hooks->event_cb = NULL;
	hooks->event_user = NULL;
	react->hook_context = NULL;
	memcpy(react->turn_id, previous_turn_id, sizeof(react->turn_id));
	react->turn_id_user_set = previous_turn_id_user_set;
	react->history_db = NULL;
	react->history_session_id = 0;
	react->history_enabled = 0;
	if (rc < 0)
		return rc;
	after = model_history_count(&ctx->database,
		ctx->current_session.id, 1);
	if (after < 0)
		return after;
	if (trace_removed)
		*trace_removed = 0;
	if (window_removed)
		*window_removed = before - after;
	if (kept)
		*kept = after;
	return 0;
}

int runtime_tool_count(const struct runtime *runtime)
{
	int count;

	if (!runtime)
		return 0;
	nb_mutex_lock(runtime->context.tool_registry_lock);
	tool_registry_lock((struct tool_registry *)&runtime->context.tools);
	count = runtime->context.tools.count;
	tool_registry_unlock((struct tool_registry *)&runtime->context.tools);
	nb_mutex_unlock(runtime->context.tool_registry_lock);
	return count;
}

static int runtime_tool_get(const struct runtime *runtime, int index,
	struct tool_desc *desc, unsigned *flags, int *enabled, enum tool_origin *origin)
{
	const struct tool_registry *tools;
	int rc = 0;

	if (!runtime || index < 0)
		return NB_ERR_INVAL;
	nb_mutex_lock(runtime->context.tool_registry_lock);
	tool_registry_lock((struct tool_registry *)&runtime->context.tools);
	tools = &runtime->context.tools;
	if (index >= tools->count) {
		rc = NB_ERR_INVAL;
	} else {
		if (desc)
			*desc = tools->entries[index].desc;
		if (flags)
			*flags = tools->entries[index].flags;
		if (enabled)
			*enabled = !tool_is_disabled(tools, tools->entries[index].desc.name);
		if (origin)
			*origin = tools->entries[index].origin;
	}
	tool_registry_unlock((struct tool_registry *)&runtime->context.tools);
	nb_mutex_unlock(runtime->context.tool_registry_lock);
	return rc;
}

int runtime_tool_info_borrow(const struct runtime *runtime, int index, struct tool_desc *out)
{
	return out ? runtime_tool_get(runtime, index, out, NULL, NULL, NULL) : NB_ERR_INVAL;
}

int runtime_tool_flags(const struct runtime *runtime, int index, unsigned *out)
{
	return out ? runtime_tool_get(runtime, index, NULL, out, NULL, NULL) : NB_ERR_INVAL;
}

int runtime_tool_enabled(const struct runtime *runtime, int index, int *out)
{
	return out ? runtime_tool_get(runtime, index, NULL, NULL, out, NULL) : NB_ERR_INVAL;
}

int runtime_tool_origin(const struct runtime *runtime, int index, enum tool_origin *out)
{
	return out ? runtime_tool_get(runtime, index, NULL, NULL, NULL, out) : NB_ERR_INVAL;
}

int runtime_tool_find_borrow(const struct runtime *runtime, const char *name, struct tool_desc *out)
{
	struct tool_entry *entry;

	if (!runtime || !name || !out)
		return NB_ERR_INVAL;
	nb_mutex_lock(runtime->context.tool_registry_lock);
	tool_registry_lock((struct tool_registry *)&runtime->context.tools);
	entry = tool_borrow((struct tool_registry *)&runtime->context.tools, name);
	if (entry)
		*out = entry->desc;
	tool_registry_unlock((struct tool_registry *)&runtime->context.tools);
	nb_mutex_unlock(runtime->context.tool_registry_lock);
	return entry ? 0 : NB_ERR_NOT_FOUND;
}

int runtime_skill_count(const struct runtime *runtime)
{
	return runtime && runtime->context.skills ? runtime->context.skills->count : 0;
}

int runtime_skill_info_borrow(const struct runtime *runtime, int index, struct skill_entry *out)
{
	if (!runtime || !runtime->context.skills || !out || index < 0 ||
	    index >= runtime->context.skills->count)
		return NB_ERR_INVAL;
	*out = *skill_entry_borrow_at(runtime->context.skills, (size_t)index);
	return 0;
}

int runtime_skill_find_borrow(const struct runtime *runtime, const char *name,
			      struct skill_entry *out)
{
	struct skill_entry *entry;
	if (!runtime || !runtime->context.skills || !name || !out)
		return NB_ERR_INVAL;
	entry = skill_borrow(runtime->context.skills, name);
	if (!entry)
		return NB_ERR_NOT_FOUND;
	*out = *entry;
	return 0;
}

static void runtime_skill_unlock(struct runtime_context **context)
{
	if (context && *context) {
		nb_mutex_unlock((*context)->execution_lock);
		*context = NULL;
	}
}

int runtime_skill_set_active(struct runtime *runtime, const char *name,
			     int active, int *changed)
{
	if (changed)
		*changed = 0;
	if (!runtime || !runtime->context.skills || !name)
		return NB_ERR_INVAL;
	struct skill_entry *known = skill_borrow(runtime->context.skills, name);
	if (!known)
		return NB_ERR_NOT_FOUND;
	if (active && !known->enabled)
		return NB_ERR_PERMISSION;
	int rc = runtime_ensure_current_session(runtime);
	if (rc != 0)
		return rc;
	rc = nb_mutex_trylock(runtime->context.execution_lock);
	if (rc != 0)
		return rc;
	struct runtime_context *context NB_CLEANUP(runtime_skill_unlock) = &runtime->context;
	struct skill_entry *entry = skill_borrow(context->skills, name);
	if (!entry)
		return NB_ERR_NOT_FOUND;
	int before = entry->activated;
	rc = skill_activate_session(context->skills, &context->database,
		context->current_session.id, name, active);
	if (changed)
		*changed = before != entry->activated;
	return rc;
}

static int runtime_preferences_bind(struct runtime *runtime)
{
	int rc;

	if (!runtime)
		return NB_ERR_INVAL;
	rc = runtime_ensure_current_session(runtime);
	if (rc != 0)
		return rc;
	return preference_bind(&runtime->context.database,
		runtime->context.current_session.id,
		runtime->context.turn_scope.bound ? runtime->context.turn_scope.user_id : "local",
		runtime->context.workdir);
}

char *runtime_memory_background_render_copy(struct runtime *runtime)
{
	if (runtime_preferences_bind(runtime) != 0)
		return NULL;
	return memory_background_text_create(&runtime->context.database,
					     runtime->context.current_session.id);
}

char *runtime_preferences_render_copy(struct runtime *runtime, int history)
{
	if (runtime_preferences_bind(runtime) != 0)
		return NULL;
	return preference_render_copy(&runtime->context.database,
				      runtime->context.current_session.id, history);
}

int runtime_preference_set(struct runtime *runtime, const char *scope,
			   const char *key, const char *value)
{
	int rc = runtime_preferences_bind(runtime);

	if (rc != 0)
		return rc;
	return preference_set(&runtime->context.database,
		runtime->context.current_session.id, scope, key, value,
		"explicit /memory command", NULL);
}

char *runtime_memory_render_current_copy(struct runtime *runtime, int max_past_tasks)
{
	if (!runtime)
		return NULL;
	return memory_session_text_create(&runtime->context.database,
					  runtime->context.current_session.id, max_past_tasks);
}

int runtime_memory_report_copy(struct runtime *runtime, const char *query, cJSON **out)
{
	int rc = runtime_preferences_bind(runtime);

	if (rc != 0)
		return rc;
	rc = memory_report_create(&runtime->context.database, runtime->context.current_session.id,
				  &runtime->context.memory_options, query, out);
	if (rc != 0)
		return rc;
	const struct react_context *react = runtime->context.react;
	const char *reply_memory = NULL;

	if (react && react->memory_reply_session_id == runtime->context.current_session.id)
		reply_memory = react->memory_reply_text;

	if (!cJSON_AddStringToObject(*out, "reply_memory", reply_memory ? reply_memory : "")) {
		cJSON_Delete(*out);
		*out = NULL;
		return NB_ERR_NOMEM;
	}
	return 0;
}

int runtime_memory_retry_current(struct runtime *runtime)
{
	if (!runtime)
		return NB_ERR_INVAL;
	if (!runtime->context.memory_options.enabled)
		return MORPH_ERR_NOT_CONFIGURED;
	int rc = runtime_preferences_bind(runtime);

	if (rc != 0)
		return rc;
	return memory_retry_updates(&runtime->context.database, runtime->context.current_session.id,
				    &runtime->context.memory_options);
}

int runtime_memory_clear_current(struct runtime *runtime, enum memory_clear_scope scope)
{
	int rc;
	if (!runtime)
		return NB_ERR_INVAL;
	rc = memory_clear(&runtime->context.database, runtime->context.current_session.id, scope);
	if (rc == 0 && runtime->context.react)
		react_set_memory_reply_text_copy(runtime->context.react, NULL);
	return rc;
}

int runtime_credit_summary_today_get(struct runtime *runtime,
				     struct credit_summary *out)
{
	return runtime && out ? credit_summary_today(&runtime->context.database, "local", out)
			      : NB_ERR_INVAL;
}

int runtime_credit_summary_current_get(struct runtime *runtime,
				       struct credit_summary *out)
{
	char key[64];
	if (!runtime || !out)
		return NB_ERR_INVAL;
	runtime_context_credit_session_key(&runtime->context, key, sizeof(key));
	return credit_summary_session(&runtime->context.database, key, out);
}

int runtime_credit_record_media(struct runtime *runtime, const char *kind,
				int64_t image_units, int64_t video_seconds,
				const char *provider, const char *model,
				const char *metadata_json)
{
	struct credit_event event;
	char key[64];
	if (!runtime || !kind)
		return NB_ERR_INVAL;
	runtime_context_credit_session_key(&runtime->context, key, sizeof(key));
	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = key;
	event.kind = kind;
	event.provider = provider;
	event.model = model;
	event.image_units = image_units;
	event.video_seconds = video_seconds;
	event.metadata_json = metadata_json;
	return credit_record_event(&runtime->context.database,
				   &runtime->context.config.credits, &event, NULL);
}
