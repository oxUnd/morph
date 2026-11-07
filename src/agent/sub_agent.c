#include "sub_agent_internal.h"
#include "util/signals.h"
#include "sub_agent.h"
#include "tool_runtime.h"
#include "guardrail.h"
#include "tokenizer.h"
#include "compress.h"
#include "system_prompt.h"
#include "turn.h"
#include "session.h"
#include "models/llm.h"
#include "http/client.h"
#include "util/log.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include "util/utf8.h"
#include "util/error.h"
#include "util/file.h"
#include "util/json.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>

static int64_t now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

static void generate_task_id(int seq, char *out, size_t out_size)
{
	snprintf(out, out_size, "sa_%d", seq);
}

struct sub_agent_event_sink {
	struct sub_agent_runtime *rt;
	struct sub_agent_task *task;
};

static struct sub_agent_task *sub_agent_task_find_locked(
	struct sub_agent_runtime *rt, const char *task_id)
{
	if (!rt || !task_id)
		return NULL;
	for (int i = 0; i < rt->task_count; i++) {
		if (strcmp(rt->tasks[i].id, task_id) == 0)
			return &rt->tasks[i];
	}
	return NULL;
}

static int sub_agent_storage_exec_task(struct sub_agent_runtime *rt,
				       struct sub_agent_task *task,
				       const char *agent_name)
{
	struct session child;
	sqlite3_stmt *stmt = NULL;
	char name[256];
	int rc;

	if (!rt || !task || !agent_name)
		return -EINVAL;
	if (!rt->db || !rt->db->handle || task->parent_session_id <= 0)
		return 0;
	snprintf(name, sizeof(name), "agent_%lld_%s_%s",
		 (long long)task->parent_session_id, agent_name, task->id);
	pthread_mutex_lock(&rt->storage_mutex);
	rc = session_create(rt->db, name,
		rt->default_llm ? rt->default_llm->model_id : "", &child);
	if (rc != 0) {
		pthread_mutex_unlock(&rt->storage_mutex);
		return rc;
	}
	(void)session_ensure_display_id(rt->db, &child);
	task->child_session_id = child.id;
	rc = sqlite3_prepare_v2(rt->db->handle,
		"INSERT INTO sub_agent_tasks(task_id,parent_session_id,"
		"child_session_id,agent_name,description,mode,status,"
		"started_at) VALUES(?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, task->id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, task->parent_session_id);
		sqlite3_bind_int64(stmt, 3, task->child_session_id);
		sqlite3_bind_text(stmt, 4, agent_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, task->task_description, -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 6, task->mode, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 7, (int)task->status);
		sqlite3_bind_int64(stmt, 8, task->started_at_ms);
		rc = sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		(void)session_delete(rt->db, child.id);
		task->child_session_id = 0;
	}
	pthread_mutex_unlock(&rt->storage_mutex);
	if (rc != SQLITE_DONE)
		return MORPH_ERR_DB;
	return 0;
}

static void sub_agent_storage_update_task(struct sub_agent_runtime *rt,
					  struct sub_agent_task *task)
{
	sqlite3_stmt *stmt = NULL;
	char *result = NULL;
	enum sub_agent_task_status status;
	int error_code;
	int iteration_count;
	int64_t ended_at_ms;

	if (!rt || !task || !rt->db || !rt->db->handle ||
	    task->child_session_id <= 0)
		return;
	pthread_mutex_lock(&task->mutex);
	status = task->status;
	error_code = task->error_code;
	iteration_count = task->iteration_count;
	ended_at_ms = task->ended_at_ms;
	if (task->result)
		result = strdup(task->result);
	pthread_mutex_unlock(&task->mutex);
	pthread_mutex_lock(&rt->storage_mutex);
	if (sqlite3_prepare_v2(rt->db->handle,
		"UPDATE sub_agent_tasks SET status=?,result=?,error_code=?,"
		"iterations=?,ended_at=? WHERE task_id=?", -1, &stmt,
		NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, (int)status);
		if (result)
			sqlite3_bind_text(stmt, 2, result, -1,
					  SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(stmt, 2);
		sqlite3_bind_int(stmt, 3, error_code);
		sqlite3_bind_int(stmt, 4, iteration_count);
		sqlite3_bind_int64(stmt, 5, ended_at_ms);
		sqlite3_bind_text(stmt, 6, task->id, -1, SQLITE_TRANSIENT);
		(void)sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&rt->storage_mutex);
	free(result);
}

static struct sub_agent_task *sub_agent_task_create(struct sub_agent_runtime *rt,
						    struct sub_agent_entry *entry,
						    const char *description, const char *mode,
						    int joined, int depth, int *error)
{
	struct sub_agent_task *task;
	char *task_description;
	int rc;

	if (error)
		*error = 0;
	if (!rt || !entry || !description || !mode) {
		if (error)
			*error = -EINVAL;
		return NULL;
	}
	task_description = strdup(description);
	if (!task_description) {
		if (error)
			*error = -ENOMEM;
		return NULL;
	}
	pthread_mutex_lock(&rt->mutex);
	if (rt->task_count >= SUB_AGENT_TASK_MAX) {
		pthread_mutex_unlock(&rt->mutex);
		free(task_description);
		if (error)
			*error = -ENOSPC;
		return NULL;
	}
	task = &rt->tasks[rt->task_count++];
	memset(task, 0, sizeof(*task));
	generate_task_id(rt->next_task_id++, task->id, sizeof(task->id));
	task->depth = depth;
	task->agent_index = (int)(entry - rt->entries);
	task->parent_session_id = rt->parent_session_id;
	strncpy(task->mode, mode, sizeof(task->mode) - 1);
	task->task_description = task_description;
	task->status = SUB_AGENT_PENDING;
	task->joined = joined;
	task->started_at_ms = now_ms();
	pthread_mutex_init(&task->mutex, NULL);
	rc = morph_array_init(&task->events, MORPH_ARRAY_INIT_CAP,
			      sizeof(char *));
	if (rc == 0) {
		task->events_initialized = 1;
	} else {
		pthread_mutex_destroy(&task->mutex);
		free(task->task_description);
		memset(task, 0, sizeof(*task));
		rt->task_count--;
	}
	if (rc != 0) {
		pthread_mutex_unlock(&rt->mutex);
		if (error)
			*error = rc;
		return NULL;
	}
	rc = sub_agent_storage_exec_task(rt, task, entry->cfg.name);
	if (rc != 0) {
		morph_array_cleanup(&task->events);
		pthread_mutex_destroy(&task->mutex);
		free(task->task_description);
		memset(task, 0, sizeof(*task));
		rt->task_count--;
		pthread_mutex_unlock(&rt->mutex);
		if (error)
			*error = rc;
		return NULL;
	}
	pthread_mutex_unlock(&rt->mutex);
	return task;
}

static int sub_agent_child_event(const struct morph_event *ev,
				 void *user_data)
{
	struct sub_agent_event_sink *sink = user_data;
	struct sub_agent_runtime *rt;
	struct sub_agent_task *task;
	struct morph_event scoped;
	cJSON *data;
	char *json;
	char *active;
	int forward = 0;
	int rc = 0;

	if (!sink || !sink->rt || !sink->task || !ev)
		return -EINVAL;
	rt = sink->rt;
	task = sink->task;
	data = ev->data ? cJSON_Duplicate(ev->data, 1) : cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_DeleteItemFromObjectCaseSensitive(data, "task_id");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "agent");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "session_id");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "parent_session_id");
	cJSON_AddStringToObject(data, "task_id", task->id);
	cJSON_AddStringToObject(data, "agent",
		rt->entries[task->agent_index].cfg.name);
	cJSON_AddNumberToObject(data, "session_id",
		(double)task->child_session_id);
	cJSON_AddNumberToObject(data, "parent_session_id",
		(double)task->parent_session_id);
	scoped = *ev;
	scoped.data = data;
	json = morph_event_json_text_copy(&scoped);
	if (json) {
		pthread_mutex_lock(&task->mutex);
		if (task->events_initialized) {
			char **slot = morph_array_push_borrow(&task->events);
			if (slot)
				*slot = json;
			else
				free(json);
		} else {
			free(json);
		}
		pthread_mutex_unlock(&task->mutex);
	}
	if (rt->db && rt->db->handle && task->child_session_id > 0) {
		sqlite3_stmt *stmt = NULL;
		char *stored = morph_event_json_text_copy(&scoped);

		if (stored) {
			pthread_mutex_lock(&rt->storage_mutex);
			if (sqlite3_prepare_v2(rt->db->handle,
				"INSERT INTO sub_agent_events(task_id,event_json,"
				"created_at) VALUES(?,?,?)", -1, &stmt,
				NULL) == SQLITE_OK) {
				sqlite3_bind_text(stmt, 1, task->id, -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 2, stored, -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_int64(stmt, 3, now_ms());
				(void)sqlite3_step(stmt);
			}
			sqlite3_finalize(stmt);
			pthread_mutex_unlock(&rt->storage_mutex);
			free(stored);
		}
	}
	pthread_mutex_lock(&rt->mutex);
	active = rt->active_task_id;
	forward = active[0] && strcmp(active, task->id) == 0;
	pthread_mutex_unlock(&rt->mutex);
	if (forward && rt->event_cb)
		rc = morph_event_emit(rt->event_cb, rt->event_user_data, &scoped);
	cJSON_Delete(data);
	return rc;
}

