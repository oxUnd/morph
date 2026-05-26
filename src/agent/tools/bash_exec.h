#ifndef BASH_EXEC_H
#define BASH_EXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

/*
 * Verdict returned by the bash_exec approval callback when a command
 * is not matched by the static allowlist (but not blocked by the
 * dangerous-command blocklist either).
 */
enum bash_exec_verdict {
	BASH_EXEC_DENY = 0,
	BASH_EXEC_ALLOW = 1,	/* allow this invocation only */
	BASH_EXEC_ALWAYS = 2,	/* allow and remember program/cwd */
};

/*
 * Callback invoked by bash_exec_run when a command falls outside the
 * static allowlist. The callback is expected to ask the user (typically
 * via the same y/n/a UX as HITL) and return its decision. The callback
 * runs in the same thread that invoked the bash_exec tool.
 *
 * command - The full command string the agent wants to execute.
 * cwd     - Working directory requested by the agent (may be NULL).
 * user_data - Opaque pointer registered alongside the callback.
 */
typedef enum bash_exec_verdict (*bash_exec_approval_cb_t)(
	const char *command, const char *cwd, void *user_data);

int bash_exec_init(struct tool_registry *reg);
void bash_exec_set_default_timeout(int seconds);
void bash_exec_clear_allowlist(void);
int bash_exec_allow_command(const char *pattern);
int bash_exec_allow_cwd(const char *path);
void bash_exec_set_approval_callback(bash_exec_approval_cb_t cb,
				     void *user_data);

#ifdef __cplusplus
}
#endif

#endif
