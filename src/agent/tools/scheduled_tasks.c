#include "scheduled_tasks.h"
#include "db/scheduled_task.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct scheduled_tasks_tool_context {
	struct db *db;
	struct tool_registry *actions;
	int64_t time_anchor;
};

static void scheduled_tasks_tool_context_destroy(void *user_data)
{
	free(user_data);
}

static char *json_error(const char *message)
{
	cJSON *obj;
	char *out;

	obj = cJSON_CreateObject();
	if (!obj)
		return strdup("{\"error\":\"out of memory\"}");
	cJSON_AddStringToObject(obj, "error", message ? message : "error");
	out = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	return out ? out : strdup("{\"error\":\"out of memory\"}");
}

static void add_task_json(cJSON *arr, const struct scheduled_task *task)
{
	cJSON *obj;

	if (!arr || !task)
		return;
	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddNumberToObject(obj, "id", (double)task->id);
	cJSON_AddStringToObject(obj, "title", task->title);
	cJSON_AddStringToObject(obj, "kind", task->kind);
	cJSON_AddStringToObject(obj, "status", task->status);
	cJSON_AddStringToObject(obj, "trigger_type", task->trigger_type);
	cJSON_AddNumberToObject(obj, "next_run_at", (double)task->next_run_at);
	cJSON_AddNumberToObject(obj, "interval_seconds",
				task->interval_seconds);
	cJSON_AddNumberToObject(obj, "timeout_at", (double)task->timeout_at);
	cJSON_AddNumberToObject(obj, "attempts", task->attempts);
	cJSON_AddNumberToObject(obj, "max_attempts", task->max_attempts);
	cJSON_AddStringToObject(obj, "action_type", task->action_type);
	if (task->last_error[0])
		cJSON_AddStringToObject(obj, "last_error", task->last_error);
	cJSON_AddItemToArray(arr, obj);
}