struct sub_agent_runtime *
sub_agent_runtime_create(struct tool_registry *parent_tools,
			 struct model *default_llm,
			 struct tokenizer *tokenizer,
			 struct compress_config *compress)
{
	struct sub_agent_runtime *rt = calloc(1, sizeof(*rt));
	if (!rt)
		return NULL;
	rt->logger = parent_tools ? parent_tools->logger : NULL;
	rt->parent_tools = parent_tools;
	rt->default_llm = default_llm;
	rt->tokenizer = tokenizer;
	rt->compress = compress;
	rt->depth = 0;
	rt->auto_approve_readonly = 1;
	rt->next_task_id = 0;
	pthread_mutex_init(&rt->mutex, NULL);
	pthread_mutex_init(&rt->storage_mutex, NULL);
	rt->mutexes_initialized = 1;
	{
		char *home = file_expand_path_copy("~/.morph/log");
		if (home) {
			file_ensure_dir(home);
			snprintf(rt->trace_file, sizeof(rt->trace_file),
				 "%s/traces.jsonl", home);
			free(home);
		}
	}
	return rt;
}

void sub_agent_runtime_cancel(struct sub_agent_runtime *rt)
{
	if (!rt)
		return;
	pthread_mutex_lock(&rt->mutex);
	for (int i = 0; i < rt->task_count; i++) {
		pthread_mutex_lock(&rt->tasks[i].mutex);
		if (rt->tasks[i].child_ctx)
			react_cancel(rt->tasks[i].child_ctx);
		pthread_mutex_unlock(&rt->tasks[i].mutex);
	}
	pthread_mutex_unlock(&rt->mutex);
}

void sub_agent_runtime_destroy(struct sub_agent_runtime *rt)
{
	if (!rt)
		return;
	for (int i = 0; i < rt->entry_count; i++)
		free(rt->entries[i].system_prompt);
	for (int i = 0; i < rt->task_count; i++) {
		if (!rt->tasks[i].joined &&
		    strcmp(rt->tasks[i].mode, "delegate") == 0) {
			pthread_mutex_lock(&rt->tasks[i].mutex);
			if (rt->tasks[i].child_ctx)
				react_cancel(rt->tasks[i].child_ctx);
			pthread_mutex_unlock(&rt->tasks[i].mutex);
			pthread_join(rt->tasks[i].thread, NULL);
			rt->tasks[i].joined = 1;
		}
		if (rt->tasks[i].events_initialized) {
			char **event;

			morph_array_foreach(event, &rt->tasks[i].events, char *)
				free(*event);
			morph_array_cleanup(&rt->tasks[i].events);
		}
		pthread_mutex_destroy(&rt->tasks[i].mutex);
		free(rt->tasks[i].task_description);
		free(rt->tasks[i].result);
	}
	if (rt->mutexes_initialized) {
		pthread_mutex_destroy(&rt->storage_mutex);
		pthread_mutex_destroy(&rt->mutex);
	}
	free(rt);
}

int sub_agent_runtime_set_event_callback(struct sub_agent_runtime *rt,
					 morph_event_cb cb, void *user)
{
	if (!rt)
		return -EINVAL;
	rt->event_cb = cb;
	rt->event_user_data = user;
	return 0;
}

int sub_agent_runtime_set_storage(struct sub_agent_runtime *rt,
				  struct db *db)
{
	sqlite3_stmt *stmt = NULL;
	int next_id = 0;

