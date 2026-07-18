#include "sapi/cli/commands/registry.h"

static int parse_task_time_arg(const char *arg, int64_t *out)
{
	char *end = NULL;
	long long val;

	if (!arg || !out || !arg[0])
		return -EINVAL;
	if (arg[0] == '+') {
		val = strtoll(arg + 1, &end, 10);
		if (!end || *end != '\0' || val < 0)
			return -EINVAL;
		*out = (int64_t)time(NULL) + (int64_t)val;
		return 0;
	}
	val = strtoll(arg, &end, 10);
	if (!end || *end != '\0' || val < 0)
		return -EINVAL;
	*out = (int64_t)val;
	return 0;
}

static char *task_join_title(int argc, char **argv, int start)
{
	morph_buf_t title;
	int rc;

	rc = morph_buf_init(&title, 128);
	if (rc != 0)
		return NULL;
	for (int i = start; i < argc; i++) {
		if (title.len > 0) {
			rc = morph_buf_putc(&title, ' ');
			if (rc != 0)
				goto fail;
		}
		rc = morph_buf_puts(&title, argv[i]);
		if (rc != 0)
			goto fail;
	}
	return morph_buf_detach(&title);

fail:
	morph_buf_cleanup(&title);
	return NULL;
}

static int append_task_table_row(morph_buf_t *buf,
				 const struct scheduled_task *task);
static int append_task_show_markdown(morph_buf_t *buf,
				     const struct scheduled_task *task);

