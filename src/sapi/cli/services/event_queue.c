#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/event_queue_item.h"
#include "sapi/cli/application.h"
#include "cJSON.h"
#include "errors.h"
#include "nb/cleanup.h"
#include <morph/tui/application.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_EVENT_QUEUE_UI_EVENT_BUDGET 64

struct cli_event_queue_owner_call {
	nb_mutex *mutex;
	nb_cond *cond;
	cli_event_queue_call_fn fn;
	void *data;
	int result;
	int complete;
};

struct cli_event_queue {
	struct morph_tui_application *application;
	cli_event_queue_dispatch_fn dispatch;
	nb_mutex *mutex;
	nb_queue pending;
	nb_thread_id owner;
	int wake_read;
	int wake_write;
	int mutex_initialized;
	int owner_calls_stopped;
	int events_enabled;
};

static void cli_event_queue_item_free(struct cli_event_queue_item *item)
{
	if (!item)
		return;
	free(item->name);
	free(item->phase);
	free(item->message);
	free(item->turn_id);
	cJSON_Delete(item->event.data);
	free(item->notification_title);
	free(item->notification_body);
	free(item);
}

static void cli_event_queue_item_clear(struct cli_event_queue_item **owner)
{
	cli_event_queue_item_free(*owner);
	*owner = NULL;
}

static void cli_event_queue_complete_owner_call(struct cli_event_queue_owner_call *call, int result)
{
	if (!call)
		return;
	nb_mutex_lock(call->mutex);
	call->result = result;
	call->complete = 1;
	nb_cond_signal(call->cond);
	nb_mutex_unlock(call->mutex);
}

static int cli_event_queue_dup_optional(const char *source, char **out)
{
	if (!out)
		return NB_ERR_INVAL;
	*out = NULL;
	if (!source)
		return 0;
	*out = strdup(source);
	if (!*out)
		return NB_ERR_NOMEM;
	return 0;
}

static int cli_event_queue_set_fd_flags(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return nb_status_from_errno(errno, NULL);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return nb_status_from_errno(errno, NULL);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags < 0)
		return nb_status_from_errno(errno, NULL);
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static void cli_event_queue_wake(struct cli_event_queue *ui)
{
	if (!ui || ui->wake_write < 0)
		return;
	nb_mutex_lock(ui->mutex);
	if (ui->application)
		(void)morph_tui_application_wake(ui->application);
	nb_mutex_unlock(ui->mutex);
	unsigned char byte = 1;
	ssize_t count;

	do {
		count = write(ui->wake_write, &byte, sizeof(byte));
	} while (count < 0 && errno == EINTR);
}

void cli_event_queue_notify(struct cli_application *ctx)
{
	if (ctx && ctx->events)
		cli_event_queue_wake(ctx->events);
}

static void cli_event_queue_consume_wake(struct cli_event_queue *ui)
{
	unsigned char bytes[64];
	ssize_t count;

	if (!ui || ui->wake_read < 0)
		return;
	do {
		count = read(ui->wake_read, bytes, sizeof(bytes));
	} while (count > 0 || (count < 0 && errno == EINTR));
}

static int cli_event_queue_enqueue_take(struct cli_application *ctx,
					struct cli_event_queue_item **owner)
{
	struct cli_event_queue *ui;

	if (!ctx || !ctx->events || !owner || !*owner)
		return NB_ERR_INVAL;
	struct cli_event_queue_item *item = *owner;

	ui = ctx->events;
	nb_mutex_lock(ui->mutex);
	if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL && ui->owner_calls_stopped) {
		nb_mutex_unlock(ui->mutex);
		return NB_ERR_CANCELLED;
	}
	nb_queue_insert_tail(&ui->pending, &item->link);
	*owner = NULL;
	nb_mutex_unlock(ui->mutex);
	cli_event_queue_wake(ui);
	return 0;
}

