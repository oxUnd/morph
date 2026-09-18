#include "process.h"
#include "sandbox.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/id.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

extern char **environ;

#define PROCESS_DEFAULT_MAX_SESSION_OUTPUT (1024u * 1024u)
#define PROCESS_DEFAULT_MAX_INLINE_OUTPUT (32u * 1024u)
#define PROCESS_DEFAULT_KILL_GRACE_MS 500u
#define PROCESS_POLL_MS 25
#define PROCESS_SECRET_MAX 16

struct process_secret {
	const char *name;
	const char *value;
};

struct process_stream {
	morph_buf_t data;
	size_t base;
	size_t cursor;
	size_t omitted;
};

struct process_session {
	char id[PROCESS_SESSION_ID_MAX];
	char command[PROCESS_COMMAND_MAX];
	pid_t pid;
	pid_t pgid;
	int stdout_fd;
	int stderr_fd;
	int stdin_fd;
	int pty;
	int stdout_open;
	int stderr_open;
	int stdin_open;
	enum process_state state;
	int exit_code;
	int signal_number;
	uint64_t started_ms;
	uint64_t timeout_ms;
	uint64_t kill_deadline_ms;
	int timeout_requested;
	int kill_requested;
	struct process_stream stdout_stream;
	struct process_stream stderr_stream;
};

struct process_manager {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_t thread;
	int thread_started;
	int stopping;
	size_t max_session_output;
	size_t max_inline_output;
	uint64_t kill_grace_ms;
	char shell[PATH_MAX];
	struct process_secret secrets[PROCESS_SECRET_MAX];
	size_t secret_count;
	struct process_session *sessions;
	size_t session_count;
	size_t session_capacity;
};

static uint64_t process_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000u +
		(uint64_t)now.tv_nsec / 1000000u;
}

static int set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		MORPH_RETURN_ERRNO();
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

