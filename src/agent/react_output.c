#include "react_output.h"

#include <errno.h>

int react_output_emit(react_output_cb callback, void *user_data,
		      const struct react_output_event *event)
{
	if (!event)
		return -EINVAL;
	return callback ? callback(event, user_data) : 0;
}