	if (!rt)
		return -EINVAL;
	rt->db = db;
	if (!db || !db->handle)
		return 0;
	pthread_mutex_lock(&rt->storage_mutex);
	if (sqlite3_prepare_v2(db->handle,
		"SELECT COALESCE(MAX(CAST(SUBSTR(task_id,4) AS INTEGER)),"
		"-1)+1 FROM sub_agent_tasks", -1, &stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		next_id = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&rt->storage_mutex);
	pthread_mutex_lock(&rt->mutex);
	if (next_id > rt->next_task_id)
		rt->next_task_id = next_id;
	pthread_mutex_unlock(&rt->mutex);
	return 0;
}

int sub_agent_runtime_set_parent_session(struct sub_agent_runtime *rt,
					 int64_t session_id)
{
	if (!rt || session_id < 0)
		return -EINVAL;
	pthread_mutex_lock(&rt->mutex);
	rt->parent_session_id = session_id;
	pthread_mutex_unlock(&rt->mutex);
	return 0;
}

int sub_agent_runtime_select_task(struct sub_agent_runtime *rt,
				  const char *task_id)
{
	int found = 0;

	if (!rt)
		return -EINVAL;
	if (!task_id || !task_id[0]) {
		pthread_mutex_lock(&rt->mutex);
		rt->active_task_id[0] = '\0';
		pthread_mutex_unlock(&rt->mutex);
		return 0;
	}
	pthread_mutex_lock(&rt->mutex);
	found = sub_agent_task_find_locked(rt, task_id) != NULL;
	pthread_mutex_unlock(&rt->mutex);
	if (!found && rt->db && rt->db->handle) {
		sqlite3_stmt *stmt = NULL;

		pthread_mutex_lock(&rt->storage_mutex);
		if (sqlite3_prepare_v2(rt->db->handle,
			"SELECT 1 FROM sub_agent_tasks WHERE task_id=?", -1,
			&stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, task_id, -1,
					  SQLITE_TRANSIENT);
			found = sqlite3_step(stmt) == SQLITE_ROW;
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&rt->storage_mutex);
	}
	if (found) {
		pthread_mutex_lock(&rt->mutex);
		strncpy(rt->active_task_id, task_id,
			sizeof(rt->active_task_id) - 1);
		rt->active_task_id[sizeof(rt->active_task_id) - 1] = '\0';
		pthread_mutex_unlock(&rt->mutex);
	}
	return found ? 0 : -ENOENT;
}

static int sub_agent_emit_background_event(struct sub_agent_runtime *rt,
					   const char *name,
					   const char *phase,
					   const char *message,
					   const char *agent,
					   const char *task_id,
					   const char *task,
					   int error_code)
{
	cJSON *data;
	int rc;

	if (!rt || !rt->event_cb)
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "task", "sub_agent");
	cJSON_AddStringToObject(data, "agent", agent ? agent : "");
	if (task_id)
		cJSON_AddStringToObject(data, "task_id", task_id);
	if (task)
		cJSON_AddStringToObject(data, "description", task);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = morph_event_emit_simple(rt->event_cb, rt->event_user_data,
				     MORPH_EVENT_BACKGROUND, name, phase,
				     message, data);
	cJSON_Delete(data);
	return rc;
}

int sub_agent_runtime_load_config(struct sub_agent_runtime *rt,
				  struct config_sub_agents *cfg)
{
	if (!rt || !cfg)
		return -EINVAL;
	for (int i = 0; i < cfg->count && rt->entry_count < SUB_AGENT_MAX;
	     i++) {
		struct sub_agent_entry *e = &rt->entries[rt->entry_count];
		e->cfg = cfg->entries[i];
		int rc = morph_prompt_load(rt ? rt->logger : NULL, e->cfg.system_prompt_file,
					   e->cfg.system_prompt_dir, &e->system_prompt);
		if (rc != 0)
			return rc;
		if (e->cfg.model[0])
			e->llm = rt->default_llm;
		else
			e->llm = rt->default_llm;
		if (e->cfg.max_iterations <= 0)
			e->cfg.max_iterations = 5;
		rt->entry_count++;
	}
	log_info(rt ? rt->logger : NULL, "sub-agent: loaded %d entries", rt->entry_count);
	return 0;
}

struct sub_agent_entry *sub_agent_borrow(struct sub_agent_runtime *rt, const char *name)
{
	if (!rt || !name)
		return NULL;
	for (int i = 0; i < rt->entry_count; i++) {
		if (strcmp(rt->entries[i].cfg.name, name) == 0)
			return &rt->entries[i];
	}
	return NULL;
}

struct tool_registry *sub_agent_tool_registry_create(struct sub_agent_runtime *rt,
						     struct sub_agent_entry *entry)
{
	if (!rt || !entry)
		return NULL;
	struct tool_registry *child = calloc(1, sizeof(*child));
	if (!child)
		return NULL;
	tool_registry_init(rt ? rt->logger : NULL, child);
	if (tool_registry_copy(child, rt->parent_tools) != 0) {
		tool_registry_cleanup(child);
		free(child);
		return NULL;
	}
	for (int i = child->count - 1; i >= 0; i--) {
		const char *name = child->entries[i].desc.name;
		int allowed = entry->cfg.allowed_tools_count == 0;
		for (int j = 0; j < entry->cfg.allowed_tools_count; j++) {
			if (!strcmp(name, entry->cfg.allowed_tools[j])) {
				allowed = 1;
			}
		}
		if (!allowed || tool_is_disabled(rt->parent_tools, name) ||
		    !strncmp(name, "agent_", 6) || !strcmp(name, "delegate") ||
		    !strcmp(name, "fanout")) {
			(void)tool_unregister(child, name);
		}
	}
	for (int i = 0; i < entry->cfg.disabled_tools_count; i++)
		tool_disable(child, entry->cfg.disabled_tools[i]);
	return child;
}

void sub_agent_context_cleanup(struct sub_agent_context *context)
{
	if (!context)
		return;
	react_context_destroy(context->react);
	context->react = NULL;
	if (context->tools) {
		tool_registry_cleanup(context->tools);
		free(context->tools);
		context->tools = NULL;
	}
}

int sub_agent_context_create(struct sub_agent_runtime *rt,
			     const struct tool_runtime_context *runtime,
			     struct sub_agent_entry *entry, const char *task,
			     struct sub_agent_context *out)
{
	struct sub_agent_context candidate MORPH_CLEANUP(sub_agent_context_cleanup) = {0};
	struct guardrail_config guardrail = {0};