static char *tasks_to_json(struct scheduled_task *tasks, int count)
{
	cJSON *root;
	cJSON *arr;
	char *out;

	root = cJSON_CreateObject();
	if (!root)
		return NULL;
	arr = cJSON_CreateArray();
	if (!arr) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddNumberToObject(root, "count", count);
	cJSON_AddItemToObject(root, "tasks", arr);
	for (int i = 0; i < count; i++)
		add_task_json(arr, &tasks[i]);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static void add_notification_json(cJSON *arr,
				  const struct notification *notification)
{
	cJSON *obj;

	if (!arr || !notification)
		return;
	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddNumberToObject(obj, "id", (double)notification->id);
	cJSON_AddNumberToObject(obj, "task_id", (double)notification->task_id);
	cJSON_AddStringToObject(obj, "level", notification->level);
	cJSON_AddStringToObject(obj, "title", notification->title);
	cJSON_AddStringToObject(obj, "body", notification->body);
	cJSON_AddNumberToObject(obj, "created_at",
				(double)notification->created_at);
	cJSON_AddStringToObject(obj, "delivery_status",
				notification->delivery_status);
	cJSON_AddItemToArray(arr, obj);
}

static char *notifications_to_json(struct notification *notifications,
				   int count)
{
	cJSON *root;
	cJSON *arr;
	char *out;

	root = cJSON_CreateObject();
	if (!root)
		return NULL;
	arr = cJSON_CreateArray();
	if (!arr) {
		cJSON_Delete(root);
		return NULL;
	}
	cJSON_AddNumberToObject(root, "count", count);
	cJSON_AddItemToObject(root, "notifications", arr);
	for (int i = 0; i < count; i++)
		add_notification_json(arr, &notifications[i]);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static int bind_create_input(cJSON *root, struct scheduled_task_input *input,
			     int64_t time_anchor)
{
	cJSON *item;
	int has_next_run_at = 0;

	if (!root || !input)
		MORPH_RETURN(-EINVAL);
	memset(input, 0, sizeof(*input));
	item = cJSON_GetObjectItem(root, "title");
	if (cJSON_IsString(item))
		input->title = item->valuestring;
	item = cJSON_GetObjectItem(root, "kind");
	input->kind = cJSON_IsString(item) ? item->valuestring : "reminder";
	item = cJSON_GetObjectItem(root, "trigger_type");
	input->trigger_type = cJSON_IsString(item) ? item->valuestring : "once";
	item = cJSON_GetObjectItem(root, "next_run_at");
	if (cJSON_IsNumber(item)) {
		input->next_run_at = (int64_t)item->valuedouble;
		has_next_run_at = 1;
	}
	item = cJSON_GetObjectItem(root, "delay_seconds");
	if (cJSON_IsNumber(item)) {
		int64_t delay = (int64_t)item->valuedouble;
		if (delay < 0)
			MORPH_RETURN(-EINVAL);
		if (!has_next_run_at) {
			if (time_anchor <= 0)
				time_anchor = (int64_t)time(NULL);
			input->next_run_at = time_anchor + delay;
			has_next_run_at = 1;
		}
	}
	item = cJSON_GetObjectItem(root, "interval_seconds");
	if (cJSON_IsNumber(item))
		input->interval_seconds = (int)item->valuedouble;
	item = cJSON_GetObjectItem(root, "timeout_at");
	if (cJSON_IsNumber(item))
		input->timeout_at = (int64_t)item->valuedouble;
	item = cJSON_GetObjectItem(root, "max_attempts");
	if (cJSON_IsNumber(item))
		input->max_attempts = (int)item->valuedouble;
	item = cJSON_GetObjectItem(root, "action_type");
	input->action_type = cJSON_IsString(item) ? item->valuestring : "reminder";
	item = cJSON_GetObjectItem(root, "payload_json");
	if (cJSON_IsString(item))
		input->payload_json = item->valuestring;
	item = cJSON_GetObjectItem(root, "policy_json");
	if (cJSON_IsString(item))
		input->policy_json = item->valuestring;
	item = cJSON_GetObjectItem(root, "notify_json");
	if (cJSON_IsString(item))
		input->notify_json = item->valuestring;

	if (!input->title || !has_next_run_at || input->next_run_at < 0)
		MORPH_RETURN(-EINVAL);
	return 0;
}

static char *tasks_tool_create(struct scheduled_tasks_tool_context *ctx,
			       cJSON *root)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	cJSON *out;
	cJSON *arr;
	char *json;
	int rc;

	rc = bind_create_input(root, &input, ctx->time_anchor);
	if (rc != 0)
		return json_error("missing or invalid task create fields");
	rc = scheduled_task_create(ctx->db, &input, &task);
	if (rc != 0)
		return json_error(morph_strerror(rc));

	out = cJSON_CreateObject();
	arr = cJSON_CreateArray();
	if (!out || !arr) {
		cJSON_Delete(out);
		cJSON_Delete(arr);
		scheduled_task_cleanup(&task);
		return json_error("out of memory");
	}
	cJSON_AddStringToObject(out, "status", "created");
	cJSON_AddNumberToObject(out, "time_anchor",
				(double)ctx->time_anchor);
	cJSON_AddItemToObject(out, "tasks", arr);
	add_task_json(arr, &task);
	cJSON_AddNumberToObject(out, "id", (double)task.id);
	json = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	scheduled_task_cleanup(&task);
	return json ? json : json_error("out of memory");
}

static char *tasks_tool_update(struct scheduled_tasks_tool_context *ctx,
			       cJSON *root)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	cJSON *id_item;
	cJSON *out;
	cJSON *arr;
	char *json;
	int rc;

	id_item = cJSON_GetObjectItem(root, "id");
	if (!cJSON_IsNumber(id_item))
		return json_error("missing task id");
	rc = bind_create_input(root, &input, ctx->time_anchor);
	if (rc != 0)
		return json_error("missing or invalid task update fields");
	rc = scheduled_task_update(ctx->db, (int64_t)id_item->valuedouble,
				   &input, &task);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = cJSON_CreateObject();
	arr = cJSON_CreateArray();
	if (!out || !arr) {
		cJSON_Delete(out);
		cJSON_Delete(arr);
		scheduled_task_cleanup(&task);
		return json_error("out of memory");
	}
	cJSON_AddStringToObject(out, "status", "updated");
	cJSON_AddNumberToObject(out, "time_anchor",
				(double)ctx->time_anchor);
	cJSON_AddItemToObject(out, "tasks", arr);
	add_task_json(arr, &task);
	json = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	scheduled_task_cleanup(&task);
	return json ? json : json_error("out of memory");
}

