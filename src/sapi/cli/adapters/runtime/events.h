#ifndef MORPH_CLI_ADAPTERS_RUNTIME_EVENTS_H
#define MORPH_CLI_ADAPTERS_RUNTIME_EVENTS_H

#include "event/event.h"

struct cli_application;

int cli_runtime_events_callback(const struct morph_event *ev, void *user_data);

#endif