	if (!rt || !entry || !task || !out)
		return -EINVAL;
	if (out->react || out->tools)
		return -EALREADY;
	candidate.tools = sub_agent_tool_registry_create(rt, entry);
	if (!candidate.tools)
		return -ENOMEM;
	guardrail.max_retries = 1;
	guardrail.max_empty_rounds = 2;
	candidate.react =
		react_context_create(candidate.tools, rt->tokenizer, rt->compress, &guardrail);
	if (!candidate.react)
		return -ENOMEM;
	struct react_context *child = candidate.react;
	child->llm_model = entry->llm;
	child->hitl.auto_approve_readonly = rt->auto_approve_readonly;
	child->hitl.auto_approve_media = rt->auto_approve_media;
	react_set_event_callback(child, NULL, NULL);
	child->max_iterations = entry->cfg.max_iterations;
	child->sub_agent_depth = (runtime ? runtime->sub_agent_depth : rt->depth) + 1;
	child->system_prompt_replace = strcmp(entry->cfg.system_prompt_mode, "replace") == 0;
	if (entry->system_prompt) {
		child->system_prompt = strdup(entry->system_prompt);
		if (!child->system_prompt)
			return -ENOMEM;
	}
	*out = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

static int sub_agent_run_task(struct sub_agent_runtime *rt,
			      struct sub_agent_entry *entry,
			      struct sub_agent_task *task, char **result)
{
	struct sub_agent_event_sink sink;
	struct react_context *child;
	struct agent_session_runtime session_runtime;
	struct agent_turn turn;
	struct sub_agent_context context MORPH_CLEANUP(sub_agent_context_cleanup) = {0};
	char *final_result MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *result_copy MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int64_t start;
	int64_t end;
	int rc;

	if (!rt || !entry || !task)
		return -EINVAL;
	if (task->depth >= SUB_AGENT_MAX_DEPTH)
		return -ELOOP;
	struct tool_runtime_context runtime = {.sub_agent_depth = task->depth};

	rc = sub_agent_context_create(rt, &runtime, entry, task->task_description, &context);
	if (rc != 0)
		return rc;
	child = context.react;
	sink.rt = rt;
	sink.task = task;
	react_set_event_callback(child, sub_agent_child_event, &sink);
	pthread_mutex_lock(&task->mutex);
	task->child_ctx = child;
	task->status = SUB_AGENT_RUNNING;
	pthread_mutex_unlock(&task->mutex);
	sub_agent_storage_update_task(rt, task);

	memset(&turn, 0, sizeof(turn));
	memset(&session_runtime, 0, sizeof(session_runtime));
	if (rt->db && task->child_session_id > 0) {
		session_runtime.db = rt->db;
		session_runtime.session_id = task->child_session_id;
		session_runtime.react = child;
		session_runtime.flags = AGENT_TURN_LOAD_HISTORY |
			AGENT_TURN_SAVE_TRACE | AGENT_TURN_SAVE_MESSAGES |
			AGENT_TURN_UPDATE_TOKENS;
		(void)agent_turn_begin(&turn, &session_runtime,
			&(struct agent_turn_input){
				.model_input = task->task_description,
				.stored_user_input = task->task_description,
			});
	}
	start = now_ms();
	rc = react_run(child, task->task_description, NULL, NULL);
	end = now_ms();
	if (turn.begun) {
		int finish_rc = agent_turn_finish(&turn, NULL);

		if (rc == 0 && finish_rc != 0)
			rc = finish_rc;
	}
	if (rc == 0 && child->final_answer) {
		if (entry->cfg.output_schema) {
			rc = sub_agent_apply_output_schema(child->final_answer,
				entry->cfg.output_schema, entry->llm,
				&final_result);
		} else {
			final_result = strdup(child->final_answer);
			if (!final_result)
				rc = -ENOMEM;
		}
	} else if (rc < 0) {
		final_result = strdup(morph_strerror(rc));
	} else {
		final_result = strdup("(no answer)");
	}
	if (!final_result && rc == 0)
		rc = -ENOMEM;
	if (result && rc == 0) {
		result_copy = strdup(final_result);
		if (!result_copy)
			rc = -ENOMEM;
	}
	{
		struct sub_agent_trace_event ev = {0};

		strncpy(ev.trace_id, task->id, sizeof(ev.trace_id) - 1);
		strncpy(ev.mode, task->mode, sizeof(ev.mode) - 1);
		strncpy(ev.agent_name, entry->cfg.name,
			sizeof(ev.agent_name) - 1);
		ev.start_ms = start;
		ev.end_ms = end;
		ev.iteration_count = child->step_count;
		if (final_result)
			ev.result_preview = utf8_dup_clamped(final_result, 200);
		sub_agent_trace_write(rt, &ev);
		free(ev.result_preview);
	}
	pthread_mutex_lock(&task->mutex);
	task->child_ctx = NULL;
	task->iteration_count = child->step_count;
	pthread_mutex_unlock(&task->mutex);
	sub_agent_context_cleanup(&context);

	pthread_mutex_lock(&task->mutex);
	task->result = morph_charp_steal(&final_result);
	task->error_code = rc < 0 ? rc : 0;
	task->status = rc == 0 ? SUB_AGENT_COMPLETED :
		(rc == -ECANCELED ? SUB_AGENT_CANCELLED : SUB_AGENT_FAILED);
	task->ended_at_ms = end;
	if (result && rc == 0)
		*result = morph_charp_steal(&result_copy);
	pthread_mutex_unlock(&task->mutex);
	sub_agent_storage_update_task(rt, task);
	return rc;
}

int sub_agent_invoke_sync(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
			  struct sub_agent_entry *entry, const char *task, char **result)
{
	struct sub_agent_task *task_record;
	int create_rc;
	int rc;

	if (!rt || !entry || !task || !result)
		return -EINVAL;
	if (*result)
		return -EALREADY;
	if ((runtime ? runtime->sub_agent_depth : rt->depth) >= SUB_AGENT_MAX_DEPTH)
		return -ELOOP;
	task_record =
		sub_agent_task_create(rt, entry, task, "tool", 1,
				      runtime ? runtime->sub_agent_depth : rt->depth, &create_rc);
	if (!task_record)
		return create_rc;
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent started",
					entry->cfg.name, task_record->id,
					task, 0);
	rc = sub_agent_run_task(rt, entry, task_record, result);
	sub_agent_emit_background_event(rt,
					rc == 0 ? "background.completed" :
					"background.failed",
					rc == 0 ? "end" : "failed",
					rc == 0 ? "sub-agent completed" :
					"sub-agent failed",
					entry->cfg.name, task_record->id, task, rc);
	return rc;
}

