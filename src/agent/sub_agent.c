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
#include "util/arena.h"
#include "util/buf.h"
#include "util/utf8.h"
#include "util/error.h"
#include "util/file.h"
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

static _Thread_local int sub_agent_thread_depth;

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
		MORPH_RETURN(MORPH_ERR_DB);
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

static struct sub_agent_task *sub_agent_task_create(
	struct sub_agent_runtime *rt, struct sub_agent_entry *entry,
	const char *description, const char *mode, int joined, int *error)
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
	json = morph_event_to_json_string(&scoped);
	if (json) {
		pthread_mutex_lock(&task->mutex);
		if (task->events_initialized) {
			char **slot = morph_array_push(&task->events);
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
		char *stored = morph_event_to_json_string(&scoped);

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
	rt->parent_tools = parent_tools;
	rt->default_llm = default_llm;
	rt->tokenizer = tokenizer;
	rt->compress = compress;
	rt->depth = 0;
	rt->next_task_id = 0;
	pthread_mutex_init(&rt->mutex, NULL);
	pthread_mutex_init(&rt->storage_mutex, NULL);
	rt->mutexes_initialized = 1;
	{
		char *home = file_expand_path("~/.morph/log");
		if (home) {
			file_ensure_dir(home);
			snprintf(rt->trace_file, sizeof(rt->trace_file),
				 "%s/traces.jsonl", home);
			free(home);
		}
	}
	return rt;
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

static char *load_file_contents(const char *path)
{
	if (!path || !*path)
		return NULL;
	char *expanded = file_expand_path(path);
	if (!expanded)
		expanded = strdup(path);
	if (!expanded)
		return NULL;
	FILE *f = fopen(expanded, "r");
	free(expanded);
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, f);
	buf[rd] = '\0';
	fclose(f);
	return buf;
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
		if (e->cfg.system_prompt_file[0])
			e->system_prompt = load_file_contents(
				e->cfg.system_prompt_file);
		if (e->cfg.model[0])
			e->llm = rt->default_llm;
		else
			e->llm = rt->default_llm;
		if (e->cfg.max_iterations <= 0)
			e->cfg.max_iterations = 5;
		rt->entry_count++;
	}
	log_info("sub-agent: loaded %d entries", rt->entry_count);
	return 0;
}

struct sub_agent_entry *
sub_agent_find(struct sub_agent_runtime *rt, const char *name)
{
	if (!rt || !name)
		return NULL;
	for (int i = 0; i < rt->entry_count; i++) {
		if (strcmp(rt->entries[i].cfg.name, name) == 0)
			return &rt->entries[i];
	}
	return NULL;
}

struct tool_registry *
sub_agent_build_tool_registry(struct sub_agent_runtime *rt,
			      struct sub_agent_entry *entry)
{
	if (!rt || !entry)
		return NULL;
	struct tool_registry *child = calloc(1, sizeof(*child));
	if (!child)
		return NULL;
	tool_registry_init(child);
	if (!entry->cfg.allowed_tools_count) {
		for (int i = 0; i < rt->parent_tools->count; i++) {
			struct tool_entry *pe = &rt->parent_tools->entries[i];
			if (strncmp(pe->desc.name, "agent_", 6) == 0 ||
			    strcmp(pe->desc.name, "delegate") == 0 ||
			    strcmp(pe->desc.name, "fanout") == 0 ||
			    strcmp(pe->desc.name, "agent_status") == 0)
				continue;
			if (tool_is_disabled(rt->parent_tools, pe->desc.name))
				continue;
			struct tool_spec spec = {
				.origin = pe->origin,
				.name = pe->desc.name,
				.title = pe->desc.title,
				.description = pe->desc.description,
				.input_schema = pe->desc.input_schema,
				.output_schema = pe->desc.output_schema,
				.input_kind = pe->desc.input_kind,
				.input_format = pe->desc.input_format,
				.exec = pe->exec,
				.user_data = pe->user_data,
				.flags = pe->flags,
				.timeout_seconds = pe->timeout_seconds,
			};
			(void)tool_register(child, &spec);
		}
	} else {
		for (int i = 0; i < entry->cfg.allowed_tools_count; i++) {
			const char *tname = entry->cfg.allowed_tools[i];
			struct tool_entry *pe = tool_lookup(rt->parent_tools,
							   tname);
			if (!pe)
				continue;
			struct tool_spec spec = {
				.origin = pe->origin,
				.name = pe->desc.name,
				.title = pe->desc.title,
				.description = pe->desc.description,
				.input_schema = pe->desc.input_schema,
				.output_schema = pe->desc.output_schema,
				.input_kind = pe->desc.input_kind,
				.input_format = pe->desc.input_format,
				.exec = pe->exec,
				.user_data = pe->user_data,
				.flags = pe->flags,
				.timeout_seconds = pe->timeout_seconds,
			};
			(void)tool_register(child, &spec);
		}
	}
	for (int i = 0; i < entry->cfg.disabled_tools_count; i++)
		tool_disable(child, entry->cfg.disabled_tools[i]);
	return child;
}

