#ifndef MORPH_CLI_SERVICES_EVENT_QUEUE_ITEM_H
#define MORPH_CLI_SERVICES_EVENT_QUEUE_ITEM_H

#include "event/event.h"
#include "util/queue.h"

enum cli_event_queue_item_kind {
	CLI_EVENT_QUEUE_UI_ITEM_MORPH_EVENT,
	CLI_EVENT_QUEUE_UI_ITEM_NOTIFICATION,
	CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL,
};

struct cli_event_queue_owner_call;

struct cli_event_queue_item {
	morph_queue_t link;
	enum cli_event_queue_item_kind kind;
	struct morph_event event;
	char *name;
	char *phase;
	char *message;
	char *turn_id;
	char *notification_title;
	char *notification_body;
	int64_t notification_id;
	int64_t notification_task_id;
	int64_t notification_created_at;
	int notification_failed;
	int notification_warning;
	struct cli_event_queue_owner_call *owner_call;
};

#endif