struct delegate_thread_arg {
	struct sub_agent_runtime *rt;
	struct sub_agent_entry *entry;
	struct sub_agent_task *task;
};

static void delegate_arg_cleanup(struct delegate_thread_arg **owner)
{
	free(*owner);
	*owner = NULL;
}

static void *delegate_thread_fn(void *arg)
{
	struct delegate_thread_arg *da = arg;
	struct sub_agent_runtime *rt = da->rt;
	struct sub_agent_task *task = da->task;
	int rc;

	sub_agent_emit_background_event(rt, "background.progress",
					"progress",
					"sub-agent delegate running",
					da->entry->cfg.name, task->id,
					task->task_description, 0);

	rc = sub_agent_run_task(rt, da->entry, task, NULL);
	sub_agent_emit_background_event(rt,
					task->status == SUB_AGENT_COMPLETED ?
					"background.completed" :
					"background.failed",
					task->status == SUB_AGENT_COMPLETED ?
					"end" : "failed",
					task->status == SUB_AGENT_COMPLETED ?
					"sub-agent delegate completed" :
					"sub-agent delegate failed",
					da->entry->cfg.name, task->id,
					task->task_description, rc);
	free(da);
	return NULL;
}

int sub_agent_delegate(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
		       const char *agent_name, const char *task, char **task_id_out)
{
	struct delegate_thread_arg *argument MORPH_CLEANUP(delegate_arg_cleanup) = NULL;
	char *task_id MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	struct sub_agent_entry *entry;
	struct sub_agent_task *record;
	int create_rc;
	int rc;

	if (!rt || !agent_name || !task)
		return -EINVAL;
	if (task_id_out && *task_id_out)
		return -EALREADY;
	if ((runtime ? runtime->sub_agent_depth : rt->depth) >= SUB_AGENT_MAX_DEPTH)
		return -ELOOP;
	entry = sub_agent_borrow(rt, agent_name);
	if (!entry)
		return -ENOENT;
	argument = calloc(1, sizeof(*argument));
	if (!argument)
		return -ENOMEM;
	if (task_id_out) {
		task_id = malloc(SUB_AGENT_TASK_ID_MAX);
		if (!task_id)
			return -ENOMEM;
	}
	record = sub_agent_task_create(rt, entry, task, "delegate", 0,
				       runtime ? runtime->sub_agent_depth : rt->depth, &create_rc);
	if (!record)
		return create_rc;
	if (task_id)
		snprintf(task_id, SUB_AGENT_TASK_ID_MAX, "%s", record->id);
	argument->rt = rt;
	argument->entry = entry;
	argument->task = record;
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent delegate started", entry->cfg.name, record->id,
					task, 0);
	rc = pthread_create(&record->thread, NULL, delegate_thread_fn, argument);
	if (rc != 0) {
		pthread_mutex_lock(&record->mutex);
		record->status = SUB_AGENT_FAILED;
		record->joined = 1;
		record->error_code = -rc;
		record->ended_at_ms = now_ms();
		pthread_mutex_unlock(&record->mutex);
		sub_agent_storage_update_task(rt, record);
		sub_agent_emit_background_event(rt, "background.failed", "failed",
						"sub-agent delegate failed", entry->cfg.name,
						record->id, task, -rc);
		return -rc;
	}
	argument = NULL;
	if (task_id_out)
		*task_id_out = morph_charp_steal(&task_id);
	return 0;
}

int sub_agent_check_status(struct sub_agent_runtime *rt, const char *task_id,
			   enum sub_agent_task_status *status_out, char **result_out)
{
	char *result MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	struct sub_agent_task *found;
	enum sub_agent_task_status status;
	int join_needed;

	if (!rt || !task_id)
		return -EINVAL;
	if (result_out && *result_out)
		return -EALREADY;
	pthread_mutex_lock(&rt->mutex);
	found = sub_agent_task_find_locked(rt, task_id);
	pthread_mutex_unlock(&rt->mutex);
	if (!found)
		return -ENOENT;
	pthread_mutex_lock(&found->mutex);
	status = found->status;
	if (result_out && found->result) {
		result = strdup(found->result);
		if (!result) {
			pthread_mutex_unlock(&found->mutex);
			return -ENOMEM;
		}
	}
	join_needed = (status == SUB_AGENT_COMPLETED || status == SUB_AGENT_FAILED ||
		       status == SUB_AGENT_CANCELLED) &&
		      !found->joined;
	if (join_needed)
		found->joined = 1;
	pthread_mutex_unlock(&found->mutex);
	if (join_needed) {
		int rc = pthread_join(found->thread, NULL);
		if (rc != 0) {
			pthread_mutex_lock(&found->mutex);
			found->joined = 0;
			pthread_mutex_unlock(&found->mutex);
			return -rc;
		}
	}
	if (status_out)
		*status_out = status;
	if (result_out)
		*result_out = morph_charp_steal(&result);
	return 0;
}

void sub_agent_runtime_free_task_list(struct sub_agent_task_info *tasks,
				      int count)
{
	if (!tasks)
		return;
	for (int i = 0; i < count; i++) {
		free(tasks[i].description);
		free(tasks[i].result);
	}
	free(tasks);
}

static void sub_agent_task_info_array_cleanup(morph_array_t *items)
{
	for (size_t i = 0; i < items->nelts; i++) {
		struct sub_agent_task_info *item = morph_array_borrow(items, i);
		free(item->description);
		free(item->result);
	}
	morph_array_cleanup(items);
}

static void sub_agent_events_array_cleanup(morph_array_t *events)
{
	for (size_t i = 0; i < events->nelts; i++) {
		char **event = morph_array_borrow(events, i);
		free(*event);
	}
	morph_array_cleanup(events);
}

