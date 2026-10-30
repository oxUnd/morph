#ifndef MORPH_SUB_AGENT_INTERNAL_H
#define MORPH_SUB_AGENT_INTERNAL_H

#include "sub_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal platform seam. Functions and userdata are borrowed through fanout.
 * create/join return positive pthread errors. Successful create owns one
 * joinable worker; a join error does not grant permission to free its data. */
struct sub_agent_thread_ops {
	int (*create)(pthread_t *, void *(*)(void *), void *, void *);
	int (*join)(pthread_t, void *);
	void *user_data;
};

int sub_agent_fanout_with_ops(struct sub_agent_runtime *rt,
			      const struct tool_runtime_context *runtime, const char *agent_name,
			      const char **tasks, int task_count,
			      enum sub_agent_merge_strategy merge, char **result,
			      const struct sub_agent_thread_ops *ops);

#ifdef __cplusplus
}
#endif
#endif
