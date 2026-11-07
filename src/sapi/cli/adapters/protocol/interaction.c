#include "sapi/cli/adapters/protocol/interaction.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/adapters/input/reader.h"
#include "util/json.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "util/child_io.h"
#include "util/cancel.h"

#define CLI_INTERACTION_ID_MAX 64
#define CLI_INTERACTION_INPUT_MAX (1024 * 1024)

struct cli_interaction_call {
	struct cli_application *ctx;
	const char *kind;
	const struct json *request_data;
	cli_interaction_validate_fn validate;
	void *validate_user_data;
	struct json **response_data;
};

struct cli_interaction_decision_validation {
	const char *const *allowed;
	int allowed_count;
};

static int cli_interaction_emit(struct cli_application *ctx, const char *name, const char *phase,
				const char *request_id, const char *kind,
				const struct json *request, const char *reason)
{
	struct json *data;
	struct morph_event event;
	int rc;

	if (!ctx || !name || !phase || !request_id || !kind)
		return -EINVAL;
	data = json_object();
	if (!data)
		return -ENOMEM;
	if (!json_add_string(data, "request_id", request_id) ||
	    !json_add_string(data, "kind", kind)) {
		json_free(data);
		return -ENOMEM;
	}
	if (request) {
		struct json *copy = json_clone(request, 1);

		if (!copy) {
			json_free(data);
			return -ENOMEM;
		}
		rc = json_object_add(data, "request", copy);
		if (rc) {
			json_free(copy);
			json_free(data);
			return rc;
		}
	}
	if (reason && !json_add_string(data, "reason", reason)) {
		json_free(data);
		return -ENOMEM;
	}
	event = (struct morph_event){
		.type = MORPH_EVENT_INTERACTION,
		.name = name,
		.phase = phase,
		.message = reason,
		.data = data,
		.turn_id = NULL,
	};
	rc = morph_event_emit(ctx->event_cb, ctx->event_user_data, &event);
	json_free(data);
	return rc;
}

static int cli_interaction_read_line(struct cli_application *ctx, morph_buf_t *line,
				     int64_t *deadline)
{
	char chunk[BUFSIZ];

	if (!line)
		return -EINVAL;
	morph_buf_reset(line);
	for (;;) {
		size_t len;
		int complete;
		int rc;

		if (!cli_input_fgets_until(ctx, chunk, sizeof(chunk), stdin, deadline)) {
			if (feof(stdin))
				return -ECANCELED;
			if (errno == EINTR) {
				clearerr(stdin);
				if (morph_signal_flag_take(&ctx->interrupted) ||
				    morph_signal_flag_get(&ctx->terminate_requested) ||
				    morph_cancel_token_is_cancelled(
					    ctx->input ? ctx->input->input_cancel_token : NULL)) {
					return -ECANCELED;
				}
				continue;
			}
			return errno ? -errno : -EIO;
		}
		len = strlen(chunk);
		complete = len > 0 && chunk[len - 1] == '\n';
		if (complete)
			chunk[--len] = '\0';
		if (len > 0 && chunk[len - 1] == '\r')
			chunk[--len] = '\0';
		if (line->len > CLI_INTERACTION_INPUT_MAX - len)
			return -E2BIG;
		rc = morph_buf_append(line, chunk, len);
		if (rc != 0)
			return rc;
		if (complete)
			return 0;
	}
}

static int cli_interaction_parse_response(const char *json, const char *request_id,
					  struct json **response_data, int *cancelled)
{
	struct json *root;
	struct json *type;
	struct json *id;
	struct json *data;

	if (!json || !request_id || !response_data || !cancelled)
		return -EINVAL;
	*response_data = NULL;
	*cancelled = 0;
	root = json_parse_prefix(json, NULL, NULL);
	if (!root)
		return MORPH_ERR_PARSE;
	type = json_member(root, "type");
	id = json_member(root, "request_id");
	if (!json_is_string_value(type) || !json_string_value(type) || !json_is_string_value(id) ||
	    !json_string_value(id) || strcmp(json_string_value(id), request_id) != 0) {
		json_free(root);
		return MORPH_ERR_PROTOCOL;
	}
	if (strcmp(json_string_value(type), "interaction.cancel") == 0) {
		*cancelled = 1;
		json_free(root);
		return 0;
	}
	if (strcmp(json_string_value(type), "interaction.response") != 0) {
		json_free(root);
		return MORPH_ERR_PROTOCOL;
	}
	data = json_member(root, "data");
	if (!json_is_object_value(data)) {
		json_free(root);
		return MORPH_ERR_PROTOCOL;
	}
	*response_data = json_clone(data, 1);
	json_free(root);
	if (!*response_data)
		return -ENOMEM;
	return 0;
}

