#ifndef MORPH_RUNTIME_REQUEST_H
#define MORPH_RUNTIME_REQUEST_H

#include "agent/react.h"
#include "agent/turn.h"
#include "event/event.h"
#include "agent/memory.h"
#include <stdint.h>

struct runtime_request {
	int64_t session_id;
	const char *turn_id;
	const char *model_input;
	const char *stored_user_input;
	const struct memory_options *memory_options;
	agent_turn_render_fn render_assistant;
	void *render_user_data;
	react_output_cb output_cb;
	void *output_user_data;
	morph_event_cb event_cb;
	void *event_user_data;
	void *usage_user_data;
	int bind_usage_user_data;
	hitl_approval_cb hitl_cb;
	void *hitl_user_data;
	ask_user_callback_fn ask_user_fn;
	void *ask_user_user_data;
	int override_hitl;
	int override_ask_user;
	unsigned turn_flags;
};

#endif
