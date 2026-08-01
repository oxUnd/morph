#ifndef MORPH_RUNTIME_OUTPUT_H
#define MORPH_RUNTIME_OUTPUT_H

#include "agent/react.h"
#include "config/config.h"
#include "db/database.h"
#include "event/event.h"

#include <stdint.h>

struct runtime_output_context {
	struct db *db;
	const struct config *config;
	int64_t session_id;
	const char *request_prompt;
	const char *turn_id;
	morph_event_cb event_cb;
	void *event_user_data;
};

int runtime_output_record_event(const struct runtime_output_context *context,
				const struct react_output_event *event);
char *runtime_output_get_json_by_path(struct db *db, const char *path);
char *runtime_output_get_json_from_database(const char *database_path,
					    const char *artifact_path);

#endif
