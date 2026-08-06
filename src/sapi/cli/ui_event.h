#ifndef CLI_UI_EVENT_H
#define CLI_UI_EVENT_H

#include "event/event.h"
#include "db/scheduled_task.h"

struct cli_context;
typedef int (*cli_ui_owner_call_fn)(void *data);

int cli_ui_init(struct cli_context *ctx);
void cli_ui_cleanup(struct cli_context *ctx);
int cli_ui_is_owner(const struct cli_context *ctx);
int cli_ui_wake_fd(const struct cli_context *ctx);
int cli_ui_post_morph_event(struct cli_context *ctx,
			    const struct morph_event *event);
int cli_ui_post_notification(struct cli_context *ctx,
			     const struct notification *notification);
int cli_ui_call_owner(struct cli_context *ctx, cli_ui_owner_call_fn fn,
		      void *data);
void cli_ui_notify(struct cli_context *ctx);
int cli_ui_drain(struct cli_context *ctx);

#endif
