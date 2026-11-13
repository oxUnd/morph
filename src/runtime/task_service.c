#include "runtime/services.h"
#include "runtime/runtime_internal.h"
#include "errors.h"
#include "nb/cleanup.h"

#include <errno.h>

int runtime_task_create(struct runtime *runtime,
			const struct scheduled_task_input *input,
			struct scheduled_task *out)
{
	if (!runtime || !input || !out)
		return NB_ERR_INVAL;
	if (out->payload_json || out->policy_json || out->notify_json)
		return NB_ERR_EXISTS;
	return scheduled_task_create_with_events(&runtime->context.database,
		input, out, runtime->options.task_events);
}

int runtime_task_update(struct runtime *runtime, int64_t id,
			const struct scheduled_task_input *input,
			struct scheduled_task *out)
{
	if (!runtime || !input || !out)
		return NB_ERR_INVAL;
	if (out->payload_json || out->policy_json || out->notify_json)
		return NB_ERR_EXISTS;
	return scheduled_task_update_with_events(&runtime->context.database, id,
		input, out, runtime->options.task_events);
}

int runtime_task_count_active(struct runtime *runtime,
			      struct scheduled_task_counts *out)
{
	if (!runtime)
		return NB_ERR_INVAL;
	return scheduled_task_count_active(&runtime->context.database, out);
}

int runtime_task_list(struct runtime *runtime, const char *status, int limit,
		      struct scheduled_task **out, int *count)
{
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	return runtime ? scheduled_task_list(&runtime->context.database, status, limit, out, count)
		       : NB_ERR_INVAL;
}

int runtime_task_copy(struct runtime *runtime, int64_t id, struct scheduled_task *out)
{
	if (!out)
		return NB_ERR_INVAL;
	if (out->payload_json || out->policy_json || out->notify_json)
		return NB_ERR_EXISTS;
	return runtime ? scheduled_task_get(&runtime->context.database, id, out) : NB_ERR_INVAL;
}

int runtime_task_cancel(struct runtime *runtime, int64_t id)
{
	return runtime ? scheduled_task_cancel_with_events(&runtime->context.database, id,
							   runtime->options.task_events)
		       : NB_ERR_INVAL;
}

int runtime_notification_list(struct runtime *runtime, int limit,
			      struct notification **out, int *count)
{
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	return runtime ? notification_list_unread(&runtime->context.database, limit, out, count)
		       : NB_ERR_INVAL;
}

int runtime_notification_mark_read(struct runtime *runtime, int64_t id)
{
	return runtime ? notification_mark_read(&runtime->context.database, id, 0) : NB_ERR_INVAL;
}
