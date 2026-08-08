#include "sapi/cli/internal.h"
#include "sapi/cli/ui_event.h"

#include "util/queue.h"

#include <fcntl.h>

enum cli_ui_item_kind {
	CLI_UI_ITEM_MORPH_EVENT,
	CLI_UI_ITEM_NOTIFICATION,
	CLI_UI_ITEM_OWNER_CALL,
};

struct cli_ui_owner_call {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	cli_ui_owner_call_fn fn;
	void *data;
	int result;
	int complete;
};

struct cli_ui_item {
	morph_queue_t link;
	enum cli_ui_item_kind kind;
	struct morph_event event;
	char *name;
	char *phase;
	char *message;
	char *turn_id;
	char *notification_title;
	char *notification_body;
	int64_t notification_id;
	struct cli_ui_owner_call *owner_call;
};

struct cli_ui {
	pthread_mutex_t mutex;
	morph_queue_t pending;
	pthread_t owner;
	int wake_read;
	int wake_write;
	int mutex_initialized;
};

static void cli_ui_item_free(struct cli_ui_item *item)
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

static void cli_ui_complete_owner_call(struct cli_ui_owner_call *call,
				       int result)
{
	if (!call)
		return;
	pthread_mutex_lock(&call->mutex);
	call->result = result;
	call->complete = 1;
	pthread_cond_signal(&call->cond);
	pthread_mutex_unlock(&call->mutex);
}

static int cli_ui_dup_optional(const char *source, char **out)
{
	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	if (!source)
		return 0;
	*out = strdup(source);
	if (!*out)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static int cli_ui_set_fd_flags(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		MORPH_RETURN(-errno);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		MORPH_RETURN(-errno);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags < 0)
		MORPH_RETURN(-errno);
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		MORPH_RETURN(-errno);
	return 0;
}

static void cli_ui_wake(struct cli_ui *ui)
{
	unsigned char byte = 1;
	ssize_t count;

	if (!ui || ui->wake_write < 0)
		return;
	do {
		count = write(ui->wake_write, &byte, sizeof(byte));
	} while (count < 0 && errno == EINTR);
}

void cli_ui_notify(struct cli_context *ctx)
{
	if (ctx && ctx->ui)
		cli_ui_wake(ctx->ui);
}

static void cli_ui_consume_wake(struct cli_ui *ui)
{
	unsigned char bytes[64];
	ssize_t count;

	if (!ui || ui->wake_read < 0)
		return;
	do {
		count = read(ui->wake_read, bytes, sizeof(bytes));
	} while (count > 0 || (count < 0 && errno == EINTR));
}

static int cli_ui_enqueue(struct cli_context *ctx,
			  struct cli_ui_item *item)
{
	struct cli_ui *ui;

	if (!ctx || !ctx->ui || !item)
		MORPH_RETURN(-EINVAL);
	ui = ctx->ui;
	pthread_mutex_lock(&ui->mutex);
	morph_queue_insert_tail(&ui->pending, &item->link);
	pthread_mutex_unlock(&ui->mutex);
	cli_ui_wake(ui);
	return 0;
}

int cli_ui_init(struct cli_context *ctx)
{
	struct cli_ui *ui;
	int wake_fds[2] = {-1, -1};
	int mutex_rc;
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	ui = calloc(1, sizeof(*ui));
	if (!ui)
		MORPH_RETURN(-ENOMEM);
	ui->wake_read = -1;
	ui->wake_write = -1;
	morph_queue_init(&ui->pending);
	mutex_rc = pthread_mutex_init(&ui->mutex, NULL);
	if (mutex_rc != 0) {
		free(ui);
		MORPH_RETURN(-mutex_rc);
	}
	ui->mutex_initialized = 1;
	if (pipe(wake_fds) != 0) {
		rc = -errno;
		pthread_mutex_destroy(&ui->mutex);
		free(ui);
		MORPH_RETURN(rc);
	}
	rc = cli_ui_set_fd_flags(wake_fds[0]);
	if (rc == 0)
		rc = cli_ui_set_fd_flags(wake_fds[1]);
	if (rc != 0) {
		close(wake_fds[0]);
		close(wake_fds[1]);
		pthread_mutex_destroy(&ui->mutex);
		free(ui);
		MORPH_RETURN(rc);
	}
	ui->wake_read = wake_fds[0];
	ui->wake_write = wake_fds[1];
	ui->owner = pthread_self();
	ctx->ui = ui;
	return 0;
}

