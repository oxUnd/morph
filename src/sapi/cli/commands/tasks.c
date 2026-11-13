#include "sapi/cli/commands/tasks.h"
#include "nb/err.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/output.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "db/scheduled_task.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/services/scheduler.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"
#include "nb/utf8.h"
#include "cJSON.h"

static int cli_tasks_parse_task_time_arg(const char *arg, int64_t *out)
{
	char *end = NULL;
	long long val;

	if (!arg || !out || !arg[0])
		return NB_ERR_INVAL;
	if (arg[0] == '+') {
		val = strtoll(arg + 1, &end, 10);
		if (!end || *end != '\0' || val < 0)
			return NB_ERR_INVAL;
		*out = (int64_t)time(NULL) + (int64_t)val;
		return 0;
	}
	val = strtoll(arg, &end, 10);
	if (!end || *end != '\0' || val < 0)
		return NB_ERR_INVAL;
	*out = (int64_t)val;
	return 0;
}

static char *cli_tasks_task_join_title(int argc, char **argv, int start)
{
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	rc = (nb_buf_init(&title), nb_buf_reserve(&title, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	for (int i = start; i < argc; i++) {
		if (title.len > 0) {
			rc = (nb_buf_append_char(&title, ' ') ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				return NULL;
		}
		rc = (nb_buf_append_cstr(&title, argv[i]) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return NULL;
	}
	return (nb_buf_ok(&title) ? strdup(nb_buf_cstr(&title)) : NULL);
}

struct cli_tasks_prompt {
	char *text;
	char *title;
	char *json;
};

static void cli_tasks_prompt_cleanup(struct cli_tasks_prompt *prompt)
{
	free(prompt->text);
	free(prompt->title);
	cJSON_free(prompt->json);
	*prompt = (struct cli_tasks_prompt){0};
}

static int cli_tasks_prompt_create(int argc, char **argv, int start, struct cli_tasks_prompt *out)
{
	struct cli_tasks_prompt value NB_CLEANUP(cli_tasks_prompt_cleanup) = {0};
	nb_buf title NB_CLEANUP(nb_buf_cleanup) = {0};
	cJSON *payload NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	value.text = cli_tasks_task_join_title(argc, argv, start);
	if (!value.text)
		return NB_ERR_NOMEM;
	if (!value.text[0] || nb_utf8_validate(nb_str_from_cstr(value.text), NULL))
		return NB_ERR_INVAL;
	rc = (nb_buf_init(&title),
	      nb_buf_reserve(&title, SCHEDULED_TASK_TEXT_MAX) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	size_t bytes = strlen(value.text);
	int shortened = bytes >= SCHEDULED_TASK_TEXT_MAX;
	if (shortened)
		bytes = nb_utf8_clamp_bytes(value.text, SCHEDULED_TASK_TEXT_MAX - sizeof("…"));
	rc = (nb_buf_append(&title, value.text, bytes) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && shortened)
		rc = (nb_buf_append_cstr(&title, "…") ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	value.title = (nb_buf_ok(&title) ? strdup(nb_buf_cstr(&title)) : NULL);
	payload = cJSON_CreateObject();
	if (!payload || !cJSON_AddStringToObject(payload, "prompt", value.text))
		return NB_ERR_NOMEM;
	value.json = cJSON_PrintUnformatted(payload);
	if (!value.json)
		return NB_ERR_NOMEM;
	*out = value;
	value = (struct cli_tasks_prompt){0};
	return 0;
}

static int cli_tasks_print_task_list_tree(struct cli_application *ctx,
					  const struct scheduled_task *tasks, int count);
static int cli_tasks_append_task_show_markdown(nb_buf *buf, const struct scheduled_task *task);

static int cli_tasks_command_tasks_add(struct cli_application *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	struct session current;
	struct cli_tasks_prompt prompt NB_CLEANUP(cli_tasks_prompt_cleanup) = {0};
	int64_t next_run_at;
	int rc;

	if (argc < 4) {
		CMD_ERROR(ctx, "usage: /tasks add <unix_time|+seconds> <title>");
		return NB_ERR_INVAL;
	}
	rc = cli_tasks_parse_task_time_arg(argv[2], &next_run_at);
	if (rc != 0) {
		CMD_ERROR(ctx, "invalid time: %s", argv[2]);
		return rc;
	}
	rc = cli_tasks_prompt_create(argc, argv, 3, &prompt);
	if (rc != 0) {
		CMD_ERROR(ctx, "Invalid task prompt: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}

	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = prompt.title;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = next_run_at;
	input.action_type = "agent_run";
	input.payload_json = prompt.json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_create(ctx->runtime, &input, &task);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to create task: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "task created: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cli_tasks_command_tasks_every(struct cli_application *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	struct session current;
	struct cli_tasks_prompt prompt NB_CLEANUP(cli_tasks_prompt_cleanup) = {0};
	char *end = NULL;
	long interval;
	int rc;

	if (argc < 4) {
		CMD_ERROR(ctx, "usage: /tasks every <seconds> <prompt>");
		return NB_ERR_INVAL;
	}
	interval = strtol(argv[2], &end, 10);
	if (!end || *end != '\0' || interval <= 0 || interval > INT_MAX) {
		CMD_ERROR(ctx, "invalid interval seconds: %s", argv[2]);
		return NB_ERR_INVAL;
	}
	rc = cli_tasks_prompt_create(argc, argv, 3, &prompt);
	if (rc != 0) {
		CMD_ERROR(ctx, "Invalid task prompt: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}

	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = prompt.title;
	input.kind = "agent";
	input.trigger_type = "interval";
	input.next_run_at = (int64_t)time(NULL) + interval;
	input.interval_seconds = (int)interval;
	input.action_type = "agent_run";
	input.payload_json = prompt.json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_create(ctx->runtime, &input, &task);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to create task: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "recurring task created: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cli_tasks_command_tasks_update(struct cli_application *ctx, int argc, char **argv)
{
	struct scheduled_task_input input;
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	struct session current;
	struct cli_tasks_prompt prompt NB_CLEANUP(cli_tasks_prompt_cleanup) = {0};
	char *end = NULL;
	long long id;
	int64_t next_run_at;
	int rc;

	if (argc < 5) {
		CMD_ERROR(ctx, "usage: /tasks update <id> <unix_time|+seconds> <title>");
		return NB_ERR_INVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR(ctx, "invalid task id: %s", argv[2]);
		return NB_ERR_INVAL;
	}
	rc = cli_tasks_parse_task_time_arg(argv[3], &next_run_at);
	if (rc != 0) {
		CMD_ERROR(ctx, "invalid time: %s", argv[3]);
		return rc;
	}
	rc = cli_tasks_prompt_create(argc, argv, 4, &prompt);
	if (rc != 0) {
		CMD_ERROR(ctx, "Invalid task prompt: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}

	memset(&input, 0, sizeof(input));
	(void)runtime_session_current(ctx->runtime, &current);
	input.source_session_id = current.id;
	input.title = prompt.title;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = next_run_at;
	input.action_type = "agent_run";
	input.payload_json = prompt.json;
	input.notify_json = "{\"targets\":[\"inbox\"]}";
	rc = runtime_task_update(ctx->runtime, (int64_t)id, &input, &task);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to update task: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "task updated: #%lld", (long long)task.id);
	scheduled_task_cleanup(&task);
	return 0;
}

static int cli_tasks_command_tasks_list(struct cli_application *ctx, int argc, char **argv)
{
	struct scheduled_task *tasks = NULL;
	int count = 0;
	const char *status = NULL;
	int rc;

	if (argc > 2)
		status = argv[2];
	rc = runtime_task_list(ctx->runtime, status, 50, &tasks, &count);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to list tasks: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_HEADER(ctx, "scheduled tasks (%d)", count);
	if (count == 0)
		cli_command_print_tree_note(ctx, "no tasks");
	else
		rc = cli_tasks_print_task_list_tree(ctx, tasks, count);
	scheduled_task_free_list(tasks, count);
	return rc;
}

static int cli_tasks_command_tasks_show(struct cli_application *ctx, int argc, char **argv)
{
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {0};
	nb_buf md NB_CLEANUP(nb_buf_cleanup) = {0};
	char *end = NULL;
	long long id;
	int rc;

	if (argc < 3) {
		CMD_ERROR(ctx, "usage: /tasks show <id>");
		return NB_ERR_INVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR(ctx, "invalid task id: %s", argv[2]);
		return NB_ERR_INVAL;
	}
	memset(&task, 0, sizeof(task));
	rc = runtime_task_copy(ctx->runtime, (int64_t)id, &task);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to load task: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_HEADER(ctx, "task");
	rc = (nb_buf_init(&md), nb_buf_reserve(&md, 2048) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = cli_tasks_append_task_show_markdown(&md, &task);
	if (rc == 0)
		rc = cli_command_output_markdown_render(ctx, nb_buf_cstr(&md));
	return rc;
}

static int cli_tasks_command_tasks_cancel(struct cli_application *ctx, int argc, char **argv)
{
	char *end = NULL;
	long long id;
	int rc;

	if (argc < 3) {
		CMD_ERROR(ctx, "usage: /tasks cancel <id>");
		return NB_ERR_INVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR(ctx, "invalid task id: %s", argv[2]);
		return NB_ERR_INVAL;
	}
	rc = runtime_task_cancel(ctx->runtime, (int64_t)id);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to cancel task: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "task cancelled: #%lld", id);
	return 0;
}

static int cli_tasks_command_tasks_run(struct cli_application *ctx, int argc, char **argv)
{
	int ran = 0;
	int limit = 50;
	int rc;

	if (argc > 2)
		limit = atoi(argv[2]);
	rc = runtime_tasks_run_due_for_runtime(ctx->runtime, limit, cli_scheduler_run_task, ctx,
					       NULL, NULL);
	if (rc >= 0) {
		ran = rc;
		rc = 0;
	}
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to run due tasks: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "processed due tasks: %d", ran);
	return 0;
}

static int cli_tasks_command_tasks(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "list";

	if (strcmp(sub, "add") == 0)
		return cli_tasks_command_tasks_add(ctx, argc, argv);
	if (strcmp(sub, "every") == 0)
		return cli_tasks_command_tasks_every(ctx, argc, argv);
	if (strcmp(sub, "update") == 0 || strcmp(sub, "edit") == 0)
		return cli_tasks_command_tasks_update(ctx, argc, argv);
	if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0)
		return cli_tasks_command_tasks_list(ctx, argc, argv);
	if (strcmp(sub, "show") == 0 || strcmp(sub, "info") == 0)
		return cli_tasks_command_tasks_show(ctx, argc, argv);
	if (strcmp(sub, "cancel") == 0 || strcmp(sub, "rm") == 0)
		return cli_tasks_command_tasks_cancel(ctx, argc, argv);
	if (strcmp(sub, "run") == 0 || strcmp(sub, "due") == 0)
		return cli_tasks_command_tasks_run(ctx, argc, argv);

	CMD_ERROR(ctx, "usage: /tasks [list [status]|show <id>|add <unix_time|+seconds> "
		       "<prompt>|every <seconds> <prompt>|update <id> <unix_time|+seconds> "
		       "<prompt>|cancel <id>|run [limit]]");
	return NB_ERR_INVAL;
}

static void cli_tasks_notification_time_string(int64_t ts, char *buf, size_t buf_len)
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

static const char *cli_tasks_task_prompt_from_payload(cJSON **payload,
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

static int cli_tasks_task_status_is_terminal(const char *status)
{
	return status && (strcmp(status, "completed") == 0 || strcmp(status, "cancelled") == 0 ||
			  strcmp(status, "failed") == 0);
}

static enum cli_output_status cli_tasks_task_status(const char *status)
{
	if (!status)
		return CLI_OUTPUT_STATUS_PENDING;
	if (strcmp(status, "completed") == 0)
		return CLI_OUTPUT_STATUS_DONE;
	if (strcmp(status, "failed") == 0)
		return CLI_OUTPUT_STATUS_FAILED;
	if (strcmp(status, "cancelled") == 0)
		return CLI_OUTPUT_STATUS_CANCELLED;
	if (strcmp(status, "running") == 0)
		return CLI_OUTPUT_STATUS_RUNNING;
	return CLI_OUTPUT_STATUS_PENDING;
}

static void cli_tasks_task_list_schedule(const struct scheduled_task *task, char *schedule,
					 size_t schedule_size)
{
	if (task->interval_seconds > 0)
		snprintf(schedule, schedule_size, "every %ds", task->interval_seconds);
	else
		snprintf(schedule, schedule_size, "once");
}

static int cli_tasks_print_active_task(struct cli_application *ctx,
				       const struct scheduled_task *task, int group_is_last,
				       int is_last)
{
	char due[32];
	char attempts[32];
	char schedule[32];
	nb_buf summary NB_CLEANUP(nb_buf_cleanup) = {0};
	cJSON *payload = NULL;
	const char *prompt;
	int rc;

	if (!task)
		return NB_ERR_INVAL;
	cli_tasks_notification_time_string(task->next_run_at, due, sizeof(due));
	if (task->max_attempts > 0)
		snprintf(attempts, sizeof(attempts), "%d/%d", task->attempts, task->max_attempts);
	else
		snprintf(attempts, sizeof(attempts), "%d/∞", task->attempts);
	cli_tasks_task_list_schedule(task, schedule, sizeof(schedule));
	prompt = cli_tasks_task_prompt_from_payload(&payload, task);
	rc = (nb_buf_init(&summary), nb_buf_reserve(&summary, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		cJSON_Delete(payload);
		return rc;
	}
	rc = (nb_buf_printf(&summary, "#%lld  %s · %s · next %s · %s", (long long)task->id,
			    task->status, schedule, due, attempts)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc == 0) {
		struct cli_output_status_row row = {
			.status = cli_tasks_task_status(task->status),
			.place = CLI_OUTPUT_ROW_GROUP,
			.group_is_last = group_is_last,
			.is_last = is_last,
			.wide = 1,
			.name = nb_buf_cstr(&summary),
			.description = prompt,
		};

		rc = cli_command_output_status_rows(ctx, &row, 1);
	}
	nb_buf_cleanup(&summary);
	cJSON_Delete(payload);
	return rc;
}

static void cli_tasks_print_historical_task(struct cli_application *ctx,
					    const struct scheduled_task *task, int is_last)
{
	char label[96];
	const char *description = task->title[0] ? task->title : "task";
	struct cli_output_status_row row;

	snprintf(label, sizeof(label), "#%lld  %s", (long long)task->id, task->status);
	row = (struct cli_output_status_row){
		.status = cli_tasks_task_status(task->status),
		.place = CLI_OUTPUT_ROW_INDENTED,
		.is_last = is_last,
		.name = label,
		.description = description,
	};
	(void)cli_command_output_status_rows(ctx, &row, 1);
}

static int cli_tasks_print_task_list_tree(struct cli_application *ctx,
					  const struct scheduled_task *tasks, int count)
{
	int active_count = 0;
	int history_count = 0;
	int active_index = 0;
	int history_index = 0;
	int rc = 0;

	if (!tasks || count <= 0)
		return NB_ERR_INVAL;
	for (int i = 0; i < count; i++) {
		if (cli_tasks_task_status_is_terminal(tasks[i].status))
			history_count++;
		else
			active_count++;
	}
	if (active_count > 0) {
		(void)cli_command_output_group(ctx, "active", active_count,
					       history_count == 0);
		for (int i = 0; rc == 0 && i < count; i++) {
			if (cli_tasks_task_status_is_terminal(tasks[i].status))
				continue;
			active_index++;
			rc = cli_tasks_print_active_task(
					ctx, &tasks[i], history_count == 0,
					active_index == active_count);
		}
	}
	if (rc == 0 && history_count > 0) {
		(void)cli_command_output_group(ctx, "history", history_count, 1);
		for (int i = 0; i < count; i++) {
			if (!cli_tasks_task_status_is_terminal(tasks[i].status))
				continue;
			history_index++;
			cli_tasks_print_historical_task(ctx, &tasks[i],
							history_index == history_count);
		}
	}
	return rc;
}

static int cli_tasks_append_task_show_markdown(nb_buf *buf, const struct scheduled_task *task)
{
	char id[32];
	char heading[48];
	char due[32];
	char created[32];
	char updated[32];
	char attempts[32];
	char schedule[32];
	const char *cells[18];
	size_t rows = 0;
	cJSON *payload = NULL;
	const char *prompt;
	static const struct cli_output_column columns[] = {
		{.header = "Field", .align = CLI_OUTPUT_ALIGN_LEFT},
		{.header = "Value", .align = CLI_OUTPUT_ALIGN_LEFT},
	};
	struct cli_output_table table = {
		.columns = columns,
		.column_count = 2,
		.cells = cells,
	};
	int rc;

	if (!buf || !task)
		return NB_ERR_INVAL;
	cli_tasks_notification_time_string(task->next_run_at, due, sizeof(due));
	cli_tasks_notification_time_string(task->created_at, created, sizeof(created));
	cli_tasks_notification_time_string(task->updated_at, updated, sizeof(updated));
	snprintf(id, sizeof(id), "%lld", (long long)task->id);
	snprintf(heading, sizeof(heading), "Task #%lld", (long long)task->id);
	if (task->max_attempts > 0)
		snprintf(attempts, sizeof(attempts), "%d/%d", task->attempts, task->max_attempts);
	else
		snprintf(attempts, sizeof(attempts), "%d/∞", task->attempts);
	if (task->interval_seconds > 0)
		snprintf(schedule, sizeof(schedule), "every %d seconds", task->interval_seconds);
	else
		snprintf(schedule, sizeof(schedule), "once");
	prompt = cli_tasks_task_prompt_from_payload(&payload, task);
	cells[rows * 2] = "ID";
	cells[rows++ * 2 + 1] = id;
	cells[rows * 2] = "Title";
	cells[rows++ * 2 + 1] = task->title;
	cells[rows * 2] = "Status";
	cells[rows++ * 2 + 1] = task->status;
	cells[rows * 2] = "Schedule";
	cells[rows++ * 2 + 1] = schedule;
	cells[rows * 2] = "Next run";
	cells[rows++ * 2 + 1] = due;
	cells[rows * 2] = "Attempts";
	cells[rows++ * 2 + 1] = attempts;
	if (task->last_error[0]) {
		cells[rows * 2] = "Last error";
		cells[rows++ * 2 + 1] = task->last_error;
	}
	cells[rows * 2] = "Created";
	cells[rows++ * 2 + 1] = created;
	cells[rows * 2] = "Updated";
	cells[rows++ * 2 + 1] = updated;
	table.row_count = rows;
	rc = cli_command_output_markdown_heading(buf, 2, heading);
	if (rc == 0)
		rc = cli_command_output_markdown_table(buf, &table);
	if (rc == 0)
		rc = cli_command_output_markdown_heading(buf, 3, "Prompt");
	if (rc == 0)
		rc = cli_command_output_markdown_block(buf, prompt);
	cJSON_Delete(payload);
	return rc;
}
static int cli_tasks_render_inbox(struct cli_application *ctx,
		const struct notification *notifications, int count)
{
	static const struct cli_output_column columns[] = {
		{.header = "ID", .align = CLI_OUTPUT_ALIGN_RIGHT},
		{.header = "Task", .align = CLI_OUTPUT_ALIGN_RIGHT},
		{.header = "Level", .align = CLI_OUTPUT_ALIGN_LEFT},
		{.header = "Created", .align = CLI_OUTPUT_ALIGN_LEFT},
		{.header = "Delivery", .align = CLI_OUTPUT_ALIGN_LEFT},
		{.header = "Title", .align = CLI_OUTPUT_ALIGN_LEFT},
		{.header = "Body", .align = CLI_OUTPUT_ALIGN_LEFT},
	};
	struct cli_output_table table = {
		.columns = columns,
		.column_count = sizeof(columns) / sizeof(columns[0]),
	};
	nb_buf md NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = 0;

	CMD_HEADER(ctx, "inbox");
	if (count == 0) {
		(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
					       "No unread notifications.");
		return 0;
	}
	rc = (nb_buf_init(&md), nb_buf_reserve(&md, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = cli_command_output_markdown_table_header(&md, &table);
	for (int i = 0; rc == 0 && i < count; i++) {
		char id[32];
		char task_id[32];
		char created[32];
		const char *cells[] = {
			id,
			task_id,
			notifications[i].level,
			created,
			notifications[i].delivery_status,
			notifications[i].title,
			notifications[i].body,
		};

		snprintf(id, sizeof(id), "%lld", (long long)notifications[i].id);
		snprintf(task_id, sizeof(task_id), "%lld",
			 (long long)notifications[i].task_id);
		cli_tasks_notification_time_string(notifications[i].created_at, created,
						   sizeof(created));
		rc = cli_command_output_markdown_table_row(&md, &table, cells);
	}
	if (rc == 0)
		rc = cli_command_output_markdown_render(ctx, nb_buf_cstr(&md));
	return rc;
}
static int cli_tasks_command_inbox_list(struct cli_application *ctx, int argc, char **argv)
{
	struct notification *notifications = NULL;
	int count = 0;
	int limit = 20;
	int rc;

	if (argc > 2)
		limit = atoi(argv[2]);
	rc = runtime_notification_list(ctx->runtime, limit, &notifications, &count);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to list inbox: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	rc = cli_tasks_render_inbox(ctx, notifications, count);
	notification_free_list(notifications, count);
	return rc;
}

static int cli_tasks_command_inbox_read(struct cli_application *ctx, int argc, char **argv)
{
	char *end = NULL;
	long long id;
	int rc;

	if (argc < 3) {
		CMD_ERROR(ctx, "usage: /inbox read <id>");
		return NB_ERR_INVAL;
	}
	id = strtoll(argv[2], &end, 10);
	if (!end || *end != '\0' || id <= 0) {
		CMD_ERROR(ctx, "invalid notification id: %s", argv[2]);
		return NB_ERR_INVAL;
	}
	rc = runtime_notification_mark_read(ctx->runtime, (int64_t)id);
	if (rc != 0) {
		CMD_ERROR(ctx, "failed to mark notification read: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	CMD_OK(ctx, "notification marked read: #%lld", id);
	return 0;
}

static int cli_tasks_command_inbox(struct cli_application *ctx, int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "list";

	if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0)
		return cli_tasks_command_inbox_list(ctx, argc, argv);
	if (strcmp(sub, "read") == 0)
		return cli_tasks_command_inbox_read(ctx, argc, argv);
	CMD_ERROR(ctx, "usage: /inbox [list [limit]|read <id>]");
	return NB_ERR_INVAL;
}

static const struct cli_command cli_tasks_entries[] = {
	{"/tasks", cli_tasks_command_tasks, "Manage scheduled tasks",
	 "/tasks [list|show|add|every|update|cancel|run]"},
	{"/todo", cli_tasks_command_tasks, "Alias for /tasks",
	 "/todo [list|show|add|every|update|cancel|run]"},
	{"/inbox", cli_tasks_command_inbox, "Show task notifications", "/inbox [list|read]"},
};

struct cli_command_group cli_task_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_tasks_entries,
		.count = (int)(sizeof(cli_tasks_entries) / sizeof(cli_tasks_entries[0])),
		.category = "Automation",
	};
}