int cli_event_queue_init(struct cli_application *ctx, cli_event_queue_dispatch_fn dispatch)
{
	struct cli_event_queue *ui;
	int wake_fds[2] = {-1, -1};
	int mutex_rc;
	int rc;

	if (!ctx || !dispatch)
		return NB_ERR_INVAL;
	ui = calloc(1, sizeof(*ui));
	if (!ui)
		return NB_ERR_NOMEM;
	ui->wake_read = -1;
	ui->wake_write = -1;
	ui->events_enabled = 1;
	nb_queue_init(&ui->pending);
	mutex_rc = ((ui->mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (mutex_rc != 0) {
		free(ui);
		return mutex_rc;
	}
	ui->mutex_initialized = 1;
	if (pipe(wake_fds) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		(nb_mutex_free(ui->mutex), ui->mutex = NULL);
		free(ui);
		return rc;
	}
	rc = cli_event_queue_set_fd_flags(wake_fds[0]);
	if (rc == 0)
		rc = cli_event_queue_set_fd_flags(wake_fds[1]);
	if (rc != 0) {
		close(wake_fds[0]);
		close(wake_fds[1]);
		(nb_mutex_free(ui->mutex), ui->mutex = NULL);
		free(ui);
		return rc;
	}
	ui->wake_read = wake_fds[0];
	ui->wake_write = wake_fds[1];
	ui->owner = nb_thread_current_id();
	ui->dispatch = dispatch;
	ctx->events = ui;
	return 0;
}

void cli_event_queue_cleanup(struct cli_application *ctx)
{
	struct cli_event_queue *ui;
	nb_queue local;
	nb_queue *link;
	nb_queue *next;

	if (!ctx || !ctx->events)
		return;
	ui = ctx->events;
	nb_queue_init(&local);
	if (ui->mutex_initialized) {
		nb_mutex_lock(ui->mutex);
		nb_queue_add(&local, &ui->pending);
		nb_mutex_unlock(ui->mutex);
	}
	NB_QUEUE_FOREACH_SAFE(link, next, &local)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);

		nb_queue_remove(link);
		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL)
			cli_event_queue_complete_owner_call(item->owner_call, NB_ERR_CANCELLED);
		cli_event_queue_item_free(item);
	}
	if (ui->wake_read >= 0)
		close(ui->wake_read);
	if (ui->wake_write >= 0)
		close(ui->wake_write);
	if (ui->mutex_initialized)
		(nb_mutex_free(ui->mutex), ui->mutex = NULL);
	free(ui);
	ctx->events = NULL;
}

int cli_event_queue_is_owner(const struct cli_application *ctx)
{
	return ctx && ctx->events && nb_thread_id_equal(ctx->events->owner, nb_thread_current_id());
}

int cli_event_queue_wake_fd(const struct cli_application *ctx)
{
	return ctx && ctx->events ? ctx->events->wake_read : -1;
}

void cli_event_queue_attach_app(struct cli_application *ctx, struct morph_tui_application *app)
{
	if (!ctx || !ctx->events)
		return;
	nb_mutex_lock(ctx->events->mutex);
	ctx->events->application = app;
	nb_mutex_unlock(ctx->events->mutex);
	if (app)
		(void)morph_tui_application_wake(app);
}

int cli_event_queue_post_morph_event(struct cli_application *ctx, const struct morph_event *event)
{
	struct cli_event_queue_item *item NB_CLEANUP(cli_event_queue_item_clear) = NULL;
	int rc;

	if (!ctx || !event)
		return NB_ERR_INVAL;
	item = calloc(1, sizeof(*item));
	if (!item)
		return NB_ERR_NOMEM;
	item->kind = CLI_EVENT_QUEUE_UI_ITEM_MORPH_EVENT;
	item->event.type = event->type;
	rc = cli_event_queue_dup_optional(event->name, &item->name);
	if (rc == 0)
		rc = cli_event_queue_dup_optional(event->phase, &item->phase);
	if (rc == 0)
		rc = cli_event_queue_dup_optional(event->message, &item->message);
	if (rc == 0)
		rc = cli_event_queue_dup_optional(event->turn_id, &item->turn_id);
	if (rc == 0 && event->data) {
		item->event.data = cJSON_Duplicate(event->data, 1);
		if (!item->event.data)
			rc = NB_ERR_NOMEM;
	}
	if (rc != 0) {
		return rc;
	}
	item->event.name = item->name;
	item->event.phase = item->phase;
	item->event.message = item->message;
	item->event.turn_id = item->turn_id;
	rc = cli_event_queue_enqueue_take(ctx, &item);
	if (rc != 0) {
		return rc;
	}
	return 0;
}