struct react_context *
sub_agent_create_context(struct sub_agent_runtime *rt,
			 struct sub_agent_entry *entry,
			 const char *task)
{
	if (!rt || !entry || !task)
		return NULL;
	struct tool_registry *child_tools =
		sub_agent_build_tool_registry(rt, entry);
	if (!child_tools)
		return NULL;
	struct guardrail_config gcfg = {0};
	gcfg.enabled = 0;
	gcfg.max_retries = 1;
	gcfg.max_empty_rounds = 2;
	struct react_context *child = react_context_create(
		child_tools, rt->tokenizer, rt->compress, &gcfg);
	if (!child) {
		tool_registry_cleanup(child_tools);
		free(child_tools);
		return NULL;
	}
	child->llm_model = entry->llm;
	react_set_event_callback(child, NULL, NULL);
	child->max_iterations = entry->cfg.max_iterations;
	child->sub_agent_depth = rt->depth + sub_agent_thread_depth + 1;
	if (entry->system_prompt) {
		free(child->system_prompt);
		child->system_prompt = strdup(entry->system_prompt);
	}
	return child;
}

static int sub_agent_run_task(struct sub_agent_runtime *rt,
			      struct sub_agent_entry *entry,
			      struct sub_agent_task *task, char **result)
{
	struct sub_agent_event_sink sink;
	struct react_context *child;
	struct agent_session_runtime session_runtime;
	struct agent_turn turn;
	struct tool_registry *child_tools;
	char *final_result = NULL;
	int64_t start;
	int64_t end;
	int rc;

