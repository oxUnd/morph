#ifndef MORPH_FCGI_ADAPTERS_TURN_INTERACTIONS_H
#define MORPH_FCGI_ADAPTERS_TURN_INTERACTIONS_H

#include "agent/react.h"
#include "agent/tool_context.h"

int fcgi_turn_interactions_drain(void *data, struct react_action *out, int timeout_sec);
enum hitl_verdict fcgi_turn_interactions_hitl(const char *tool, const char *args, void *data);
int fcgi_turn_ask_user(const char *question, const char *const *choices, int choices_count,
		       const char *selection_mode, int min_choices, int max_choices,
		       const struct ask_user_options *options, char ***answers, int *answers_count,
		       void *data);
enum tool_operation_verdict fcgi_turn_operation_approval(const struct tool_operation *operation,
							 void *data);

#endif
