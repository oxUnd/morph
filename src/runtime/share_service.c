#include "runtime/runtime.h"
#include "runtime/runtime_internal.h"
#include "persistence/history_store.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "errors.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *share_text(const char *text)
{
	return text ? text : "";
}

static const char *share_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static cJSON *share_document(const char *answer)
{
	cJSON *document = cJSON_CreateObject();
	if (!document || !cJSON_AddArrayToObject(document, "questions") ||
	    !cJSON_AddArrayToObject(document, "steps") ||
	    !cJSON_AddStringToObject(document, "answer", answer)) {
		cJSON_Delete(document);
		return NULL;
	}
	return document;
}

static int share_question(cJSON *document, const char *text)
{
	cJSON *question = cJSON_CreateString(text);
	if (!question || !cJSON_AddItemToArray(
		cJSON_GetObjectItemCaseSensitive(document, "questions"), question)) {
		cJSON_Delete(question);
		return NB_ERR_NOMEM;
	}
	return 0;
}

static int share_step(cJSON *document, const struct model_history_item *row)
{
	const char *metadata = share_text(row->payload_json);
	cJSON *payload = metadata[0] ? cJSON_Parse(metadata) : NULL;
	const char *status;
	cJSON *step;
	int ok;

	if (metadata[0] && !cJSON_IsObject(payload)) {
		cJSON_Delete(payload);
		return NB_ERR_PARSE;
	}
	status = share_string(payload, "status");
	step = cJSON_CreateObject();
	ok = step && cJSON_AddStringToObject(step, "name", row->tool_name) &&
	     cJSON_AddStringToObject(step, "status", status[0] ? status : "recorded") &&
	     cJSON_AddStringToObject(step, "content", share_text(row->content)) &&
	     cJSON_AddBoolToObject(step, "truncated", row->truncated);
	cJSON_Delete(payload);
	if (!ok || !cJSON_AddItemToArray(
		cJSON_GetObjectItemCaseSensitive(document, "steps"), step)) {
		cJSON_Delete(step);
		return NB_ERR_NOMEM;
	}
	return 0;
}

static int share_turn_question(const struct model_history_item *item, void *data)
{
	int *found = data;
	const char *metadata = share_text(item->payload_json);
	cJSON *payload;
	const cJSON *outcome;
	const char *status;
	int failed;

	if (strcmp(item->kind, "user_message") != 0)
		return 0;
	payload = metadata[0] ? cJSON_Parse(metadata) : NULL;
	if (metadata[0] && !cJSON_IsObject(payload)) {
		cJSON_Delete(payload);
		return NB_ERR_PARSE;
	}
	outcome = cJSON_GetObjectItemCaseSensitive(payload, "turn_outcome");
	status = share_string(outcome, "status");
	failed = status[0] && strcmp(status, "success") != 0 && strcmp(status, "answered") != 0;
	cJSON_Delete(payload);
	*found = !failed;
	return failed ? 1 : 0;
}

static int share_turn_item(const struct model_history_item *item, void *data)
{
	return strcmp(item->kind, "user_message") == 0
		       ? share_question(data, share_text(item->content))
		       : share_step(data, item);
}

struct share_context {
	struct db *db;
	int64_t session_id;
	cJSON **out;
};

static int share_answer(const struct model_history_item *item, void *data)
{
	static const char legacy_prefix[] = "legacy:message:";
	struct share_context *context = data;
	const char *turn = share_text(item->turn_id);
	const char *key = share_text(item->idempotency_key);
	int migrated = !turn[0] && !strncmp(key, legacy_prefix, sizeof(legacy_prefix) - 1);
	int complete = migrated;
	cJSON *document;
	int rc = 0;

	if (turn[0])
		rc = history_store_visit_turn(context->db, context->session_id, turn,
					      item->sequence_no, share_turn_question, &complete);
	if (rc != 0 || !complete)
		return rc;
	document = share_document(share_text(item->content));
	if (!document)
		return NB_ERR_NOMEM;
	if (migrated) {
		char *question = NULL;

		rc = history_store_previous_question_copy(context->db, context->session_id,
							  item->sequence_no, &question);
		if (rc == 0)
			rc = share_question(document, question);
		free(question);
	} else {
		rc = history_store_visit_turn(context->db, context->session_id, turn,
					      item->sequence_no, share_turn_item, document);
	}
	if (rc == 0) {
		*context->out = document;
		return 1;
	}
	cJSON_Delete(document);
	return rc == NB_ERR_NO_DATA ? 0 : rc;
}

static int share_history(struct db *db, int64_t session_id, cJSON **out)
{
	struct share_context context = {.db = db, .session_id = session_id, .out = out};
	int rc = history_store_visit_answers(db, session_id, share_answer, &context);

	return rc != 0 ? rc : *out ? 0 : NB_ERR_NO_DATA;
}

static int share_legacy(struct db *db, int64_t session_id, cJSON **out)
{
	char *answer NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *question NB_CLEANUP(nb_charp_cleanup) = NULL;
	cJSON *document = NULL;
	int rc;

	rc = history_store_legacy_answer_copy(db, session_id, &answer, &question);
	if (rc == 0) {
		document = share_document(answer);
		rc = document ? share_question(document, question) : NB_ERR_NOMEM;
	}
	if (rc == 0)
		*out = document;
	else
		cJSON_Delete(document);
	return rc;
}

int runtime_session_share_current(struct runtime *runtime, cJSON **out)
{
	struct runtime_context *context;
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!runtime)
		return NB_ERR_INVAL;
	context = &runtime->context;
	rc = nb_mutex_trylock(context->execution_lock);
	if (rc != 0)
		return rc;
	if (context->current_session.id <= 0) {
		rc = NB_ERR_NO_DATA;
	} else {
		rc = share_history(&context->database, context->current_session.id, &document);
		if (rc == NB_ERR_NO_DATA)
			rc = share_legacy(&context->database, context->current_session.id,
					  &document);
	}
	nb_mutex_unlock(context->execution_lock);
	if (rc == 0) {
		*out = document;
		document = NULL;
	}
	return rc;
}
