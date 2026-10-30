#include "sapi/cli/callbacks.h"
#include "sapi/cli/internal.h"
#include "sapi/cli/interaction.h"
#include "util/child_io.h"
#include "util/cancel.h"

#define CLI_INTERACTION_ID_MAX 64
#define CLI_INTERACTION_INPUT_MAX (1024 * 1024)

struct cli_interaction_call {
	struct cli_context *ctx;
	const char *kind;
	const cJSON *request_data;
	cli_interaction_validate_fn validate;
	void *validate_user_data;
	cJSON **response_data;
};

struct cli_decision_validation {
	const char *const *allowed;
	int allowed_count;
};

static int interaction_emit(struct cli_context *ctx, const char *name,
			    const char *phase, const char *request_id,
			    const char *kind, const cJSON *request,
			    const char *reason)
{
	cJSON *data;
	struct morph_event event;
	int rc;

	if (!ctx || !name || !phase || !request_id || !kind)
		return -EINVAL;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(data, "request_id", request_id) ||
	    !cJSON_AddStringToObject(data, "kind", kind)) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	if (request) {
		cJSON *copy = cJSON_Duplicate(request, 1);

		if (!copy) {
			cJSON_Delete(data);
			return -ENOMEM;
		}
		cJSON_AddItemToObject(data, "request", copy);
	}
	if (reason && !cJSON_AddStringToObject(data, "reason", reason)) {
		cJSON_Delete(data);
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
	cJSON_Delete(data);
	return rc;
}

static int interaction_read_line(struct cli_context *ctx, morph_buf_t *line, int64_t *deadline)
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
				    morph_cancel_token_is_cancelled(ctx->input_cancel_token)) {
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

static int interaction_parse_response(const char *json,
				      const char *request_id,
				      cJSON **response_data,
				      int *cancelled)
{
	cJSON *root;
	cJSON *type;
	cJSON *id;
	cJSON *data;

	if (!json || !request_id || !response_data || !cancelled)
		return -EINVAL;
	*response_data = NULL;
	*cancelled = 0;
	root = cJSON_Parse(json);
	if (!root)
		return MORPH_ERR_PARSE;
	type = cJSON_GetObjectItemCaseSensitive(root, "type");
	id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
	if (!cJSON_IsString(type) || !type->valuestring ||
	    !cJSON_IsString(id) || !id->valuestring ||
	    strcmp(id->valuestring, request_id) != 0) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	if (strcmp(type->valuestring, "interaction.cancel") == 0) {
		*cancelled = 1;
		cJSON_Delete(root);
		return 0;
	}
	if (strcmp(type->valuestring, "interaction.response") != 0) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!cJSON_IsObject(data)) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	*response_data = cJSON_Duplicate(data, 1);
	cJSON_Delete(root);
	if (!*response_data)
		return -ENOMEM;
	return 0;
}

static int interaction_request_on_owner(void *opaque)
{
	struct cli_interaction_call *call = opaque;
	struct cli_context *ctx = call->ctx;
	(void)ctx;
	char request_id[CLI_INTERACTION_ID_MAX];
	morph_buf_t line;
	int rc;

	if (!call || !call->ctx || !call->kind || !call->response_data)
		return -EINVAL;
	call->ctx->interaction_sequence++;
	snprintf(request_id, sizeof(request_id), "interaction-%llu",
		 (unsigned long long)call->ctx->interaction_sequence);
	rc = interaction_emit(call->ctx, "interaction.request", "begin",
		request_id, call->kind, call->request_data, NULL);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&line, BUFSIZ);
	if (rc != 0)
		return rc;
	cJSON *policy = cJSON_GetObjectItemCaseSensitive(call->request_data, "response_policy");
	cJSON *timeout = cJSON_GetObjectItemCaseSensitive(call->request_data, "timeout_seconds");
	int64_t deadline = strcmp(call->kind, "ask_user") == 0 &&
		cJSON_IsString(policy) && strcmp(policy->valuestring, "optional") == 0 &&
		cJSON_IsNumber(timeout) && timeout->valueint > 0 ?
		morph_io_now_ms() + (int64_t)timeout->valueint * 1000 : 0;
	for (;;) {
		cJSON *response = NULL;
		int cancelled = 0;
		int64_t pending_deadline = deadline;

		rc = interaction_read_line(call->ctx, &line, &deadline);
		if (rc != 0) {
			cJSON *indices = cJSON_GetObjectItemCaseSensitive(call->request_data,
				"recommended_indices");
			const char *reason = rc == -ETIMEDOUT ?
				(cJSON_GetArraySize(indices) ? "timeout_recommended" :
				 "timeout_no_input") : morph_strerror(rc);
			(void)interaction_emit(call->ctx,
				rc == -ETIMEDOUT ? "interaction.resolved" : "interaction.cancelled",
				rc == -ETIMEDOUT ? "end" : "failed", request_id,
				call->kind, NULL, reason);
			morph_buf_cleanup(&line);
			return rc;
		}
		/* Hosts can report editing activity before they have a complete answer. */
		cJSON *progress = cJSON_Parse(morph_buf_cstr(&line));
		cJSON *type = cJSON_GetObjectItemCaseSensitive(progress, "type");
		cJSON *id = cJSON_GetObjectItemCaseSensitive(progress, "request_id");
		int editing = cJSON_IsString(type) &&
			strcmp(type->valuestring, "interaction.progress") == 0 &&
			cJSON_IsString(id) && strcmp(id->valuestring, request_id) == 0;
		if (cJSON_IsString(id) && strcmp(id->valuestring, request_id) != 0)
			deadline = pending_deadline;
		cJSON_Delete(progress);
		if (editing) {
			deadline = 0;
			continue;
		}
		rc = interaction_parse_response(morph_buf_cstr(&line),
			request_id, &response, &cancelled);
		if (rc != 0) {
			(void)interaction_emit(call->ctx,
				"interaction.invalid_response", "failed",
				request_id, call->kind, NULL,
				morph_strerror(rc));
			continue;
		}
		if (cancelled) {
			(void)interaction_emit(call->ctx,
				"interaction.cancelled", "failed", request_id,
				call->kind, NULL, "interaction cancelled");
			morph_buf_cleanup(&line);
			return -ECANCELED;
		}
		if (call->validate) {
			rc = call->validate(response, call->validate_user_data);
			if (rc != 0) {
				cJSON_Delete(response);
				(void)interaction_emit(call->ctx,
					"interaction.invalid_response", "failed",
					request_id, call->kind, NULL,
					morph_strerror(rc));
				continue;
			}
		}
		*call->response_data = response;
		(void)interaction_emit(call->ctx, "interaction.resolved",
			"end", request_id, call->kind, NULL, NULL);
		morph_buf_cleanup(&line);
		return 0;
	}
}