static int cmd_tasks_add(struct cli_context *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	struct session current;
	cJSON *payload = NULL;
	char *payload_json = NULL;
	char *title = NULL;
	int64_t next_run_at;
	int rc;

	if (argc < 4) {
		CMD_ERROR("usage: /tasks add <unix_time|+seconds> <title>");
		return -EINVAL;
	}
	rc = parse_task_time_arg(argv[2], &next_run_at);
	if (rc != 0) {
		CMD_ERROR("invalid time: %s", argv[2]);
		return rc;
	}
	title = task_join_title(argc, argv, 3);
	if (!title || !title[0]) {
		free(title);
		CMD_ERROR("missing task title");
		return -EINVAL;
	}

	payload = cJSON_CreateObject();
	if (!payload) {
		free(title);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(payload, "prompt", title);
	payload_json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!payload_json) {
		free(title);
		return -ENOMEM;
	}

	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = title;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = next_run_at;
	input.action_type = "agent_run";
	input.payload_json = payload_json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_create(ctx->runtime, &input, &task);
	free(payload_json);
	free(title);
	if (rc != 0) {
		CMD_ERROR("failed to create task: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK("task created: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cmd_tasks_every(struct cli_context *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	struct session current;
	cJSON *payload = NULL;
	char *payload_json = NULL;
	char *title = NULL;
	char *end = NULL;
	long interval;
	int rc;

	if (argc < 4) {
		CMD_ERROR("usage: /tasks every <seconds> <prompt>");
		return -EINVAL;
	}
	interval = strtol(argv[2], &end, 10);
	if (!end || *end != '\0' || interval <= 0 || interval > INT_MAX) {
		CMD_ERROR("invalid interval seconds: %s", argv[2]);
		return -EINVAL;
	}
	title = task_join_title(argc, argv, 3);
	if (!title || !title[0]) {
		free(title);
		CMD_ERROR("missing task prompt");
		return -EINVAL;
	}
	payload = cJSON_CreateObject();
	if (!payload) {
		free(title);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(payload, "prompt", title);
	payload_json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!payload_json) {
		free(title);
		return -ENOMEM;
	}

	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = title;
	input.kind = "agent";
	input.trigger_type = "interval";
	input.next_run_at = (int64_t)time(NULL) + interval;
	input.interval_seconds = (int)interval;
	input.action_type = "agent_run";
	input.payload_json = payload_json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_create(ctx->runtime, &input, &task);
	free(payload_json);
	free(title);
	if (rc != 0) {
		CMD_ERROR("failed to create task: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK("recurring task created: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cmd_tasks_update(struct cli_context *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task;
	struct session current;
	cJSON *payload = NULL;
	char *payload_json = NULL;
	char *title = NULL;
	char *end = NULL;
	long long id;
	int64_t next_run_at;
	int rc;

	if (argc < 5) {
		CMD_ERROR("usage: /tasks update <id> <unix_time|+seconds> <title>");
		return -EINVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR("invalid task id: %s", argv[2]);
		return -EINVAL;
	}
	rc = parse_task_time_arg(argv[3], &next_run_at);
	if (rc != 0) {
		CMD_ERROR("invalid time: %s", argv[3]);
		return rc;
	}
	title = task_join_title(argc, argv, 4);
	if (!title || !title[0]) {
		free(title);
		CMD_ERROR("missing task title");
		return -EINVAL;
	}
	payload = cJSON_CreateObject();
	if (!payload) {
		free(title);
		return -ENOMEM;
	}
	cJSON_AddStringToObject(payload, "prompt", title);
	payload_json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!payload_json) {
		free(title);
		return -ENOMEM;
	}
	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = title;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = next_run_at;
	input.action_type = "agent_run";
	input.payload_json = payload_json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_update(ctx->runtime, (int64_t)id, &input, &task);
	free(payload_json);
	free(title);
	if (rc != 0) {
		CMD_ERROR("failed to update task: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK("task updated: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cmd_tasks_list(struct cli_context *ctx, int argc, char **argv)
{
	struct scheduled_task *tasks = NULL;
	morph_buf_t md;
	int md_ready = 0;
	int count = 0;
	const char *status = NULL;
	int rc;

	if (argc > 2)
		status = argv[2];
	rc = runtime_task_list(ctx->runtime, status, 50, &tasks, &count);
	if (rc != 0) {
		CMD_ERROR("failed to list tasks: %s", morph_strerror(rc));
		return rc;
	}
	CMD_HEADER("scheduled tasks");
	if (count == 0) {
		printf("No tasks.\n");
	} else {
		rc = morph_buf_init(&md, 2048);
		if (rc != 0)
			goto out;
		md_ready = 1;
		rc = morph_buf_puts(&md,
			"| ID | Status | Schedule | Next run | Attempts | Task |\n"
			"|---:|---|---|---|---|---|\n");
		for (int i = 0; rc == 0 && i < count; i++)
			rc = append_task_table_row(&md, &tasks[i]);
		if (rc == 0)
			cli_markdown_render_ansi(morph_buf_cstr(&md));
	}
out:
	if (md_ready)
		morph_buf_cleanup(&md);
	scheduled_task_free_list(tasks, count);
	return rc;
}

static int cmd_tasks_show(struct cli_context *ctx, int argc, char **argv)
{
	struct scheduled_task task;
	morph_buf_t md;
	char *end = NULL;
	long long id;
	int md_ready = 0;
	int rc;

	if (argc < 3) {
		CMD_ERROR("usage: /tasks show <id>");
		return -EINVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR("invalid task id: %s", argv[2]);
		return -EINVAL;
	}
	memset(&task, 0, sizeof(task));
	rc = runtime_task_get(ctx->runtime, (int64_t)id, &task);
	if (rc != 0) {
		CMD_ERROR("failed to load task: %s", morph_strerror(rc));
		return rc;
	}
	CMD_HEADER("task");
	rc = morph_buf_init(&md, 2048);
	if (rc != 0)
		goto out;
	md_ready = 1;
	rc = append_task_show_markdown(&md, &task);
	if (rc == 0)
		cli_markdown_render_ansi(morph_buf_cstr(&md));
out:
	if (md_ready)
		morph_buf_cleanup(&md);
	scheduled_task_cleanup(&task);
	return rc;
}

static int cmd_tasks_cancel(struct cli_context *ctx, int argc, char **argv)
{
	char *end = NULL;
	long long id;
	int rc;

	if (argc < 3) {
		CMD_ERROR("usage: /tasks cancel <id>");
		return -EINVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR("invalid task id: %s", argv[2]);
		return -EINVAL;
	}
	rc = runtime_task_cancel(ctx->runtime, (int64_t)id);
	if (rc != 0) {
		CMD_ERROR("failed to cancel task: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK("task cancelled: #%lld", id);
	return 0;
}

static int cmd_tasks_run(struct cli_context *ctx, int argc, char **argv)
{
	int ran = 0;
	int limit = 50;
	int rc;

	if (argc > 2)
		limit = atoi(argv[2]);
	rc = runtime_tasks_run_due_for_runtime(ctx->runtime, limit,
		cli_scheduled_task_runner, ctx, NULL, NULL);
	if (rc >= 0) {
		ran = rc;
		rc = 0;
	}
	if (rc != 0) {
		CMD_ERROR("failed to run due tasks: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK("processed due tasks: %d", ran);
	return 0;
}

static int cmd_tasks(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "list";

	if (strcmp(sub, "add") == 0)
		return cmd_tasks_add(ctx, argc, argv);
	if (strcmp(sub, "every") == 0)
		return cmd_tasks_every(ctx, argc, argv);
	if (strcmp(sub, "update") == 0 || strcmp(sub, "edit") == 0)
		return cmd_tasks_update(ctx, argc, argv);
	if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0)
		return cmd_tasks_list(ctx, argc, argv);
	if (strcmp(sub, "show") == 0 || strcmp(sub, "info") == 0)
		return cmd_tasks_show(ctx, argc, argv);
	if (strcmp(sub, "cancel") == 0 || strcmp(sub, "rm") == 0)
		return cmd_tasks_cancel(ctx, argc, argv);
	if (strcmp(sub, "run") == 0 || strcmp(sub, "due") == 0)
		return cmd_tasks_run(ctx, argc, argv);

	CMD_ERROR("usage: /tasks [list [status]|show <id>|add <unix_time|+seconds> <prompt>|every <seconds> <prompt>|update <id> <unix_time|+seconds> <prompt>|cancel <id>|run [limit]]");
	return -EINVAL;
}

static void notification_time_string(int64_t ts, char *buf, size_t buf_len)
{
	time_t t = (time_t)ts;
	struct tm tm_local;

	if (!buf || buf_len == 0)
		return;
	if (ts <= 0) {
		snprintf(buf, buf_len, "-");
		return;
	}
	if (!localtime_r(&t, &tm_local)) {
		snprintf(buf, buf_len, "%lld", (long long)ts);
		return;
	}
	if (strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_local) == 0)
		snprintf(buf, buf_len, "%lld", (long long)ts);
}

static int markdown_table_cell(morph_buf_t *buf, const char *text)
{
	const char *s = text ? text : "";
	int rc = 0;

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '|':
			rc = morph_buf_puts(buf, "\\|");
			break;
		case '\\':
			rc = morph_buf_puts(buf, "\\\\");
			break;
		case '\n':
		case '\t':
			rc = morph_buf_putc(buf, ' ');
			break;
		case '\r':
			break;
		default:
			if (*p < 0x20)
				rc = morph_buf_putc(buf, ' ');
			else
				rc = morph_buf_putc(buf, (char)*p);
			break;
		}
		if (rc != 0)
			return rc;
	}
	return 0;
}

static const char *task_prompt_from_payload(cJSON **payload,
					    const struct scheduled_task *task)
{
	cJSON *prompt_json;

	if (!payload || !task)
		return "";
	*payload = task->payload_json ? cJSON_Parse(task->payload_json) : NULL;
	prompt_json = *payload ? cJSON_GetObjectItem(*payload, "prompt") : NULL;
	if (cJSON_IsString(prompt_json) && prompt_json->valuestring[0])
		return prompt_json->valuestring;
	return task->title[0] ? task->title : "";
}

static int append_markdown_indented_block(morph_buf_t *buf, const char *text)
{
	const char *s = text ? text : "";
	int line_start = 1;
	int rc = 0;

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (line_start) {
			rc = morph_buf_puts(buf, "    ");
			if (rc != 0)
				return rc;
			line_start = 0;
		}
		if (*p == '\r')
			continue;
		rc = morph_buf_putc(buf, (char)*p);
		if (rc != 0)
			return rc;
		if (*p == '\n')
			line_start = 1;
	}
	if (line_start)
		rc = morph_buf_puts(buf, "    ");
	if (rc == 0)
		rc = morph_buf_putc(buf, '\n');
	return rc;
}

static int append_task_table_row(morph_buf_t *buf,
				 const struct scheduled_task *task)
{
	char due[32];
	char attempts[32];
	char schedule[32];
	cJSON *payload = NULL;
	const char *prompt;
	int rc;

	if (!buf || !task)
		MORPH_RETURN(-EINVAL);
	notification_time_string(task->next_run_at, due, sizeof(due));
	if (task->max_attempts > 0)
		snprintf(attempts, sizeof(attempts), "%d/%d",
			 task->attempts, task->max_attempts);
	else
		snprintf(attempts, sizeof(attempts), "%d/∞", task->attempts);
	if (task->interval_seconds > 0)
		snprintf(schedule, sizeof(schedule), "every %ds",
			 task->interval_seconds);
	else
		snprintf(schedule, sizeof(schedule), "once");
	prompt = task_prompt_from_payload(&payload, task);
	rc = morph_buf_printf(buf, "| %lld | ", (long long)task->id);
	if (rc == 0)
		rc = markdown_table_cell(buf, task->status);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, schedule);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, due);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, attempts);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, prompt);
	if (rc == 0)
		rc = morph_buf_puts(buf, " |\n");
	cJSON_Delete(payload);
	return rc;
}

static int append_task_detail_row(morph_buf_t *buf, const char *field,
				  const char *value)
{
	int rc;

	rc = morph_buf_puts(buf, "| ");
	if (rc == 0)
		rc = markdown_table_cell(buf, field);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, value);
	if (rc == 0)
		rc = morph_buf_puts(buf, " |\n");
	return rc;
}

static int append_task_show_markdown(morph_buf_t *buf,
				     const struct scheduled_task *task)
{
	char id[32];
	char due[32];
	char created[32];
	char updated[32];
	char attempts[32];
	char schedule[32];
	cJSON *payload = NULL;
	const char *prompt;
	int rc;

	if (!buf || !task)
		MORPH_RETURN(-EINVAL);
	notification_time_string(task->next_run_at, due, sizeof(due));
	notification_time_string(task->created_at, created, sizeof(created));
	notification_time_string(task->updated_at, updated, sizeof(updated));
	snprintf(id, sizeof(id), "%lld", (long long)task->id);
	if (task->max_attempts > 0)
		snprintf(attempts, sizeof(attempts), "%d/%d",
			 task->attempts, task->max_attempts);
	else
		snprintf(attempts, sizeof(attempts), "%d/∞", task->attempts);
	if (task->interval_seconds > 0)
		snprintf(schedule, sizeof(schedule), "every %d seconds",
			 task->interval_seconds);
	else
		snprintf(schedule, sizeof(schedule), "once");
	prompt = task_prompt_from_payload(&payload, task);
	rc = morph_buf_printf(buf, "## Task #%lld\n\n",
			      (long long)task->id);
	if (rc == 0)
		rc = morph_buf_puts(buf,
			"| Field | Value |\n"
			"|---|---|\n");
	if (rc == 0)
		rc = append_task_detail_row(buf, "ID", id);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Title", task->title);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Status", task->status);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Schedule", schedule);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Next run", due);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Attempts", attempts);
	if (rc == 0 && task->last_error[0])
		rc = append_task_detail_row(buf, "Last error", task->last_error);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Created", created);
	if (rc == 0)
		rc = append_task_detail_row(buf, "Updated", updated);
	if (rc == 0)
		rc = morph_buf_puts(buf, "\n### Prompt\n\n");
	if (rc == 0)
		rc = append_markdown_indented_block(buf, prompt);
	cJSON_Delete(payload);
	return rc;
}

