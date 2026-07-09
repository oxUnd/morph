#include "sub_agent_tools.h"
#include "util/log.h"
#include "util/error.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int agent_sync_exec(const char *args_json, struct tool_result *result,
			   void *user_data)
{
	struct sub_agent_tool_binding *bind = user_data;
	if (!bind || !result)
		return -EINVAL;
	if (!args_json)
		args_json = "{}";
	cJSON *root = cJSON_Parse(args_json);
	const char *task = NULL;
	if (root) {
		cJSON *t = cJSON_GetObjectItem(root, "task");
		if (cJSON_IsString(t) && t->valuestring)
			task = t->valuestring;
	}
	if (!task) {
		if (root)
			cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'task' parameter\"}"));
		return -EINVAL;
	}
	struct sub_agent_entry *entry = sub_agent_find(bind->rt,
						       bind->agent_name);
	if (!entry) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"sub-agent not found\"}"));
		return -ENOENT;
	}
	char *sub_result = NULL;
	int rc = sub_agent_invoke_sync(bind->rt, entry, task, &sub_result);
	cJSON_Delete(root);
	cJSON *out = cJSON_CreateObject();
	if (rc == 0) {
		cJSON_AddStringToObject(out, "status", "completed");
		if (sub_result) {
			cJSON *parsed = cJSON_Parse(sub_result);
			if (parsed) {
				cJSON_AddItemToObject(out, "result", parsed);
				free(sub_result);
			} else {
				cJSON_AddStringToObject(out, "result",
							sub_result);
				free(sub_result);
			}
		}
	} else {
		cJSON_AddStringToObject(out, "status", "failed");
		cJSON_AddStringToObject(out, "error",
					sub_result ? sub_result
					: morph_strerror(rc));
		free(sub_result);
	}
	(void)tool_result_success_json_text(result, cJSON_PrintUnformatted(out));
	cJSON_Delete(out);
	return rc;
}

static void binding_destroy(void *ptr)
{
	free(ptr);
}

int sub_agent_sync_init(struct tool_registry *reg,
			struct sub_agent_runtime *rt)
{
	if (!reg || !rt)
		return -EINVAL;
	for (int i = 0; i < rt->entry_count; i++) {
		struct sub_agent_tool_binding *bind = calloc(1, sizeof(*bind));
		if (!bind)
			return -ENOMEM;
		bind->rt = rt;
		strncpy(bind->agent_name, rt->entries[i].cfg.name,
			sizeof(bind->agent_name) - 1);
		char name[TOOL_NAME_MAX];
		snprintf(name, sizeof(name), "agent_%s",
			 rt->entries[i].cfg.name);
		char desc[TOOL_DESC_MAX];
		snprintf(desc, sizeof(desc),
			 "Delegate a task to the %s sub-agent. %s",
			 rt->entries[i].cfg.name,
			 rt->entries[i].cfg.description);
			char args[TOOL_SCHEMA_MAX];
			snprintf(args, sizeof(args),
				 "{\"type\":\"object\",\"properties\""
				 ":{\"task\":{\"type\":\"string\","
				 "\"description\":\"The task to delegate\"}},"
				 "\"required\":[\"task\"],"
				 "\"additionalProperties\":false}");
			struct tool_spec spec = {
				.origin = TOOL_ORIGIN_BUILTIN,
				.name = name,
				.description = desc,
				.input_schema = args,
				.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
				.exec = agent_sync_exec,
				.user_data = bind,
				.user_data_destroy = binding_destroy,
			};
			int rc = tool_register(reg, &spec);
		if (rc < 0) {
			free(bind);
			return rc;
		}
		log_info("registered sub-agent tool: %s", name);
	}
	return 0;
}