static void close_fd(int *fd)
{
	if (fd && *fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void close_session_fds(struct process_session *session)
{
	if (session->stdin_fd == session->stdout_fd)
		session->stdin_fd = -1;
	close_fd(&session->stdout_fd);
	close_fd(&session->stderr_fd);
	close_fd(&session->stdin_fd);
	session->stdout_open = 0;
	session->stderr_open = 0;
	session->stdin_open = 0;
}

static void stream_cleanup(struct process_stream *stream)
{
	if (!stream)
		return;
	morph_buf_cleanup(&stream->data);
	memset(stream, 0, sizeof(*stream));
}

static int stream_init(struct process_stream *stream)
{
	memset(stream, 0, sizeof(*stream));
	return morph_buf_init(&stream->data, 4096);
}

static int stream_append(struct process_manager *manager,
			 struct process_stream *stream,
			 const char *data, size_t length)
{
	size_t drop;

	if (!data || length == 0)
		return 0;
	if (length >= manager->max_session_output) {
		drop = length - manager->max_session_output;
		data += drop;
		length -= drop;
		stream->omitted += drop;
	}
	if (stream->data.len + length > manager->max_session_output) {
		drop = stream->data.len + length - manager->max_session_output;
		if (drop > stream->data.len)
			drop = stream->data.len;
		if (drop > 0) {
			memmove(stream->data.data, stream->data.data + drop,
				stream->data.len - drop);
			stream->data.len -= drop;
			stream->data.data[stream->data.len] = '\0';
			stream->base += drop;
			stream->omitted += drop;
			if (stream->cursor < stream->base)
				stream->cursor = stream->base;
		}
	}
	return morph_buf_append(&stream->data, data, length);
}

static char *redact_text(const struct process_manager *manager,
			 const char *input, size_t length)
{
	morph_buf_t output;
	const char *cursor = input;
	const char *end = input + length;

	if (morph_buf_init(&output, length + 1) != 0)
		return NULL;
	while (cursor < end) {
		const char *match = NULL;
		const struct process_secret *secret = NULL;

		for (size_t i = 0; i < manager->secret_count; i++) {
			const struct process_secret *candidate = &manager->secrets[i];
			const char *found;

			found = strstr(cursor, candidate->value);
			if (found && found < end &&
				(size_t)(end - found) >= strlen(candidate->value) &&
				(!match || found < match)) {
				match = found;
				secret = candidate;
			}
		}
		if (!match) {
			(void)morph_buf_append(&output, cursor,
				(size_t)(end - cursor));
			break;
		}
		(void)morph_buf_append(&output, cursor,
			(size_t)(match - cursor));
		(void)morph_buf_printf(&output, "[REDACTED:%s]", secret->name);
		cursor = match + strlen(secret->value);
	}
	{
		char *result = morph_buf_detach(&output);

		morph_buf_cleanup(&output);
		return result;
	}
}

static struct process_session *find_session(struct process_manager *manager,
					    const char *id)
{
	if (!manager || !id)
		return NULL;
	for (size_t i = 0; i < manager->session_count; i++)
		if (strcmp(manager->sessions[i].id, id) == 0)
			return &manager->sessions[i];
	return NULL;
}

static int append_snapshot_stream(const struct process_manager *manager,
				  struct process_stream *stream, char **out,
				  size_t *omitted, int *truncated)
{
	size_t start;
	size_t length;
	size_t limit = manager->max_inline_output;
	morph_buf_t value;

	*out = NULL;
	*omitted = stream->omitted;
	*truncated = 0;
	if (stream->cursor < stream->base)
		start = 0;
	else
		start = stream->cursor - stream->base;
	if (start > stream->data.len)
		start = stream->data.len;
	length = stream->data.len - start;
	if (length <= limit) {
		*out = redact_text(manager, stream->data.data + start, length);
		return *out ? 0 : -ENOMEM;
	}
	*truncated = 1;
	*omitted += length - limit;
	if (morph_buf_init(&value, limit + 64) != 0)
		return -ENOMEM;
	{
		size_t head = limit / 4;
		size_t tail = limit - head;

		(void)morph_buf_append(&value, stream->data.data + start, head);
		(void)morph_buf_puts(&value,
			"\n...[output truncated]...\n");
		(void)morph_buf_append(&value,
			stream->data.data + stream->data.len - tail, tail);
	}
	{
		char *raw = morph_buf_detach(&value);

		*out = raw ? redact_text(manager, raw, strlen(raw)) : NULL;
		free(raw);
	}
	morph_buf_cleanup(&value);
	return *out ? 0 : -ENOMEM;
}

static int snapshot_locked(struct process_manager *manager,
				   struct process_session *session,
				   struct process_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->state = session->state;
	snapshot->exit_code = session->exit_code;
	snapshot->signal_number = session->signal_number;
	snapshot->duration_ms = process_now_ms() - session->started_ms;
	if (append_snapshot_stream(manager, &session->stdout_stream,
				   &snapshot->stdout_text,
				   &snapshot->stdout_omitted_bytes,
				   &snapshot->stdout_truncated) != 0)
		return -ENOMEM;
	if (append_snapshot_stream(manager, &session->stderr_stream,
				   &snapshot->stderr_text,
				   &snapshot->stderr_omitted_bytes,
				   &snapshot->stderr_truncated) != 0) {
		free(snapshot->stdout_text);
		snapshot->stdout_text = NULL;
		return -ENOMEM;
	}
	session->stdout_stream.cursor = session->stdout_stream.base +
		session->stdout_stream.data.len;
	session->stderr_stream.cursor = session->stderr_stream.base +
		session->stderr_stream.data.len;
	return 0;
}

static void drain_fd(struct process_manager *manager, int *fd,
			     struct process_stream *stream, int *open, int pty)
{
	char buffer[BUFSIZ];

	if (!*open || *fd < 0)
		return;
	for (;;) {
		ssize_t count = read(*fd, buffer, sizeof(buffer));

		if (count > 0) {
			(void)stream_append(manager, stream, buffer, (size_t)count);
			continue;
		}
		if (count == 0) {
			close_fd(fd);
			*open = 0;
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		if (errno == EIO && pty) {
			close_fd(fd);
			*open = 0;
			return;
		}
		close_fd(fd);
		*open = 0;
		return;
	}
}

static void finish_session(struct process_session *session, int status)
{
	if (WIFEXITED(status)) {
		session->exit_code = WEXITSTATUS(status);
		session->state = session->timeout_requested ?
			PROCESS_TIMED_OUT : session->kill_requested ?
			PROCESS_KILLED : PROCESS_EXITED;
	} else if (WIFSIGNALED(status)) {
		session->signal_number = WTERMSIG(status);
		session->state = session->timeout_requested ?
			PROCESS_TIMED_OUT : session->kill_requested ?
			PROCESS_KILLED : PROCESS_SIGNALED;
	}
}

static void monitor_session(struct process_manager *manager,
				    struct process_session *session,
				    uint64_t now)
{
	int status;
	pid_t result;

	if (session->state >= PROCESS_EXITED) {
		if (session->kill_deadline_ms > 0 &&
			now >= session->kill_deadline_ms) {
			(void)kill(-session->pgid, SIGKILL);
			session->kill_deadline_ms = 0;
		}
		return;
	}
	drain_fd(manager, &session->stdout_fd, &session->stdout_stream,
			 &session->stdout_open, session->pty);
	if (!session->pty)
		drain_fd(manager, &session->stderr_fd, &session->stderr_stream,
			 &session->stderr_open, 0);
	if (session->timeout_ms > 0 &&
		now >= session->started_ms + session->timeout_ms &&
		!session->timeout_requested) {
		session->timeout_requested = 1;
		(void)kill(-session->pgid, SIGTERM);
		session->kill_deadline_ms = now + manager->kill_grace_ms;
	}
	if (session->timeout_requested && session->kill_deadline_ms > 0 &&
		now >= session->kill_deadline_ms) {
		(void)kill(-session->pgid, SIGKILL);
		session->kill_deadline_ms = 0;
	}
	result = waitpid(session->pid, &status, WNOHANG);
	if (result == session->pid) {
		drain_fd(manager, &session->stdout_fd, &session->stdout_stream,
			 &session->stdout_open, session->pty);
		if (!session->pty)
			drain_fd(manager, &session->stderr_fd, &session->stderr_stream,
				 &session->stderr_open, 0);
		finish_session(session, status);
		close_session_fds(session);
		pthread_cond_broadcast(&manager->condition);
	}
}

static void *process_monitor_thread(void *opaque)
{
	struct process_manager *manager = opaque;

	for (;;) {
		struct timespec deadline;

		pthread_mutex_lock(&manager->mutex);
		if (manager->stopping) {
			pthread_mutex_unlock(&manager->mutex);
			break;
		}
		for (size_t i = 0; i < manager->session_count; i++)
			monitor_session(manager, &manager->sessions[i],
					process_now_ms());
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += PROCESS_POLL_MS * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		(void)pthread_cond_timedwait(&manager->condition,
			&manager->mutex, &deadline);
		pthread_mutex_unlock(&manager->mutex);
	}
	return NULL;
}

static int validate_workdir(const char *workdir)
{
	struct stat info;

	if (!workdir || !*workdir)
		return 0;
	if (stat(workdir, &info) != 0)
		MORPH_RETURN_ERRNO();
	if (!S_ISDIR(info.st_mode))
		MORPH_RETURN(-ENOTDIR);
	if (access(workdir, X_OK) != 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

static int session_reserve(struct process_manager *manager)
{
	struct process_session *sessions;
	size_t capacity;

	if (manager->session_count < manager->session_capacity)
		return 0;
	capacity = manager->session_capacity ? manager->session_capacity * 2 : 16;
	sessions = realloc(manager->sessions,
		capacity * sizeof(*manager->sessions));
	if (!sessions)
		MORPH_RETURN(-ENOMEM);
	manager->sessions = sessions;
	manager->session_capacity = capacity;
	return 0;
}

static int spawn_pipe_process(struct process_manager *manager,
				      const struct process_spawn_options *options,
				      struct process_session *session)
{
	int stdout_pipe[2] = {-1, -1};
	int stderr_pipe[2] = {-1, -1};
	int stdin_pipe[2] = {-1, -1};
	pid_t pid;

	if (pipe(stdout_pipe) != 0)
		MORPH_RETURN_ERRNO();
	if (pipe(stderr_pipe) != 0) {
		int rc = -errno;
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		return rc;
	}
	if (pipe(stdin_pipe) != 0) {
		int rc = -errno;
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		return rc;
	}
	pid = fork();
	if (pid < 0) {
		int rc = -errno;
		close(stdout_pipe[0]); close(stdout_pipe[1]);
		close(stderr_pipe[0]); close(stderr_pipe[1]);
		close(stdin_pipe[0]); close(stdin_pipe[1]);
		return rc;
	}
	if (pid == 0) {
		const char *shell = manager->shell[0] ? manager->shell : "/bin/bash";

		(void)setpgid(0, 0);
		if (options->workdir && chdir(options->workdir) != 0)
			_exit(126);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		close(stdin_pipe[1]);
		(void)dup2(stdout_pipe[1], STDOUT_FILENO);
		(void)dup2(stderr_pipe[1], STDERR_FILENO);
		(void)dup2(stdin_pipe[0], STDIN_FILENO);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		close(stdin_pipe[0]);
		(void)setenv("TERM", "dumb", 0);
		(void)setenv("NO_COLOR", "1", 0);
		if (options->sandbox && sandbox_enter(options->sandbox) != 0)
			_exit(126);
		execl(shell, shell, "-lc", options->command, (char *)NULL);
		_exit(127);
	}
	(void)setpgid(pid, pid);
	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	close(stdin_pipe[0]);
	session->pid = pid;
	session->pgid = pid;
	session->stdout_fd = stdout_pipe[0];
	session->stderr_fd = stderr_pipe[0];
	session->stdin_fd = stdin_pipe[1];
	if (set_nonblocking(session->stdout_fd) != 0 ||
		set_nonblocking(session->stderr_fd) != 0 ||
		set_nonblocking(session->stdin_fd) != 0) {
		int rc = -errno;

		(void)kill(-session->pgid, SIGKILL);
		(void)waitpid(session->pid, NULL, 0);
		close_session_fds(session);
		return rc;
	}
	return 0;
}

static int spawn_pty_process(struct process_manager *manager,
				     const struct process_spawn_options *options,
				     struct process_session *session)
{
#if defined(__APPLE__) || defined(__linux__)
	int master = -1;
	pid_t pid;

	pid = forkpty(&master, NULL, NULL, NULL);
	if (pid < 0)
		MORPH_RETURN_ERRNO();
	if (pid == 0) {
		const char *shell = manager->shell[0] ? manager->shell : "/bin/bash";

		(void)setpgid(0, 0);
		if (options->workdir && chdir(options->workdir) != 0)
			_exit(126);
		if (options->sandbox && sandbox_enter(options->sandbox) != 0)
			_exit(126);
		execl(shell, shell, "-lc", options->command, (char *)NULL);
		_exit(127);
	}
	(void)setpgid(pid, pid);
	session->pid = pid;
	session->pgid = pid;
	session->stdout_fd = master;
	session->stderr_fd = -1;
	session->stdin_fd = master;
	session->pty = 1;
	if (set_nonblocking(master) != 0) {
		close(master);
		return -errno;
	}
	return 0;
#else
	(void)manager;
	(void)options;
	(void)session;
	MORPH_RETURN(-ENOTSUP);
#endif
}

int process_manager_create(const struct process_manager_config *config,
				   struct process_manager **out)
{
	struct process_manager *manager;
	int rc;

	if (!out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	manager = calloc(1, sizeof(*manager));
	if (!manager)
		MORPH_RETURN(-ENOMEM);
	manager->max_session_output = config && config->max_session_output ?
		config->max_session_output : PROCESS_DEFAULT_MAX_SESSION_OUTPUT;
	manager->max_inline_output = config && config->max_inline_output ?
		config->max_inline_output : PROCESS_DEFAULT_MAX_INLINE_OUTPUT;
	manager->kill_grace_ms = config && config->kill_grace_ms ?
		config->kill_grace_ms : PROCESS_DEFAULT_KILL_GRACE_MS;
	if (config && config->shell)
		snprintf(manager->shell, sizeof(manager->shell), "%s", config->shell);
	{
		static const char *const secret_names[] = {
			"OPENAI_API_KEY", "ANTHROPIC_API_KEY", "GITHUB_TOKEN",
			"AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", NULL
		};

		for (size_t i = 0; secret_names[i]; i++) {
			const char *value = getenv(secret_names[i]);

			if (value && strlen(value) >= 4 &&
				manager->secret_count < PROCESS_SECRET_MAX) {
				manager->secrets[manager->secret_count++] =
					(struct process_secret){secret_names[i], value};
			}
		}
	}
	rc = pthread_mutex_init(&manager->mutex, NULL);
	if (rc != 0) {
		free(manager);
		MORPH_RETURN(-rc);
	}
	rc = pthread_cond_init(&manager->condition, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&manager->mutex);
		free(manager);
		MORPH_RETURN(-rc);
	}
	rc = pthread_create(&manager->thread, NULL, process_monitor_thread,
		manager);
	if (rc != 0) {
		pthread_cond_destroy(&manager->condition);
		pthread_mutex_destroy(&manager->mutex);
		free(manager);
		MORPH_RETURN(-rc);
	}
	manager->thread_started = 1;
	*out = manager;
	return 0;
}

void process_manager_destroy(struct process_manager *manager)
{
	if (!manager)
		return;
	pthread_mutex_lock(&manager->mutex);
	manager->stopping = 1;
	for (size_t i = 0; i < manager->session_count; i++) {
		struct process_session *session = &manager->sessions[i];

		if (session->state < PROCESS_EXITED ||
			session->kill_deadline_ms > 0)
			(void)kill(-session->pgid, SIGTERM);
	}
	pthread_cond_broadcast(&manager->condition);
	pthread_mutex_unlock(&manager->mutex);
	if (manager->thread_started)
		pthread_join(manager->thread, NULL);
	for (size_t i = 0; i < manager->session_count; i++) {
		struct process_session *session = &manager->sessions[i];

		if (session->state < PROCESS_EXITED ||
			session->kill_deadline_ms > 0) {
			(void)kill(-session->pgid, SIGKILL);
			if (session->state < PROCESS_EXITED)
				(void)waitpid(session->pid, NULL, 0);
		}
		close_session_fds(session);
		stream_cleanup(&session->stdout_stream);
		stream_cleanup(&session->stderr_stream);
	}
	free(manager->sessions);
	pthread_cond_destroy(&manager->condition);
	pthread_mutex_destroy(&manager->mutex);
	free(manager);
}

int process_spawn(struct process_manager *manager,
			  const struct process_spawn_options *options,
			  char session_id[PROCESS_SESSION_ID_MAX],
			  struct process_snapshot *snapshot)
{
	struct process_session *session;
	int rc;

	if (!manager || !options || !options->command || !session_id)
		MORPH_RETURN(-EINVAL);
	rc = validate_workdir(options->workdir);
	if (rc != 0)
		return rc;
	pthread_mutex_lock(&manager->mutex);
	rc = session_reserve(manager);
	if (rc != 0) {
		pthread_mutex_unlock(&manager->mutex);
		return rc;
	}
	session = &manager->sessions[manager->session_count++];
	memset(session, 0, sizeof(*session));
	session->stdout_fd = -1;
	session->stderr_fd = -1;
	session->stdin_fd = -1;
	session->state = PROCESS_STARTING;
	session->timeout_ms = options->timeout_ms;
	session->started_ms = process_now_ms();
	snprintf(session->command, sizeof(session->command), "%s",
		 options->command);
	rc = morph_random_id("proc_", session->id, sizeof(session->id));
	if (rc == 0)
		rc = stream_init(&session->stdout_stream);
	if (rc == 0)
		rc = stream_init(&session->stderr_stream);
	if (rc == 0)
		rc = options->pty ? spawn_pty_process(manager, options, session) :
		spawn_pipe_process(manager, options, session);
	if (rc != 0) {
		close_fd(&session->stdout_fd);
		close_fd(&session->stderr_fd);
		close_fd(&session->stdin_fd);
		stream_cleanup(&session->stdout_stream);
		stream_cleanup(&session->stderr_stream);
		manager->session_count--;
		pthread_mutex_unlock(&manager->mutex);
		return rc;
	}
	session->pty = options->pty;
	session->stdout_open = 1;
	session->stderr_open = !options->pty;
	session->stdin_open = 1;
	session->state = PROCESS_RUNNING;
	snprintf(session_id, PROCESS_SESSION_ID_MAX, "%s", session->id);
	pthread_cond_broadcast(&manager->condition);
	if (!options->background && options->yield_time_ms > 0) {
		uint64_t deadline = process_now_ms() + options->yield_time_ms;

		while (session->state < PROCESS_EXITED) {
			uint64_t now = process_now_ms();
			struct timespec wait_until;

			if (now >= deadline)
				break;
			clock_gettime(CLOCK_REALTIME, &wait_until);
			uint64_t remain = deadline - now;
			wait_until.tv_sec += (time_t)(remain / 1000u);
			wait_until.tv_nsec += (long)((remain % 1000u) * 1000000u);
			if (wait_until.tv_nsec >= 1000000000L) {
				wait_until.tv_sec++;
				wait_until.tv_nsec -= 1000000000L;
			}
			(void)pthread_cond_timedwait(&manager->condition,
				&manager->mutex, &wait_until);
		}
	}
	if (snapshot)
		rc = snapshot_locked(manager, session, snapshot);
	pthread_mutex_unlock(&manager->mutex);
	return rc;
}

int process_session_snapshot(struct process_manager *manager,
				     const char *session_id,
				     struct process_snapshot *snapshot)
{
	struct process_session *session;
	int rc;

	if (!manager || !session_id || !snapshot)
		MORPH_RETURN(-EINVAL);
	pthread_mutex_lock(&manager->mutex);
	session = find_session(manager, session_id);
	if (!session)
		rc = -ENOENT;
	else
		rc = snapshot_locked(manager, session, snapshot);
	pthread_mutex_unlock(&manager->mutex);
	return rc;
}

static int signal_session(struct process_manager *manager,
				  const char *session_id, int signal_number,
				  int kill_request)
{
	struct process_session *session;
	int rc = 0;

	pthread_mutex_lock(&manager->mutex);
	session = find_session(manager, session_id);
	if (!session)
		rc = -ENOENT;
	else if (session->state >= PROCESS_EXITED)
		rc = -EPIPE;
	else if (kill(-session->pgid, signal_number) != 0)
		rc = -errno;
	else if (kill_request) {
		session->kill_requested = 1;
		session->kill_deadline_ms = process_now_ms() +
			manager->kill_grace_ms;
	}
	pthread_mutex_unlock(&manager->mutex);
	return rc;
}

int process_session_interrupt(struct process_manager *manager,
				      const char *session_id)
{
	if (!manager || !session_id)
		MORPH_RETURN(-EINVAL);
	return signal_session(manager, session_id, SIGINT, 0);
}

int process_session_kill(struct process_manager *manager,
				  const char *session_id)
{
	if (!manager || !session_id)
		MORPH_RETURN(-EINVAL);
	return signal_session(manager, session_id, SIGTERM, 1);
}

int process_session_write(struct process_manager *manager,
				  const char *session_id, const char *input,
				  size_t input_len)
{
	struct process_session *session;
	size_t offset = 0;

	if (!manager || !session_id || (!input && input_len > 0))
		MORPH_RETURN(-EINVAL);
	pthread_mutex_lock(&manager->mutex);
	session = find_session(manager, session_id);
	if (!session) {
		pthread_mutex_unlock(&manager->mutex);
		MORPH_RETURN(-ENOENT);
	}
	if (session->state >= PROCESS_EXITED || !session->stdin_open) {
		pthread_mutex_unlock(&manager->mutex);
		MORPH_RETURN(-EPIPE);
	}
	while (offset < input_len) {
		ssize_t written = write(session->stdin_fd, input + offset,
			input_len - offset);

		if (written > 0) {
			offset += (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pthread_mutex_unlock(&manager->mutex);
			return -EAGAIN;
		}
		pthread_mutex_unlock(&manager->mutex);
		MORPH_RETURN_ERRNO();
	}
	pthread_mutex_unlock(&manager->mutex);
	return 0;
}

void process_snapshot_cleanup(struct process_snapshot *snapshot)
{
	if (!snapshot)
		return;
	free(snapshot->stdout_text);
	free(snapshot->stderr_text);
	memset(snapshot, 0, sizeof(*snapshot));
}

const char *process_state_name(enum process_state state)
{
	switch (state) {
	case PROCESS_STARTING: return "starting";
	case PROCESS_RUNNING: return "running";
	case PROCESS_EXITED: return "exited";
	case PROCESS_SIGNALED: return "signaled";
	case PROCESS_TIMED_OUT: return "timed_out";
	case PROCESS_KILLED: return "killed";
	default: return "unknown";
	}
}
