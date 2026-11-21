#include "runtime/task_runner.h"

#include "runtime/client.h"
#include "runtime/runtime.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/random.h>
#include <nb/utf8.h>
#include <stdlib.h>
#include <string.h>

static void task_client_cleanup(struct runtime_client **client)
{
	runtime_client_close(*client);
	*client = NULL;
}

static int task_client_call(struct runtime_client *client, const char *id,
	const char *method, const cJSON *params, cJSON **out)
{
	struct runtime_command command = {.id = id, .method = method, .params = params};
	int rc = runtime_client_request_copy(client, &command);

	if (rc != 0)
		return rc;

	for (;;) {
		struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) = NULL;

		rc = runtime_client_receive(client, -1, &message);
		if (rc != 0)
			return rc;

		if (message->kind != RUNTIME_MESSAGE_RESPONSE || !message->id ||
		    strcmp(message->id, id))
			continue;

		if (message->error_code != 0)
			return message->error_code;

		*out = message->data;
		message->data = NULL;
		return 0;
	}
}

static int task_client_wait(struct runtime_client *client, const char *turn_id)
{
	for (;;) {
		struct runtime_message *message NB_CLEANUP(runtime_message_cleanup) = NULL;
		int rc = runtime_client_receive(client, 1000, &message);

		if (rc == NB_ERR_TIMEOUT)
			return NB_ERR_AGAIN;

		if (rc != 0)
			return rc;

		if (message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
		    !strcmp(message->method, "turn.completed") && message->turn_id &&
		    !strcmp(message->turn_id, turn_id))
			return 0;

		if (message->kind == RUNTIME_MESSAGE_EVENT && message->method &&
		    !strcmp(message->method, "runtime.resync_required"))
			return NB_ERR_AGAIN;
	}
}

int runtime_task_run_agent(const struct scheduled_task *task,
	struct scheduled_task_action_result *result, void *user_data)
{
	struct runtime *runtime = user_data;
	struct runtime_client *client NB_CLEANUP(task_client_cleanup) = NULL;
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *created NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *started NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *finished NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *source NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	char *task_prompt NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf prompt NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
	const struct runtime_client_options options = {
		.capabilities = RUNTIME_CLIENT_READ | RUNTIME_CLIENT_EXECUTE |
			RUNTIME_CLIENT_MANAGE,
		.event_capacity = 4096,
	};
	int rc;

	if (!runtime || !task || !result)
		return NB_ERR_INVAL;

	if (!params)
		return NB_ERR_NOMEM;

	task_prompt = runtime_scheduled_task_prompt_copy(task);
	if (!task_prompt)
		return NB_ERR_INVAL;

	nb_buf_init(&prompt);
	nb_buf_init(&name);
	if (!nb_buf_printf(&prompt,
		"Run this scheduled task now.\n\nTask title: %s\n\nTask prompt:\n%s\n\n"
		"Return the result as a concise notification. Do not ask follow-up questions.",
		task->title, task_prompt) ||
	    !nb_buf_printf(&name, "Task #%lld: %.*s / ", (long long)task->id,
		(int)nb_utf8_clamp_bytes(task->title, 160), task->title))
		return NB_ERR_NOMEM;

	rc = nb_random_hex(&name, 6, NULL);
	if (rc != 0)
		return rc;

	if (!cJSON_AddStringToObject(params, "name", nb_buf_cstr(&name)))
		return NB_ERR_NOMEM;

	rc = runtime_client_open(runtime, &options, &client);
	if (rc == 0 && task->source_session_id > 0) {
		cJSON *query NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

		if (!query || !cJSON_AddNumberToObject(query, "session_id",
			(double)task->source_session_id))
			return NB_ERR_NOMEM;

		rc = task_client_call(client, "source", "session.get", query, &source);
		if (rc != 0)
			return rc;

		const char *workdir = cJSON_GetStringValue(cJSON_GetObjectItem(source, "workdir"));
		const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(source, "model"));

		if ((workdir && workdir[0] &&
		     !cJSON_AddStringToObject(params, "workdir", workdir)) ||
		    (model && model[0] && !cJSON_AddStringToObject(params, "model", model)))
			return NB_ERR_NOMEM;
	}

	if (rc == 0)
		rc = task_client_call(client, "create", "session.create", params, &created);

	if (rc != 0)
		return rc;

	int64_t session_id = (int64_t)cJSON_GetNumberValue(
		cJSON_GetObjectItem(created, "session_id"));

	result->session_id = session_id;
	cJSON_Delete(params);
	params = cJSON_CreateObject();
	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)session_id) ||
	    !cJSON_AddStringToObject(params, "input", nb_buf_cstr(&prompt)))
		return NB_ERR_NOMEM;

	rc = task_client_call(client, "start", "turn.start", params, &started);
	if (rc != 0)
		return rc;

	const char *turn_id = cJSON_GetStringValue(cJSON_GetObjectItem(started, "turn_id"));

	if (!turn_id)
		return NB_ERR_PROTOCOL;

	if (!cJSON_AddStringToObject(params, "turn_id", turn_id))
		return NB_ERR_NOMEM;

	const char *status = NULL;

	for (;;) {
		rc = task_client_wait(client, turn_id);
		if (rc != 0 && rc != NB_ERR_AGAIN)
			return rc;

		cJSON_Delete(finished);
		finished = NULL;
		rc = task_client_call(client, "result", "turn.get", params, &finished);
		if (rc != 0)
			return rc;

		status = cJSON_GetStringValue(cJSON_GetObjectItem(finished, "status"));
		if (!status)
			return NB_ERR_PROTOCOL;

		if (!strcmp(status, "completed") || !strcmp(status, "failed") ||
		    !strcmp(status, "interrupted"))
			break;
	}

	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(finished, "final_text"));

	rc = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(finished, "execution_rc"));
	if (rc == 0 && strcmp(status, "completed"))
		rc = NB_ERR_IO;

	if (rc == 0)
		result->body = strdup(text ? text : "");
	else
		result->error = strdup(text && text[0] ? text :
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));

	if ((rc == 0 && !result->body) || (rc != 0 && !result->error))
		return NB_ERR_NOMEM;

	result->completed = rc == 0;
	return rc;
}