void cli_ui_cleanup(struct cli_context *ctx)
{
	struct cli_ui *ui;
	morph_queue_t local;
	morph_queue_t *link;
	morph_queue_t *next;

	if (!ctx || !ctx->ui)
		return;
	ui = ctx->ui;
	morph_queue_init(&local);
	if (ui->mutex_initialized) {
		pthread_mutex_lock(&ui->mutex);
		morph_queue_add(&local, &ui->pending);
		pthread_mutex_unlock(&ui->mutex);
	}
	morph_queue_foreach_safe(link, next, &local) {
		struct cli_ui_item *item = morph_queue_data(
			link, struct cli_ui_item, link);

		morph_queue_remove(link);
		if (item->kind == CLI_UI_ITEM_OWNER_CALL)
			cli_ui_complete_owner_call(item->owner_call, -ECANCELED);
		cli_ui_item_free(item);
	}
	if (ui->wake_read >= 0)
		close(ui->wake_read);
	if (ui->wake_write >= 0)
		close(ui->wake_write);
	if (ui->mutex_initialized)
		pthread_mutex_destroy(&ui->mutex);
	free(ui);
	ctx->ui = NULL;
}

int cli_ui_is_owner(const struct cli_context *ctx)
{
	return ctx && ctx->ui &&
		pthread_equal(ctx->ui->owner, pthread_self());
}

int cli_ui_wake_fd(const struct cli_context *ctx)
{
	return ctx && ctx->ui ? ctx->ui->wake_read : -1;
}

int cli_ui_post_morph_event(struct cli_context *ctx,
			    const struct morph_event *event)
{
	struct cli_ui_item *item;
	int rc;

	if (!ctx || !event)
		MORPH_RETURN(-EINVAL);
	item = calloc(1, sizeof(*item));
	if (!item)
		MORPH_RETURN(-ENOMEM);
	item->kind = CLI_UI_ITEM_MORPH_EVENT;
	item->event.type = event->type;
	rc = cli_ui_dup_optional(event->name, &item->name);
	if (rc == 0)
		rc = cli_ui_dup_optional(event->phase, &item->phase);
	if (rc == 0)
		rc = cli_ui_dup_optional(event->message, &item->message);
	if (rc == 0)
		rc = cli_ui_dup_optional(event->turn_id, &item->turn_id);
	if (rc == 0 && event->data) {
		item->event.data = cJSON_Duplicate(event->data, 1);
		if (!item->event.data)
			rc = -ENOMEM;
	}
	if (rc != 0) {
		cli_ui_item_free(item);
		MORPH_RETURN(rc);
	}
	item->event.name = item->name;
	item->event.phase = item->phase;
	item->event.message = item->message;
	item->event.turn_id = item->turn_id;
	rc = cli_ui_enqueue(ctx, item);
	if (rc != 0) {
		cli_ui_item_free(item);
		MORPH_RETURN(rc);
	}
	return 0;
}

int cli_ui_post_notification(struct cli_context *ctx,
			     const struct notification *notification)
{
	struct cli_ui_item *item;
	int rc;

	if (!ctx || !notification)
		MORPH_RETURN(-EINVAL);
	item = calloc(1, sizeof(*item));
	if (!item)
		MORPH_RETURN(-ENOMEM);
	item->kind = CLI_UI_ITEM_NOTIFICATION;
	item->notification_id = notification->id;
	rc = cli_ui_dup_optional(notification->title,
				 &item->notification_title);
	if (rc == 0)
		rc = cli_ui_dup_optional(notification->body,
				 &item->notification_body);
	if (rc != 0) {
		cli_ui_item_free(item);
		MORPH_RETURN(rc);
	}
	rc = cli_ui_enqueue(ctx, item);
	if (rc != 0) {
		cli_ui_item_free(item);
		MORPH_RETURN(rc);
	}
	return 0;
}