static int delegate_exec(const char *args_json, struct tool_result *result,
			 void *user_data)
{
	struct sub_agent_runtime *rt = user_data;
	if (!rt || !result)
		return -EINVAL;
	if (!args_json)
		args_json = "{}";
	cJSON *root = cJSON_Parse(args_json);
	const char *agent = NULL;
	const char *task = NULL;
	if (root) {
		cJSON *a = cJSON_GetObjectItem(root, "agent");
		if (cJSON_IsString(a))
			agent = a->valuestring;
		cJSON *t = cJSON_GetObjectItem(root, "task");
		if (cJSON_IsString(t))
			task = t->valuestring;
	}
	if (!agent || !task) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'agent' or 'task' parameter\"}"));
		return -EINVAL;
	}
	char *task_id = NULL;
	int rc = sub_agent_delegate(rt, agent, task, &task_id);
	cJSON_Delete(root);
	cJSON *out = cJSON_CreateObject();
	if (rc == 0) {
		cJSON_AddStringToObject(out, "task_id", task_id);
		cJSON_AddStringToObject(out, "status", "running");
	} else {
		cJSON_AddStringToObject(out, "status", "failed");
		cJSON_AddStringToObject(out, "error", morph_strerror(rc));
	}
	free(task_id);
	(void)tool_result_success_json_text(result, cJSON_PrintUnformatted(out));
	cJSON_Delete(out);
	return rc;
}

int sub_agent_delegate_init(struct tool_registry *reg,
			    struct sub_agent_runtime *rt)
{
	if (!reg || !rt)
		return -EINVAL;
	return tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "delegate", .description = "Start a sub-agent asynchronously and return a task ID", .input_schema = "{\"type\":\"object\",\"properties\""
		":{\"agent\":{\"type\":\"string\","
		"\"description\":\"Name of the sub-agent to invoke\"},"
		"\"task\":{\"type\":\"string\","
		"\"description\":\"The task description\"}},"
		"\"required\":[\"agent\",\"task\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = delegate_exec, .user_data = rt, .user_data_destroy = NULL });
}

static int agent_status_exec(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	struct sub_agent_runtime *rt = user_data;
	if (!rt || !result)
		return -EINVAL;
	if (!args_json)
		args_json = "{}";
	cJSON *root = cJSON_Parse(args_json);
	const char *task_id = NULL;
	if (root) {
		cJSON *t = cJSON_GetObjectItem(root, "task_id");
		if (cJSON_IsString(t))
			task_id = t->valuestring;
	}
	if (!task_id) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'task_id' parameter\"}"));
		return -EINVAL;
	}
	enum sub_agent_task_status status = SUB_AGENT_PENDING;
	char *res = NULL;
	int rc = sub_agent_check_status(rt, task_id, &status, &res);
	cJSON_Delete(root);
	cJSON *out = cJSON_CreateObject();
	if (rc == 0) {
		const char *st = "unknown";
		switch (status) {
		case SUB_AGENT_PENDING:   st = "pending"; break;
		case SUB_AGENT_RUNNING:   st = "running"; break;
		case SUB_AGENT_COMPLETED: st = "completed"; break;
		case SUB_AGENT_FAILED:    st = "failed"; break;
		case SUB_AGENT_CANCELLED: st = "cancelled"; break;
		}
		cJSON_AddStringToObject(out, "task_id", task_id);
		cJSON_AddStringToObject(out, "status", st);
		if (res) {
			cJSON *parsed = cJSON_Parse(res);
			if (parsed) {
				cJSON_AddItemToObject(out, "result", parsed);
			} else {
				cJSON_AddStringToObject(out, "result", res);
			}
		}
	} else {
		cJSON_AddStringToObject(out, "error", morph_strerror(rc));
	}
	free(res);
	(void)tool_result_success_json_text(result, cJSON_PrintUnformatted(out));
	cJSON_Delete(out);
	return rc;
}

int sub_agent_status_init(struct tool_registry *reg,
			  struct sub_agent_runtime *rt)
{
	if (!reg || !rt)
		return -EINVAL;
	return tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "agent_status", .description = "Check the status of an asynchronously running sub-agent", .input_schema = "{\"type\":\"object\",\"properties\""
		":{\"task_id\":{\"type\":\"string\","
		"\"description\":\"The task ID returned by delegate\"}},"
		"\"required\":[\"task_id\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = agent_status_exec, .user_data = rt, .user_data_destroy = NULL });
}