int cli_event_queue_post_notification(struct cli_application *ctx,
				      const struct notification *notification)
{
	struct cli_event_queue_item *item NB_CLEANUP(cli_event_queue_item_clear) = NULL;
	int rc;

	if (!ctx || !notification)
		return NB_ERR_INVAL;
	item = calloc(1, sizeof(*item));
	if (!item)
		return NB_ERR_NOMEM;
	item->kind = CLI_EVENT_QUEUE_UI_ITEM_NOTIFICATION;
	item->notification_id = notification->id;
	item->notification_task_id = notification->task_id;
	item->notification_created_at = notification->created_at;
	item->notification_failed = strcmp(notification->level, "error") == 0;
	item->notification_warning = strcmp(notification->level, "warning") == 0;
	rc = cli_event_queue_dup_optional(notification->title, &item->notification_title);
	if (rc == 0)
		rc = cli_event_queue_dup_optional(notification->body, &item->notification_body);
	if (rc != 0) {
		return rc;
	}
	rc = cli_event_queue_enqueue_take(ctx, &item);
	if (rc != 0) {
		return rc;
	}
	return 0;
}

int cli_event_queue_call_owner(struct cli_application *ctx, cli_event_queue_call_fn fn, void *data)
{
	struct cli_event_queue_owner_call call;
	struct cli_event_queue_item *item NB_CLEANUP(cli_event_queue_item_clear) = NULL;
	int mutex_rc;
	int cond_rc;
	int rc;

	if (!ctx || !ctx->events || !fn)
		return NB_ERR_INVAL;
	if (cli_event_queue_is_owner(ctx)) {
		nb_mutex_lock(ctx->events->mutex);
		int stopped = ctx->events->owner_calls_stopped;

		nb_mutex_unlock(ctx->events->mutex);
		if (stopped)
			return NB_ERR_CANCELLED;
		return fn(data);
	}
	memset(&call, 0, sizeof(call));
	mutex_rc = ((call.mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (mutex_rc != 0)
		return mutex_rc;
	cond_rc = ((call.cond = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM);
	if (cond_rc != 0) {
		(nb_mutex_free(call.mutex), call.mutex = NULL);
		return cond_rc;
	}
	call.fn = fn;
	call.data = data;
	item = calloc(1, sizeof(*item));
	if (!item) {
		(nb_cond_free(call.cond), call.cond = NULL);
		(nb_mutex_free(call.mutex), call.mutex = NULL);
		return NB_ERR_NOMEM;
	}
	item->kind = CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL;
	item->owner_call = &call;
	rc = cli_event_queue_enqueue_take(ctx, &item);
	if (rc != 0) {
		(nb_cond_free(call.cond), call.cond = NULL);
		(nb_mutex_free(call.mutex), call.mutex = NULL);
		return rc;
	}
	nb_mutex_lock(call.mutex);
	while (!call.complete)
		nb_cond_wait(call.cond, call.mutex);
	rc = call.result;
	nb_mutex_unlock(call.mutex);
	(nb_cond_free(call.cond), call.cond = NULL);
	(nb_mutex_free(call.mutex), call.mutex = NULL);
	return rc;
}

int cli_event_queue_stop_owner_calls(struct cli_application *ctx)
{
	if (!ctx || !ctx->events)
		return NB_ERR_INVAL;
	if (!cli_event_queue_is_owner(ctx))
		return NB_ERR_NOT_PERMITTED;
	struct cli_event_queue *ui = ctx->events;
	nb_queue cancelled;
	nb_queue *link, *next;

	nb_queue_init(&cancelled);
	nb_mutex_lock(ui->mutex);
	ui->owner_calls_stopped = 1;
	NB_QUEUE_FOREACH_SAFE(link, next, &ui->pending)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);

		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL) {
			nb_queue_remove(link);
			nb_queue_insert_tail(&cancelled, link);
		}
	}
	nb_mutex_unlock(ui->mutex);
	NB_QUEUE_FOREACH_SAFE(link, next, &cancelled)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);

		nb_queue_remove(link);
		cli_event_queue_complete_owner_call(item->owner_call, NB_ERR_CANCELLED);
		cli_event_queue_item_free(item);
	}
	return 0;
}

