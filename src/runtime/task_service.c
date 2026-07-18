#include "runtime/services.h"
#include "runtime/runtime_internal.h"

#include <errno.h>

int runtime_task_create(struct runtime *runtime,
			const struct scheduled_task_input *input,
			struct scheduled_task *out)
{
	if (!runtime || !input || !out)
		return -EINVAL;
	return scheduled_task_create_with_events(&runtime->context.database,
		input, out, runtime->options.task_events);
}

int runtime_task_update(struct runtime *runtime, int64_t id,
			const struct scheduled_task_input *input,
			struct scheduled_task *out)
{
	if (!runtime || !input || !out)
		return -EINVAL;
	return scheduled_task_update_with_events(&runtime->context.database, id,
		input, out, runtime->options.task_events);
}

int runtime_task_list(struct runtime *runtime, const char *status, int limit,
		      struct scheduled_task **out, int *count)
{
	return runtime ? scheduled_task_list(&runtime->context.database, status,
		limit, out, count) : -EINVAL;
}

int runtime_task_get(struct runtime *runtime, int64_t id,
		     struct scheduled_task *out)
{
	return runtime ? scheduled_task_get(&runtime->context.database, id, out)
		: -EINVAL;
}

int runtime_task_cancel(struct runtime *runtime, int64_t id)
{
	return runtime ? scheduled_task_cancel_with_events(
		&runtime->context.database, id, runtime->options.task_events)
		: -EINVAL;
}

int runtime_notification_list(struct runtime *runtime, int limit,
			      struct notification **out, int *count)
{
	return runtime ? notification_list_unread(&runtime->context.database,
		limit, out, count) : -EINVAL;
}

int runtime_notification_mark_read(struct runtime *runtime, int64_t id)
{
	return runtime ? notification_mark_read(&runtime->context.database, id, 0)
		: -EINVAL;
}
