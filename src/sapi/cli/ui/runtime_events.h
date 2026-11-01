#ifndef MORPH_CLI_UI_RUNTIME_EVENTS_H
#define MORPH_CLI_UI_RUNTIME_EVENTS_H

#include "event/event.h"

struct cli_context;

int cli_runtime_events_callback(const struct morph_event *ev, void *user_data);

#endif