int cli_event_queue_run_owner_call(struct cli_application *ctx,
				   struct cli_event_queue_owner_call *call)
{
	int result;

	if (!call)
		return NB_ERR_INVAL;
	result = cli_event_queue_call_owner(ctx, call->fn, call->data);
	cli_event_queue_complete_owner_call(call, result);
	return result;
}

int cli_event_queue_events_enabled(const struct cli_application *ctx)
{
	struct cli_event_queue *ui = ctx ? ctx->events : NULL;
	if (!ui)
		return 0;
	nb_mutex_lock(ui->mutex);
	int enabled = ui->events_enabled;
	nb_mutex_unlock(ui->mutex);
	return enabled;
}

int cli_event_queue_set_events_enabled(struct cli_application *ctx, int enabled)
{
	if (!ctx || !ctx->events)
		return NB_ERR_INVAL;
	if (!cli_event_queue_is_owner(ctx))
		return NB_ERR_NOT_PERMITTED;
	struct cli_event_queue *ui = ctx->events;
	nb_mutex_lock(ui->mutex);
	int changed = ui->events_enabled != !!enabled;
	ui->events_enabled = !!enabled;
	int pending = !nb_queue_empty(&ui->pending);
	nb_mutex_unlock(ui->mutex);
	if (changed && enabled && pending)
		cli_event_queue_wake(ui);
	return 0;
}

static int cli_event_queue_pending_ready(struct cli_event_queue *ui)
{
	nb_queue *link;
	NB_QUEUE_FOREACH(link, &ui->pending)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);
		if (ui->events_enabled || item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL)
			return 1;
	}
	return 0;
}

static int cli_event_queue_drain_limit(struct cli_application *ctx, int limit)
{
	struct cli_event_queue *ui;
	nb_queue local;
	nb_queue *link;
	nb_queue *next;
	int first_error = 0;

	if (!ctx || !ctx->events)
		return NB_ERR_INVAL;
	if (!cli_event_queue_is_owner(ctx))
		return NB_ERR_NOT_PERMITTED;
	ui = ctx->events;
	cli_event_queue_consume_wake(ui);
	nb_queue_init(&local);
	nb_mutex_lock(ui->mutex);
	int taken = 0;
	NB_QUEUE_FOREACH_SAFE(link, next, &ui->pending)
	{
		if (limit && taken == limit)
			break;
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);
		if (!ui->events_enabled && item->kind != CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL)
			continue;
		nb_queue_remove(link);
		nb_queue_insert_tail(&local, link);
		taken++;
	}
	int remaining = cli_event_queue_pending_ready(ui);

	nb_mutex_unlock(ui->mutex);
	if (remaining)
		cli_event_queue_wake(ui);
	NB_QUEUE_FOREACH_SAFE(link, next, &local)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);
		nb_queue_remove(link);
		int rc = ui->dispatch(ctx, item);

		if (first_error == 0 && rc != 0)
			first_error = rc;
		cli_event_queue_item_free(item);
	}
	return first_error;
}

int cli_event_queue_drain(struct cli_application *ctx)
{
	return cli_event_queue_drain_limit(ctx, 0);
}

int cli_event_queue_drain_batch(struct cli_application *ctx)
{
	return cli_event_queue_drain_limit(ctx, CLI_EVENT_QUEUE_UI_EVENT_BUDGET);
}

int cli_event_queue_owner_call_pending(struct cli_application *ctx)
{
	struct cli_event_queue *ui = ctx ? ctx->events : NULL;
	if (!ui)
		return 0;
	int pending = 0;
	nb_mutex_lock(ui->mutex);
	nb_queue *link;
	NB_QUEUE_FOREACH(link, &ui->pending)
	{
		struct cli_event_queue_item *item =
			NB_QUEUE_DATA(link, struct cli_event_queue_item, link);
		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL) {
			pending = 1;
			break;
		}
	}
	nb_mutex_unlock(ui->mutex);
	return pending;
}
