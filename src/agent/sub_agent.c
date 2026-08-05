#include "sub_agent.h"
#include "tool_runtime.h"
#include "guardrail.h"
#include "tokenizer.h"
#include "compress.h"
#include "system_prompt.h"
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
		pthread_mutex_destroy(&rt->tasks[i].mutex);
		free(rt->tasks[i].task_description);
		free(rt->tasks[i].result);
		if (rt->tasks[i].child_ctx && !rt->tasks[i].joined) {
			struct tool_registry *ct = rt->tasks[i].child_ctx->tools;
			react_cancel(rt->tasks[i].child_ctx);
			pthread_join(rt->tasks[i].thread, NULL);
			react_context_destroy(rt->tasks[i].child_ctx);
			tool_registry_cleanup(ct);
			free(ct);
		}
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
	react_set_event_callback(child, rt->event_cb, rt->event_user_data);
	child->max_iterations = entry->cfg.max_iterations;
	child->sub_agent_depth = rt->depth + 1;
	if (entry->system_prompt) {
		free(child->system_prompt);
		child->system_prompt = strdup(entry->system_prompt);
	}
	return child;
}

int sub_agent_invoke_sync(struct sub_agent_runtime *rt,
			  struct sub_agent_entry *entry,
			  const char *task, char **result)
{
	if (!rt || !entry || !task || !result)
		return -EINVAL;
	if (rt->depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	rt->depth++;
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent started",
					entry->cfg.name, NULL, task, 0);
	struct react_context *child =
		sub_agent_create_context(rt, entry, task);
	if (!child) {
		rt->depth--;
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent failed",
						entry->cfg.name, NULL,
						task, -ENOMEM);
		MORPH_RETURN(-ENOMEM);
	}
	int64_t start = now_ms();
	int rc = react_run(child, task, NULL, NULL);
	int64_t end = now_ms();
	{
		struct sub_agent_trace_event ev = {0};
		snprintf(ev.trace_id, sizeof(ev.trace_id), "sa_%d",
			 rt->next_task_id++);
		snprintf(ev.mode, sizeof(ev.mode), "tool");
		strncpy(ev.agent_name, entry->cfg.name,
			sizeof(ev.agent_name) - 1);
		ev.start_ms = start;
		ev.end_ms = end;
		ev.iteration_count = child->step_count;
		if (child->final_answer) {
			size_t preview_len = strlen(child->final_answer);
			if (preview_len > 200)
				preview_len = 200;
			ev.result_preview = strndup(child->final_answer,
						   preview_len);
		}
		sub_agent_trace_write(rt, &ev);
		free(ev.result_preview);
	}
	if (rc == 0 && child->final_answer) {
		if (entry->cfg.output_schema) {
			rc = sub_agent_apply_output_schema(
				child->final_answer,
				entry->cfg.output_schema,
				entry->llm, result);
		} else {
			*result = strdup(child->final_answer);
		}
	} else if (rc < 0) {
		*result = strdup(morph_strerror(rc));
	} else {
		*result = strdup("(no answer)");
	}
	struct tool_registry *child_tools = child->tools;
	react_context_destroy(child);
	tool_registry_cleanup(child_tools);
	free(child_tools);
	rt->depth--;
	sub_agent_emit_background_event(rt,
					rc == 0 ? "background.completed" :
					"background.failed",
					rc == 0 ? "end" : "failed",
					rc == 0 ? "sub-agent completed" :
					"sub-agent failed",
					entry->cfg.name, NULL, task, rc);
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

	pthread_mutex_lock(&task->mutex);
	task->status = SUB_AGENT_RUNNING;
	pthread_mutex_unlock(&task->mutex);
	sub_agent_emit_background_event(rt, "background.progress",
					"progress",
					"sub-agent delegate running",
					da->entry->cfg.name, task->id,
					task->task_description, 0);

	rt->depth++;
	struct react_context *child =
		sub_agent_create_context(rt, da->entry,
					 task->task_description);
	if (!child) {
		pthread_mutex_lock(&task->mutex);
		task->status = SUB_AGENT_FAILED;
		task->error_code = -ENOMEM;
		task->result = strdup("failed to create sub-agent context");
		pthread_mutex_unlock(&task->mutex);
		rt->depth--;
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent delegate failed",
						da->entry->cfg.name, task->id,
						task->task_description,
						-ENOMEM);
		free(da);
		return NULL;
	}
	task->child_ctx = child;

	int64_t start = now_ms();
	int rc = react_run(child, task->task_description, NULL, NULL);
	int64_t end = now_ms();

	pthread_mutex_lock(&task->mutex);
	if (rc == 0 && child->final_answer) {
		if (da->entry->cfg.output_schema) {
			char *structured = NULL;
			int src = sub_agent_apply_output_schema(
				child->final_answer,
				da->entry->cfg.output_schema,
				da->entry->llm, &structured);
			if (src == 0 && structured)
				task->result = structured;
			else
				task->result = strdup(child->final_answer);
		} else {
			task->result = strdup(child->final_answer);
		}
		task->status = SUB_AGENT_COMPLETED;
	} else if (rc == -ECANCELED) {
		task->status = SUB_AGENT_CANCELLED;
		task->result = strdup("cancelled");
	} else {
		task->status = SUB_AGENT_FAILED;
		task->error_code = rc;
		task->result = strdup(morph_strerror(rc));
	}
	pthread_mutex_unlock(&task->mutex);
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

	{
		struct sub_agent_trace_event ev = {0};
		snprintf(ev.trace_id, sizeof(ev.trace_id), "%s",
			 task->id);
		snprintf(ev.mode, sizeof(ev.mode), "delegate");
		strncpy(ev.agent_name, da->entry->cfg.name,
			sizeof(ev.agent_name) - 1);
		ev.start_ms = start;
		ev.end_ms = end;
		ev.iteration_count = child->step_count;
		sub_agent_trace_write(rt, &ev);
	}

	rt->depth--;
	free(da);
	return NULL;
}