static int fanout_exec(const char *args_json, struct tool_result *result,
		       void *user_data)
{
	struct sub_agent_runtime *rt = user_data;
	if (!rt || !result)
		return -EINVAL;
	if (!args_json)
		args_json = "{}";
	cJSON *root = cJSON_Parse(args_json);
	const char *agent = NULL;
	cJSON *tasks_arr = NULL;
	const char *merge_str = NULL;
	if (root) {
		cJSON *a = cJSON_GetObjectItem(root, "agent");
		if (cJSON_IsString(a))
			agent = a->valuestring;
		tasks_arr = cJSON_GetObjectItem(root, "tasks");
		cJSON *m = cJSON_GetObjectItem(root, "merge");
		if (cJSON_IsString(m))
			merge_str = m->valuestring;
	}
	if (!agent || !tasks_arr || !cJSON_IsArray(tasks_arr)) {
		cJSON_Delete(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'agent' or 'tasks' parameter\"}"));
		return -EINVAL;
	}
	int n = cJSON_GetArraySize(tasks_arr);
	const char **tasks = calloc((size_t)n, sizeof(*tasks));
	if (!tasks) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	for (int i = 0; i < n; i++) {
		cJSON *item = cJSON_GetArrayItem(tasks_arr, i);
		tasks[i] = cJSON_IsString(item) ? item->valuestring : "";
	}
	enum sub_agent_merge_strategy merge =
		SUB_AGENT_MERGE_SYNTHESIZE;
	if (merge_str) {
		if (strcmp(merge_str, "concat") == 0)
			merge = SUB_AGENT_MERGE_CONCAT;
		else if (strcmp(merge_str, "raw") == 0)
			merge = SUB_AGENT_MERGE_RAW;
	}
	char *fanout_result = NULL;
	int rc = sub_agent_fanout(rt, agent, tasks, n, merge, &fanout_result);
	free(tasks);
	cJSON_Delete(root);
	if (rc == 0 && fanout_result) {
		cJSON *parsed = cJSON_Parse(fanout_result);
		if (parsed) {
			cJSON *out = cJSON_CreateObject();
			cJSON_AddStringToObject(out, "status", "completed");
			cJSON_AddItemToObject(out, "result", parsed);
			(void)tool_result_success_json_text(result, cJSON_PrintUnformatted(out));
			cJSON_Delete(out);
			free(fanout_result);
		} else {
			(void)tool_result_success_json_text(result, fanout_result);
		}
	} else {
		cJSON *out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "status", "failed");
		cJSON_AddStringToObject(out, "error",
					fanout_result ? fanout_result
					: morph_strerror(rc));
		(void)tool_result_success_json_text(result, cJSON_PrintUnformatted(out));
		cJSON_Delete(out);
		free(fanout_result);
	}
	return rc;
}

int sub_agent_fanout_init(struct tool_registry *reg,
			  struct sub_agent_runtime *rt)
{
	if (!reg || !rt)
		return -EINVAL;
	return tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "fanout", .description = "Run multiple tasks in parallel using a sub-agent and merge results", .input_schema = "{\"type\":\"object\",\"properties\""
		":{\"agent\":{\"type\":\"string\","
		"\"description\":\"Name of the sub-agent\"},"
		"\"tasks\":{\"type\":\"array\",\"items\""
		":{\"type\":\"string\"},"
		"\"description\":\"Array of task descriptions\"},"
		"\"merge\":{\"type\":\"string\","
		"\"enum\":[\"synthesize\",\"concat\",\"raw\"],"
		"\"description\":\"How to merge results "
		"(default: synthesize)\"}},"
		"\"required\":[\"agent\",\"tasks\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = fanout_exec, .user_data = rt, .user_data_destroy = NULL });
}

void sub_agent_tools_register_all(struct tool_registry *reg,
				  struct sub_agent_runtime *rt)
{
	if (!reg || !rt || rt->entry_count == 0)
		return;
	sub_agent_sync_init(reg, rt);
	sub_agent_delegate_init(reg, rt);
	sub_agent_status_init(reg, rt);
	sub_agent_fanout_init(reg, rt);
}
