#include "runtime/runtime_internal.h"

#include "runtime/session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int runtime_session_current(const struct runtime *runtime, struct session *out)
{
	if (!runtime || !out)
		return -EINVAL;
	*out = runtime->context.current_session;
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
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		runtime_session_load_history(&ctx->engine, session.id);
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
	rc = runtime_session_delete(&ctx->engine, id);
	if (rc == 0) {
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

const char *runtime_session_current_name(const struct runtime *runtime)
{
	return runtime ? runtime->context.current_session.name : NULL;
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
		MORPH_RETURN(-EINVAL);
	items = model_history_list(&runtime->context.database,
		runtime->context.current_session.id, 1, &count);
	rc = agent_history_diagnose(items, runtime->context.tokenizer,
		diagnostic);
	model_history_free_list(items);
	return rc;
}

int runtime_session_history_repair(struct runtime *runtime,
	struct agent_history_diagnostic *before, int *changed)
{
	if (!runtime)
		MORPH_RETURN(-EINVAL);
	return agent_history_repair(&runtime->context.database,
		runtime->context.current_session.id, runtime->context.tokenizer,
		before, changed);
}
