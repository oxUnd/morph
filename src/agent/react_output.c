#include "react_output.h"
#include "nb/err.h"

#include <errno.h>

int react_output_emit(react_output_cb callback, void *user_data,
		      const struct react_output_event *event)
{
	if (!event)
		return NB_ERR_INVAL;
	return callback ? callback(event, user_data) : 0;
}
