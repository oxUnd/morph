#include "sapi/fastcgi/adapters/turn_interactions.h"
#include "sapi/fastcgi/services/turn_job_internal.h"
#include "sapi/fastcgi/storage/events.h"
#include "util/json.h"
#include "util/error.h"
#include "util/child_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sapi/fastcgi/action_pump.h"
#include "util/cancel.h"

#define FCGI_ACTION_WAIT_SECONDS 300

static void turn_action_record_cleanup(struct action_record *record)
{
	if (!record)
		return;
	free(record->payload_json);
	memset(record, 0, sizeof(*record));
}

static int turn_take_pending_action(struct fcgi_turn_job *j, struct action_record *record)
{
	if (!j || !record || !j->pending_type[0])
		return 0;
	memset(record, 0, sizeof(*record));
	snprintf(record->type, sizeof(record->type), "%s", j->pending_type);
	record->payload_json = j->pending_payload;
	j->pending_type[0] = '\0';
	j->pending_payload = NULL;
	return 1;
}

static void turn_save_pending_action(struct fcgi_turn_job *j, struct action_record *record)
{
	if (!j || !record)
		return;
	if (j->pending_type[0]) {
		turn_action_record_cleanup(record);
		return;
	}
	snprintf(j->pending_type, sizeof(j->pending_type), "%s", record->type);
	j->pending_payload = record->payload_json;
	record->payload_json = NULL;
}

int fcgi_turn_interactions_drain(void *user_data, struct react_action *out, int timeout_sec)
{
	struct fcgi_turn_job *j = user_data;
	struct action_record record;
	int got;

	if (!j || !out)
		return -EINVAL;
	free(j->action_payload);
	j->action_payload = NULL;
	j->action_type[0] = '\0';
	memset(&record, 0, sizeof(record));
	got = action_pump_wait(j->store, j->session_id, timeout_sec, &record);
	if (got <= 0)
		return got;
	/* Replies to closed questions must never become model steering messages. */
	struct json *payload = json_parse_prefix(record.payload_json, NULL, NULL);
	struct json *request_id = json_member(payload, "request_id");
	int question_reply =
		strcmp(record.type, "answer") == 0 || strcmp(record.type, "progress") == 0 ||
		(strcmp(record.type, "prompt") == 0 && json_is_string_value(request_id));
	json_free(payload);
	if (question_reply) {
		turn_action_record_cleanup(&record);
		return 0;
	}
	if (strcmp(record.type, "approve") == 0 || strcmp(record.type, "reject") == 0 ||
	    strcmp(record.type, "always") == 0) {
		turn_save_pending_action(j, &record);
		return 0;
	}
	snprintf(j->action_type, sizeof(j->action_type), "%s", record.type);
	j->action_payload = record.payload_json;
	out->type = j->action_type;
	out->payload_json = j->action_payload;
	return 1;
}

static void publish_interaction(struct fcgi_turn_job *j, const char *type, const char *message,
				struct json *data)
{
	struct json *root;
	char *json;

	if (!j || !type) {
		json_free(data);
		return;
	}
	root = json_object();
	if (!root) {
		json_free(data);
		return;
	}
	json_add_string(root, "turn_id", j->turn_id);
	if (message)
		json_add_string(root, "message", message);
	if (data && json_object_add(root, "data", data)) {
		json_free(data);
		json_free(root);
		return;
	}
	json = json_stringify(root);
	events_publish(j->store, j->session_id, type, json ? json : "{}");
	free(json);
	json_free(root);
}

static int action_answers(const char *payload, char ***answers, int *answers_count)
{
	struct json *root;
	struct json *items;
	struct json *text;
	char **values;
	int count;
	int written = 0;

	if (!answers || !answers_count)
		return -EINVAL;
	*answers = NULL;
	*answers_count = 0;
	root = json_parse_prefix(payload ? payload : "{}", NULL, NULL);
	if (!json_is_object_value(root)) {
		json_free(root);
		return -EINVAL;
	}
	items = json_member_ci(root, "answers");
	text = json_member_ci(root, "text");
	count = json_is_array_value(items) ? json_count(items)
					   : (json_is_string_value(text) ? 1 : 0);
	if (count > 64)
		count = 64;
	if (count == 0) {
		json_free(root);
		return 0;
	}
	values = calloc((size_t)count, sizeof(*values));
	if (!values) {
		json_free(root);
		return -ENOMEM;
	}
	for (int i = 0; i < count; i++) {
		struct json *item = json_is_array_value(items) ? json_at(items, i) : text;
		if (!json_is_string_value(item) || !json_string_value(item) ||
		    !json_string_value(item)[0])
			continue;
		values[written] = strdup(json_string_value(item));
		if (!values[written]) {
			for (int k = 0; k < written; k++)
				free(values[k]);
			free(values);
			json_free(root);
			return -ENOMEM;
		}
		written++;
	}
	json_free(root);
	*answers = values;
	*answers_count = written;
	return 0;
}