static char *tasks_tool_list(struct db *db, cJSON *root)
{
	struct scheduled_task *tasks = NULL;
	int count = 0;
	int limit = 20;
	const char *status = NULL;
	cJSON *item;
	char *out;
	int rc;

	item = cJSON_GetObjectItem(root, "limit");
	if (cJSON_IsNumber(item))
		limit = (int)item->valuedouble;
	item = cJSON_GetObjectItem(root, "status");
	if (cJSON_IsString(item) && item->valuestring[0])
		status = item->valuestring;
	rc = scheduled_task_list(db, status, limit, &tasks, &count);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = tasks_to_json(tasks, count);
	scheduled_task_free_list(tasks, count);
	return out ? out : json_error("out of memory");
}

static char *tasks_tool_cancel(struct db *db, cJSON *root)
{
	cJSON *item = cJSON_GetObjectItem(root, "id");
	int rc;

	if (!cJSON_IsNumber(item))
		return json_error("missing task id");
	rc = scheduled_task_cancel(db, (int64_t)item->valuedouble);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	return strdup("{\"status\":\"cancelled\"}");
}

static char *tasks_tool_run_due(struct scheduled_tasks_tool_context *ctx,
				cJSON *root)
{
	cJSON *item;
	int64_t now = (int64_t)time(NULL);
	int limit = 20;
	int ran = 0;
	cJSON *out;
	char *json;
	int rc;

	item = cJSON_GetObjectItem(root, "now");
	if (cJSON_IsNumber(item))
		now = (int64_t)item->valuedouble;
	item = cJSON_GetObjectItem(root, "limit");
	if (cJSON_IsNumber(item))
		limit = (int)item->valuedouble;
	rc = scheduled_task_run_due_with_runner(
		ctx->db, now, limit, scheduled_tasks_tool_runner,
		ctx->actions, &ran);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = cJSON_CreateObject();
	if (!out)
		return json_error("out of memory");
	cJSON_AddNumberToObject(out, "ran", ran);
	json = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	return json ? json : json_error("out of memory");
}

static char *payload_args_json(cJSON *payload)
{
	cJSON *args_json;
	cJSON *args;

	if (!payload)
		return strdup("{}");
	args_json = cJSON_GetObjectItem(payload, "args_json");
	if (cJSON_IsString(args_json) && args_json->valuestring)
		return strdup(args_json->valuestring);
	args = cJSON_GetObjectItem(payload, "args");
	if (args)
		return cJSON_PrintUnformatted(args);
	return strdup("{}");
}

static int result_completed_from_json(const char *text,
				      int *completed,
				      int *retry_after_seconds)
{
	cJSON *root;
	cJSON *item;
	const char *status;

	if (completed)
		*completed = 0;
	if (retry_after_seconds)
		*retry_after_seconds = 0;
	if (!text)
		return 0;
	root = cJSON_Parse(text);
	if (!root)
		return 0;
	item = cJSON_GetObjectItem(root, "completed");
	if (!cJSON_IsBool(item))
		item = cJSON_GetObjectItem(root, "done");
	if (!cJSON_IsBool(item))
		item = cJSON_GetObjectItem(root, "ready");
	if (cJSON_IsBool(item) && cJSON_IsTrue(item) && completed)
		*completed = 1;
	item = cJSON_GetObjectItem(root, "status");
	status = cJSON_IsString(item) ? item->valuestring : NULL;
	if (status && completed &&
	    (strcmp(status, "completed") == 0 ||
	     strcmp(status, "success") == 0 ||
	     strcmp(status, "ready") == 0 ||
	     strcmp(status, "passed") == 0))
		*completed = 1;
	item = cJSON_GetObjectItem(root, "retry_after_seconds");
	if (cJSON_IsNumber(item) && retry_after_seconds)
		*retry_after_seconds = (int)item->valuedouble;
	cJSON_Delete(root);
	return 0;
}