static int sub_agent_task_info_from_runtime(struct sub_agent_runtime *rt,
					    int64_t parent_session_id,
					    morph_array_t *items)
{
	if (!rt || !items)
		return -EINVAL;
	pthread_mutex_lock(&rt->mutex);
	for (int i = 0; i < rt->task_count; i++) {
		struct sub_agent_task *task = &rt->tasks[i];
		struct sub_agent_task_info *info;

		if (parent_session_id > 0 &&
		    task->parent_session_id != parent_session_id)
			continue;
		info = morph_array_push_borrow(items);
		if (!info) {
			pthread_mutex_unlock(&rt->mutex);
			return -ENOMEM;
		}
		memset(info, 0, sizeof(*info));
		pthread_mutex_lock(&task->mutex);
		strncpy(info->id, task->id, sizeof(info->id) - 1);
		strncpy(info->agent_name,
			rt->entries[task->agent_index].cfg.name,
			sizeof(info->agent_name) - 1);
		strncpy(info->mode, task->mode, sizeof(info->mode) - 1);
		info->description = task->task_description ?
			strdup(task->task_description) : NULL;
		info->result = task->result ? strdup(task->result) : NULL;
		if ((task->task_description && !info->description) ||
		    (task->result && !info->result)) {
			pthread_mutex_unlock(&task->mutex);
			pthread_mutex_unlock(&rt->mutex);
			return -ENOMEM;
		}
		info->status = task->status;
		info->error_code = task->error_code;
		info->iteration_count = task->iteration_count;
		info->parent_session_id = task->parent_session_id;
		info->child_session_id = task->child_session_id;
		info->started_at_ms = task->started_at_ms;
		info->ended_at_ms = task->ended_at_ms;
		pthread_mutex_unlock(&task->mutex);
	}
	pthread_mutex_unlock(&rt->mutex);
	return 0;
}

int sub_agent_runtime_task_list_copy(struct sub_agent_runtime *rt, int64_t parent_session_id,
				     struct sub_agent_task_info **out, int *count)
{
	morph_array_t items MORPH_CLEANUP(sub_agent_task_info_array_cleanup) = {0};
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!rt || !out || !count || parent_session_id < 0)
		return -EINVAL;
	if (*out)
		return -EALREADY;
	*count = 0;
	rc = morph_array_init(&items, MORPH_ARRAY_INIT_CAP,
			      sizeof(struct sub_agent_task_info));
	if (rc != 0)
		return rc;
	if (!rt->db || !rt->db->handle) {
		rc = sub_agent_task_info_from_runtime(rt, parent_session_id,
						      &items);
		if (rc != 0) {
			return rc;
		}
	} else {
		pthread_mutex_lock(&rt->storage_mutex);
		rc = sqlite3_prepare_v2(rt->db->handle,
			"SELECT task_id,agent_name,mode,description,status,result,"
			"error_code,iterations,parent_session_id,child_session_id,"
			"started_at,ended_at FROM sub_agent_tasks WHERE "
			"(?=0 OR parent_session_id=?) ORDER BY started_at DESC", -1,
			&stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, parent_session_id);
			sqlite3_bind_int64(stmt, 2, parent_session_id);
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				struct sub_agent_task_info *info = morph_array_push_borrow(&items);
				const char *value;

				if (!info) {
					rc = SQLITE_NOMEM;
					break;
				}
				memset(info, 0, sizeof(*info));
				value = (const char *)sqlite3_column_text(stmt, 0);
				if (value)
					strncpy(info->id, value,
						sizeof(info->id) - 1);
				value = (const char *)sqlite3_column_text(stmt, 1);
				if (value)
					strncpy(info->agent_name, value,
						sizeof(info->agent_name) - 1);
				value = (const char *)sqlite3_column_text(stmt, 2);
				if (value)
					strncpy(info->mode, value,
						sizeof(info->mode) - 1);
				value = (const char *)sqlite3_column_text(stmt, 3);
				info->description = value ? strdup(value) : NULL;
				if (value && !info->description) {
					rc = SQLITE_NOMEM;
					break;
				}
				info->status = (enum sub_agent_task_status)
					sqlite3_column_int(stmt, 4);
				value = (const char *)sqlite3_column_text(stmt, 5);
				info->result = value ? strdup(value) : NULL;
				if (value && !info->result) {
					rc = SQLITE_NOMEM;
					break;
				}
				info->error_code = sqlite3_column_int(stmt, 6);
				info->iteration_count = sqlite3_column_int(stmt, 7);
				info->parent_session_id = sqlite3_column_int64(stmt, 8);
				info->child_session_id = sqlite3_column_int64(stmt, 9);
				info->started_at_ms = sqlite3_column_int64(stmt, 10);
				info->ended_at_ms = sqlite3_column_int64(stmt, 11);
			}
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&rt->storage_mutex);
		if (rc != SQLITE_DONE) {
			return rc == SQLITE_NOMEM ? -ENOMEM : MORPH_ERR_DB;
		}
	}
	if (items.nelts > INT_MAX) {
		return -EOVERFLOW;
	}
	*out = items.elts;
	*count = (int)items.nelts;
	memset(&items, 0, sizeof(items));
	return 0;
}

void sub_agent_runtime_free_events(char **events, int count)
{
	if (!events)
		return;
	for (int i = 0; i < count; i++)
		free(events[i]);
	free(events);
}

int sub_agent_runtime_task_events_copy(struct sub_agent_runtime *rt, const char *task_id,
				       char ***events, int *count)
{
	morph_array_t out MORPH_CLEANUP(sub_agent_events_array_cleanup) = {0};
	struct sub_agent_task *task = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!rt || !task_id || !events || !count)
		return -EINVAL;
	if (*events)
		return -EALREADY;
	*count = 0;
	rc = morph_array_init(&out, MORPH_ARRAY_INIT_CAP, sizeof(char *));
	if (rc != 0)
		return rc;
	if (rt->db && rt->db->handle) {
		pthread_mutex_lock(&rt->storage_mutex);
		rc = sqlite3_prepare_v2(rt->db->handle,
			"SELECT event_json FROM sub_agent_events WHERE task_id=? "
			"ORDER BY id", -1, &stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
				const char *value =
					(const char *)sqlite3_column_text(stmt, 0);
				char **slot = morph_array_push_borrow(&out);

				if (!slot || !(*slot = strdup(value ? value : ""))) {
					rc = SQLITE_NOMEM;
					break;
				}
			}
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&rt->storage_mutex);
		if (rc != SQLITE_DONE) {
			return rc == SQLITE_NOMEM ? -ENOMEM : MORPH_ERR_DB;
		}
	} else {
		pthread_mutex_lock(&rt->mutex);
		task = sub_agent_task_find_locked(rt, task_id);
		pthread_mutex_unlock(&rt->mutex);
		if (!task) {
			return -ENOENT;
		}
		pthread_mutex_lock(&task->mutex);
		for (size_t i = 0; i < task->events.nelts; i++) {
			char **source = morph_array_borrow(&task->events, i);
			char **slot = morph_array_push_borrow(&out);

			if (!slot || !(*slot = strdup(*source))) {
				pthread_mutex_unlock(&task->mutex);
				return -ENOMEM;
			}
		}
		pthread_mutex_unlock(&task->mutex);
	}
	if (out.nelts > INT_MAX) {
		return -EOVERFLOW;
	}
	*events = out.elts;
	*count = (int)out.nelts;
	memset(&out, 0, sizeof(out));
	return 0;
}

