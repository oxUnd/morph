#include "sapi/cli/presentation/notifications.h"

#include "sapi/cli/widgets/notification.h"

void cli_notifications_render(struct cli_application *ctx, const struct cli_event_queue_item *item)
{
	cli_notification_write(ctx, item);
}