static int cli_interaction_request_on_owner(void *opaque)
{
	struct cli_interaction_call *call = opaque;
	struct cli_application *ctx = call->ctx;
	(void)ctx;
	char request_id[CLI_INTERACTION_ID_MAX];
	morph_buf_t line;
	int rc;

	if (!call || !call->ctx || !call->kind || !call->response_data)
		return -EINVAL;
	call->ctx->interaction_sequence++;
	snprintf(request_id, sizeof(request_id), "interaction-%llu",
		 (unsigned long long)call->ctx->interaction_sequence);
	rc = cli_interaction_emit(call->ctx, "interaction.request", "begin", request_id, call->kind,
				  call->request_data, NULL);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&line, BUFSIZ);
	if (rc != 0)
		return rc;
	struct json *policy = json_member(call->request_data, "response_policy");
	struct json *timeout = json_member(call->request_data, "timeout_seconds");
	int64_t deadline = strcmp(call->kind, "ask_user") == 0 && json_is_string_value(policy) &&
					   strcmp(json_string_value(policy), "optional") == 0 &&
					   json_is_number_value(timeout) &&
					   json_int_value(timeout) > 0
				   ? morph_io_now_ms() + (int64_t)json_int_value(timeout) * 1000
				   : 0;
	for (;;) {
		struct json *response = NULL;
		int cancelled = 0;
		int64_t pending_deadline = deadline;

		rc = cli_interaction_read_line(call->ctx, &line, &deadline);
		if (rc != 0) {
			struct json *indices =
				json_member(call->request_data, "recommended_indices");
			const char *reason = rc == -ETIMEDOUT
						     ? (json_count(indices) ? "timeout_recommended"
									    : "timeout_no_input")
						     : morph_strerror(rc);
			(void)cli_interaction_emit(call->ctx,
						   rc == -ETIMEDOUT ? "interaction.resolved"
								    : "interaction.cancelled",
						   rc == -ETIMEDOUT ? "end" : "failed", request_id,
						   call->kind, NULL, reason);
			morph_buf_cleanup(&line);
			return rc;
		}
		/* Hosts can report editing activity before they have a complete answer. */
		struct json *progress = json_parse_prefix(morph_buf_cstr(&line), NULL, NULL);
		struct json *type = json_member(progress, "type");
		struct json *id = json_member(progress, "request_id");
		int editing = json_is_string_value(type) &&
			      strcmp(json_string_value(type), "interaction.progress") == 0 &&
			      json_is_string_value(id) &&
			      strcmp(json_string_value(id), request_id) == 0;
		if (json_is_string_value(id) && strcmp(json_string_value(id), request_id) != 0)
			deadline = pending_deadline;
		json_free(progress);
		if (editing) {
			deadline = 0;
			continue;
		}
		rc = cli_interaction_parse_response(morph_buf_cstr(&line), request_id, &response,
						    &cancelled);
		if (rc != 0) {
			(void)cli_interaction_emit(call->ctx, "interaction.invalid_response",
						   "failed", request_id, call->kind, NULL,
						   morph_strerror(rc));
			continue;
		}
		if (cancelled) {
			(void)cli_interaction_emit(call->ctx, "interaction.cancelled", "failed",
						   request_id, call->kind, NULL,
						   "interaction cancelled");
			morph_buf_cleanup(&line);
			return -ECANCELED;
		}
		if (call->validate) {
			rc = call->validate(response, call->validate_user_data);
			if (rc != 0) {
				json_free(response);
				(void)cli_interaction_emit(
					call->ctx, "interaction.invalid_response", "failed",
					request_id, call->kind, NULL, morph_strerror(rc));
				continue;
			}
		}
		*call->response_data = response;
		(void)cli_interaction_emit(call->ctx, "interaction.resolved", "end", request_id,
					   call->kind, NULL, NULL);
		morph_buf_cleanup(&line);
		return 0;
	}
}

