#include "sapi/fastcgi/handlers/handlers.h"
#include "sapi/fastcgi/services/turn_job.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "util/json.h"
#include "util/error.h"

#include <stdlib.h>

void handle_post_turn(request_t *request)
{
	const char *session_id = path_param(request, "id");
	char *body = NULL;
	size_t body_length = 0;
	struct fcgi_turn_receipt receipt;
	struct json *root;
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
	root = json_parse_prefix(body ? body : "{}", NULL, NULL);
	if (json_is_object_value(root)) {
		const struct json *prompt = json_member(root, "prompt");

		if (json_is_string_value(prompt) && json_string_value(prompt))
			input = json_string_value(prompt);
	}
	rc = fcgi_turn_job_submit(request->bridge, request->store, session_id, request->user_id,
				  input, &receipt);
	json_free(root);
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
	struct json *accepted = json_object();
	char *json = NULL;

	if (accepted) {
		json_add_bool(accepted, "accepted", 1);
		json_add_string(accepted, "turn_id", receipt.turn_id);
		json_add_number(accepted, "queue_position", receipt.queue_position);
		json = json_stringify(accepted);
		json_free(accepted);
	}
	reply_202_json(request, json ? json : "{\"accepted\":true}");
	free(json);
}
