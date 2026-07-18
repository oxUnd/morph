#include "runtime/scheduler.h"

#include "cJSON.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *runtime_scheduled_task_prompt(const struct scheduled_task *task)
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

char *runtime_scheduled_task_turn_id(const struct scheduled_task *task)
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

char *runtime_scheduled_task_display_prompt(const struct scheduled_task *task,
					    const char *task_prompt)
{
	const char *label = "Scheduled task";
	const char *user_req;
	const char *line_end;
	size_t req_len;
	morph_buf_t buf;
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
		if (req_len > 0 && morph_buf_init(&buf, req_len + 96) == 0) {
			(void)morph_buf_printf(&buf, "%s: %.*s",
					       task->title[0] ? task->title :
					       label, (int)req_len, user_req);
			out = morph_buf_detach(&buf);
			morph_buf_cleanup(&buf);
			return out;
		}
	}
	if (morph_buf_init(&buf, 128) != 0)
		return NULL;
	(void)morph_buf_printf(&buf, "%s: %s", label,
			       task->title[0] ? task->title : "Untitled");
	out = morph_buf_detach(&buf);
	morph_buf_cleanup(&buf);
	return out;
}

char *runtime_react_error_message(struct react_context *react, int rc)
{
	morph_buf_t buf;
	const char *answer;
	const char *reason;
	const char *error;
	const char *outcome;
	char *out = NULL;

	if (morph_buf_init(&buf, 256) != 0)
		return NULL;
	answer = react && react->final_answer ? react->final_answer : "";
	reason = react && react->outcome_reason[0] ?
		react->outcome_reason : NULL;
	error = morph_strerror(rc);
	outcome = react ? react_outcome_name(react->outcome) : "unknown";
	if (answer[0]) {
		if (reason)
			(void)morph_buf_printf(&buf, "%s\n\n%s (%s)",
					       answer, reason, outcome);
		else
			(void)morph_buf_puts(&buf, answer);
	} else if (reason) {
		(void)morph_buf_printf(&buf, "%s (%s): %s", reason,
				       outcome, error);
	} else {
		(void)morph_buf_printf(&buf, "%s: %s", outcome, error);
	}
	out = morph_buf_detach(&buf);
	morph_buf_cleanup(&buf);
	return out;
}

static int runtime_artifact_path_seen(morph_array_t *paths, const char *path)
{
	if (!paths || !path)
		return 0;
	for (size_t i = 0; i < paths->nelts; i++) {
		const char **seen = morph_array_get(paths, i);
		if (seen && *seen && strcmp(*seen, path) == 0)
			return 1;
	}
	return 0;
}

static int runtime_append_react_artifact_markdown(morph_buf_t *body,
						  struct react_context *react,
						  const char *answer)
{
	morph_array_t paths;
	struct react_step *step;
	int paths_ready = 0;
	int appended = 0;
	int rc;

	rc = morph_array_init(&paths, 4, sizeof(const char *));
	if (rc != 0)
		return rc;
	paths_ready = 1;

	for (step = react ? react->steps : NULL; step; step = step->next) {
		for (int i = 0; i < step->artifacts.count; i++) {
			const struct tool_artifact *artifact =
				&step->artifacts.items[i];
			const char **slot;
			const char *label;

			if (artifact->kind != TOOL_ARTIFACT_IMAGE ||
			    artifact->path[0] == '\0')
				continue;
			if (answer && strstr(answer, artifact->path))
				continue;
			if (runtime_artifact_path_seen(&paths, artifact->path))
				continue;
			slot = morph_array_push(&paths);
			if (!slot) {
				rc = -ENOMEM;
				goto out;
			}
			*slot = artifact->path;
			if (!appended) {
				if (body->len > 0) {
					rc = morph_buf_puts(body, "\n\n");
					if (rc != 0)
						goto out;
				}
				appended = 1;
			} else {
				rc = morph_buf_putc(body, '\n');
				if (rc != 0)
					goto out;
			}
			label = artifact->label[0] ?
				artifact->label : "Generated image";
			rc = morph_buf_printf(body, "![%s](file://%s)",
					      label, artifact->path);
			if (rc != 0)
				goto out;
		}
	}

out:
	if (paths_ready)
		morph_array_cleanup(&paths);
	return rc;
}

char *runtime_react_notification_body(struct react_context *react)
{
	morph_buf_t body;
	const char *answer;
	char *out = NULL;
	int body_ready = 0;
	int rc;

	answer = react && react->final_answer ? react->final_answer : "";
	rc = morph_buf_init(&body, 1024);
	if (rc != 0)
		return NULL;
	body_ready = 1;
	rc = morph_buf_puts(&body, answer);
	if (rc != 0)
		goto out;
	rc = runtime_append_react_artifact_markdown(&body, react, answer);
	if (rc != 0)
		goto out;
	out = morph_buf_detach(&body);
	body_ready = 0;

out:
	if (body_ready)
		morph_buf_cleanup(&body);
	return out;
}
