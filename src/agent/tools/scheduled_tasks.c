#include "scheduled_tasks.h"
#include "db/scheduled_task.h"
#include "util/error.h"
#include "util/json.h"
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
	struct json *obj;
	char *out;

	obj = json_object();
	if (!obj)
		return strdup("{\"error\":\"out of memory\"}");
	json_add_string(obj, "error", message ? message : "error");
	out = json_stringify(obj);
	json_free(obj);
	return out ? out : strdup("{\"error\":\"out of memory\"}");
}

static void add_task_json(struct json *arr, const struct scheduled_task *task)
{
	struct json *obj;
	struct json *payload;
	struct json *prompt;

	if (!arr || !task)
		return;
	obj = json_object();
	if (!obj)
		return;
	json_add_number(obj, "id", (double)task->id);
	json_add_number(obj, "source_session_id", (double)task->source_session_id);
	json_add_number(obj, "latest_session_id", (double)task->latest_session_id);
	json_add_string(obj, "title", task->title);
	json_add_string(obj, "kind", task->kind);
	json_add_string(obj, "status", task->status);
	json_add_string(obj, "trigger_type", task->trigger_type);
	json_add_number(obj, "next_run_at", (double)task->next_run_at);
	json_add_number(obj, "interval_seconds", task->interval_seconds);
	json_add_number(obj, "timeout_at", (double)task->timeout_at);
	json_add_number(obj, "attempts", task->attempts);
	json_add_number(obj, "max_attempts", task->max_attempts);
	json_add_string(obj, "schedule", task->interval_seconds > 0 ? "interval" : "once");
	payload = task->payload_json ? json_parse_prefix(task->payload_json, NULL, NULL) : NULL;
	prompt = payload ? json_member_ci(payload, "prompt") : NULL;
	if (json_is_string_value(prompt))
		json_add_string(obj, "prompt", json_string_value(prompt));
	json_free(payload);
	if (task->last_error[0])
		json_add_string(obj, "last_error", task->last_error);
	json_array_add(arr, obj);
}

static char *tasks_to_json(struct scheduled_task *tasks, int count)
{
	struct json *root;
	struct json *arr;
	char *out;

	root = json_object();
	if (!root)
		return NULL;
	arr = json_array();
	if (!arr) {
		json_free(root);
		return NULL;
	}
	json_add_number(root, "count", count);
	json_object_add(root, "tasks", arr);
	for (int i = 0; i < count; i++)
		add_task_json(arr, &tasks[i]);
	out = json_stringify(root);
	json_free(root);
	return out;
}

static void add_notification_json(struct json *arr, const struct notification *notification)
{
	struct json *obj;

	if (!arr || !notification)
		return;
	obj = json_object();
	if (!obj)
		return;
	json_add_number(obj, "id", (double)notification->id);
	json_add_number(obj, "task_id", (double)notification->task_id);
	json_add_number(obj, "session_id", (double)notification->session_id);
	json_add_string(obj, "level", notification->level);
	json_add_string(obj, "title", notification->title);
	json_add_string(obj, "body", notification->body);
	json_add_number(obj, "created_at", (double)notification->created_at);
	json_add_string(obj, "delivery_status", notification->delivery_status);
	json_array_add(arr, obj);
}

static char *notifications_to_json(struct notification *notifications,
				   int count)
{
	struct json *root;
	struct json *arr;
	char *out;

	root = json_object();
	if (!root)
		return NULL;
	arr = json_array();
	if (!arr) {
		json_free(root);
		return NULL;
	}
	json_add_number(root, "count", count);
	json_object_add(root, "notifications", arr);
	for (int i = 0; i < count; i++)
		add_notification_json(arr, &notifications[i]);
	out = json_stringify(root);
	json_free(root);
	return out;
}

