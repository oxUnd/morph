#include "sapi/fastcgi/handlers/handlers.h"
#include "sapi/fastcgi/services/turn_job.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "cJSON.h"
#include "util/error.h"

#include <stdlib.h>

void handle_post_turn(request_t *request)
{
	const char *session_id = path_param_borrow(request, "id");
	char *body = NULL;
	size_t body_length = 0;
	struct fcgi_turn_receipt receipt;
	cJSON *root;
	const char *input = "";
	int rc;

	if (!session_id) {
		reply_400(request, "missing id");
		return;
	}
	if (!store_session_owned_by(request->store, session_id, request->user_id)) {
		reply_403(request);
		return;
	}
	rc = fcgi_read_body(request, &body, &body_length);
	if (rc) {
		reply_body_error(request, rc);
		return;
	}
	root = cJSON_Parse(body ? body : "{}");
	if (cJSON_IsObject(root)) {
		const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(root, "prompt");

		if (cJSON_IsString(prompt) && prompt->valuestring)
			input = prompt->valuestring;
	}
	rc = fcgi_turn_job_submit(request->bridge, request->store, session_id, request->user_id,
				  input, &receipt);
	cJSON_Delete(root);
	free(body);
	if (rc && (receipt.failure == FCGI_TURN_FAILURE_DAILY_QUOTA ||
		   receipt.failure == FCGI_TURN_FAILURE_CONCURRENCY_QUOTA)) {
		reply_json(request, 429,
			   rc == -EDQUOT ? "{\"error\":\"quota_exceeded\",\"resource\":\"turns\"}"
					 : "{\"error\":\"quota_exceeded\",\"resource\":"
					   "\"concurrent_turns\"}");
		return;
	}
	if (rc && receipt.failure == FCGI_TURN_FAILURE_QUEUE_FULL) {
		reply_json(request, 429, "{\"error\":\"queue_full\",\"retryable\":true}");
		return;
	}
	if (rc && receipt.failure == FCGI_TURN_FAILURE_SHUTDOWN) {
		reply_json(request, 503, "{\"error\":\"shutting_down\"}");
		return;
	}
	if (rc) {
		reply_500(request, receipt.failure == FCGI_TURN_FAILURE_THREAD ? "thread spawn"
									       : "turn failed");
		return;
	}
	cJSON *accepted = cJSON_CreateObject();
	char *json = NULL;

	if (accepted) {
		cJSON_AddBoolToObject(accepted, "accepted", 1);
		cJSON_AddStringToObject(accepted, "turn_id", receipt.turn_id);
		cJSON_AddNumberToObject(accepted, "queue_position", receipt.queue_position);
		json = cJSON_PrintUnformatted(accepted);
		cJSON_Delete(accepted);
	}
	reply_202_json(request, json ? json : "{\"accepted\":true}");
	free(json);
}
