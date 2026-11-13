#ifndef MORPH_CLI_ADAPTERS_RUNTIME_EVENTS_H
#define MORPH_CLI_ADAPTERS_RUNTIME_EVENTS_H

#include "event/event.h"

struct cli_application;

int cli_runtime_events_callback(const struct morph_event *ev, void *user_data);
/* Owner-thread delivery shared by immediate callbacks and queued events. */
int cli_runtime_events_deliver(struct cli_application *ctx, const struct morph_event *ev);

#endif