int cli_ui_call_owner(struct cli_context *ctx, cli_ui_owner_call_fn fn,
		      void *data)
{
	struct cli_ui_owner_call call;
	struct cli_ui_item *item;
	int mutex_rc;
	int cond_rc;
	int rc;

	if (!ctx || !ctx->ui || !fn)
		MORPH_RETURN(-EINVAL);
	if (cli_ui_is_owner(ctx))
		return fn(data);
	memset(&call, 0, sizeof(call));
	mutex_rc = pthread_mutex_init(&call.mutex, NULL);
	if (mutex_rc != 0)
		MORPH_RETURN(-mutex_rc);
	cond_rc = pthread_cond_init(&call.cond, NULL);
	if (cond_rc != 0) {
		pthread_mutex_destroy(&call.mutex);
		MORPH_RETURN(-cond_rc);
	}
	call.fn = fn;
	call.data = data;
	item = calloc(1, sizeof(*item));
	if (!item) {
		pthread_cond_destroy(&call.cond);
		pthread_mutex_destroy(&call.mutex);
		MORPH_RETURN(-ENOMEM);
	}
	item->kind = CLI_UI_ITEM_OWNER_CALL;
	item->owner_call = &call;
	rc = cli_ui_enqueue(ctx, item);
	if (rc != 0) {
		free(item);
		pthread_cond_destroy(&call.cond);
		pthread_mutex_destroy(&call.mutex);
		MORPH_RETURN(rc);
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

static void cli_ui_run_owner_call(struct cli_ui_owner_call *call)
{
	int result;

	if (!call)
		return;
	result = call->fn(call->data);
	cli_ui_complete_owner_call(call, result);
}

static void cli_ui_render_notification(const struct cli_ui_item *item)
{
	printf("\n" ANSI_BOLD ANSI_CYAN "[task]" ANSI_RESET " ");
	(void)cli_print_untrusted_text(
		item->notification_title ? item->notification_title : "Task",
		UTF8_TERMINAL_TEXT_SINGLE_LINE);
	printf("\n");
	if (item->notification_body && item->notification_body[0]) {
		(void)cli_print_untrusted_text(item->notification_body,
			UTF8_TERMINAL_TEXT_MULTILINE);
		printf("\n");
	}
	printf(ANSI_DIM "stored in /inbox as #%lld" ANSI_RESET "\n",
	       (long long)item->notification_id);
}

int cli_ui_drain(struct cli_context *ctx)
{
	struct cli_ui *ui;
	morph_queue_t local;
	morph_queue_t *link;
	morph_queue_t *next;
	int first_error = 0;

	if (!ctx || !ctx->ui)
		MORPH_RETURN(-EINVAL);
	if (!cli_ui_is_owner(ctx))
		MORPH_RETURN(-EPERM);
	ui = ctx->ui;
	cli_ui_consume_wake(ui);
	morph_queue_init(&local);
	pthread_mutex_lock(&ui->mutex);
	morph_queue_add(&local, &ui->pending);
	pthread_mutex_unlock(&ui->mutex);
	morph_queue_foreach_safe(link, next, &local) {
		struct cli_ui_item *item = morph_queue_data(
			link, struct cli_ui_item, link);
		int rc = 0;

		morph_queue_remove(link);
		if (item->kind == CLI_UI_ITEM_MORPH_EVENT)
			rc = cli_presentation_event(ctx, &item->event);
		else if (item->kind == CLI_UI_ITEM_OWNER_CALL)
			cli_ui_run_owner_call(item->owner_call);
		else if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON) {
			cli_terminal_history_begin(ctx);
			cli_ui_render_notification(item);
			cli_terminal_history_end(ctx);
		}
		if (first_error == 0 && rc != 0)
			first_error = rc;
		cli_ui_item_free(item);
	}
	fflush(stdout);
	return first_error;
}
