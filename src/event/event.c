#include "event/event.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "cJSON.h"

#include <errno.h>
#include <stdlib.h>

const char *morph_event_type_name(enum morph_event_type type)
{
	switch (type) {
	case MORPH_EVENT_STARTUP:
		return "startup";
	case MORPH_EVENT_REACT:
		return "react";
	case MORPH_EVENT_TOOL:
		return "tool";
	case MORPH_EVENT_MCP:
		return "mcp";
	case MORPH_EVENT_HITL:
		return "hitl";
	case MORPH_EVENT_ARTIFACT:
		return "artifact";
	case MORPH_EVENT_BACKGROUND:
		return "background";
	case MORPH_EVENT_TASK:
		return "task";
	case MORPH_EVENT_COMMAND:
		return "command";
	case MORPH_EVENT_INTERACTION:
		return "interaction";
	case MORPH_EVENT_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

int morph_event_json_create(const struct morph_event *ev, cJSON **out)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	cJSON *data_copy NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!ev || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;

	root = cJSON_CreateObject();
	if (!root)
		return NB_ERR_NOMEM;

	if (!cJSON_AddStringToObject(root, "type",
				     morph_event_type_name(ev->type)) ||
	    !cJSON_AddStringToObject(root, "name",
				     ev->name ? ev->name : "")) {
		return NB_ERR_NOMEM;
	}
	if (ev->phase &&
	    !cJSON_AddStringToObject(root, "phase", ev->phase)) {
		return NB_ERR_NOMEM;
	}
	if (ev->message &&
	    !cJSON_AddStringToObject(root, "message", ev->message)) {
		return NB_ERR_NOMEM;
	}
	if (ev->turn_id &&
	    !cJSON_AddStringToObject(root, "turn_id", ev->turn_id)) {
		return NB_ERR_NOMEM;
	}
	if (ev->data) {
		data_copy = cJSON_Duplicate(ev->data, 1);
		if (!data_copy)
			return NB_ERR_NOMEM;
	} else {
		data_copy = cJSON_CreateObject();
		if (!data_copy)
			return NB_ERR_NOMEM;
	}
	int rc = (cJSON_AddItemToObject(root, "data", data_copy) ? (data_copy = NULL, NB_OK)
								 : NB_ERR_NOMEM);
	if (rc < 0)
		return rc;

	*out = root;
	root = NULL;
	return 0;
}

char *morph_event_json_text_copy(const struct morph_event *ev)
{
	cJSON *root = NULL;
	char *json;

	if (morph_event_json_create(ev, &root) < 0)
		return NULL;
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

int morph_event_emit(morph_event_cb cb, void *user_data,
		     const struct morph_event *ev)
{
	if (!ev)
		return NB_ERR_INVAL;
	if (!cb)
		return 0;
	return cb(ev, user_data);
}

int morph_event_emit_simple(morph_event_cb cb, void *user_data,
			    enum morph_event_type type, const char *name,
			    const char *phase, const char *message,
			    cJSON *data)
{
	struct morph_event ev;

	ev.type = type;
	ev.name = name;
	ev.phase = phase;
	ev.message = message;
	ev.data = data;
	ev.turn_id = NULL;
	return morph_event_emit(cb, user_data, &ev);
}

int morph_event_recorder_init(struct morph_event_recorder *rec)
{
	if (!rec)
		return NB_ERR_INVAL;
	return (nb_vec_init(&rec->records, sizeof(struct morph_event_record)),
		nb_vec_reserve(&rec->records, 16) ? 0 : NB_ERR_NOMEM);
}

void morph_event_recorder_cleanup(struct morph_event_recorder *rec)
{
	struct morph_event_record *record;

	if (!rec)
		return;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&rec->records) &&
			     (record = (struct morph_event_record *)nb_vec_at(&rec->records, _i_));
	     _i_++) {
		cJSON_free(record->json);
		record->json = NULL;
	}
	nb_vec_cleanup(&rec->records);
}

int morph_event_recorder_cb(const struct morph_event *ev, void *user_data)
{
	struct morph_event_recorder *rec = user_data;
	struct morph_event_record *record;
	char *json;

	if (!rec || !ev)
		return NB_ERR_INVAL;
	json = morph_event_json_text_copy(ev);
	if (!json)
		return NB_ERR_NOMEM;
	record = nb_vec_push(&rec->records);
	if (!record) {
		cJSON_free(json);
		return NB_ERR_NOMEM;
	}
	record->json = json;
	return 0;
}

size_t morph_event_recorder_count(const struct morph_event_recorder *rec)
{
	if (!rec)
		return 0;
	return rec->records.len;
}

const char *morph_event_recorder_borrow(const struct morph_event_recorder *rec, size_t index)
{
	struct morph_event_record *record;

	if (!rec)
		return NULL;
	record = nb_vec_at(&rec->records, index);
	if (!record)
		return NULL;
	return record->json;
}