static int append_notification_table_row(morph_buf_t *buf,
					 const struct notification *n)
{
	char created[32];
	int rc;

	if (!buf || !n)
		MORPH_RETURN(-EINVAL);
	notification_time_string(n->created_at, created, sizeof(created));
	rc = morph_buf_printf(buf, "| %lld | ",
			      (long long)n->id);
	if (rc == 0)
		rc = morph_buf_printf(buf, "%lld | ",
				      (long long)n->task_id);
	if (rc == 0)
		rc = markdown_table_cell(buf, n->level);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, created);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, n->delivery_status);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, n->title);
	if (rc == 0)
		rc = morph_buf_puts(buf, " | ");
	if (rc == 0)
		rc = markdown_table_cell(buf, n->body);
	if (rc == 0)
		rc = morph_buf_puts(buf, " |\n");
	return rc;
}

static int cmd_inbox_list(struct cli_context *ctx, int argc, char **argv)
{
	struct notification *notifications = NULL;
	morph_buf_t md;
	int md_ready = 0;
	int count = 0;
	int limit = 20;
	int rc;

	if (argc > 2)
		limit = atoi(argv[2]);
	rc = runtime_notification_list(ctx->runtime, limit, &notifications,
				       &count);
	if (rc != 0) {
		CMD_ERROR("failed to list inbox: %s", morph_strerror(rc));
		return rc;
	}
	CMD_HEADER("inbox");
	if (count == 0) {
		printf("No unread notifications.\n");
	} else {
		rc = morph_buf_init(&md, 1024);
		if (rc != 0)
			goto out;
		md_ready = 1;
		rc = morph_buf_puts(&md,
			"| ID | Task | Level | Created | Delivery | Title | Body |\n"
			"|---:|---:|---|---|---|---|---|\n");
		for (int i = 0; rc == 0 && i < count; i++)
			rc = append_notification_table_row(&md,
							   &notifications[i]);
		if (rc == 0)
			cli_markdown_render_ansi(morph_buf_cstr(&md));
	}
out:
	if (md_ready)
		morph_buf_cleanup(&md);
	notification_free_list(notifications, count);
	return rc;
}

