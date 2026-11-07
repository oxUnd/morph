#include "runtime/runtime.h"
#include "runtime/runtime_internal.h"
#include "persistence/history_store.h"

#include "runtime/session.h"
#include "util/error.h"
#include "util/json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_session_current(const struct runtime *runtime, struct session *out)
{
	if (!runtime || !out)
		return -EINVAL;
	*out = runtime->context.current_session;
	if (out->id > 0)
		return session_get_by_id(&runtime->context.database,
			out->id, out);
	if (out->id <= 0) {
		strncpy(out->model, runtime->context.config.models.text.model,
			sizeof(out->model) - 1);
		out->model[sizeof(out->model) - 1] = '\0';
	}
	return 0;
}

int runtime_session_current_id(const struct runtime *runtime, int64_t *out)
{
	if (!runtime || !out)
		return -EINVAL;
	*out = runtime->context.current_session.id;
	return 0;
}

int runtime_session_select(struct runtime *runtime, const char *name,
			   struct session *out, int *created)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;

	if (!ctx || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = runtime_session_switch(&ctx->engine, name,
			ctx->config.models.text.model, &session, created);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		/*
		 * Selecting a session is a navigation operation.  Loading and
		 * repairing its entire model history here makes large sessions slow
		 * to open, and agent_turn_begin() loads the same history again before
		 * the next model turn.  Drop the previous session's in-memory context
		 * now and defer the incoming history load until it is actually needed.
		 */
		runtime_session_clear_history(ctx->react);
		if (out)
			*out = session;
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_create_and_select(struct runtime *runtime, const char *name,
				      struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;

	if (!ctx || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = runtime_session_create(&ctx->engine, name,
			ctx->config.models.text.model, &session);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		runtime_session_clear_history(ctx->react);
		if (out)
			*out = session;
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_create_detached(struct runtime *runtime, const char *name,
				    struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || !name || !name[0] || !out)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = session_create(&ctx->database, name, ctx->config.models.text.model, out);
	if (rc == 0)
		(void)session_ensure_display_id(&ctx->database, out);
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_delete_and_update(struct runtime *runtime, int64_t id)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || id <= 0)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	runtime_title_cancel(runtime, id);
	rc = runtime_session_delete(&ctx->engine, id);
	if (rc == 0) {
		runtime_recap_invalidate(runtime);
		runtime_context_forget_plan_session(ctx, id);
		if (ctx->current_session.id == id) {
			runtime_session_clear_history(ctx->react);
			memset(&ctx->current_session, 0, sizeof(ctx->current_session));
		}
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_rename_and_update(struct runtime *runtime, int64_t id,
				      const char *name)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || id <= 0 || !name || !name[0])
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	runtime_title_cancel(runtime, id);
	rc = runtime_session_rename(&ctx->engine, id, name);
	if (rc == 0 && ctx->current_session.id == id) {
		strncpy(ctx->current_session.name, name,
			sizeof(ctx->current_session.name) - 1);
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_list_all(struct runtime *runtime, struct session **out,
			     int *count, int recent_first)
{
	if (!runtime || !out || !count)
		return -EINVAL;
	return session_list(&runtime->context.database, out, count,
			    recent_first, NULL);
}

int runtime_session_context_tokens(struct runtime *runtime, int64_t session_id, int64_t *out)
{
	if (!runtime)
		return -EINVAL;
	return history_store_context_tokens(&runtime->context.database, session_id, out);
}

void runtime_session_list_free(struct session *sessions)
{
	free(sessions);
}

int runtime_session_reload_current(struct runtime *runtime)
{
	if (!runtime || runtime->context.current_session.id <= 0)
		return -EINVAL;
	runtime_session_load_history(&runtime->context.engine,
				     runtime->context.current_session.id);
	return 0;
}

const char *runtime_session_current_name(struct runtime *runtime)
{
	struct session session;

	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	if (runtime_session_current(runtime, &session) == 0)
		runtime->context.current_session = session;
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return runtime->context.current_session.name;
}

struct message *runtime_session_messages_current(struct runtime *runtime,
					 int *count)
{
	if (!runtime || !count || runtime->context.current_session.id <= 0)
		return NULL;
	return message_list(&runtime->context.database,
			    runtime->context.current_session.id, count);
}

void runtime_session_messages_free(struct message *messages)
{
	message_free_list(messages);
}

int runtime_session_last_answer(struct runtime *runtime, char **out)
{
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!runtime)
		return -EINVAL;
	rc = pthread_mutex_trylock(&runtime->context.execution_lock);
	if (rc != 0)
		return -rc;
	rc = runtime->context.current_session.id > 0
		     ? history_store_last_answer(&runtime->context.database,
						 runtime->context.current_session.id, out)
		     : -ENODATA;
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return rc;
}

static int transcript_message(const struct message *message, void *data)
{
	struct json *items = data;
	struct json *item = json_object();

	if (!item || !json_add_number(item, "id", (double)message->id) ||
	    !json_add_string(item, "role", message->role) ||
	    !json_add_string(item, "content", message->content ? message->content : "") ||
	    !json_add_string(item, "turn_id", message->turn_id ? message->turn_id : "") ||
	    !json_add_number(item, "created_at", (double)message->created_at) ||
	    json_array_add(items, item) != 0) {
		json_free(item);
		return -ENOMEM;
	}
	return 0;
}

static int transcript_nullable_text(struct json *item, const char *key, const char *value)
{
	return !value || json_add_string(item, key, value);
}

static int transcript_display(const struct history_display_record *record, void *data)
{
	struct json *item = json_object();
	int ok = item && transcript_nullable_text(item, "type", record->type) &&
		 transcript_nullable_text(item, "content", record->content) &&
		 transcript_nullable_text(item, "attachments_json", record->attachments_json) &&
		 transcript_nullable_text(item, "structured_data", record->structured_data) &&
		 transcript_nullable_text(item, "agent_ui_ir", record->agent_ui_ir) &&
		 transcript_nullable_text(item, "turn_id", record->turn_id);

	if (ok && (record->present & HISTORY_DISPLAY_VERDICT))
		ok = json_add_number(item, "hitl_verdict", (double)record->hitl_verdict) != NULL;
	if (ok && (record->present & HISTORY_DISPLAY_MESSAGE))
		ok = json_add_number(item, "core_message_id", (double)record->core_message_id) !=
		     NULL;
	if (ok && (record->present & HISTORY_DISPLAY_CREATED))
		ok = json_add_number(item, "created_at", (double)record->created_at) != NULL;
	if (!ok || json_array_add(data, item) != 0) {
		json_free(item);
		return -ENOMEM;
	}
	return 0;
}

static int transcript_trace(const struct history_trace_record *record, void *data)
{
	struct json *item = json_object();

	if (!item || !json_add_number(item, "id", (double)record->id) ||
	    !json_add_string(item, "steps_json", record->steps_json ? record->steps_json : "[]") ||
	    !json_add_number(item, "created_at", (double)record->created_at) ||
	    json_array_add(data, item) != 0) {
		json_free(item);
		return -ENOMEM;
	}
	return 0;
}

int runtime_session_transcript_json(struct runtime *runtime, int64_t session_id, char **out)
{
	struct session session;
	struct db *db;
	struct json *root;
	struct json *items;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!runtime || session_id <= 0)
		return -EINVAL;
	db = &runtime->context.database;
	rc = session_get_by_id(db, session_id, &session);
	if (rc != 0)
		return rc;
	root = json_object();
	items = root ? json_add_array(root, "messages") : NULL;
	if (!items || !json_add_string(root, "identity", session.display_id)) {
		json_free(root);
		return -ENOMEM;
	}
	rc = history_store_visit_messages(db, session_id, transcript_message, items);
	if (rc == 0) {
		items = json_add_array(root, "display");
		rc = items ? history_store_visit_display(db, session_id, transcript_display, items)
			   : -ENOMEM;
		if (rc == -ENODATA) {
			json_object_remove(root, "display");
			rc = 0;
		}
	}
	if (rc == 0) {
		items = json_add_array(root, "traces");
		rc = items ? history_store_visit_traces(db, session_id, transcript_trace, items)
			   : -ENOMEM;
	}
	if (rc == 0) {
		*out = json_stringify(root);
		if (!*out)
			rc = -ENOMEM;
	}
	json_free(root);
	return rc;
}

struct model_history_item *runtime_session_model_history_current(
	struct runtime *runtime, int active_only, int *count)
{
	if (!runtime || !count || runtime->context.current_session.id <= 0)
		return NULL;
	return model_history_list(&runtime->context.database,
		runtime->context.current_session.id, active_only, count);
}

void runtime_session_model_history_free(struct model_history_item *items)
{
	model_history_free_list(items);
}

int runtime_session_history_diagnose(struct runtime *runtime,
	struct agent_history_diagnostic *diagnostic)
{
	struct model_history_item *items;
	int count = 0;
	int rc;

	if (!runtime || !diagnostic)
		return -EINVAL;
	items = model_history_list(&runtime->context.database,
		runtime->context.current_session.id, 1, &count);
	if (count < 0)
		return MORPH_ERR_DB;
	rc = agent_history_diagnose(items, runtime->context.tokenizer,
		diagnostic);
	model_history_free_list(items);
	return rc;
}

int runtime_session_history_repair(struct runtime *runtime,
	struct agent_history_diagnostic *before, int *changed)
{
	if (!runtime)
		return -EINVAL;
	return agent_history_repair(&runtime->context.database,
		runtime->context.current_session.id, runtime->context.tokenizer,
		before, changed);
}
