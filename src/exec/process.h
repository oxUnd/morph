#ifndef MORPH_PROCESS_H
#define MORPH_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct sandbox_config;

#define PROCESS_SESSION_ID_MAX 64
#define PROCESS_COMMAND_MAX 8192

enum process_state {
	PROCESS_STARTING = 0,
	PROCESS_RUNNING,
	PROCESS_EXITED,
	PROCESS_SIGNALED,
	PROCESS_TIMED_OUT,
	PROCESS_KILLED,
};

struct process_manager_config {
	size_t max_session_output;
	size_t max_inline_output;
	uint64_t kill_grace_ms;
	const char *shell;
};

struct process_manager;

struct process_spawn_options {
	const char *command;
	const char *workdir;
	uint64_t timeout_ms;
	uint64_t yield_time_ms;
	int pty;
	int background;
	struct sandbox_config *sandbox;
};

struct process_snapshot {
	enum process_state state;
	int exit_code;
	int signal_number;
	uint64_t duration_ms;
	char *stdout_text;
	char *stderr_text;
	size_t stdout_omitted_bytes;
	size_t stderr_omitted_bytes;
	int stdout_truncated;
	int stderr_truncated;
};

int process_manager_create(const struct process_manager_config *config,
			   struct process_manager **out);
void process_manager_destroy(struct process_manager *manager);

int process_spawn(struct process_manager *manager,
		  const struct process_spawn_options *options,
		  char session_id[PROCESS_SESSION_ID_MAX],
		  struct process_snapshot *snapshot);
int process_session_snapshot(struct process_manager *manager,
			     const char *session_id,
			     struct process_snapshot *snapshot);
int process_session_write(struct process_manager *manager,
			  const char *session_id, const char *input,
			  size_t input_len);
int process_session_interrupt(struct process_manager *manager,
			      const char *session_id);
int process_session_kill(struct process_manager *manager,
			  const char *session_id);
void process_snapshot_cleanup(struct process_snapshot *snapshot);
const char *process_state_name(enum process_state state);

#ifdef __cplusplus
}
#endif

#endif