static int cmd_inbox_read(struct cli_context *ctx, int argc, char **argv)
{
	char *end = NULL;
	long long id;
	int rc;

	if (argc < 3) {
		CMD_ERROR("usage: /inbox read <id>");
		return -EINVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR("invalid notification id: %s", argv[2]);
		return -EINVAL;
	}
	rc = runtime_notification_mark_read(ctx->runtime, (int64_t)id);
	if (rc != 0) {
		CMD_ERROR("failed to mark notification read: %s",
			  morph_strerror(rc));
		return rc;
	}
	CMD_OK("notification marked read: #%lld", id);
	return 0;
}

static int cmd_inbox(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "list";

	if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0)
		return cmd_inbox_list(ctx, argc, argv);
	if (strcmp(sub, "read") == 0)
		return cmd_inbox_read(ctx, argc, argv);
	CMD_ERROR("usage: /inbox [list [limit]|read <id>]");
	return -EINVAL;
}


static const struct cli_command task_commands[] = {
	{ "/tasks",   cmd_tasks,   "Manage scheduled tasks",            "/tasks [list|show|add|every|update|cancel|run]" },
	{ "/todo",    cmd_tasks,   "Alias for /tasks",                  "/todo [list|show|add|every|update|cancel|run]" },
	{ "/inbox",   cmd_inbox,   "Show task notifications",           "/inbox [list|read]" },
};

int cli_register_task_commands(void)
{
	return cli_command_register_many(task_commands,
		(int)(sizeof(task_commands) / sizeof(task_commands[0])));
}
