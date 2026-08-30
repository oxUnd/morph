#include "sapi/cli/internal.h"
#include "sapi/cli/interaction.h"

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
		MORPH_RETURN(-EINVAL);
	data = cJSON_CreateObject();
	if (!data)
		MORPH_RETURN(-ENOMEM);
	if (!cJSON_AddStringToObject(data, "request_id", request_id) ||
	    !cJSON_AddStringToObject(data, "kind", kind)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
	}
	if (request) {
		cJSON *copy = cJSON_Duplicate(request, 1);

		if (!copy) {
			cJSON_Delete(data);
			MORPH_RETURN(-ENOMEM);
		}
		cJSON_AddItemToObject(data, "request", copy);
	}
	if (reason && !cJSON_AddStringToObject(data, "reason", reason)) {
		cJSON_Delete(data);
		MORPH_RETURN(-ENOMEM);
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

static int interaction_read_line(morph_buf_t *line)
{
	char chunk[BUFSIZ];

	if (!line)
		MORPH_RETURN(-EINVAL);
	morph_buf_reset(line);
	for (;;) {
		size_t len;
		int complete;
		int rc;

		if (!fgets(chunk, sizeof(chunk), stdin)) {
			if (feof(stdin))
				MORPH_RETURN(-ECANCELED);
			if (errno == EINTR) {
				clearerr(stdin);
				if (cli_sigint_received) {
					cli_sigint_received = 0;
					MORPH_RETURN(-ECANCELED);
				}
				continue;
			}
			MORPH_RETURN(errno ? -errno : -EIO);
		}
		len = strlen(chunk);
		complete = len > 0 && chunk[len - 1] == '\n';
		if (complete)
			chunk[--len] = '\0';
		if (len > 0 && chunk[len - 1] == '\r')
			chunk[--len] = '\0';
		if (line->len > CLI_INTERACTION_INPUT_MAX - len)
			MORPH_RETURN(-E2BIG);
		rc = morph_buf_append(line, chunk, len);
		if (rc != 0)
			MORPH_RETURN(rc);
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
		MORPH_RETURN(-EINVAL);
	*response_data = NULL;
	*cancelled = 0;
	root = cJSON_Parse(json);
	if (!root)
		MORPH_RETURN(MORPH_ERR_PARSE);
	type = cJSON_GetObjectItemCaseSensitive(root, "type");
	id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
	if (!cJSON_IsString(type) || !type->valuestring ||
	    !cJSON_IsString(id) || !id->valuestring ||
	    strcmp(id->valuestring, request_id) != 0) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	if (strcmp(type->valuestring, "interaction.cancel") == 0) {
		*cancelled = 1;
		cJSON_Delete(root);
		return 0;
	}
	if (strcmp(type->valuestring, "interaction.response") != 0) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!cJSON_IsObject(data)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	*response_data = cJSON_Duplicate(data, 1);
	cJSON_Delete(root);
	if (!*response_data)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int interaction_request_on_owner(void *opaque)
{
	struct cli_interaction_call *call = opaque;
	char request_id[CLI_INTERACTION_ID_MAX];
	morph_buf_t line;
	int rc;

	if (!call || !call->ctx || !call->kind || !call->response_data)
		MORPH_RETURN(-EINVAL);
	call->ctx->interaction_sequence++;
	snprintf(request_id, sizeof(request_id), "interaction-%llu",
		 (unsigned long long)call->ctx->interaction_sequence);
	rc = interaction_emit(call->ctx, "interaction.request", "begin",
		request_id, call->kind, call->request_data, NULL);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = morph_buf_init(&line, BUFSIZ);
	if (rc != 0)
		MORPH_RETURN(rc);
	for (;;) {
		cJSON *response = NULL;
		int cancelled = 0;

		rc = interaction_read_line(&line);
		if (rc != 0) {
			(void)interaction_emit(call->ctx,
				"interaction.cancelled", "failed", request_id,
				call->kind, NULL, morph_strerror(rc));
			morph_buf_cleanup(&line);
			MORPH_RETURN(rc);
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
			MORPH_RETURN(-ECANCELED);
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

int cli_interaction_request(struct cli_context *ctx, const char *kind,
			    const cJSON *request_data,
			    cli_interaction_validate_fn validate,
			    void *validate_user_data,
			    cJSON **response_data)
{
	struct cli_interaction_call call;

	if (!ctx || !kind || !response_data)
		MORPH_RETURN(-EINVAL);
	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON)
		MORPH_RETURN(-ENOTSUP);
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
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	decision = cJSON_GetObjectItemCaseSensitive(response_data, "decision");
	if (!cJSON_IsString(decision) || !decision->valuestring)
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	for (int i = 0; i < validation->allowed_count; i++) {
		if (strcmp(decision->valuestring, validation->allowed[i]) == 0)
			return 0;
	}
	MORPH_RETURN(-EINVAL);
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
		MORPH_RETURN(-EINVAL);
	decisions = cJSON_CreateArray();
	if (!decisions)
		MORPH_RETURN(-ENOMEM);
	for (int i = 0; i < allowed_count; i++) {
		cJSON *item = cJSON_CreateString(allowed[i]);

		if (!item) {
			cJSON_Delete(decisions);
			MORPH_RETURN(-ENOMEM);
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
		MORPH_RETURN(rc);
	value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
		response, "decision"));
	if (!value || strlen(value) >= decision_size) {
		cJSON_Delete(response);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
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
		MORPH_RETURN(-EINVAL);
	*confirmed = 0;
	rc = cli_interaction_decision(ctx, kind, request_data, allowed,
		(int)(sizeof(allowed) / sizeof(allowed[0])), decision,
		sizeof(decision));
	if (rc != 0)
		MORPH_RETURN(rc);
	*confirmed = strcmp(decision, "confirm") == 0;
	return 0;
}