	if (!rt || !entry || !task)
		return -EINVAL;
	if (rt->depth + sub_agent_thread_depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	sub_agent_thread_depth++;
	child = sub_agent_create_context(rt, entry, task->task_description);
	if (!child) {
		sub_agent_thread_depth--;
		MORPH_RETURN(-ENOMEM);
	}
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
	child_tools = child->tools;
	pthread_mutex_lock(&task->mutex);
	task->child_ctx = NULL;
	task->iteration_count = child->step_count;
	pthread_mutex_unlock(&task->mutex);
	react_context_destroy(child);
	tool_registry_cleanup(child_tools);
	free(child_tools);

	pthread_mutex_lock(&task->mutex);
	task->result = final_result;
	task->error_code = rc < 0 ? rc : 0;
	task->status = rc == 0 ? SUB_AGENT_COMPLETED :
		(rc == -ECANCELED ? SUB_AGENT_CANCELLED : SUB_AGENT_FAILED);
	task->ended_at_ms = end;
	if (result)
		*result = strdup(task->result ? task->result : "(no answer)");
	pthread_mutex_unlock(&task->mutex);
	sub_agent_storage_update_task(rt, task);
	sub_agent_thread_depth--;
	return rc;
}

int sub_agent_invoke_sync(struct sub_agent_runtime *rt,
			  struct sub_agent_entry *entry,
			  const char *task, char **result)
{
	struct sub_agent_task *task_record;
	int create_rc;
	int rc;

	if (!rt || !entry || !task || !result)
		return -EINVAL;
	if (rt->depth + sub_agent_thread_depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	task_record = sub_agent_task_create(rt, entry, task, "tool", 1,
					    &create_rc);
	if (!task_record)
		MORPH_RETURN(create_rc);
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

int sub_agent_delegate(struct sub_agent_runtime *rt,
		       const char *agent_name, const char *task,
		       char **task_id_out)
{
	struct sub_agent_entry *entry;
	struct sub_agent_task *t;
	struct delegate_thread_arg *da;
	int create_rc;
	int rc;

	if (!rt || !agent_name || !task)
		return -EINVAL;
	if (rt->depth + sub_agent_thread_depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	entry = sub_agent_find(rt, agent_name);
	if (!entry)
		MORPH_RETURN(-ENOENT);
	t = sub_agent_task_create(rt, entry, task, "delegate", 0,
				  &create_rc);
	if (!t)
		MORPH_RETURN(create_rc);
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent delegate started",
					entry->cfg.name, t->id, task, 0);
	da = calloc(1, sizeof(*da));
	if (!da) {
		pthread_mutex_lock(&t->mutex);
		t->status = SUB_AGENT_FAILED;
		t->error_code = -ENOMEM;
		t->ended_at_ms = now_ms();
		pthread_mutex_unlock(&t->mutex);
		sub_agent_storage_update_task(rt, t);
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent delegate failed",
						entry->cfg.name, t->id, task,
						-ENOMEM);
		MORPH_RETURN(-ENOMEM);
	}
	da->rt = rt;
	da->entry = entry;
	da->task = t;
	rc = pthread_create(&t->thread, NULL, delegate_thread_fn, da);
	if (rc != 0) {
		free(da);
		pthread_mutex_lock(&t->mutex);
		t->status = SUB_AGENT_FAILED;
		t->error_code = -rc;
		t->ended_at_ms = now_ms();
		pthread_mutex_unlock(&t->mutex);
		sub_agent_storage_update_task(rt, t);
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent delegate failed",
						entry->cfg.name, t->id, task,
						-rc);
		MORPH_RETURN(-rc);
	}
	if (task_id_out)
		*task_id_out = strdup(t->id);
	return 0;
}

int sub_agent_check_status(struct sub_agent_runtime *rt,
			   const char *task_id,
			   enum sub_agent_task_status *status_out,
			   char **result_out)
{
	struct sub_agent_task *found;
	enum sub_agent_task_status st;
	char *res;

	if (!rt || !task_id)
		return -EINVAL;
	pthread_mutex_lock(&rt->mutex);
	found = sub_agent_task_find_locked(rt, task_id);
	pthread_mutex_unlock(&rt->mutex);
	if (!found)
		MORPH_RETURN(-ENOENT);
	pthread_mutex_lock(&found->mutex);
	st = found->status;
	res = found->result ? strdup(found->result) : NULL;
	if ((st == SUB_AGENT_COMPLETED || st == SUB_AGENT_FAILED ||
	     st == SUB_AGENT_CANCELLED) && !found->joined) {
		found->joined = 1;
		pthread_mutex_unlock(&found->mutex);
		pthread_join(found->thread, NULL);
		if (found->child_ctx) {
			struct tool_registry *ct = found->child_ctx->tools;
			react_context_destroy(found->child_ctx);
			found->child_ctx = NULL;
			tool_registry_cleanup(ct);
			free(ct);
		}
	} else {
		pthread_mutex_unlock(&found->mutex);
	}
	if (status_out)
		*status_out = st;
	if (result_out)
		*result_out = res;
	else
		free(res);
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
		info = morph_array_push(items);
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

int sub_agent_runtime_list_tasks(struct sub_agent_runtime *rt,
				 int64_t parent_session_id,
				 struct sub_agent_task_info **out,
				 int *count)
{
	morph_array_t items;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!rt || !out || !count || parent_session_id < 0)
		return -EINVAL;
	*out = NULL;
	*count = 0;
	rc = morph_array_init(&items, MORPH_ARRAY_INIT_CAP,
			      sizeof(struct sub_agent_task_info));
	if (rc != 0)
		return rc;
	if (!rt->db || !rt->db->handle) {
		rc = sub_agent_task_info_from_runtime(rt, parent_session_id,
						      &items);
		if (rc != 0) {
			morph_array_cleanup(&items);
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
				struct sub_agent_task_info *info =
					morph_array_push(&items);
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
				info->status = (enum sub_agent_task_status)
					sqlite3_column_int(stmt, 4);
				value = (const char *)sqlite3_column_text(stmt, 5);
				info->result = value ? strdup(value) : NULL;
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
			sub_agent_runtime_free_task_list(
				(struct sub_agent_task_info *)items.elts,
				(int)items.nelts);
			return rc == SQLITE_NOMEM ? -ENOMEM : MORPH_ERR_DB;
		}
	}
	if (items.nelts > INT_MAX) {
		sub_agent_runtime_free_task_list(
			(struct sub_agent_task_info *)items.elts,
			(int)items.nelts);
		return -EOVERFLOW;
	}
	*out = items.elts;
	*count = (int)items.nelts;
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

int sub_agent_runtime_task_events(struct sub_agent_runtime *rt,
				  const char *task_id,
				  char ***events, int *count)
{
	morph_array_t out;
	struct sub_agent_task *task = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!rt || !task_id || !events || !count)
		return -EINVAL;
	*events = NULL;
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
				char **slot = morph_array_push(&out);

				if (!slot || !(*slot = strdup(value ? value : ""))) {
					rc = SQLITE_NOMEM;
					break;
				}
			}
		}
		sqlite3_finalize(stmt);
		pthread_mutex_unlock(&rt->storage_mutex);
		if (rc != SQLITE_DONE) {
			sub_agent_runtime_free_events((char **)out.elts,
						      (int)out.nelts);
			return rc == SQLITE_NOMEM ? -ENOMEM : MORPH_ERR_DB;
		}
	} else {
		pthread_mutex_lock(&rt->mutex);
		task = sub_agent_task_find_locked(rt, task_id);
		pthread_mutex_unlock(&rt->mutex);
		if (!task) {
			morph_array_cleanup(&out);
			return -ENOENT;
		}
		pthread_mutex_lock(&task->mutex);
		for (size_t i = 0; i < task->events.nelts; i++) {
			char **source = morph_array_get(&task->events, i);
			char **slot = morph_array_push(&out);

			if (!slot || !(*slot = strdup(*source))) {
				pthread_mutex_unlock(&task->mutex);
				sub_agent_runtime_free_events((char **)out.elts,
							      (int)out.nelts);
				return -ENOMEM;
			}
		}
		pthread_mutex_unlock(&task->mutex);
	}
	if (out.nelts > INT_MAX) {
		sub_agent_runtime_free_events((char **)out.elts, (int)out.nelts);
		return -EOVERFLOW;
	}
	*events = out.elts;
	*count = (int)out.nelts;
	return 0;
}

