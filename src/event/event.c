#include "event/event.h"

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
	case MORPH_EVENT_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

int morph_event_to_json_object(const struct morph_event *ev, cJSON **out)
{
	cJSON *root;
	cJSON *data_copy;

	if (!ev || !out)
		return -EINVAL;

	root = cJSON_CreateObject();
	if (!root)
		return -ENOMEM;

	if (!cJSON_AddStringToObject(root, "type",
				     morph_event_type_name(ev->type)) ||
	    !cJSON_AddStringToObject(root, "name",
				     ev->name ? ev->name : "")) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	if (ev->phase &&
	    !cJSON_AddStringToObject(root, "phase", ev->phase)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	if (ev->message &&
	    !cJSON_AddStringToObject(root, "message", ev->message)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	if (ev->turn_id &&
	    !cJSON_AddStringToObject(root, "turn_id", ev->turn_id)) {
		cJSON_Delete(root);
		return -ENOMEM;
	}
	if (ev->data) {
		data_copy = cJSON_Duplicate(ev->data, 1);
		if (!data_copy) {
			cJSON_Delete(root);
			return -ENOMEM;
		}
		cJSON_AddItemToObject(root, "data", data_copy);
	} else {
		data_copy = cJSON_CreateObject();
		if (!data_copy) {
			cJSON_Delete(root);
			return -ENOMEM;
		}
		cJSON_AddItemToObject(root, "data", data_copy);
	}

	*out = root;
	return 0;
}

char *morph_event_to_json_string(const struct morph_event *ev)
{
	cJSON *root = NULL;
	char *json;

	if (morph_event_to_json_object(ev, &root) < 0)
		return NULL;
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

int morph_event_emit(morph_event_cb cb, void *user_data,
		     const struct morph_event *ev)
{
	if (!ev)
		return -EINVAL;
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
		return -EINVAL;
	return morph_array_init(&rec->records, MORPH_ARRAY_INIT_CAP,
				sizeof(struct morph_event_record));
}

void morph_event_recorder_cleanup(struct morph_event_recorder *rec)
{
	struct morph_event_record *record;

	if (!rec)
		return;
	morph_array_foreach(record, &rec->records, struct morph_event_record) {
		free(record->json);
		record->json = NULL;
	}
	morph_array_cleanup(&rec->records);
}

int morph_event_recorder_cb(const struct morph_event *ev, void *user_data)
{
	struct morph_event_recorder *rec = user_data;
	struct morph_event_record *record;
	char *json;

	if (!rec || !ev)
		return -EINVAL;
	json = morph_event_to_json_string(ev);
	if (!json)
		return -ENOMEM;
	record = morph_array_push(&rec->records);
	if (!record) {
		free(json);
		return -ENOMEM;
	}
	record->json = json;
	return 0;
}

size_t morph_event_recorder_count(const struct morph_event_recorder *rec)
{
	if (!rec)
		return 0;
	return rec->records.nelts;
}

const char *morph_event_recorder_get(const struct morph_event_recorder *rec,
				     size_t index)
{
	struct morph_event_record *record;

	if (!rec)
		return NULL;
	record = morph_array_get(&rec->records, index);
	if (!record)
		return NULL;
	return record->json;
}
