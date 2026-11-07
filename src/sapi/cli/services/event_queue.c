#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/event_queue_item.h"
#include "sapi/cli/application.h"
#include "cJSON.h"
#include "util/error.h"
#include <morph/tui/application.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_EVENT_QUEUE_UI_EVENT_BUDGET 64

struct cli_event_queue_owner_call {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	cli_event_queue_call_fn fn;
	void *data;
	int result;
	int complete;
};

struct cli_event_queue {
	struct morph_tui_application *application;
	cli_event_queue_dispatch_fn dispatch;
	pthread_mutex_t mutex;
	morph_queue_t pending;
	pthread_t owner;
	int wake_read;
	int wake_write;
	int mutex_initialized;
	int owner_calls_stopped;
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

static void cli_event_queue_complete_owner_call(struct cli_event_queue_owner_call *call, int result)
{
	if (!call)
		return;
	pthread_mutex_lock(&call->mutex);
	call->result = result;
	call->complete = 1;
	pthread_cond_signal(&call->cond);
	pthread_mutex_unlock(&call->mutex);
}

static int cli_event_queue_dup_optional(const char *source, char **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!source)
		return 0;
	*out = strdup(source);
	if (!*out)
		return -ENOMEM;
	return 0;
}

static int cli_event_queue_set_fd_flags(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -errno;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	flags = fcntl(fd, F_GETFD, 0);
	if (flags < 0)
		return -errno;
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return -errno;
	return 0;
}

static void cli_event_queue_wake(struct cli_event_queue *ui)
{
	if (!ui || ui->wake_write < 0)
		return;
	pthread_mutex_lock(&ui->mutex);
	if (ui->application)
		(void)morph_tui_application_wake(ui->application);
	pthread_mutex_unlock(&ui->mutex);
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

static int cli_event_queue_enqueue(struct cli_application *ctx, struct cli_event_queue_item *item)
{
	struct cli_event_queue *ui;

	if (!ctx || !ctx->events || !item)
		return -EINVAL;
	ui = ctx->events;
	pthread_mutex_lock(&ui->mutex);
	if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL && ui->owner_calls_stopped) {
		pthread_mutex_unlock(&ui->mutex);
		return -ECANCELED;
	}
	morph_queue_insert_tail(&ui->pending, &item->link);
	pthread_mutex_unlock(&ui->mutex);
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
		return -EINVAL;
	ui = calloc(1, sizeof(*ui));
	if (!ui)
		return -ENOMEM;
	ui->wake_read = -1;
	ui->wake_write = -1;
	morph_queue_init(&ui->pending);
	mutex_rc = pthread_mutex_init(&ui->mutex, NULL);
	if (mutex_rc != 0) {
		free(ui);
		return -mutex_rc;
	}
	ui->mutex_initialized = 1;
	if (pipe(wake_fds) != 0) {
		rc = -errno;
		pthread_mutex_destroy(&ui->mutex);
		free(ui);
		return rc;
	}
	rc = cli_event_queue_set_fd_flags(wake_fds[0]);
	if (rc == 0)
		rc = cli_event_queue_set_fd_flags(wake_fds[1]);
	if (rc != 0) {
		close(wake_fds[0]);
		close(wake_fds[1]);
		pthread_mutex_destroy(&ui->mutex);
		free(ui);
		return rc;
	}
	ui->wake_read = wake_fds[0];
	ui->wake_write = wake_fds[1];
	ui->owner = pthread_self();
	ui->dispatch = dispatch;
	ctx->events = ui;
	return 0;
}