int sub_agent_delegate(struct sub_agent_runtime *rt,
		       const char *agent_name, const char *task,
		       char **task_id_out)
{
	if (!rt || !agent_name || !task)
		return -EINVAL;
	if (rt->depth >= SUB_AGENT_MAX_DEPTH)
		MORPH_RETURN(-ELOOP);
	if (rt->task_count >= SUB_AGENT_TASK_MAX)
		MORPH_RETURN(-ENOSPC);
	struct sub_agent_entry *entry = sub_agent_find(rt, agent_name);
	if (!entry)
		MORPH_RETURN(-ENOENT);
	struct sub_agent_task *t = &rt->tasks[rt->task_count];
	memset(t, 0, sizeof(*t));
	pthread_mutex_init(&t->mutex, NULL);
	generate_task_id(rt->next_task_id++, t->id, sizeof(t->id));
	t->agent_index = (int)(entry - rt->entries);
	t->task_description = strdup(task);
	t->status = SUB_AGENT_PENDING;
	sub_agent_emit_background_event(rt, "background.started", "begin",
					"sub-agent delegate started",
					entry->cfg.name, t->id, task, 0);
	struct delegate_thread_arg *da = calloc(1, sizeof(*da));
	if (!da) {
		pthread_mutex_destroy(&t->mutex);
		free(t->task_description);
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
	int rc = pthread_create(&t->thread, NULL, delegate_thread_fn, da);
	if (rc != 0) {
		pthread_mutex_destroy(&t->mutex);
		free(t->task_description);
		free(da);
		sub_agent_emit_background_event(rt, "background.failed",
						"failed",
						"sub-agent delegate failed",
						entry->cfg.name, t->id, task,
						-rc);
		MORPH_RETURN(-rc);
	}
	rt->task_count++;
	if (task_id_out)
		*task_id_out = strdup(t->id);
	return 0;
}

int sub_agent_check_status(struct sub_agent_runtime *rt,
			   const char *task_id,
			   enum sub_agent_task_status *status_out,
			   char **result_out)
{
	if (!rt || !task_id)
		return -EINVAL;
	struct sub_agent_task *found = NULL;
	for (int i = 0; i < rt->task_count; i++) {
		if (strcmp(rt->tasks[i].id, task_id) == 0) {
			found = &rt->tasks[i];
			break;
		}
	}
	if (!found)
		MORPH_RETURN(-ENOENT);
	pthread_mutex_lock(&found->mutex);
	enum sub_agent_task_status st = found->status;
	char *res = found->result ? strdup(found->result) : NULL;
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