struct fanout_worker_arg {
	struct sub_agent_runtime *rt;
	struct sub_agent_entry *entry;
	const char *task;
	char *result;
	int rc;
};

static void *fanout_worker_fn(void *arg)
{
	struct fanout_worker_arg *fw = arg;
	fw->rc = sub_agent_invoke_sync(fw->rt, fw->entry, fw->task,
				       &fw->result);
	return NULL;
}

int sub_agent_fanout(struct sub_agent_runtime *rt,
		     const char *agent_name,
		     const char **tasks, int task_count,
		     enum sub_agent_merge_strategy merge,
		     char **result)
{
	if (!rt || !agent_name || !tasks || task_count <= 0 || !result)
		return -EINVAL;
	if (rt->depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	struct sub_agent_entry *entry = sub_agent_find(rt, agent_name);
	if (!entry)
		MORPH_RETURN(-ENOENT);
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent fanout started",
					entry->cfg.name, NULL, agent_name, 0);
	int n = task_count > SUB_AGENT_TASK_MAX
		? SUB_AGENT_TASK_MAX : task_count;
	struct fanout_worker_arg *workers = calloc((size_t)n,
						   sizeof(*workers));
	pthread_t *threads = calloc((size_t)n, sizeof(*threads));
	if (!workers || !threads) {
		free(workers);
		free(threads);
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent fanout failed",
						entry->cfg.name, NULL,
						agent_name, -ENOMEM);
		MORPH_RETURN(-ENOMEM);
	}
	rt->depth++;
	for (int i = 0; i < n; i++) {
		workers[i].rt = rt;
		workers[i].entry = entry;
		workers[i].task = tasks[i];
		workers[i].result = NULL;
		workers[i].rc = 0;
		pthread_create(&threads[i], NULL, fanout_worker_fn,
			       &workers[i]);
	}
	for (int i = 0; i < n; i++)
		pthread_join(threads[i], NULL);
	rt->depth--;
	int any_success = 0;
	for (int i = 0; i < n; i++) {
		if (workers[i].rc == 0)
			any_success = 1;
	}
	if (!any_success) {
		int first_err = workers[0].rc;
		for (int i = 0; i < n; i++)
			free(workers[i].result);
		free(workers);
		free(threads);
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent fanout failed",
						entry->cfg.name, NULL,
						agent_name,
						first_err ? first_err : -EIO);
		MORPH_RETURN(first_err ? first_err : -EIO);
	}
	if (merge == SUB_AGENT_MERGE_RAW) {
		cJSON *arr = cJSON_CreateArray();
		for (int i = 0; i < n; i++) {
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "task", tasks[i]);
			if (workers[i].result)
				cJSON_AddStringToObject(item, "result",
							workers[i].result);
			else
				cJSON_AddStringToObject(item, "result",
							"(no result)");
			cJSON_AddItemToArray(arr, item);
		}
		*result = cJSON_PrintUnformatted(arr);
		cJSON_Delete(arr);
	} else if (merge == SUB_AGENT_MERGE_SYNTHESIZE) {
		morph_buf_t combined_buf;
		int buf_rc = morph_buf_init(&combined_buf, 4096);
		if (buf_rc != 0) {
			for (int i = 0; i < n; i++)
				free(workers[i].result);
			free(workers);
			free(threads);
			MORPH_RETURN(buf_rc);
		}
		for (int i = 0; i < n; i++) {
			const char *r = workers[i].result
				? workers[i].result : "(no result)";
			buf_rc = morph_buf_printf(&combined_buf,
						  "--- Task %d: %s ---\n%s\n\n",
						  i + 1, tasks[i], r);
			if (buf_rc != 0) {
				morph_buf_cleanup(&combined_buf);
				for (int j = 0; j < n; j++)
					free(workers[j].result);
				free(workers);
				free(threads);
				MORPH_RETURN(buf_rc);
			}
		}
		struct model *llm = entry->llm;
		if (llm && llm->chat && llm->api_key[0]) {
			struct arena *arena = arena_create(64 * 1024);
			if (arena) {
				const char *sys =
					"You are a synthesis engine. "
					"Given the following results from "
					"parallel sub-tasks, produce a "
					"coherent, unified summary that "
					"preserves all key findings, data, "
					"and insights. Do not lose any "
					"important information.";
				const char *msgs[] = { combined_buf.data };
				morph_buf_t syn_buf;
				if (morph_buf_init(&syn_buf, 8192) == 0) {
					struct tool_runtime_stream_sink stream;
					stream.tool = "fanout";
					stream.kind = "text";
					stream.buf = &syn_buf;
					int src = llm->chat(llm, arena, sys,
							    msgs, 1, NULL,
							    tool_runtime_stream_to_buf_cb,
							    &stream);
					if (src == 0 && syn_buf.len > 0) {
						morph_buf_cleanup(&combined_buf);
						combined_buf = syn_buf;
					} else {
						morph_buf_cleanup(&syn_buf);
					}
				}
				arena_destroy(arena);
			}
		}
		*result = morph_buf_detach(&combined_buf);
	} else {
		morph_buf_t combined_buf;
		int buf_rc = morph_buf_init(&combined_buf, 4096);
		if (buf_rc != 0) {
			for (int i = 0; i < n; i++)
				free(workers[i].result);
			free(workers);
			free(threads);
			MORPH_RETURN(buf_rc);
		}
		for (int i = 0; i < n; i++) {
			const char *r = workers[i].result
				? workers[i].result : "(no result)";
			buf_rc = morph_buf_printf(&combined_buf, "%s\n", r);
			if (buf_rc != 0) {
				morph_buf_cleanup(&combined_buf);
				for (int j = 0; j < n; j++)
					free(workers[j].result);
				free(workers);
				free(threads);
				MORPH_RETURN(buf_rc);
			}
		}
		*result = morph_buf_detach(&combined_buf);
	}
	for (int i = 0; i < n; i++)
		free(workers[i].result);
	free(workers);
	free(threads);
	sub_agent_emit_background_event(rt, "background.completed", "end",
					"sub-agent fanout completed",
					entry->cfg.name, NULL, agent_name, 0);
	return 0;
}

