#ifndef ASK_USER_H
#define ASK_USER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

typedef int (*ask_user_callback_fn)(const char *question,
				    const char *const *choices,
				    int choices_count,
				    char **answer,
				    void *user_data);

int ask_user_init(struct tool_registry *reg, ask_user_callback_fn cb,
		  void *user_data);

#ifdef __cplusplus
}
#endif

#endif
