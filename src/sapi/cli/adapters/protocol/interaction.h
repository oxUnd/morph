#ifndef MORPH_CLI_ADAPTERS_PROTOCOL_INTERACTION_H
#define MORPH_CLI_ADAPTERS_PROTOCOL_INTERACTION_H

#include <stddef.h>
#include "util/json.h"

struct cli_application;
/* Reserved protocol frames arriving after a request closes are never chat input. */
int cli_interaction_is_frame(const char *json);

typedef int (*cli_interaction_validate_fn)(const struct json *response_data, void *user_data);

/*
 * Submit one machine-readable interaction request and wait for the matching
 * response.  request_data is borrowed.  On success, response_data receives an
 * owned JSON object that the caller must release with json_free().
 */
int cli_interaction_request(struct cli_application *ctx, const char *kind,
			    const struct json *request_data, cli_interaction_validate_fn validate,
			    void *validate_user_data, struct json **response_data);

/* Add allowed_decisions to request_data and return one validated decision. */
int cli_interaction_decision(struct cli_application *ctx, const char *kind,
			     struct json *request_data, const char *const *allowed,
			     int allowed_count, char *decision, size_t decision_size);

int cli_interaction_confirm(struct cli_application *ctx, const char *kind,
			    struct json *request_data, int *confirmed);

#endif