static int bind_create_input(struct json *root, struct scheduled_task_input *input,
			     int64_t time_anchor, char **owned_payload_json)
{
	struct json *item;
	struct json *payload = NULL;
	char *payload_json = NULL;
	int has_next_run_at = 0;
	int rc = 0;

	if (!root || !input || !owned_payload_json)
		return -EINVAL;
	memset(input, 0, sizeof(*input));
	input->source_session_id = 0;
	*owned_payload_json = NULL;
	item = json_member_ci(root, "title");
	if (json_is_string_value(item))
		input->title = json_string_value(item);
	item = json_member_ci(root, "kind");
	input->kind = json_is_string_value(item) ? json_string_value(item) : "agent";
	item = json_member_ci(root, "trigger_type");
	input->trigger_type = json_is_string_value(item) ? json_string_value(item) : "once";
	item = json_member_ci(root, "next_run_at");
	if (json_is_number_value(item)) {
		input->next_run_at = (int64_t)json_number_value(item);
		has_next_run_at = 1;
	}
	item = json_member_ci(root, "delay_seconds");
	if (json_is_number_value(item)) {
		int64_t delay = (int64_t)json_number_value(item);
		if (delay < 0)
			return -EINVAL;
		if (!has_next_run_at) {
			if (time_anchor <= 0)
				time_anchor = (int64_t)time(NULL);
			input->next_run_at = time_anchor + delay;
			has_next_run_at = 1;
		}
	}
	item = json_member_ci(root, "interval_seconds");
	if (json_is_number_value(item))
		input->interval_seconds = (int)json_number_value(item);
	item = json_member_ci(root, "timeout_at");
	if (json_is_number_value(item))
		input->timeout_at = (int64_t)json_number_value(item);
	item = json_member_ci(root, "max_attempts");
	if (json_is_number_value(item))
		input->max_attempts = (int)json_number_value(item);
	input->action_type = "agent_run";
	item = json_member_ci(root, "prompt");
	if (json_is_string_value(item) && json_string_value(item)[0]) {
		payload = json_object();
		if (!payload)
			return -ENOMEM;
		json_add_string(payload, "prompt", json_string_value(item));
		payload_json = json_stringify(payload);
		json_free(payload);
		if (!payload_json)
			return -ENOMEM;
		input->payload_json = payload_json;
		*owned_payload_json = payload_json;
	}
	item = json_member_ci(root, "policy_json");
	if (json_is_string_value(item))
		input->policy_json = json_string_value(item);
	item = json_member_ci(root, "notify_json");
	if (json_is_string_value(item))
		input->notify_json = json_string_value(item);

	if (!input->title || !has_next_run_at || input->next_run_at < 0 ||
	    !input->payload_json)
		rc = -EINVAL;
	return rc;
}

static char *tasks_tool_create(struct scheduled_tasks_tool_context *ctx, struct json *root)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	struct json *out;
	struct json *arr;
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

	out = json_object();
	arr = json_array();
	if (!out || !arr) {
		json_free(out);
		json_free(arr);
		scheduled_task_cleanup(&task);
		return json_error("out of memory");
	}
	json_add_string(out, "status", "created");
	json_add_number(out, "time_anchor", (double)ctx->time_anchor);
	json_object_add(out, "tasks", arr);
	add_task_json(arr, &task);
	json_add_number(out, "id", (double)task.id);
	json = json_stringify(out);
	json_free(out);
	scheduled_task_cleanup(&task);
	return json ? json : json_error("out of memory");
}

static char *tasks_tool_update(struct scheduled_tasks_tool_context *ctx, struct json *root)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	struct json *id_item;
	struct json *out;
	struct json *arr;
	char *owned_payload_json = NULL;
	char *json;
	int rc;

	id_item = json_member_ci(root, "id");
	if (!json_is_number_value(id_item))
		return json_error("missing task id");
	rc = bind_create_input(root, &input, ctx->time_anchor,
			       &owned_payload_json);
	if (rc != 0) {
		free(owned_payload_json);
		return json_error("missing or invalid task update fields");
	}
	input.source_session_id = ctx->source_session_id;
	rc = scheduled_task_update_with_events(ctx->db, (int64_t)json_number_value(id_item), &input,
					       &task, &ctx->events);
	free(owned_payload_json);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = json_object();
	arr = json_array();
	if (!out || !arr) {
		json_free(out);
		json_free(arr);
		scheduled_task_cleanup(&task);
		return json_error("out of memory");
	}
	json_add_string(out, "status", "updated");
	json_add_number(out, "time_anchor", (double)ctx->time_anchor);
	json_object_add(out, "tasks", arr);
	add_task_json(arr, &task);
	json = json_stringify(out);
	json_free(out);
	scheduled_task_cleanup(&task);
	return json ? json : json_error("out of memory");
}