struct fanout_worker_arg {
	volatile sig_atomic_t finished;
	struct tool_runtime_context runtime;
	struct sub_agent_runtime *rt;
	struct sub_agent_entry *entry;
	const char *task;
	char *result;
	int rc;
};

struct fanout_workers {
	struct fanout_worker_arg *items;
	pthread_t *threads;
	int count;
};

static void fanout_workers_cleanup(struct fanout_workers *owner)
{
	if (owner->items) {
		for (int i = 0; i < owner->count; i++)
			free(owner->items[i].result);
	}
	free(owner->items);
	free(owner->threads);
	memset(owner, 0, sizeof(*owner));
}

static int fanout_raw_result_create(const struct fanout_worker_arg *workers, const char **tasks,
				    int count, char **out)
{
	cJSON *array MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	char *text MORPH_CLEANUP(morph_json_text_cleanup) = NULL;

	if (!array)
		return -ENOMEM;
	for (int i = 0; i < count; i++) {
		cJSON *item MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();

		if (!item || !cJSON_AddStringToObject(item, "task", tasks[i]) ||
		    !cJSON_AddStringToObject(item, "result",
					     workers[i].result ? workers[i].result : "(no result)"))
			return -ENOMEM;
		if (morph_json_array_take(array, &item) != 0)
			return -ENOMEM;
	}
	text = morph_json_print_copy(array);
	if (!text)
		return -ENOMEM;
	*out = strdup(text);
	return *out ? 0 : -ENOMEM;
}

static void *fanout_worker_fn(void *arg)
{
	struct fanout_worker_arg *fw = arg;
	fw->rc = sub_agent_invoke_sync(fw->rt, &fw->runtime, fw->entry, fw->task, &fw->result);
	__atomic_store_n(&fw->finished, 1, __ATOMIC_RELEASE);
	return NULL;
}

/**
 * \brief Execute fanout using optional instance-local platform operations.
 * \param[in,out] rt: Borrowed runtime, kept alive through worker reclamation.
 * \param[in] runtime: Optional borrowed execution context until all workers finish.
 * \param[in] agent_name: Borrowed configured agent name.
 * \param[in] tasks: Borrowed task strings through worker completion.
 * \param[in] task_count: Positive number of tasks.
 * \param[in] merge: Existing result merge policy.
 * \param[out] result: Owned merged text in an empty output; failures preserve NULL.
 * \param[in] ops: Optional borrowed platform seam; NULL uses pthread directly.
 * \return Zero on success, negative error after reclaiming every started worker.
 */
int sub_agent_fanout_with_ops(struct sub_agent_runtime *rt,
			      const struct tool_runtime_context *runtime, const char *agent_name,
			      const char **tasks, int task_count,
			      enum sub_agent_merge_strategy merge, char **result,
			      const struct sub_agent_thread_ops *ops)
{
	struct fanout_workers owner MORPH_CLEANUP(fanout_workers_cleanup) = {0};
	char *merged MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int started = 0;
	int thread_rc = 0;

	if (!rt || !agent_name || !tasks || task_count <= 0 || !result)
		return -EINVAL;
	if (*result)
		return -EALREADY;
	if ((runtime ? runtime->sub_agent_depth : rt->depth) >= SUB_AGENT_MAX_DEPTH)
		return -ELOOP;
	struct sub_agent_entry *entry = sub_agent_borrow(rt, agent_name);
	if (!entry)
		return -ENOENT;
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent fanout started",
					entry->cfg.name, NULL, agent_name, 0);
	int n = task_count > SUB_AGENT_TASK_MAX
		? SUB_AGENT_TASK_MAX : task_count;
	for (int i = 0; i < n; i++) {
		if (!tasks[i])
			return -EINVAL;
	}
	owner.count = n;
	owner.items = calloc((size_t)n, sizeof(*owner.items));
	owner.threads = calloc((size_t)n, sizeof(*owner.threads));
	struct fanout_worker_arg *workers = owner.items;
	pthread_t *threads = owner.threads;
	if (!workers || !threads) {
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent fanout failed",
						entry->cfg.name, NULL,
						agent_name, -ENOMEM);
		return -ENOMEM;
	}
	for (int i = 0; i < n; i++) {
		if (runtime)
			workers[i].runtime = *runtime;
		else
			workers[i].runtime.sub_agent_depth = rt->depth;
		workers[i].rt = rt;
		workers[i].entry = entry;
		workers[i].task = tasks[i];
		workers[i].result = NULL;
		workers[i].rc = 0;
		thread_rc =
			ops != NULL && ops->create != NULL
				? ops->create(&threads[i], fanout_worker_fn, &workers[i],
					      ops->user_data)
				: pthread_create(&threads[i], NULL, fanout_worker_fn, &workers[i]);
		if (thread_rc != 0) {
			thread_rc = -thread_rc;
			break;
		}
		started++;
	}
	for (int i = 0; i < started; i++) {
		int join_rc = ops != NULL && ops->join != NULL
				      ? ops->join(threads[i], ops->user_data)
				      : pthread_join(threads[i], NULL);
		if (join_rc != 0) {
			while (!__atomic_load_n(&workers[i].finished, __ATOMIC_ACQUIRE)) {
				struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
				nanosleep(&pause, NULL);
			}
			(void)pthread_detach(threads[i]);
		}

		if (join_rc != 0 && thread_rc == 0) {
			thread_rc = -join_rc;
		}
	}
	if (thread_rc != 0) {
		return thread_rc;
	}
	int any_success = 0;
	for (int i = 0; i < n; i++) {
		if (workers[i].rc == 0)
			any_success = 1;
	}
	if (!any_success) {
		int first_err = workers[0].rc;
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent fanout failed",
						entry->cfg.name, NULL,
						agent_name,
						first_err ? first_err : -EIO);
		return first_err ? first_err : -EIO;
	}
	if (merge == SUB_AGENT_MERGE_RAW) {
		int merge_rc = fanout_raw_result_create(workers, tasks, n, &merged);

		if (merge_rc != 0)
			return merge_rc;
	} else if (merge == SUB_AGENT_MERGE_SYNTHESIZE) {
		morph_buf_t combined_buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int buf_rc = morph_buf_init(&combined_buf, 4096);
		if (buf_rc != 0) {
			return buf_rc;
		}
		for (int i = 0; i < n; i++) {
			const char *r = workers[i].result
				? workers[i].result : "(no result)";
			buf_rc = morph_buf_printf(&combined_buf,
						  "--- Task %d: %s ---\n%s\n\n",
						  i + 1, tasks[i], r);
			if (buf_rc != 0) {
				morph_buf_cleanup(&combined_buf);
				return buf_rc;
			}
		}
		struct model *llm = entry->llm;
		if (llm && llm->chat && llm->api_key[0]) {
			{
				const char *sys =
					"You are a synthesis engine. "
					"Given the following results from "
					"parallel sub-tasks, produce a "
					"coherent, unified summary that "
					"preserves all key findings, data, "
					"and insights. Do not lose any "
					"important information.";
				const char *msgs[] = { combined_buf.data };
				morph_buf_t syn_buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
				if (morph_buf_init(&syn_buf, 8192) == 0) {
					struct tool_runtime_stream_sink stream = {.runtime =
											  runtime};
					stream.tool = "fanout";
					stream.kind = "text";
					stream.buf = &syn_buf;
					int src = llm->chat(llm, sys, msgs, 1, NULL,
							    tool_runtime_stream_to_buf_cb, &stream);
					if (src == 0 && syn_buf.len > 0) {
						morph_buf_cleanup(&combined_buf);
						morph_buf_move(&combined_buf, &syn_buf);
					} else {
						morph_buf_cleanup(&syn_buf);
					}
				}
			}
		}
		merged = morph_buf_detach(&combined_buf);
	} else {
		morph_buf_t combined_buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int buf_rc = morph_buf_init(&combined_buf, 4096);
		if (buf_rc != 0) {
			return buf_rc;
		}
		for (int i = 0; i < n; i++) {
			const char *r = workers[i].result
				? workers[i].result : "(no result)";
			buf_rc = morph_buf_printf(&combined_buf, "%s\n", r);
			if (buf_rc != 0) {
				morph_buf_cleanup(&combined_buf);
				return buf_rc;
			}
		}
		merged = morph_buf_detach(&combined_buf);
	}
	if (!merged)
		return -ENOMEM;
	*result = morph_charp_steal(&merged);
	sub_agent_emit_background_event(rt, "background.completed", "end",
					"sub-agent fanout completed",
					entry->cfg.name, NULL, agent_name, 0);
	return 0;
}

