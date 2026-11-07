#ifndef ASK_USER_H
#define ASK_USER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

#define ASK_USER_CHOICES_MAX 64
#define ASK_USER_TIMEOUT_DEFAULT 60

/* Borrowed for the duration of the callback. Required questions have no deadline.
 * Return -ETIMEDOUT only for an untouched optional prompt; cancellation must
 * return -ECANCELED/-EINTR. Once the user starts answering, disable the deadline. */
struct ask_user_options {
	int recommended_indices[ASK_USER_CHOICES_MAX];
	int recommended_count;
	int optional;
	int timeout_seconds;
	const struct morph_cancel_token *cancel_token;
};

int ask_user_options_to_json(cJSON *data, const struct ask_user_options *options);

typedef int (*ask_user_callback_fn)(const char *question,
				    const char *const *choices,
				    int choices_count,
				    const char *selection_mode,
				    int min_choices,
				    int max_choices,
				    const struct ask_user_options *options,
				    char ***answers,
				    int *answers_count,
				    void *user_data);

int ask_user_init_deferred(struct tool_registry *reg);
int ask_user_init(struct tool_registry *reg, ask_user_callback_fn cb,
		  void *user_data);

#ifdef __cplusplus
}
#endif

#endif