static char *tasks_tool_list(struct db *db, struct json *root)
{
	struct scheduled_task *tasks = NULL;
	int count = 0;
	int limit = 20;
	const char *status = NULL;
	struct json *item;
	char *out;
	int rc;

	item = json_member_ci(root, "limit");
	if (json_is_number_value(item))
		limit = (int)json_number_value(item);
	item = json_member_ci(root, "status");
	if (json_is_string_value(item) && json_string_value(item)[0])
		status = json_string_value(item);
	rc = scheduled_task_list(db, status, limit, &tasks, &count);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = tasks_to_json(tasks, count);
	scheduled_task_free_list(tasks, count);
	return out ? out : json_error("out of memory");
}

static char *tasks_tool_cancel(struct scheduled_tasks_tool_context *ctx, struct json *root)
{
	struct json *item = json_member_ci(root, "id");
	int rc;

	if (!json_is_number_value(item))
		return json_error("missing task id");
	rc = scheduled_task_cancel_with_events(ctx->db, (int64_t)json_number_value(item),
					       &ctx->events);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	return strdup("{\"status\":\"cancelled\"}");
}

static char *tasks_tool_run_due(struct scheduled_tasks_tool_context *ctx, struct json *root)
{
	(void)ctx;
	(void)root;
	return json_error("run_due is scheduler-only for agent tasks");
}

static char *tasks_tool_inbox(struct db *db, struct json *root)
{
	struct notification *notifications = NULL;
	int count = 0;
	int limit = 20;
	struct json *item;
	char *out;
	int rc;

	item = json_member_ci(root, "limit");
	if (json_is_number_value(item))
		limit = (int)json_number_value(item);
	rc = notification_list_unread(db, limit, &notifications, &count);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	out = notifications_to_json(notifications, count);
	notification_free_list(notifications, count);
	return out ? out : json_error("out of memory");
}

static char *tasks_tool_mark_read(struct db *db, struct json *root)
{
	struct json *item = json_member_ci(root, "id");
	int rc;

	if (!json_is_number_value(item))
		return json_error("missing notification id");
	rc = notification_mark_read(db, (int64_t)json_number_value(item), 0);
	if (rc != 0)
		return json_error(morph_strerror(rc));
	return strdup("{\"status\":\"read\"}");
}

static int scheduled_tasks_tool_run(const char *args_json,
				    struct tool_result *result,
				    void *user_data)
{
	struct scheduled_tasks_tool_context *ctx = user_data;
	struct json *root;
	struct json *op_item;
	const char *op;
	char *out;

	if (!ctx || !ctx->db || !ctx->db->handle)
		return -EINVAL;
	root = json_parse_prefix(args_json ? args_json : "{}", NULL, NULL);
	if (!root) {
		(void)tool_result_success_json_text(result, json_error("invalid JSON"));
		return 0;
	}
	op_item = json_member_ci(root, "op");
	op = json_is_string_value(op_item) ? json_string_value(op_item) : "list";

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

	json_free(root);
	(void)tool_result_success_json_text(result, out ? out : json_error("oom"));
	return 0;
}

int scheduled_tasks_tool_init_events(
	struct tool_registry *reg, struct db *db,
	const struct scheduled_task_event_sink *events)
{
	struct scheduled_tasks_tool_context *ctx;
	int rc;

	if (!reg || !db)
		return -EINVAL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->db = db;
	ctx->time_anchor = (int64_t)time(NULL);
	if (events)
		ctx->events = *events;
	rc = tool_register(reg, &(struct tool_spec){ .origin = TOOL_ORIGIN_BUILTIN, .name = "tasks", .description = "Create and manage persistent scheduled tasks and the inbox. "
		"Use next_run_at for absolute Unix seconds, or delay_seconds "
		"for relative delays anchored at the current user turn start. "
		"Tasks run by asking the agent to complete the supplied prompt. "
		"Supported ops: "
		"create, update, list, cancel, inbox, mark_read.", .input_schema = "{\"type\":\"object\",\"properties\":{"
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
		"},\"required\":[\"op\"]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = scheduled_tasks_tool_run, .user_data = ctx, .user_data_destroy = scheduled_tasks_tool_context_destroy });
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
		return -EINVAL;
	entry = tool_lookup(reg, "tasks");
	if (!entry || !entry->user_data)
		return -ENOENT;
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
		return -EINVAL;
	entry = tool_lookup(reg, "tasks");
	if (!entry || !entry->user_data)
		return -ENOENT;
	ctx = entry->user_data;
	ctx->source_session_id = session_id > 0 ? session_id : 0;
	return 0;
}
