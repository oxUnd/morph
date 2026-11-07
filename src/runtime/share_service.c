#include "runtime/runtime.h"
#include "runtime/runtime_internal.h"
#include "persistence/history_store.h"
#include "util/arena.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *share_text(const char *text)
{
	return text ? text : "";
}

static struct json *share_document(const char *answer)
{
	struct json *document = json_object();
	if (!document || !json_add_array(document, "questions") ||
	    !json_add_array(document, "steps") || !json_add_string(document, "answer", answer)) {
		json_free(document);
		return NULL;
	}
	return document;
}

static int share_question(struct json *document, const char *text)
{
	struct json *question = json_string(text);
	if (!question || json_array_add(json_member(document, "questions"), question) != 0) {
		json_free(question);
		return -ENOMEM;
	}
	return 0;
}

static int share_step(struct json *document, const struct model_history_item *row)
{
	const char *metadata = share_text(row->payload_json);
	struct json *payload = metadata[0] ? json_parse_prefix(metadata, NULL, NULL) : NULL;
	const char *status;
	struct json *step;
	int ok;

	if (metadata[0] && !json_is_object_value(payload)) {
		json_free(payload);
		return MORPH_ERR_PARSE;
	}
	status = json_str_or(payload, "status", "");
	step = json_object();
	ok = step && json_add_string(step, "name", row->tool_name) &&
	     json_add_string(step, "status", status[0] ? status : "recorded") &&
	     json_add_string(step, "content", share_text(row->content)) &&
	     json_add_bool(step, "truncated", row->truncated);
	json_free(payload);
	if (!ok || json_array_add(json_member(document, "steps"), step) != 0) {
		json_free(step);
		return -ENOMEM;
	}
	return 0;
}

static int share_turn_question(const struct model_history_item *item, void *data)
{
	int *found = data;
	const char *metadata = share_text(item->payload_json);
	struct json *payload;
	const struct json *outcome;
	const char *status;
	int failed;

	if (strcmp(item->kind, "user_message") != 0)
		return 0;
	payload = metadata[0] ? json_parse_prefix(metadata, NULL, NULL) : NULL;
	if (metadata[0] && !json_is_object_value(payload)) {
		json_free(payload);
		return MORPH_ERR_PARSE;
	}
	outcome = json_member(payload, "turn_outcome");
	status = json_str_or(outcome, "status", "");
	failed = status[0] && strcmp(status, "success") != 0 && strcmp(status, "answered") != 0;
	json_free(payload);
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
	struct json **out;
};

static int share_answer(const struct model_history_item *item, void *data)
{
	static const char legacy_prefix[] = "legacy:message:";
	struct share_context *context = data;
	const char *turn = share_text(item->turn_id);
	const char *key = share_text(item->idempotency_key);
	int migrated = !turn[0] && !strncmp(key, legacy_prefix, sizeof(legacy_prefix) - 1);
	int complete = migrated;
	struct json *document;
	int rc = 0;

	if (turn[0])
		rc = history_store_visit_turn(context->db, context->session_id, turn,
					      item->sequence_no, share_turn_question, &complete);
	if (rc != 0 || !complete)
		return rc;
	document = share_document(share_text(item->content));
	if (!document)
		return -ENOMEM;
	if (migrated) {
		char *question = NULL;

		rc = history_store_previous_question(context->db, context->session_id,
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
	json_free(document);
	return rc == -ENODATA ? 0 : rc;
}

static int share_history(struct db *db, int64_t session_id, struct json **out)
{
	struct share_context context = {.db = db, .session_id = session_id, .out = out};
	int rc = history_store_visit_answers(db, session_id, share_answer, &context);

	return rc != 0 ? rc : *out ? 0 : -ENODATA;
}

static int share_legacy(struct db *db, int64_t session_id, struct json **out)
{
	struct arena *arena = arena_create(0);
	const char *answer;
	const char *question;
	struct json *document = NULL;
	int rc;

	if (!arena)
		return -ENOMEM;
	rc = history_store_legacy_answer(db, session_id, arena, &answer, &question);
	if (rc == 0) {
		document = share_document(answer);
		rc = document ? share_question(document, question) : -ENOMEM;
	}
	arena_destroy(arena);
	if (rc == 0)
		*out = document;
	else
		json_free(document);
	return rc;
}

int runtime_session_share_current(struct runtime *runtime, struct json **out)
{
	struct runtime_context *context;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!runtime)
		return -EINVAL;
	context = &runtime->context;
	rc = pthread_mutex_trylock(&context->execution_lock);
	if (rc != 0)
		return -rc;
	if (context->current_session.id <= 0) {
		rc = -ENODATA;
	} else {
		rc = share_history(&context->database, context->current_session.id, out);
		if (rc == -ENODATA)
			rc = share_legacy(&context->database, context->current_session.id, out);
	}
	pthread_mutex_unlock(&context->execution_lock);
	return rc;
}
