#ifndef MORPH_RUNTIME_RESULT_H
#define MORPH_RUNTIME_RESULT_H

#include "agent/turn.h"

enum runtime_outcome {
	RUNTIME_OUTCOME_COMPLETED,
	RUNTIME_OUTCOME_CANCELLED,
	RUNTIME_OUTCOME_FAILED,
	RUNTIME_OUTCOME_BUSY,
};

struct runtime_result {
	enum runtime_outcome outcome;
	int execution_rc;
	int persistence_rc;
	const char *final_text;
	struct agent_turn_result turn;
};

#endif