int scheduled_tasks_tool_runner(const struct scheduled_task *task,
				struct scheduled_task_action_result *result,
				void *user_data)
{
	struct tool_registry *actions = user_data;
	struct tool_result tool_result;
	cJSON *payload;
	cJSON *tool_item;
	const char *tool_name;
	char *tool_name_copy;
	char *args_json;
	int rc;

	if (!task || !result || !actions)
		MORPH_RETURN(-EINVAL);
	if (!task->payload_json)
		MORPH_RETURN(-EINVAL);
	payload = cJSON_Parse(task->payload_json);
	if (!payload)
		MORPH_RETURN(MORPH_ERR_PARSE);
	tool_item = cJSON_GetObjectItem(payload, "tool");
	if (!cJSON_IsString(tool_item))
		tool_item = cJSON_GetObjectItem(payload, "tool_name");
	tool_name = cJSON_IsString(tool_item) ? tool_item->valuestring : NULL;
	if (!tool_name || !*tool_name) {
		cJSON_Delete(payload);
		MORPH_RETURN(-EINVAL);
	}
	if (strcmp(tool_name, "tasks") == 0 ||
	    strcmp(tool_name, "ask_user") == 0) {
		cJSON_Delete(payload);
		result->error = strdup("scheduled task cannot run interactive "
				       "or recursive tool");
		MORPH_RETURN(-EPERM);
	}
	tool_name_copy = strdup(tool_name);
	if (!tool_name_copy) {
		cJSON_Delete(payload);
		MORPH_RETURN(-ENOMEM);
	}
	args_json = payload_args_json(payload);
	cJSON_Delete(payload);
	if (!args_json) {
		free(tool_name_copy);
		MORPH_RETURN(-ENOMEM);
	}
	tool_result_init(&tool_result);
	rc = tool_exec(actions, tool_name_copy, args_json, &tool_result);
	free(tool_name_copy);
	free(args_json);
	if (rc == 0) {
		result->body = strdup(tool_result.text.data ?
				      tool_result.text.data : "");
		if (!result->body) {
			tool_result_cleanup(&tool_result);
			MORPH_RETURN(-ENOMEM);
		}
		result_completed_from_json(tool_result.text.data,
					   &result->completed,
					   &result->retry_after_seconds);
	} else {
		const char *text = tool_result.text.data;
		result->error = strdup(text && *text ? text : morph_strerror(rc));
		if (!result->error) {
			tool_result_cleanup(&tool_result);
			MORPH_RETURN(-ENOMEM);
		}
	}
	tool_result_cleanup(&tool_result);
	return rc;
}

static char *tasks_tool_inbox(struct db *db, cJSON *root)
{
	struct notification *notifications = NULL;
	int count = 0;
	int limit = 20;
	cJSON *item;
	char *out;
	int rc;

	item = cJSON_GetObjectItem(root, "limit");
	if (cJSON_IsNumber(item))
		limit = (int)item->valuedouble;
	rc = notification_list_unread(db, limit, &notifications, &count);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = notifications_to_json(notifications, count);
	notification_free_list(notifications, count);
	return out ? out : json_error("out of memory");
}

static char *tasks_tool_mark_read(struct db *db, cJSON *root)
{
	cJSON *item = cJSON_GetObjectItem(root, "id");
	int rc;

	if (!cJSON_IsNumber(item))
		return json_error("missing notification id");
	rc = notification_mark_read(db, (int64_t)item->valuedouble, 0);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	return strdup("{\"status\":\"read\"}");
}

