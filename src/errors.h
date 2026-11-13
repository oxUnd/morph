#ifndef MORPH_ERRORS_H
#define MORPH_ERRORS_H

#include "nb/err.h"
#include <errno.h>

/* Application error catalog. Portable mechanism statuses use nb_status;
 * these codes describe morph model, configuration and runtime failures. */
#define MORPH_ERR_NOT_CONFIGURED NB_ERR_USER(1)
#define MORPH_ERR_NOT_INITIALIZED NB_ERR_USER(2)
#define MORPH_ERR_API NB_ERR_USER(3)
#define MORPH_ERR_NETWORK NB_ERR_USER(4)
#define MORPH_ERR_DB NB_ERR_USER(7)
#define MORPH_ERR_PROCESSING NB_ERR_USER(9)
#define MORPH_ERR_SANDBOX NB_ERR_USER(10)
#define MORPH_ERR_LOAD NB_ERR_USER(11)
#define MORPH_ERR_LLM NB_ERR_USER(12)
#define MORPH_ERR_REACT_MAX_ITERATIONS NB_ERR_USER(13)
#define MORPH_ERR_CONFIG NB_ERR_USER(14)
#define MORPH_ERR_CONTEXT_LIMIT NB_ERR_USER(15)
#define MORPH_ERR_TOKEN_BUDGET NB_ERR_USER(16)
#define MORPH_ERR_AGENT_BUDGET NB_ERR_USER(17)
#define MORPH_ERR_REQUEST_BUDGET NB_ERR_USER(18)

#define MORPH_ERROR_COUNT 15u

static inline const nb_err_desc *morph_error_descriptions(void)
{
	static const nb_err_desc descriptions[] = {
		{MORPH_ERR_NOT_CONFIGURED, "MORPH_ERR_NOT_CONFIGURED", "not configured"},
		{MORPH_ERR_NOT_INITIALIZED, "MORPH_ERR_NOT_INITIALIZED", "not initialized"},
		{MORPH_ERR_API, "MORPH_ERR_API", "API error"},
		{MORPH_ERR_NETWORK, "MORPH_ERR_NETWORK", "network error"},
		{MORPH_ERR_DB, "MORPH_ERR_DB", "database error"},
		{MORPH_ERR_PROCESSING, "MORPH_ERR_PROCESSING", "processing error"},
		{MORPH_ERR_SANDBOX, "MORPH_ERR_SANDBOX", "sandbox violation"},
		{MORPH_ERR_LOAD, "MORPH_ERR_LOAD", "load error"},
		{MORPH_ERR_CONTEXT_LIMIT, "MORPH_ERR_CONTEXT_LIMIT",
		 "model context window exceeded"},
		{MORPH_ERR_TOKEN_BUDGET, "MORPH_ERR_TOKEN_BUDGET",
		 "shared agent token budget exhausted"},
		{MORPH_ERR_AGENT_BUDGET, "MORPH_ERR_AGENT_BUDGET",
		 "agent allocation exhausted; remaining budget is reserved for primary "
		 "integration"},
		{MORPH_ERR_REQUEST_BUDGET, "MORPH_ERR_REQUEST_BUDGET",
		 "model request exceeds remaining agent token budget"},
		{MORPH_ERR_LLM, "MORPH_ERR_LLM", "LLM error"},
		{MORPH_ERR_REACT_MAX_ITERATIONS, "MORPH_ERR_REACT_MAX_ITERATIONS",
		 "maximum ReAct iterations reached"},
		{MORPH_ERR_CONFIG, "MORPH_ERR_CONFIG", "invalid configuration"},
	};
	return descriptions;
}

#endif
