#ifndef MORPH_CLI_SERVICES_EVENT_QUEUE_H
#define MORPH_CLI_SERVICES_EVENT_QUEUE_H

#include "event/event.h"
#include "db/scheduled_task.h"

struct cli_application;
struct morph_tui_application;
struct cli_event_queue_item;
struct cli_event_queue_owner_call;
typedef int (*cli_event_queue_dispatch_fn)(struct cli_application *,
					   const struct cli_event_queue_item *);
typedef int (*cli_event_queue_call_fn)(void *data);

int cli_event_queue_init(struct cli_application *ctx, cli_event_queue_dispatch_fn dispatch);
int cli_event_queue_run_owner_call(struct cli_application *ctx,
				   struct cli_event_queue_owner_call *call);
void cli_event_queue_cleanup(struct cli_application *ctx);
int cli_event_queue_is_owner(const struct cli_application *ctx);
int cli_event_queue_wake_fd(const struct cli_application *ctx);
void cli_event_queue_attach_app(struct cli_application *ctx, struct morph_tui_application *app);
/* Owner-only delivery gate. Paused events/notifications remain queued; owner
 * calls continue to run so startup interactions cannot deadlock. */
int cli_event_queue_set_events_enabled(struct cli_application *ctx, int enabled);
int cli_event_queue_events_enabled(const struct cli_application *ctx);
int cli_event_queue_post_morph_event(struct cli_application *ctx, const struct morph_event *event);
int cli_event_queue_post_notification(struct cli_application *ctx,
				      const struct notification *notification);
int cli_event_queue_call_owner(struct cli_application *ctx, cli_event_queue_call_fn fn, void *data);
/* Owner-only shutdown gate: cancel queued/future calls while retaining output. */
int cli_event_queue_stop_owner_calls(struct cli_application *ctx);
void cli_event_queue_notify(struct cli_application *ctx);
int cli_event_queue_drain(struct cli_application *ctx);
int cli_event_queue_drain_batch(struct cli_application *ctx);
int cli_event_queue_owner_call_pending(struct cli_application *ctx);

#endif