int sub_agent_apply_output_schema(const char *text, const char *schema, struct model *llm,
				  char **result)
{
	morph_buf_t prompt MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t response MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!text || !schema || !result)
		return -EINVAL;
	if (*result)
		return -EALREADY;
	if (!llm || !llm->chat || !llm->api_key[0]) {
		*result = strdup(text);
		return *result ? 0 : -ENOMEM;
	}
	rc = morph_buf_init(&prompt, 4096);
	if (rc == 0)
		rc = morph_buf_printf(&prompt,
				      "Extract information from the following text according "
				      "to this JSON schema. Return ONLY valid JSON matching "
				      "the schema, no other text.\n\nSchema: %s\n\nText: %s",
				      schema, text);
	if (rc == 0)
		rc = morph_buf_init(&response, 8192);
	if (rc != 0)
		return rc;
	const char *messages[] = {morph_buf_cstr_borrow(&prompt)};
	rc = llm->chat(llm,
		       "You are a structured data extraction engine. "
		       "Return ONLY valid JSON.",
		       messages, 1, NULL, morph_buf_append_cb, &response);
	if (rc >= 0 && response.len) {
		cJSON *parsed = cJSON_ParseWithOpts(morph_buf_cstr_borrow(&response), NULL, 1);
		if (parsed) {
			cJSON_Delete(parsed);
			*result = morph_buf_detach(&response);
			return 0;
		}
	}
	*result = strdup(text);
	return *result ? 0 : -ENOMEM;
}

void sub_agent_trace_write(struct sub_agent_runtime *rt,
			   struct sub_agent_trace_event *ev)
{
	if (!rt || !ev || !rt->trace_file[0])
		return;
	FILE *f = fopen(rt->trace_file, "a");
	if (!f)
		return;
	cJSON *obj = cJSON_CreateObject();
	if (!obj) {
		fclose(f);
		return;
	}
	cJSON_AddStringToObject(obj, "trace_id", ev->trace_id);
	if (ev->parent_trace_id[0])
		cJSON_AddStringToObject(obj, "parent_id",
					ev->parent_trace_id);
	cJSON_AddStringToObject(obj, "agent", ev->agent_name);
	cJSON_AddNumberToObject(obj, "start_ms",
				(double)ev->start_ms);
	cJSON_AddNumberToObject(obj, "end_ms", (double)ev->end_ms);
	cJSON_AddStringToObject(obj, "mode", ev->mode);
	cJSON_AddNumberToObject(obj, "iterations", ev->iteration_count);
	if (ev->result_preview)
		cJSON_AddStringToObject(obj, "result_preview",
					ev->result_preview);
	{
		time_t now = time(NULL);
		struct tm tm_local;
		localtime_r(&now, &tm_local);
		char ts[64];
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_local);
		cJSON_AddStringToObject(obj, "ts", ts);
	}
	char *json_str = cJSON_PrintUnformatted(obj);
	if (json_str) {
		fprintf(f, "%s\n", json_str);
		free(json_str);
	}
	cJSON_Delete(obj);
	fclose(f);
}

/** \brief Fan out tasks, returning NULL output on submission or join failure. */
int sub_agent_fanout(struct sub_agent_runtime *rt, const struct tool_runtime_context *runtime,
		     const char *agent_name, const char **tasks, int task_count,
		     enum sub_agent_merge_strategy merge, char **result)
{
	return sub_agent_fanout_with_ops(rt, runtime, agent_name, tasks, task_count, merge, result,
					 NULL);
}
