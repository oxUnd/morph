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
	int64_t time_anchor;
	int64_t source_session_id;
	struct scheduled_task_event_sink events;
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
	cJSON *payload;
	cJSON *prompt;

	if (!arr || !task)
		return;
	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddNumberToObject(obj, "id", (double)task->id);
	cJSON_AddNumberToObject(obj, "source_session_id",
				(double)task->source_session_id);
	cJSON_AddNumberToObject(obj, "latest_session_id",
				(double)task->latest_session_id);
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
	cJSON_AddStringToObject(obj, "schedule",
				task->interval_seconds > 0 ? "interval" : "once");
	payload = task->payload_json ? cJSON_Parse(task->payload_json) : NULL;
	prompt = payload ? cJSON_GetObjectItem(payload, "prompt") : NULL;
	if (cJSON_IsString(prompt))
		cJSON_AddStringToObject(obj, "prompt", prompt->valuestring);
	cJSON_Delete(payload);
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
	cJSON_AddNumberToObject(obj, "session_id",
				(double)notification->session_id);
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
			     int64_t time_anchor, char **owned_payload_json)
{
	cJSON *item;
	cJSON *payload = NULL;
	char *payload_json = NULL;
	int has_next_run_at = 0;
	int rc = 0;

	if (!root || !input || !owned_payload_json)
		MORPH_RETURN(-EINVAL);
	memset(input, 0, sizeof(*input));
	input->source_session_id = 0;
	*owned_payload_json = NULL;
	item = cJSON_GetObjectItem(root, "title");
	if (cJSON_IsString(item))
		input->title = item->valuestring;
	item = cJSON_GetObjectItem(root, "kind");
	input->kind = cJSON_IsString(item) ? item->valuestring : "agent";
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
	input->action_type = "agent_run";
	item = cJSON_GetObjectItem(root, "prompt");
	if (cJSON_IsString(item) && item->valuestring[0]) {
		payload = cJSON_CreateObject();
		if (!payload)
			MORPH_RETURN(-ENOMEM);
		cJSON_AddStringToObject(payload, "prompt", item->valuestring);
		payload_json = cJSON_PrintUnformatted(payload);
		cJSON_Delete(payload);
		if (!payload_json)
			MORPH_RETURN(-ENOMEM);
		input->payload_json = payload_json;
		*owned_payload_json = payload_json;
	}
	item = cJSON_GetObjectItem(root, "policy_json");
	if (cJSON_IsString(item))
		input->policy_json = item->valuestring;
	item = cJSON_GetObjectItem(root, "notify_json");
	if (cJSON_IsString(item))
		input->notify_json = item->valuestring;

	if (!input->title || !has_next_run_at || input->next_run_at < 0 ||
	    !input->payload_json)
		rc = -EINVAL;
	return rc;
}

static char *tasks_tool_create(struct scheduled_tasks_tool_context *ctx,
			       cJSON *root)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	cJSON *out;
	cJSON *arr;
	char *owned_payload_json = NULL;
	char *json;
	int rc;

	rc = bind_create_input(root, &input, ctx->time_anchor,
			       &owned_payload_json);
	if (rc != 0) {
		free(owned_payload_json);
		return json_error("missing or invalid task create fields");
	}
	input.source_session_id = ctx->source_session_id;
	rc = scheduled_task_create_with_events(ctx->db, &input, &task,
					       &ctx->events);
	free(owned_payload_json);
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
	char *owned_payload_json = NULL;
	char *json;
	int rc;

	id_item = cJSON_GetObjectItem(root, "id");
	if (!cJSON_IsNumber(id_item))
		return json_error("missing task id");
	rc = bind_create_input(root, &input, ctx->time_anchor,
			       &owned_payload_json);
	if (rc != 0) {
		free(owned_payload_json);
		return json_error("missing or invalid task update fields");
	}
	input.source_session_id = ctx->source_session_id;
	rc = scheduled_task_update_with_events(ctx->db,
					       (int64_t)id_item->valuedouble,
					       &input, &task, &ctx->events);
	free(owned_payload_json);
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

static char *tasks_tool_cancel(struct scheduled_tasks_tool_context *ctx,
			       cJSON *root)
{
	cJSON *item = cJSON_GetObjectItem(root, "id");
	int rc;

	if (!cJSON_IsNumber(item))
		return json_error("missing task id");
	rc = scheduled_task_cancel_with_events(ctx->db,
					       (int64_t)item->valuedouble,
					       &ctx->events);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	return strdup("{\"status\":\"cancelled\"}");
}

static char *tasks_tool_run_due(struct scheduled_tasks_tool_context *ctx,
				cJSON *root)
{
	(void)ctx;
	(void)root;
	return json_error("run_due is scheduler-only for agent tasks");
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
		out = tasks_tool_cancel(ctx, root);
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

int scheduled_tasks_tool_init_events(
	struct tool_registry *reg, struct db *db,
	const struct scheduled_task_event_sink *events)
{
	struct scheduled_tasks_tool_context *ctx;
	int rc;

	if (!reg || !db)
		MORPH_RETURN(-EINVAL);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->db = db;
	ctx->time_anchor = (int64_t)time(NULL);
	if (events)
		ctx->events = *events;
	rc = tool_register(reg, "tasks",
		"Create and manage persistent scheduled tasks and the inbox. "
		"Use next_run_at for absolute Unix seconds, or delay_seconds "
		"for relative delays anchored at the current user turn start. "
		"Tasks run by asking the agent to complete the supplied prompt. "
		"Supported ops: "
		"create, update, list, cancel, inbox, mark_read.",
		"{\"type\":\"object\",\"properties\":{"
		"\"op\":{\"type\":\"string\",\"enum\":[\"create\",\"list\","
		"\"update\",\"cancel\",\"inbox\",\"mark_read\"]},"
		"\"id\":{\"type\":\"integer\",\"description\":\"Task or notification id\"},"
		"\"title\":{\"type\":\"string\"},"
		"\"kind\":{\"type\":\"string\",\"enum\":[\"agent\"]},"
		"\"trigger_type\":{\"type\":\"string\",\"enum\":[\"once\",\"interval\"]},"
		"\"next_run_at\":{\"type\":\"integer\",\"description\":\"Unix seconds UTC\"},"
		"\"delay_seconds\":{\"type\":\"integer\",\"description\":\"Relative seconds from current turn start\"},"
		"\"interval_seconds\":{\"type\":\"integer\"},"
		"\"timeout_at\":{\"type\":\"integer\"},"
		"\"max_attempts\":{\"type\":\"integer\"},"
		"\"prompt\":{\"type\":\"string\",\"description\":\"Agent task prompt to run when due\"},"
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

int scheduled_tasks_tool_init(struct tool_registry *reg, struct db *db)
{
	return scheduled_tasks_tool_init_events(reg, db, NULL);
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

int scheduled_tasks_tool_set_source_session(struct tool_registry *reg,
					    int64_t session_id)
{
	struct tool_entry *entry;
	struct scheduled_tasks_tool_context *ctx;

	if (!reg)
		MORPH_RETURN(-EINVAL);
	entry = tool_lookup(reg, "tasks");
	if (!entry || !entry->user_data)
		MORPH_RETURN(-ENOENT);
	ctx = entry->user_data;
	ctx->source_session_id = session_id > 0 ? session_id : 0;
	return 0;
}
