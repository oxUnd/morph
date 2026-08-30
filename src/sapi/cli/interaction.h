#ifndef CLI_INTERACTION_H
#define CLI_INTERACTION_H

#include <stddef.h>
#include "cJSON.h"

struct cli_context;

typedef int (*cli_interaction_validate_fn)(const cJSON *response_data,
					   void *user_data);

/*
 * Submit one machine-readable interaction request and wait for the matching
 * response.  request_data is borrowed.  On success, response_data receives an
 * owned cJSON object that the caller must delete.
 */
int cli_interaction_request(struct cli_context *ctx, const char *kind,
			    const cJSON *request_data,
			    cli_interaction_validate_fn validate,
			    void *validate_user_data,
			    cJSON **response_data);

/* Add allowed_decisions to request_data and return one validated decision. */
int cli_interaction_decision(struct cli_context *ctx, const char *kind,
			     cJSON *request_data,
			     const char *const *allowed,
			     int allowed_count, char *decision,
			     size_t decision_size);

int cli_interaction_confirm(struct cli_context *ctx, const char *kind,
			    cJSON *request_data, int *confirmed);

#endif