static void publish_question_end(struct fcgi_turn_job *j, const char *id, const char *resolution)
{
	struct json *data = json_object();
	if (!data)
		return;
	json_add_string(data, "request_id", id);
	json_add_string(data, "resolution", resolution);
	publish_interaction(j, "interaction.resolved", resolution, data);
}

int fcgi_turn_ask_user(const char *question, const char *const *choices, int choices_count,
		       const char *selection_mode, int min_choices, int max_choices,
		       const struct ask_user_options *options, char ***answers, int *answers_count,
		       void *turn_data)
{
	struct fcgi_turn_job *j = turn_data;
	if (!j || !answers || !answers_count)
		return -EINVAL;
	*answers = NULL;
	*answers_count = 0;
	char request_id[128];
	j->ask_sequence++;
	snprintf(request_id, sizeof(request_id), "%s-ask-%llu", j->turn_id, j->ask_sequence);
	struct json *data = json_object();
	struct json *array = json_array();
	if (!data || !array) {
		json_free(array);
		json_free(data);
		return -ENOMEM;
	}
	int rc = json_object_add(data, "choices", array);
	if (rc) {
		json_free(array);
		json_free(data);
		return rc;
	}
	json_add_string(data, "question", question ? question : "");
	json_add_string(data, "request_id", request_id);
	json_add_string(data, "selection_mode", selection_mode);
	json_add_number(data, "min_choices", min_choices);
	json_add_number(data, "max_choices", max_choices);
	rc = ask_user_options_to_json(data, options);
	for (int i = 0; !rc && i < choices_count; i++) {
		struct json *choice = json_string(choices[i]);
		if (!choice)
			rc = -ENOMEM;
		else {
			rc = json_array_add(array, choice);
			if (rc)
				json_free(choice);
		}
	}
	if (rc) {
		json_free(data);
		return rc;
	}
	publish_interaction(j, "ask_user", "user input required", data);
	int64_t deadline = options && options->optional
				   ? morph_io_now_ms() + (int64_t)options->timeout_seconds * 1000
				   : 0;
	for (;;) {
		struct action_record record = {0};
		if (options && morph_cancel_token_is_cancelled(options->cancel_token)) {
			publish_question_end(j, request_id, "cancelled");
			return -ECANCELED;
		}
		/* Recheck a monotonic deadline even after unrelated queue wakeups. */
		int got = action_pump_wait(j->store, j->session_id, 1, &record);
		if (got < 0)
			return got;
		if (!got) {
			if (deadline && morph_io_now_ms() >= deadline) {
				publish_question_end(j, request_id,
						     options->recommended_count
							     ? "timeout_recommended"
							     : "timeout_no_input");
				return -ETIMEDOUT;
			}
			continue;
		}
		struct json *payload = json_parse_prefix(record.payload_json, NULL, NULL);
		struct json *id = json_member(payload, "request_id");
		int matching =
			json_is_string_value(id) && strcmp(json_string_value(id), request_id) == 0;
		int scoped = json_is_string_value(id);
		json_free(payload);
		if (strcmp(record.type, "cancel") == 0 && (!scoped || matching)) {
			turn_action_record_cleanup(&record);
			publish_question_end(j, request_id, "cancelled");
			return -ECANCELED;
		}
		if (strcmp(record.type, "progress") == 0) {
			if (matching)
				deadline = 0;
			turn_action_record_cleanup(&record);
			continue;
		}
		if (strcmp(record.type, "answer") != 0 && strcmp(record.type, "prompt") != 0) {
			turn_save_pending_action(j, &record);
			turn_action_record_cleanup(&record);
			publish_question_end(j, request_id, "interrupted");
			return -EAGAIN;
		}
		/* Legacy required prompts still accept unscoped replies. Optional
		 * prompts require IDs so late answers cannot answer a new question. */
		if (!matching && (scoped || (options && options->optional))) {
			turn_action_record_cleanup(&record);
			if (deadline && morph_io_now_ms() >= deadline) {
				publish_question_end(j, request_id,
						     options->recommended_count
							     ? "timeout_recommended"
							     : "timeout_no_input");
				return -ETIMEDOUT;
			}
			continue;
		}
		deadline = 0;
		rc = action_answers(record.payload_json, answers, answers_count);
		turn_action_record_cleanup(&record);
		if (rc)
			return rc;
		int valid = *answers_count >= min_choices &&
			    (!max_choices || *answers_count <= max_choices);
		for (int i = 0; i < *answers_count; i++) {
			for (int k = 0; k < i; k++)
				valid &= strcmp((*answers)[i], (*answers)[k]) != 0;
		}
		if (valid) {
			publish_question_end(j, request_id, "user");
			return 0;
		}
		for (int i = 0; i < *answers_count; i++)
			free((*answers)[i]);
		free(*answers);
		*answers = NULL;
		*answers_count = 0;
	}
}

