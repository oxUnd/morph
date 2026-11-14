#include "runtime/scheduler.h"
#include "nb/cleanup.h"

#include "cJSON.h"
#include "nb/vec.h"
#include "nb/buf.h"
#include "errors.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *runtime_scheduled_task_prompt_copy(const struct scheduled_task *task)
{
	cJSON *payload;
	cJSON *prompt;
	char *out = NULL;

	if (!task || !task->payload_json)
		return NULL;
	payload = cJSON_Parse(task->payload_json);
	if (!payload)
		return NULL;
	prompt = cJSON_GetObjectItem(payload, "prompt");
	if (cJSON_IsString(prompt) && prompt->valuestring)
		out = strdup(prompt->valuestring);
	cJSON_Delete(payload);
	return out;
}

char *runtime_scheduled_task_turn_id_copy(const struct scheduled_task *task)
{
	cJSON *payload;
	cJSON *turn_id;
	char *out = NULL;

	if (!task || !task->payload_json)
		return NULL;
	payload = cJSON_Parse(task->payload_json);
	if (!payload)
		return NULL;
	turn_id = cJSON_GetObjectItem(payload, "turn_id");
	if (cJSON_IsString(turn_id) && turn_id->valuestring &&
	    turn_id->valuestring[0])
		out = strdup(turn_id->valuestring);
	cJSON_Delete(payload);
	return out;
}

char *runtime_scheduled_task_display_prompt_copy(const struct scheduled_task *task,
						 const char *task_prompt)
{
	const char *label = "Scheduled task";
	const char *user_req;
	const char *line_end;
	size_t req_len;
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	char *out;

	if (!task)
		return NULL;
	user_req = task_prompt ? strstr(task_prompt, "User request:") : NULL;
	if (user_req) {
		user_req += strlen("User request:");
		while (*user_req == ' ' || *user_req == '\t')
			user_req++;
		line_end = strpbrk(user_req, "\r\n");
		req_len = line_end ? (size_t)(line_end - user_req) :
			strlen(user_req);
		if (req_len > 0 && (nb_buf_init(&buf),
				    nb_buf_reserve(&buf, req_len + 96) ? 0 : NB_ERR_NOMEM) == 0) {
			(void)nb_buf_printf(&buf, "%s: %.*s", task->title[0] ? task->title : label,
					    (int)req_len, user_req);
			out = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
			nb_buf_cleanup(&buf);
			return out;
		}
	}
	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	(void)nb_buf_printf(&buf, "%s: %s", label, task->title[0] ? task->title : "Untitled");
	out = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
	nb_buf_cleanup(&buf);
	return out;
}

char *runtime_react_error_message_copy(struct react_context *react, int rc)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *answer;
	const char *reason;
	const char *error;
	const char *outcome;
	char *out = NULL;

	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 256) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	answer = react && react->final_answer ? react->final_answer : "";
	reason = react && react->outcome_reason[0] ?
		react->outcome_reason : NULL;
	error = nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
	outcome = react ? react_outcome_name(react->outcome) : "unknown";
	if (answer[0]) {
		if (reason)
			(void)nb_buf_printf(&buf, "%s\n\n%s (%s)", answer, reason, outcome);
		else
			(void)nb_buf_append_cstr(&buf, answer);
	} else if (reason) {
		(void)nb_buf_printf(&buf, "%s (%s): %s", reason, outcome, error);
	} else {
		(void)nb_buf_printf(&buf, "%s: %s", outcome, error);
	}
	out = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
	nb_buf_cleanup(&buf);
	return out;
}

static int runtime_artifact_path_seen(nb_vec *paths, const char *path)
{
	if (!paths || !path)
		return 0;
	for (size_t i = 0; i < paths->len; i++) {
		const char **seen = nb_vec_at(paths, i);
		if (seen && *seen && strcmp(*seen, path) == 0)
			return 1;
	}
	return 0;
}

static int runtime_append_react_artifact_markdown(nb_buf *body, struct react_context *react,
						  const char *answer)
{
	nb_vec paths NB_CLEANUP(nb_vec_cleanup) = {0};
	struct react_step *step;
	int appended = 0;
	int rc;

	rc = (nb_vec_init(&paths, sizeof(const char *)),
	      nb_vec_reserve(&paths, 4) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;

	for (step = react ? react->steps : NULL; step; step = step->next) {
		for (int i = 0; i < step->artifact_count; i++) {
			const struct tool_artifact *artifact =
				&step->artifacts[i];
			const char **slot;
			const char *label;

			if (artifact->kind != TOOL_ARTIFACT_IMAGE ||
			    artifact->path[0] == '\0')
				continue;
			if (answer && strstr(answer, artifact->path))
				continue;
			if (runtime_artifact_path_seen(&paths, artifact->path))
				continue;
			slot = nb_vec_push(&paths);
			if (!slot) {
				rc = NB_ERR_NOMEM;
				return rc;
			}
			*slot = artifact->path;
			if (!appended) {
				if (body->len > 0) {
					rc = (nb_buf_append_cstr(body, "\n\n") ? 0 : NB_ERR_NOMEM);
					if (rc != 0)
						return rc;
				}
				appended = 1;
			} else {
				rc = (nb_buf_append_char(body, '\n') ? 0 : NB_ERR_NOMEM);
				if (rc != 0)
					return rc;
			}
			label = artifact->label[0] ?
				artifact->label : "Generated image";
			rc = (nb_buf_printf(body, "![%s](file://%s)", label, artifact->path)
				      ? 0
				      : NB_ERR_NOMEM);
			if (rc != 0)
				return rc;
		}
	}

	return rc;
}

char *runtime_react_notification_body_copy(struct react_context *react)
{
	nb_buf body NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *answer;
	int rc;

	answer = react && react->final_answer ? react->final_answer : "";
	rc = (nb_buf_init(&body), nb_buf_reserve(&body, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	rc = (nb_buf_append_cstr(&body, answer) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	rc = runtime_append_react_artifact_markdown(&body, react, answer);
	if (rc != 0)
		return NULL;
	return (nb_buf_ok(&body) ? strdup(nb_buf_cstr(&body)) : NULL);
}