int cli_interaction_is_frame(const char *json)
{
	struct json *root = json_parse_prefix(json, NULL, NULL);
	struct json *type = json_member(root, "type");
	struct json *id = json_member(root, "request_id");
	int frame = json_is_string_value(type) && json_is_string_value(id) &&
		    (strcmp(json_string_value(type), "interaction.response") == 0 ||
		     strcmp(json_string_value(type), "interaction.cancel") == 0 ||
		     strcmp(json_string_value(type), "interaction.progress") == 0);
	json_free(root);
	return frame;
}

int cli_interaction_request(struct cli_application *ctx, const char *kind,
			    const struct json *request_data, cli_interaction_validate_fn validate,
			    void *validate_user_data, struct json **response_data)
{
	struct cli_interaction_call call;

	if (!ctx || !kind || !response_data)
		return -EINVAL;
	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON)
		return -ENOTSUP;
	*response_data = NULL;
	call = (struct cli_interaction_call){
		.ctx = ctx,
		.kind = kind,
		.request_data = request_data,
		.validate = validate,
		.validate_user_data = validate_user_data,
		.response_data = response_data,
	};
	return cli_event_queue_call_owner(ctx, cli_interaction_request_on_owner, &call);
}

static int cli_interaction_validate_decision(const struct json *response_data, void *user_data)
{
	struct cli_interaction_decision_validation *validation = user_data;
	struct json *decision;

	if (!validation || !json_is_object_value(response_data))
		return MORPH_ERR_PROTOCOL;
	decision = json_member(response_data, "decision");
	if (!json_is_string_value(decision) || !json_string_value(decision))
		return MORPH_ERR_PROTOCOL;
	for (int i = 0; i < validation->allowed_count; i++) {
		if (strcmp(json_string_value(decision), validation->allowed[i]) == 0)
			return 0;
	}
	return -EINVAL;
}

int cli_interaction_decision(struct cli_application *ctx, const char *kind,
			     struct json *request_data, const char *const *allowed,
			     int allowed_count, char *decision, size_t decision_size)
{
	struct cli_interaction_decision_validation validation;
	struct json *decisions;
	struct json *response = NULL;
	const char *value;
	int rc;

	if (!ctx || !kind || !request_data || !allowed || allowed_count <= 0 || !decision ||
	    decision_size == 0)
		return -EINVAL;
	decisions = json_array();
	if (!decisions)
		return -ENOMEM;
	for (int i = 0; i < allowed_count; i++) {
		struct json *item = json_string(allowed[i]);

		if (!item) {
			json_free(decisions);
			return -ENOMEM;
		}
		rc = json_array_add(decisions, item);
		if (rc) {
			json_free(item);
			json_free(decisions);
			return rc;
		}
	}
	rc = json_object_add(request_data, "allowed_decisions", decisions);
	if (rc) {
		json_free(decisions);
		return rc;
	}
	validation = (struct cli_interaction_decision_validation){
		.allowed = allowed,
		.allowed_count = allowed_count,
	};
	rc = cli_interaction_request(ctx, kind, request_data, cli_interaction_validate_decision,
				     &validation, &response);
	if (rc != 0)
		return rc;
	value = json_string_value(json_member(response, "decision"));
	if (!value || strlen(value) >= decision_size) {
		json_free(response);
		return MORPH_ERR_PROTOCOL;
	}
	memcpy(decision, value, strlen(value) + 1);
	json_free(response);
	return 0;
}

int cli_interaction_confirm(struct cli_application *ctx, const char *kind,
			    struct json *request_data, int *confirmed)
{
	static const char *const allowed[] = {"confirm", "cancel"};
	char decision[16] = {0};
	int rc;

	if (!confirmed)
		return -EINVAL;
	*confirmed = 0;
	rc = cli_interaction_decision(ctx, kind, request_data, allowed,
				      (int)(sizeof(allowed) / sizeof(allowed[0])), decision,
				      sizeof(decision));
	if (rc != 0)
		return rc;
	*confirmed = strcmp(decision, "confirm") == 0;
	return 0;
}
