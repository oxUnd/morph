#include "runtime/services.h"

#include "runtime/runtime_internal.h"
#include "agent/compress.h"
#include "agent/turn.h"
#include "util/arena.h"
#include "runtime/scheduler.h"
#include "cJSON.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char *runtime_strdup_nullable(const char *value)
{
	return value ? strdup(value) : NULL;
}

void runtime_turn_status_cleanup(struct runtime_turn_status *status)
{
	if (!status)
		return;
	for (int i = 0; i < status->step_count; i++) {
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

int runtime_turn_status_get(struct runtime *runtime,
			    struct runtime_turn_status *out)
{
	struct react_context *react;
	struct react_step *step;
	int index = 0;

	if (!runtime || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&runtime->context.execution_lock);
	react = runtime->context.react;
	if (!react) {
		pthread_mutex_unlock(&runtime->context.execution_lock);
		return -ENOENT;
	}
	out->state = react->state;
	out->outcome = react->outcome;
	out->last_error_code = react->last_error_code;
	out->outcome_reason = runtime_strdup_nullable(react->outcome_reason[0]
		? react->outcome_reason : NULL);
	out->error_detail = runtime_strdup_nullable(react->last_error_detail);
	out->final_answer = runtime_strdup_nullable(react->final_answer);
	out->step_count = react->step_count;
	if (out->step_count > 0) {
		out->steps = calloc((size_t)out->step_count, sizeof(*out->steps));
		if (!out->steps)
			goto nomem;
	}
	for (step = react->steps; step && index < out->step_count;
	     step = step->next, index++) {
		out->steps[index].type = step->type;
		out->steps[index].content = runtime_strdup_nullable(step->content);
		out->steps[index].tool_name = runtime_strdup_nullable(step->tool_name);
		out->steps[index].tool_args = runtime_strdup_nullable(step->tool_args);
	}
	out->step_count = index;
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return 0;
nomem:
	pthread_mutex_unlock(&runtime->context.execution_lock);
	runtime_turn_status_cleanup(out);
	return -ENOMEM;
}

char *runtime_turn_notification_body(struct runtime *runtime)
{
	char *out;
	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	out = runtime_react_notification_body(runtime->context.react);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_message(struct runtime *runtime, int rc)
{
	char *out;
	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	out = runtime_react_error_message(runtime->context.react, rc);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_json(struct runtime *runtime)
{
	struct runtime_turn_status status;
	cJSON *root;
	char *out;
	int rc;

	if (runtime_turn_status_get(runtime, &status) != 0)
		return NULL;
	root = cJSON_CreateObject();
	if (!root) {
		runtime_turn_status_cleanup(&status);
		return NULL;
	}
	rc = status.last_error_code < 0 ? status.last_error_code : 0;
	cJSON_AddNumberToObject(root, "rc", rc);
	cJSON_AddStringToObject(root, "outcome",
		react_outcome_name(status.outcome));
	if (status.outcome_reason)
		cJSON_AddStringToObject(root, "reason", status.outcome_reason);
	if (status.error_detail)
		cJSON_AddStringToObject(root, "detail", status.error_detail);
	if (rc < 0) {
		cJSON_AddNumberToObject(root, "error_code", rc);
		cJSON_AddStringToObject(root, "error", morph_strerror(rc));
	}
	if (status.final_answer)
		cJSON_AddStringToObject(root, "text", status.final_answer);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	runtime_turn_status_cleanup(&status);
	return out;
}

int runtime_session_list_query(struct runtime *runtime, struct session **out,
			       int *count, int limit, const char *filter)
{
	if (!runtime || !out || !count)
		return -EINVAL;
	return session_list(&runtime->context.database, out, count, limit, filter);
}

int runtime_session_count_all(struct runtime *runtime)
{
	return runtime ? session_count(&runtime->context.database) : 0;
}

int runtime_session_find_ref(struct runtime *runtime, const char *ref,
			     struct session *out)
{
	char *end;
	long long id;
	int rc;
	if (!runtime || !ref || !out)
		return -EINVAL;
	rc = session_get_by_name(&runtime->context.database, ref, out);
	if (rc != 0)
		rc = session_get_by_display_id(&runtime->context.database, ref, out);
	if (rc == 0)
		return 0;
	errno = 0;
	id = strtoll(ref, &end, 10);
	if (*end != '\0' || errno != 0 || id <= 0)
		return -ENOENT;
	return session_get_by_id(&runtime->context.database, (int64_t)id, out);
}

int runtime_session_select_existing(struct runtime *runtime, int64_t id,
				    struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;
	if (!ctx || id <= 0)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = session_get_by_id(&ctx->database, id, &session);
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

int runtime_session_set_model(struct runtime *runtime, const char *model)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || !model || !model[0])
		return -EINVAL;
	rc = session_update_model(&ctx->database, ctx->current_session.id, model);
	if (rc != 0)
		return rc;
	strncpy(ctx->current_session.model, model,
		sizeof(ctx->current_session.model) - 1);
	if (ctx->llm)
		strncpy(ctx->llm->model_id, model, sizeof(ctx->llm->model_id) - 1);
	return 0;
}

int runtime_turn_prepare_tools(struct runtime *runtime, int64_t now)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	if (!ctx)
		return -EINVAL;
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
		return -EINVAL;
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

char *runtime_trace_load_latest_current(struct runtime *runtime,
					int *round_no, int *aborted)
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
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct compress_result trace = {0};
	struct compress_result window = {0};
	struct message_list *head = NULL;
	struct message *messages;
	struct arena *arena;
	int *ids;
	int count = 0;
	int id_count = 0;
	int remaining;
	int rc;

	if (!ctx)
		return -EINVAL;
	messages = message_list(&ctx->database, ctx->current_session.id, &count);
	if (!messages || count == 0) {
		message_free_list(messages);
		if (kept)
			*kept = 0;
		return 0;
	}
	arena = arena_create(0);
	ids = calloc((size_t)count, sizeof(*ids));
	if (!arena || !ids) {
		message_free_list(messages);
		arena_destroy(arena);
		free(ids);
		return -ENOMEM;
	}
	for (struct message *item = messages; item; item = item->next) {
		struct message_list *node = msg_list_create(arena, item->role,
			item->content, item->token_count);
		if (!node)
			continue;
		node->compressed = item->compressed;
		msg_list_append(&head, node);
		ids[id_count++] = (int)item->id;
	}
	message_free_list(messages);
	(void)compress_react_trace(&head, &trace);
	rc = compress_sliding_window(&head,
		ctx->config.context.keep_recent_rounds, &window);
	if (rc == 0) {
		remaining = msg_list_count(head);
		for (int i = 0; i < count - remaining && i < id_count; i++)
			(void)message_delete(&ctx->database, ids[i]);
		runtime_session_load_history(&ctx->engine,
					     ctx->current_session.id);
		if (trace_removed)
			*trace_removed = trace.messages_removed;
		if (window_removed)
			*window_removed = window.messages_removed;
		if (kept)
			*kept = remaining;
	}
	msg_list_destroy(head);
	arena_destroy(arena);
	free(ids);
	return rc;
}

int runtime_tool_count(const struct runtime *runtime)
{
	return runtime ? runtime->context.tools.count : 0;
}

int runtime_tool_info(const struct runtime *runtime, int index,
		      struct tool_desc *out)
{
	if (!runtime || !out || index < 0 || index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].desc;
	return 0;
}

int runtime_tool_flags(const struct runtime *runtime, int index,
		       unsigned *out)
{
	if (!runtime || !out || index < 0 ||
	    index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].flags;
	return 0;
}

int runtime_tool_origin(const struct runtime *runtime, int index,
			enum tool_origin *out)
{
	if (!runtime || !out || index < 0 ||
	    index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].origin;
	return 0;
}

int runtime_tool_find(const struct runtime *runtime, const char *name,
		      struct tool_desc *out)
{
	struct tool_entry *entry;
	if (!runtime || !name || !out)
		return -EINVAL;
	entry = tool_lookup((struct tool_registry *)&runtime->context.tools, name);
	if (!entry)
		return -ENOENT;
	*out = entry->desc;
	return 0;
}

int runtime_skill_count(const struct runtime *runtime)
{
	return runtime && runtime->context.skills ? runtime->context.skills->count : 0;
}

int runtime_skill_info(const struct runtime *runtime, int index,
		       struct skill_entry *out)
{
	if (!runtime || !runtime->context.skills || !out || index < 0 ||
	    index >= runtime->context.skills->count)
		return -EINVAL;
	*out = runtime->context.skills->entries[index];
	return 0;
}

int runtime_skill_find(const struct runtime *runtime, const char *name,
		       struct skill_entry *out)
{
	struct skill_entry *entry;
	if (!runtime || !runtime->context.skills || !name || !out)
		return -EINVAL;
	entry = skill_lookup(runtime->context.skills, name);
	if (!entry)
		return -ENOENT;
	*out = *entry;
	return 0;
}

int runtime_skill_set_active(struct runtime *runtime, const char *name,
			     int active, int *changed)
{
	struct skill_entry *entry;
	int before;
	int rc = 0;
	if (!runtime || !runtime->context.skills || !name)
		return -EINVAL;
	entry = skill_lookup(runtime->context.skills, name);
	if (!entry)
		return -ENOENT;
	before = entry->activated;
	if (active)
		rc = skill_activate(entry);
	else
		skill_deactivate(entry);
	if (changed)
		*changed = before != entry->activated;
	return rc;
}

char *runtime_memory_render_current(struct runtime *runtime, int max_episodes)
{
	if (!runtime)
		return NULL;
	return memory_render_session(&runtime->context.database,
				     runtime->context.current_session.id,
				     max_episodes);
}

int runtime_memory_clear_current(struct runtime *runtime,
				 enum memory_clear_scope scope)
{
	int rc;
	if (!runtime)
		return -EINVAL;
	rc = memory_clear(&runtime->context.database,
			  runtime->context.current_session.id, scope);
	if (rc == 0 && runtime->context.react)
		react_set_memory_context(runtime->context.react, NULL);
	return rc;
}

int runtime_credit_summary_today_get(struct runtime *runtime,
				     struct credit_summary *out)
{
	return runtime && out ? credit_summary_today(&runtime->context.database,
						     "local", out) : -EINVAL;
}

int runtime_credit_summary_current_get(struct runtime *runtime,
				       struct credit_summary *out)
{
	char key[64];
	if (!runtime || !out)
		return -EINVAL;
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
		return -EINVAL;
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
