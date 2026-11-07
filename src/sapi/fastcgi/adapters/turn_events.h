#ifndef MORPH_FCGI_ADAPTERS_TURN_EVENTS_H
#define MORPH_FCGI_ADAPTERS_TURN_EVENTS_H

struct morph_event;
int fcgi_turn_events_callback(const struct morph_event *event, void *data);

#endif
