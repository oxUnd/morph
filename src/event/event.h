#ifndef MORPH_EVENT_H
#define MORPH_EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"
#include "util/array.h"
#include "util/error.h"

enum morph_event_type {
	MORPH_EVENT_STARTUP,
	MORPH_EVENT_REACT,
	MORPH_EVENT_TOOL,
	MORPH_EVENT_MCP,
	MORPH_EVENT_HITL,
	MORPH_EVENT_ARTIFACT,
	MORPH_EVENT_BACKGROUND,
	MORPH_EVENT_TASK,
	MORPH_EVENT_ERROR,
	MORPH_EVENT_COMMAND,
};

struct morph_event {
	enum morph_event_type type;
	const char *name;
	const char *phase;
	const char *message;
	cJSON *data;
	const char *turn_id;
};

typedef int (*morph_event_cb)(const struct morph_event *ev, void *user_data);

struct morph_event_record {
	char *json;
};

struct morph_event_recorder {
	morph_array_t records;
};

const char *morph_event_type_name(enum morph_event_type type);
int morph_event_to_json_object(const struct morph_event *ev, cJSON **out);
char *morph_event_to_json_string(const struct morph_event *ev);
int morph_event_emit(morph_event_cb cb, void *user_data,
		     const struct morph_event *ev);
int morph_event_emit_simple(morph_event_cb cb, void *user_data,
			    enum morph_event_type type, const char *name,
			    const char *phase, const char *message,
			    cJSON *data);

int morph_event_recorder_init(struct morph_event_recorder *rec);
void morph_event_recorder_cleanup(struct morph_event_recorder *rec);
int morph_event_recorder_cb(const struct morph_event *ev, void *user_data);
size_t morph_event_recorder_count(const struct morph_event_recorder *rec);
const char *morph_event_recorder_get(const struct morph_event_recorder *rec,
				     size_t index);

#ifdef __cplusplus
}
#endif

#endif