int cli_interaction_is_frame(const char *json)
{
	cJSON *root = cJSON_Parse(json);
	cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
	cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
	int frame = cJSON_IsString(type) && cJSON_IsString(id) &&
		(strcmp(type->valuestring, "interaction.response") == 0 ||
		 strcmp(type->valuestring, "interaction.cancel") == 0 ||
		 strcmp(type->valuestring, "interaction.progress") == 0);
	cJSON_Delete(root);
	return frame;
}

int cli_interaction_request(struct cli_context *ctx, const char *kind,
			    const cJSON *request_data,
			    cli_interaction_validate_fn validate,
			    void *validate_user_data,
			    cJSON **response_data)
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
	return cli_ui_call_owner(ctx, interaction_request_on_owner, &call);
}

static int interaction_validate_decision(const cJSON *response_data,
					  void *user_data)
{
	struct cli_decision_validation *validation = user_data;
	cJSON *decision;

	if (!validation || !cJSON_IsObject(response_data))
		return MORPH_ERR_PROTOCOL;
	decision = cJSON_GetObjectItemCaseSensitive(response_data, "decision");
	if (!cJSON_IsString(decision) || !decision->valuestring)
		return MORPH_ERR_PROTOCOL;
	for (int i = 0; i < validation->allowed_count; i++) {
		if (strcmp(decision->valuestring, validation->allowed[i]) == 0)
			return 0;
	}
	return -EINVAL;
}

int cli_interaction_decision(struct cli_context *ctx, const char *kind,
			     cJSON *request_data,
			     const char *const *allowed,
			     int allowed_count, char *decision,
			     size_t decision_size)
{
	struct cli_decision_validation validation;
	cJSON *decisions;
	cJSON *response = NULL;
	const char *value;
	int rc;

	if (!ctx || !kind || !request_data || !allowed || allowed_count <= 0 ||
	    !decision || decision_size == 0)
		return -EINVAL;
	decisions = cJSON_CreateArray();
	if (!decisions)
		return -ENOMEM;
	for (int i = 0; i < allowed_count; i++) {
		cJSON *item = cJSON_CreateString(allowed[i]);

		if (!item) {
			cJSON_Delete(decisions);
			return -ENOMEM;
		}
		cJSON_AddItemToArray(decisions, item);
	}
	cJSON_AddItemToObject(request_data, "allowed_decisions", decisions);
	validation = (struct cli_decision_validation){
		.allowed = allowed,
		.allowed_count = allowed_count,
	};
	rc = cli_interaction_request(ctx, kind, request_data,
		interaction_validate_decision, &validation, &response);
	if (rc != 0)
		return rc;
	value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
		response, "decision"));
	if (!value || strlen(value) >= decision_size) {
		cJSON_Delete(response);
		return MORPH_ERR_PROTOCOL;
	}
	memcpy(decision, value, strlen(value) + 1);
	cJSON_Delete(response);
	return 0;
}

int cli_interaction_confirm(struct cli_context *ctx, const char *kind,
			    cJSON *request_data, int *confirmed)
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