void cli_event_queue_cleanup(struct cli_application *ctx)
{
	struct cli_event_queue *ui;
	morph_queue_t local;
	morph_queue_t *link;
	morph_queue_t *next;

	if (!ctx || !ctx->events)
		return;
	ui = ctx->events;
	morph_queue_init(&local);
	if (ui->mutex_initialized) {
		pthread_mutex_lock(&ui->mutex);
		morph_queue_add(&local, &ui->pending);
		pthread_mutex_unlock(&ui->mutex);
	}
	morph_queue_foreach_safe (link, next, &local) {
		struct cli_event_queue_item *item =
			morph_queue_data(link, struct cli_event_queue_item, link);

		morph_queue_remove(link);
		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL)
			cli_event_queue_complete_owner_call(item->owner_call, -ECANCELED);
		cli_event_queue_item_free(item);
	}
	if (ui->wake_read >= 0)
		close(ui->wake_read);
	if (ui->wake_write >= 0)
		close(ui->wake_write);
	if (ui->mutex_initialized)
		pthread_mutex_destroy(&ui->mutex);
	free(ui);
	ctx->events = NULL;
}

int cli_event_queue_is_owner(const struct cli_application *ctx)
{
	return ctx && ctx->events && pthread_equal(ctx->events->owner, pthread_self());
}

int cli_event_queue_wake_fd(const struct cli_application *ctx)
{
	return ctx && ctx->events ? ctx->events->wake_read : -1;
}

void cli_event_queue_attach_app(struct cli_application *ctx, struct morph_tui_application *app)
{
	if (!ctx || !ctx->events)
		return;
	pthread_mutex_lock(&ctx->events->mutex);
	ctx->events->application = app;
	pthread_mutex_unlock(&ctx->events->mutex);
	if (app)
		(void)morph_tui_application_wake(app);
}

int cli_event_queue_post_morph_event(struct cli_application *ctx, const struct morph_event *event)
{
	struct cli_event_queue_item *item;
	int rc;

	if (!ctx || !event)
		return -EINVAL;
	item = calloc(1, sizeof(*item));
	if (!item)
		return -ENOMEM;
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
			rc = -ENOMEM;
	}
	if (rc != 0) {
		cli_event_queue_item_free(item);
		return rc;
	}
	item->event.name = item->name;
	item->event.phase = item->phase;
	item->event.message = item->message;
	item->event.turn_id = item->turn_id;
	rc = cli_event_queue_enqueue(ctx, item);
	if (rc != 0) {
		cli_event_queue_item_free(item);
		return rc;
	}
	return 0;
}

int cli_event_queue_post_notification(struct cli_application *ctx,
				      const struct notification *notification)
{
	struct cli_event_queue_item *item;
	int rc;

	if (!ctx || !notification)
		return -EINVAL;
	item = calloc(1, sizeof(*item));
	if (!item)
		return -ENOMEM;
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
		cli_event_queue_item_free(item);
		return rc;
	}
	rc = cli_event_queue_enqueue(ctx, item);
	if (rc != 0) {
		cli_event_queue_item_free(item);
		return rc;
	}
	return 0;
}

int cli_event_queue_call_owner(struct cli_application *ctx, cli_event_queue_call_fn fn, void *data)
{
	struct cli_event_queue_owner_call call;
	struct cli_event_queue_item *item;
	int mutex_rc;
	int cond_rc;
	int rc;

	if (!ctx || !ctx->events || !fn)
		return -EINVAL;
	if (cli_event_queue_is_owner(ctx)) {
		pthread_mutex_lock(&ctx->events->mutex);
		int stopped = ctx->events->owner_calls_stopped;

		pthread_mutex_unlock(&ctx->events->mutex);
		if (stopped)
			return -ECANCELED;
		return fn(data);
	}
	memset(&call, 0, sizeof(call));
	mutex_rc = pthread_mutex_init(&call.mutex, NULL);
	if (mutex_rc != 0)
		return -mutex_rc;
	cond_rc = pthread_cond_init(&call.cond, NULL);
	if (cond_rc != 0) {
		pthread_mutex_destroy(&call.mutex);
		return -cond_rc;
	}
	call.fn = fn;
	call.data = data;
	item = calloc(1, sizeof(*item));
	if (!item) {
		pthread_cond_destroy(&call.cond);
		pthread_mutex_destroy(&call.mutex);
		return -ENOMEM;
	}
	item->kind = CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL;
	item->owner_call = &call;
	rc = cli_event_queue_enqueue(ctx, item);
	if (rc != 0) {
		free(item);
		pthread_cond_destroy(&call.cond);
		pthread_mutex_destroy(&call.mutex);
		return rc;
	}
	pthread_mutex_lock(&call.mutex);
	while (!call.complete)
		pthread_cond_wait(&call.cond, &call.mutex);
	rc = call.result;
	pthread_mutex_unlock(&call.mutex);
	pthread_cond_destroy(&call.cond);
	pthread_mutex_destroy(&call.mutex);
	return rc;
}

