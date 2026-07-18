#ifndef MORPH_RUNTIME_SCHEDULER_H
#define MORPH_RUNTIME_SCHEDULER_H

#include "agent/react.h"
#include "runtime/tasks.h"

char *runtime_react_error_message(struct react_context *react, int rc);
char *runtime_react_notification_body(struct react_context *react);

#endif