static int action_is_always(const char *payload)
{
	struct json *root = json_parse_prefix(payload ? payload : "{}", NULL, NULL);
	struct json *always = json_is_object_value(root) ? json_member_ci(root, "always") : NULL;
	int result = json_is_true(always);

	json_free(root);
	return result;
}

enum hitl_verdict fcgi_turn_interactions_hitl(const char *tool_name, const char *tool_args,
					      void *turn_data)
{
	struct fcgi_turn_job *j = turn_data;
	struct action_record record;
	struct json *data;
	int got;

	if (!j)
		return HITL_DENY;
	data = json_object();
	if (data) {
		json_add_string(data, "tool", tool_name ? tool_name : "");
		json_add_string(data, "args", tool_args ? tool_args : "{}");
		publish_interaction(j, "approval_required", "tool approval required", data);
	}
	memset(&record, 0, sizeof(record));
	got = turn_take_pending_action(j, &record);
	if (!got)
		got = action_pump_wait(j->store, j->session_id, FCGI_ACTION_WAIT_SECONDS, &record);
	if (got <= 0)
		return HITL_DENY;
	if (strcmp(record.type, "always") == 0 ||
	    (strcmp(record.type, "approve") == 0 && action_is_always(record.payload_json))) {
		turn_action_record_cleanup(&record);
		return HITL_ALWAYS;
	}
	got = strcmp(record.type, "approve") == 0;
	turn_action_record_cleanup(&record);
	return got ? HITL_APPROVE : HITL_DENY;
}

enum tool_operation_verdict fcgi_turn_operation_approval(const struct tool_operation *op,
							 void *turn_data)
{
	struct fcgi_turn_job *j = turn_data;
	struct action_record record;
	struct json *data;
	int got;

	if (!j || !op)
		return TOOL_OP_DENY;
	data = json_object();
	if (data) {
		struct json *programs = json_array();

		json_add_string(data, "tool", op->tool_name ? op->tool_name : "");
		json_add_string(data, "action", op->action ? op->action : "");
		json_add_string(data, "target", op->target ? op->target : "");
		json_add_string(data, "scope", op->scope ? op->scope : "");
		json_add_string(data, "reason", op->reason ? op->reason : "");
		if (op->path_access)
			json_add_string(data, "access", tool_path_access_name(op->path_access));
		if (programs) {
			for (int i = 0; i < op->programs_count; i++) {
				struct json *program = json_string(op->programs[i]);

				if (!program)
					break;
				if (json_array_add(programs, program)) {
					json_free(program);
					json_free(programs);
					json_free(data);
					return TOOL_OP_DENY;
				}
			}
			if (json_object_add(data, "programs", programs)) {
				json_free(programs);
				json_free(data);
				return TOOL_OP_DENY;
			}
		}
		publish_interaction(j, "operation_approval_required", "operation approval required",
				    data);
	}
	memset(&record, 0, sizeof(record));
	got = turn_take_pending_action(j, &record);
	if (!got)
		got = action_pump_wait(j->store, j->session_id, FCGI_ACTION_WAIT_SECONDS, &record);
	if (got <= 0)
		return TOOL_OP_DENY;
	if (strcmp(record.type, "always") == 0 ||
	    (strcmp(record.type, "approve") == 0 && action_is_always(record.payload_json))) {
		turn_action_record_cleanup(&record);
		return TOOL_OP_ALWAYS;
	}
	got = strcmp(record.type, "approve") == 0;
	turn_action_record_cleanup(&record);
	return got ? TOOL_OP_ALLOW : TOOL_OP_DENY;
}