int sub_agent_apply_output_schema(const char *text,
				  const char *schema,
				  struct model *llm,
				  char **result)
{
	if (!text || !schema || !result)
		return -EINVAL;
	if (!llm || !llm->chat || !llm->api_key[0]) {
		*result = strdup(text);
		return 0;
	}
	struct arena *arena = arena_create(64 * 1024);
	if (!arena) {
		*result = strdup(text);
		return 0;
	}
	char prompt[4096];
	snprintf(prompt, sizeof(prompt),
		 "Extract information from the following text according "
		 "to this JSON schema. Return ONLY valid JSON matching "
		 "the schema, no other text.\n\nSchema: %s\n\nText: %s",
		 schema, text);
	const char *msgs[] = { prompt };
	char *response = NULL;
	size_t res_cap = 8192;
	response = malloc(res_cap);
	if (!response) {
		arena_destroy(arena);
		*result = strdup(text);
		return 0;
	}
	response[0] = '\0';
	int rc = llm->chat(llm, arena,
		"You are a structured data extraction engine. "
		"Return ONLY valid JSON.",
		msgs, 1, NULL, NULL, NULL);
	arena_destroy(arena);
	if (rc < 0 || !response[0]) {
		free(response);
		*result = strdup(text);
		return 0;
	}
	cJSON *parsed = cJSON_Parse(response);
	if (parsed) {
		cJSON_Delete(parsed);
		*result = response;
		return 0;
	}
	free(response);
	*result = strdup(text);
	return 0;
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