int cli_event_queue_stop_owner_calls(struct cli_application *ctx)
{
	if (!ctx || !ctx->events)
		return -EINVAL;
	if (!cli_event_queue_is_owner(ctx))
		return -EPERM;
	struct cli_event_queue *ui = ctx->events;
	morph_queue_t cancelled;
	morph_queue_t *link, *next;

	morph_queue_init(&cancelled);
	pthread_mutex_lock(&ui->mutex);
	ui->owner_calls_stopped = 1;
	morph_queue_foreach_safe (link, next, &ui->pending) {
		struct cli_event_queue_item *item =
			morph_queue_data(link, struct cli_event_queue_item, link);

		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL) {
			morph_queue_remove(link);
			morph_queue_insert_tail(&cancelled, link);
		}
	}
	pthread_mutex_unlock(&ui->mutex);
	morph_queue_foreach_safe (link, next, &cancelled) {
		struct cli_event_queue_item *item =
			morph_queue_data(link, struct cli_event_queue_item, link);

		morph_queue_remove(link);
		cli_event_queue_complete_owner_call(item->owner_call, -ECANCELED);
		cli_event_queue_item_free(item);
	}
	return 0;
}

int cli_event_queue_run_owner_call(struct cli_application *ctx,
				   struct cli_event_queue_owner_call *call)
{
	int result;

	if (!call)
		return -EINVAL;
	result = cli_event_queue_call_owner(ctx, call->fn, call->data);
	cli_event_queue_complete_owner_call(call, result);
	return result;
}

static int cli_event_queue_drain_limit(struct cli_application *ctx, int limit)
{
	struct cli_event_queue *ui;
	morph_queue_t local;
	morph_queue_t *link;
	morph_queue_t *next;
	int first_error = 0;

	if (!ctx || !ctx->events)
		return -EINVAL;
	if (!cli_event_queue_is_owner(ctx))
		return -EPERM;
	ui = ctx->events;
	cli_event_queue_consume_wake(ui);
	morph_queue_init(&local);
	pthread_mutex_lock(&ui->mutex);
	if (!limit)
		morph_queue_add(&local, &ui->pending);
	else {
		for (int i = 0; i < limit && !morph_queue_empty(&ui->pending); i++) {
			link = morph_queue_head(&ui->pending);
			morph_queue_remove(link);
			morph_queue_insert_tail(&local, link);
		}
	}
	int remaining = !morph_queue_empty(&ui->pending);

	pthread_mutex_unlock(&ui->mutex);
	if (remaining)
		cli_event_queue_wake(ui);
	morph_queue_foreach_safe (link, next, &local) {
		struct cli_event_queue_item *item =
			morph_queue_data(link, struct cli_event_queue_item, link);
		morph_queue_remove(link);
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
	pthread_mutex_lock(&ui->mutex);
	morph_queue_t *link;
	morph_queue_foreach (link, &ui->pending) {
		struct cli_event_queue_item *item =
			morph_queue_data(link, struct cli_event_queue_item, link);
		if (item->kind == CLI_EVENT_QUEUE_UI_ITEM_OWNER_CALL) {
			pending = 1;
			break;
		}
	}
	pthread_mutex_unlock(&ui->mutex);
	return pending;
}