static int scheduled_tasks_tool_run(const char *args_json,
				    struct tool_result *result,
				    void *user_data)
{
	struct scheduled_tasks_tool_context *ctx = user_data;
	cJSON *root;
	cJSON *op_item;
	const char *op;
	char *out;

	if (!ctx || !ctx->db || !ctx->db->handle)
		MORPH_RETURN(-EINVAL);
	root = cJSON_Parse(args_json ? args_json : "{}");
	if (!root) {
		(void)tool_result_take_text(result, json_error("invalid JSON"));
		return 0;
	}
	op_item = cJSON_GetObjectItem(root, "op");
	op = cJSON_IsString(op_item) ? op_item->valuestring : "list";

	if (strcmp(op, "create") == 0)
		out = tasks_tool_create(ctx, root);
	else if (strcmp(op, "update") == 0)
		out = tasks_tool_update(ctx, root);
	else if (strcmp(op, "list") == 0)
		out = tasks_tool_list(ctx->db, root);
	else if (strcmp(op, "cancel") == 0)
		out = tasks_tool_cancel(ctx->db, root);
	else if (strcmp(op, "run_due") == 0)
		out = tasks_tool_run_due(ctx, root);
	else if (strcmp(op, "inbox") == 0)
		out = tasks_tool_inbox(ctx->db, root);
	else if (strcmp(op, "mark_read") == 0)
		out = tasks_tool_mark_read(ctx->db, root);
	else
		out = json_error("unknown tasks op");

	cJSON_Delete(root);
	(void)tool_result_take_text(result, out ? out : json_error("oom"));
	return 0;
}

int scheduled_tasks_tool_init(struct tool_registry *reg, struct db *db,
			      struct tool_registry *actions)
{
	struct scheduled_tasks_tool_context *ctx;
	int rc;

	if (!reg || !db)
		MORPH_RETURN(-EINVAL);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->db = db;
	ctx->actions = actions ? actions : reg;
	ctx->time_anchor = (int64_t)time(NULL);
	rc = tool_register(reg, "tasks",
		"Create and manage persistent scheduled tasks and the inbox. "
		"Use next_run_at for absolute Unix seconds, or delay_seconds "
		"for relative delays anchored at the current user turn start. "
		"Supported ops: "
		"create, update, list, cancel, run_due, inbox, mark_read.",
		"{\"type\":\"object\",\"properties\":{"
		"\"op\":{\"type\":\"string\",\"enum\":[\"create\",\"list\","
		"\"update\",\"cancel\",\"run_due\",\"inbox\",\"mark_read\"]},"
		"\"id\":{\"type\":\"integer\",\"description\":\"Task or notification id\"},"
		"\"title\":{\"type\":\"string\"},"
		"\"kind\":{\"type\":\"string\",\"enum\":[\"reminder\",\"action\",\"watch\"]},"
		"\"trigger_type\":{\"type\":\"string\",\"enum\":[\"once\",\"interval\",\"cron\",\"event\"]},"
		"\"next_run_at\":{\"type\":\"integer\",\"description\":\"Unix seconds UTC\"},"
		"\"delay_seconds\":{\"type\":\"integer\",\"description\":\"Relative seconds from current turn start\"},"
		"\"interval_seconds\":{\"type\":\"integer\"},"
		"\"timeout_at\":{\"type\":\"integer\"},"
		"\"max_attempts\":{\"type\":\"integer\"},"
		"\"action_type\":{\"type\":\"string\"},"
		"\"payload_json\":{\"type\":\"string\"},"
		"\"policy_json\":{\"type\":\"string\"},"
		"\"notify_json\":{\"type\":\"string\"},"
		"\"status\":{\"type\":\"string\"},"
		"\"limit\":{\"type\":\"integer\"},"
		"\"now\":{\"type\":\"integer\"}"
		"},\"required\":[\"op\"]}",
		scheduled_tasks_tool_run, ctx,
		scheduled_tasks_tool_context_destroy);
	if (rc != 0)
		free(ctx);
	return rc;
}

int scheduled_tasks_tool_set_time_anchor(struct tool_registry *reg,
					 int64_t time_anchor)
{
	struct tool_entry *entry;
	struct scheduled_tasks_tool_context *ctx;

	if (!reg)
		MORPH_RETURN(-EINVAL);
	entry = tool_lookup(reg, "tasks");
	if (!entry || !entry->user_data)
		MORPH_RETURN(-ENOENT);
	ctx = entry->user_data;
	ctx->time_anchor = time_anchor > 0 ? time_anchor : (int64_t)time(NULL);
	return 0;
}
