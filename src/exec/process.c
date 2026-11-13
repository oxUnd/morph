#include "nb/signal.h"
#include "nb/cleanup.h"
#include "process.h"
#include "nb/process.h"
#include "sandbox.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/utf8.h"
#include "nb/time.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include "nb/thread.h"
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
	char *value;
};

struct process_stream {
	nb_buf data;
	nb_buf pending;
	char path[PATH_MAX];
	int spool_fd;
	int spool_stopped;
	int capture_error;
	size_t file_size;
	size_t total;
	size_t base;
	size_t cursor;
};

struct process_session {
	char id[PROCESS_SESSION_ID_MAX];
	nb_process *child;
	pid_t pid;
	pid_t pgid;
	int stdout_fd;
	int stderr_fd;
	int stdin_fd;
	int pty;
	int hook_mode;
	int kill_descendants_on_exit;
	int stdout_open;
	int stderr_open;
	int stdin_open;
	enum process_state state;
	int exit_code;
	int signal_number;
	uint64_t started_ms;
	uint64_t ended_ms;
	int completion_sent;
	uint64_t timeout_ms;
	uint64_t kill_deadline_ms;
	int timeout_requested;
	int kill_requested;
	struct process_stream stdout_stream;
	struct process_stream stderr_stream;
};

struct process_manager {
	nb_mutex *mutex;
	nb_cond *condition;
	nb_thread *thread;
	int thread_started;
	int stopping;
	process_completion_fn completed;
	void *completed_user_data;
	size_t spool_bytes;
	size_t max_session_output;
	size_t max_inline_output;
	uint64_t kill_grace_ms;
	char shell[PATH_MAX];
	struct process_secret secrets[PROCESS_SECRET_MAX];
	size_t secret_count;
	nb_vec sessions;
};

static void process_secrets_cleanup(struct process_manager *manager)
{
	for (size_t i = 0; i < manager->secret_count; i++) {
		free(manager->secrets[i].value);
		manager->secrets[i].value = NULL;
	}
	manager->secret_count = 0;
}

static uint64_t process_now_ms(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? (uint64_t)now / 1000000u : 0u;
}

static int set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return nb_status_from_errno(errno, NULL);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return nb_status_from_errno(errno, NULL);
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
	if (stream->path[0]) {
		(void)unlink(stream->path);
		close_fd(&stream->spool_fd);
	}
	nb_buf_cleanup(&stream->data);
	nb_buf_cleanup(&stream->pending);
	memset(stream, 0, sizeof(*stream));
	stream->spool_fd = -1;
}

static int stream_init(struct process_stream *stream)
{
	memset(stream, 0, sizeof(*stream));
	stream->spool_fd = -1;
	return (nb_buf_init(&stream->data), nb_buf_reserve(&stream->data, 4096) ? 0 : NB_ERR_NOMEM);
}

static size_t stream_prefix_boundary(const char *data, size_t length, size_t limit)
{
	size_t keep = length < limit ? length : limit;

	while (keep > 0 && keep < length && nb_utf8_is_continuation_byte((unsigned char)data[keep]))
		keep--;
	return keep;
}

static size_t stream_suffix_boundary(const char *data, size_t length, size_t start)
{
	while (start < length && nb_utf8_is_continuation_byte((unsigned char)data[start]))
		start++;
	return start;
}

/* Spools keep a stable prefix from offset zero, independent of the memory window. */
static void stream_spool_append(struct process_manager *manager, struct process_stream *stream,
				const char *data, size_t length)
{
	if (!length || stream->spool_stopped)
		return;
	if (stream->spool_fd < 0) {
		nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = nb_file_temp_in(&path, "/tmp", &stream->spool_fd, NULL);
		if (rc == NB_OK)
			rc = nb_str_copy_to(nb_buf_str(&path), stream->path, sizeof(stream->path));
		if (rc != NB_OK) {
			stream->capture_error = rc;
			stream->spool_stopped = 1;
			close_fd(&stream->spool_fd);
			if (!nb_buf_empty(&path))
				(void)nb_file_remove(nb_buf_cstr(&path), NULL);
			stream->path[0] = '\0';
			return;
		}
	}
	size_t remaining = PROCESS_SPOOL_MAX_BYTES - manager->spool_bytes;
	size_t keep = stream_prefix_boundary(data, length, remaining);
	size_t offset = 0;
	while (offset < keep) {
		ssize_t written = write(stream->spool_fd, data + offset, keep - offset);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			stream->capture_error =
				written < 0 ? nb_status_from_errno(errno, NULL) : NB_ERR_IO;
			stream->spool_stopped = 1;
			break;
		}
		offset += (size_t)written;
	}
	manager->spool_bytes += offset;
	stream->file_size += offset;
	if (offset < length)
		stream->spool_stopped = 1;
	if (stream->spool_stopped)
		close_fd(&stream->spool_fd);
}

static int stream_store(struct process_manager *manager, struct process_stream *stream,
			const char *data, size_t length)
{
	size_t drop = 0;
	int rc;

	if (!length)
		return 0;
	if (length > SIZE_MAX - stream->total)
		return NB_ERR_RANGE;
	stream_spool_append(manager, stream, data, length);
	stream->total += length;
	if (length >= manager->max_session_output) {
		drop = stream_suffix_boundary(data, length, length - manager->max_session_output);
		data += drop;
		length -= drop;
		nb_buf_clear(&stream->data);
	} else if (nb_buf_len(&stream->data) > manager->max_session_output - length) {
		drop = stream_suffix_boundary(nb_buf_cstr(&stream->data), nb_buf_len(&stream->data),
					      nb_buf_len(&stream->data) -
						      (manager->max_session_output - length));
		nb_buf_consume(&stream->data, drop);
	}
	rc = (nb_buf_append(&stream->data, data, length) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		nb_buf_clear(&stream->data);
	stream->base = stream->total - nb_buf_len(&stream->data);
	return rc;
}

/* Keep a possible secret prefix private until a later read resolves it. */
static int stream_append(struct process_manager *manager, struct process_stream *stream,
			 const char *data, size_t length, int finish)
{
	nb_buf redacted NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t position = 0;
	size_t literal = 0;
	int rc;

	if (!length && !nb_buf_len(&stream->pending)) {
		if (finish)
			close_fd(&stream->spool_fd);
		return 0;
	}
	if (stream->capture_error == NB_ERR_NOMEM)
		return NB_ERR_NOMEM;
	if (!stream->pending.ptr) {
		rc = (nb_buf_init(&stream->pending),
		      nb_buf_reserve(&stream->pending, BUFSIZ) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	rc = (nb_buf_append(&stream->pending, data, length) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = (nb_buf_init(&redacted),
	      nb_buf_reserve(&redacted, nb_buf_len(&stream->pending) + 1) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	while (position < nb_buf_len(&stream->pending)) {
		const struct process_secret *match = NULL;
		size_t matched = 0;
		size_t available = nb_buf_len(&stream->pending) - position;
		int partial = 0;

		for (size_t i = 0; i < manager->secret_count; i++) {
			const struct process_secret *secret = &manager->secrets[i];
			size_t secret_len = strlen(secret->value);
			size_t compare = available < secret_len ? available : secret_len;

			if (memcmp(nb_buf_cstr(&stream->pending) + position, secret->value,
				   compare) != 0)
				continue;
			if (available < secret_len)
				partial = !finish;
			else if (secret_len > matched) {
				match = secret;
				matched = secret_len;
			}
		}
		if (partial)
			break;
		if (!match) {
			position++;
			continue;
		}
		rc = (nb_buf_append(&redacted, nb_buf_cstr(&stream->pending) + literal,
				    position - literal)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&redacted, "[REDACTED:%s]", match->name)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		position += matched;
		literal = position;
	}
	rc = (nb_buf_append(&redacted, nb_buf_cstr(&stream->pending) + literal, position - literal)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = stream_store(manager, stream, nb_buf_cstr(&redacted), redacted.len);
	if (rc != 0)
		return rc;
	nb_buf_consume(&stream->pending, position);
	if (finish)
		close_fd(&stream->spool_fd);
	return 0;
}

static char *redact_text(const struct process_manager *manager,
			 const char *input, size_t length)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *cursor = input;
	const char *end = input + length;

	if ((nb_buf_init(&output), nb_buf_reserve(&output, length + 1) ? 0 : NB_ERR_NOMEM) != 0)
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
			(void)nb_buf_append(&output, cursor, (size_t)(end - cursor));
			break;
		}
		(void)nb_buf_append(&output, cursor, (size_t)(match - cursor));
		(void)nb_buf_printf(&output, "[REDACTED:%s]", secret->name);
		cursor = match + strlen(secret->value);
	}
	{
		char *result = (nb_buf_ok(&output) ? strdup(nb_buf_cstr(&output)) : NULL);

		nb_buf_cleanup(&output);
		return result;
	}
}

static struct process_session *find_session(struct process_manager *manager,
					    const char *id)
{
	if (!manager || !id)
		return NULL;
	for (size_t i = 0; i < manager->sessions.len; i++)
		if (strcmp(((struct process_session *)nb_vec_at(&manager->sessions, i))->id, id) ==
		    0)
			return nb_vec_at(&manager->sessions, i);
	return NULL;
}

static int append_snapshot_stream(struct process_stream *stream, char **out,
				  size_t *omitted, int *truncated, size_t limit)
{
	size_t start;
	size_t length;
	nb_buf value NB_CLEANUP(nb_buf_cleanup) = {0};

	*out = NULL;
	*omitted = stream->cursor < stream->base ? stream->base - stream->cursor : 0;
	*truncated = *omitted > 0;
	if (stream->cursor < stream->base)
		start = 0;
	else
		start = stream->cursor - stream->base;
	if (start > nb_buf_len(&stream->data))
		start = nb_buf_len(&stream->data);
	length = nb_buf_len(&stream->data) - start;
	if (length <= limit) {
		*out = strndup(nb_buf_cstr(&stream->data) + start, length);
		return *out ? 0 : NB_ERR_NOMEM;
	}
	*truncated = 1;
	*omitted += length - limit;
	if ((nb_buf_init(&value), nb_buf_reserve(&value, limit + 64) ? 0 : NB_ERR_NOMEM) != 0)
		return NB_ERR_NOMEM;
	{
		size_t head = stream_prefix_boundary(nb_buf_cstr(&stream->data) + start, length,
						     limit / 4);
		size_t tail_start = stream_suffix_boundary(
			nb_buf_cstr(&stream->data), nb_buf_len(&stream->data),
			nb_buf_len(&stream->data) - (limit - limit / 4));
		size_t tail = nb_buf_len(&stream->data) - tail_start;

		(void)nb_buf_append(&value, nb_buf_cstr(&stream->data) + start, head);
		(void)nb_buf_append_cstr(&value, "\n...[output truncated]...\n");
		(void)nb_buf_append(&value, nb_buf_cstr(&stream->data) + tail_start, tail);
		*omitted += limit - head - tail;
	}
	{
		*out = (nb_buf_ok(&value) ? strdup(nb_buf_cstr(&value)) : NULL);
	}
	nb_buf_cleanup(&value);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int snapshot_budget_locked(struct process_session *session,
				   struct process_snapshot *snapshot, size_t limit)
{
	struct process_snapshot candidate NB_CLEANUP(process_snapshot_cleanup) = {0};

	if (snapshot->stdout_text || snapshot->stderr_text)
		return NB_ERR_EXISTS;
	candidate.state = session->state;
	candidate.exit_code = session->exit_code;
	candidate.signal_number = session->signal_number;
	candidate.duration_ms =
		(session->ended_ms ? session->ended_ms : process_now_ms()) - session->started_ms;
	if (append_snapshot_stream(&session->stdout_stream, &candidate.stdout_text,
				   &candidate.stdout_omitted_bytes, &candidate.stdout_truncated,
				   limit) != 0)
		return NB_ERR_NOMEM;
	if (append_snapshot_stream(&session->stderr_stream, &candidate.stderr_text,
				   &candidate.stderr_omitted_bytes, &candidate.stderr_truncated,
				   limit) != 0) {
		return NB_ERR_NOMEM;
	}
	session->stdout_stream.cursor =
		session->stdout_stream.base + nb_buf_len(&session->stdout_stream.data);
	session->stderr_stream.cursor =
		session->stderr_stream.base + nb_buf_len(&session->stderr_stream.data);
	candidate.next_cursor.stdout_offset = session->stdout_stream.cursor;
	candidate.next_cursor.stderr_offset = session->stderr_stream.cursor;
	const struct process_stream *output = &session->stdout_stream;
	const struct process_stream *error = &session->stderr_stream;
	snprintf(candidate.stdout_path, sizeof(candidate.stdout_path), "%s", output->path);
	snprintf(candidate.stderr_path, sizeof(candidate.stderr_path), "%s", error->path);
	candidate.stdout_total_bytes = output->total;
	candidate.stderr_total_bytes = error->total;
	candidate.stdout_path_bytes = output->file_size;
	candidate.stderr_path_bytes = error->file_size;
	candidate.stdout_retained_from = output->base;
	candidate.stderr_retained_from = error->base;
	candidate.stdout_unrecoverable_bytes = output->base > output->file_size ?
		output->base - output->file_size : 0;
	candidate.stderr_unrecoverable_bytes = error->base > error->file_size ?
		error->base - error->file_size : 0;
	candidate.stdout_path_truncated = output->spool_stopped || output->capture_error ||
		output->file_size < output->total;
	candidate.stderr_path_truncated = error->spool_stopped || error->capture_error ||
		error->file_size < error->total;
	candidate.stdout_capture_error = output->capture_error;
	candidate.stderr_capture_error = error->capture_error;
	*snapshot = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

static int snapshot_locked(struct process_manager *manager,
	struct process_session *session, struct process_snapshot *snapshot)
{
	return snapshot_budget_locked(session, snapshot, manager->max_inline_output);
}

static void stream_finish(struct process_manager *manager, struct process_stream *stream)
{
	int rc = stream_append(manager, stream, NULL, 0, 1);

	if (rc != 0)
		stream->capture_error = rc;
}

static void drain_fd(struct process_manager *manager, int *fd,
			     struct process_stream *stream, int *open, int pty)
{
	char buffer[BUFSIZ];

	if (!*open || *fd < 0)
		return;
	/* Bound each pass so a continuously writing child cannot starve timeouts. */
	for (int batch = 0; batch < 64; batch++) {
		ssize_t count = read(*fd, buffer, sizeof(buffer));

		if (count > 0) {
			int rc = stream_append(manager, stream, buffer, (size_t)count, 0);

			if (rc != 0)
				stream->capture_error = rc;
			continue;
		}
		if (count == 0) {
			stream_finish(manager, stream);
			close_fd(fd);
			*open = 0;
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		if (errno == EIO && pty) {
			stream_finish(manager, stream);
			close_fd(fd);
			*open = 0;
			return;
		}
		stream->capture_error = nb_status_from_errno(errno, NULL);
		stream_finish(manager, stream);
		close_fd(fd);
		*open = 0;
		return;
	}
}

static void finish_session(struct process_session *session, int exit_code)
{
	session->ended_ms = process_now_ms();
	session->exit_code = exit_code;
	session->signal_number = nb_process_exit_signal(session->child);
	session->state = session->timeout_requested ? PROCESS_TIMED_OUT
			 : session->kill_requested  ? PROCESS_KILLED
			 : session->signal_number   ? PROCESS_SIGNALED
						    : PROCESS_EXITED;
}

static void monitor_session(struct process_manager *manager,
				    struct process_session *session,
				    uint64_t now)
{
	int exit_code = 0;
	bool exited = false;

	if (session->state >= PROCESS_EXITED) {
		if (session->kill_deadline_ms > 0 &&
			now >= session->kill_deadline_ms) {
			(void)nb_process_signal(session->child, SIGKILL, NULL);
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
		(void)nb_process_signal(session->child, SIGTERM, NULL);
		session->kill_deadline_ms = now + manager->kill_grace_ms;
	}
	if (session->kill_deadline_ms > 0 &&
		now >= session->kill_deadline_ms) {
		(void)nb_process_signal(session->child, SIGKILL, NULL);
		session->kill_deadline_ms = 0;
	}
	(void)nb_process_wait(session->child, 0, &exit_code, &exited, NULL);
	if (exited) {
		drain_fd(manager, &session->stdout_fd, &session->stdout_stream,
			 &session->stdout_open, session->pty);
		if (!session->pty)
			drain_fd(manager, &session->stderr_fd, &session->stderr_stream,
				 &session->stderr_open, 0);
		if (session->hook_mode || session->kill_descendants_on_exit)
			(void)nb_process_signal(session->child, SIGKILL, NULL);
		stream_finish(manager, &session->stdout_stream);
		stream_finish(manager, &session->stderr_stream);
		finish_session(session, exit_code);
		close_session_fds(session);
		nb_cond_broadcast(manager->condition);
	}
}

static void *process_monitor_thread(void *opaque)
{
	struct process_manager *manager = opaque;

	for (;;) {
		nb_mutex_lock(manager->mutex);
		if (manager->stopping) {
			nb_mutex_unlock(manager->mutex);
			break;
		}
		for (size_t i = 0; i < manager->sessions.len; i++)
			monitor_session(manager, nb_vec_at(&manager->sessions, i),
					process_now_ms());
		/* Completion callbacks may inspect a session without deadlocking. */
		for (size_t i = 0; manager->completed && i < manager->sessions.len; i++) {
			struct process_session *session = nb_vec_at(&manager->sessions, i);
			char id[PROCESS_SESSION_ID_MAX];

			if (session->state < PROCESS_EXITED || session->completion_sent)
				continue;
			session->completion_sent = 1;
			snprintf(id, sizeof(id), "%s", session->id);
			nb_mutex_unlock(manager->mutex);
			manager->completed(id, manager->completed_user_data);
			nb_mutex_lock(manager->mutex);
		}
		(void)nb_cond_timedwait(manager->condition, manager->mutex,
					PROCESS_POLL_MS * INT64_C(1000000));
		nb_mutex_unlock(manager->mutex);
	}
	return NULL;
}

static int validate_workdir(const char *workdir)
{
	struct stat info;

	if (!workdir || !*workdir)
		return 0;
	if (stat(workdir, &info) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISDIR(info.st_mode))
		return NB_ERR_NOT_DIR;
	if (access(workdir, X_OK) != 0)
		return nb_status_from_errno(errno, NULL);
	return 0;
}

static int prepare_child_execution(const struct process_spawn_options *options)
{
	if (options->temp_dir && setenv("TMPDIR", options->temp_dir, 1) != 0)
		return nb_status_from_errno(errno, NULL);
	/* The forked child must not acquire a logger mutex held by another
	 * parent worker. This only changes its private post-fork copy. */
	if (options->sandbox)
		options->sandbox->logger = NULL;
	int rc = options->sandbox ? sandbox_enter(options->sandbox) : NB_OK;
	if (rc == 0 && options->temp_dir) {
		if (setenv("TMPDIR", options->temp_dir, 1) != 0 ||
		    setenv("MORPH_AGENT_SCRATCH", options->temp_dir, 1) != 0)
			rc = nb_status_from_errno(errno, NULL);
	}
	return rc;
}

static nb_status process_prepare_child(void *opaque)
{
	const struct process_spawn_options *options = opaque;
	if (options->hook_mode) {
		static const char *const allowed[] = {"PATH", "HOME", "TMPDIR", "LANG", "LC_ALL"};
		int rc = sandbox_apply_env(NULL, allowed, 5, 0);
		if (rc != NB_OK)
			return (nb_status)rc;
		if (setenv("MORPH_HOOK_ACTIVE", "1", 1) != 0)
			return nb_status_from_errno(errno, NULL);
	}
	if (!options->pty) {
		(void)setenv("TERM", "dumb", 0);
		(void)setenv("NO_COLOR", "1", 0);
	}
	return (nb_status)prepare_child_execution(options);
}

static int process_start_child(struct process_manager *manager,
			       const struct process_spawn_options *options,
			       struct process_session *session)
{
	const char *shell = process_manager_shell_borrow(manager);
	const char *argv[] = {shell, options->hook_mode ? "-c" : "-lc", options->command, NULL};
	nb_process_options child_options = {
		.argv = argv,
		.cwd = options->workdir,
		.new_session = !options->pty,
		.new_group = true,
		.close_fds = true,
		.pty = options->pty != 0,
		.stdin_mode = options->use_stdin_fd ? NB_PROCESS_FD : NB_PROCESS_PIPE,
		.stdin_fd = options->stdin_fd,
		.prepare = process_prepare_child,
		.prepare_ctx = (void *)options,
	};
	int rc = nb_process_spawn(&child_options, &session->child, NULL);
	if (rc != NB_OK)
		return rc;
	session->pid = (pid_t)nb_process_id(session->child);
	session->pgid = session->pid;
	session->stdout_fd = nb_process_take_stdout(session->child);
	session->stderr_fd = nb_process_take_stderr(session->child);
	session->stdin_fd = nb_process_take_stdin(session->child);
	session->pty = options->pty;
	if (set_nonblocking(session->stdout_fd) != NB_OK ||
	    (session->stderr_fd >= 0 && set_nonblocking(session->stderr_fd) != NB_OK) ||
	    (session->stdin_fd >= 0 && set_nonblocking(session->stdin_fd) != NB_OK)) {
		rc = nb_status_from_errno(errno, NULL);
		(void)nb_process_terminate(session->child, 0);
		return rc;
	}
	return NB_OK;
}

int process_manager_create(const struct process_manager_config *config,
				   struct process_manager **out)
{
	struct process_manager *manager;
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	manager = calloc(1, sizeof(*manager));
	if (!manager)
		return NB_ERR_NOMEM;
	nb_vec_init(&manager->sessions, sizeof(struct process_session));
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
				char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(value);

				if (!copy) {
					process_secrets_cleanup(manager);
					free(manager);
					return NB_ERR_NOMEM;
				}
				manager->secrets[manager->secret_count++] = (struct process_secret){
					secret_names[i], nb_charp_steal(&copy)};
			}
		}
	}
	rc = ((manager->mutex = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0) {
		process_secrets_cleanup(manager);
		free(manager);
		return rc;
	}
	rc = ((manager->condition = nb_cond_new()) ? NB_OK : NB_ERR_NOMEM);
	if (rc != 0) {
		(nb_mutex_free(manager->mutex), manager->mutex = NULL);
		process_secrets_cleanup(manager);
		free(manager);
		return rc;
	}
	manager->completed = config ? config->completed : NULL;
	manager->completed_user_data = config ? config->completed_user_data : NULL;
	rc = nb_thread_create(&manager->thread, process_monitor_thread, manager, NULL);
	if (rc != 0) {
		(nb_cond_free(manager->condition), manager->condition = NULL);
		(nb_mutex_free(manager->mutex), manager->mutex = NULL);
		process_secrets_cleanup(manager);
		free(manager);
		return rc;
	}
	manager->thread_started = 1;
	*out = manager;
	return 0;
}

void process_manager_destroy(struct process_manager *manager)
{
	if (!manager)
		return;
	nb_mutex_lock(manager->mutex);
	manager->stopping = 1;
	for (size_t i = 0; i < manager->sessions.len; i++) {
		struct process_session *session = nb_vec_at(&manager->sessions, i);

		if (session->state < PROCESS_EXITED ||
			session->kill_deadline_ms > 0)
			(void)nb_process_signal(session->child, SIGTERM, NULL);
	}
	nb_cond_broadcast(manager->condition);
	nb_mutex_unlock(manager->mutex);
	if (manager->thread_started)
		nb_thread_join_take(&manager->thread, NULL);
	for (size_t i = 0; i < manager->sessions.len; i++) {
		struct process_session *session = nb_vec_at(&manager->sessions, i);

		if (session->state < PROCESS_EXITED ||
			session->kill_deadline_ms > 0) {
			(void)nb_process_signal(session->child, SIGKILL, NULL);
			if (session->state < PROCESS_EXITED)
				(void)nb_process_wait(session->child, -1, NULL, NULL, NULL);
		}
		close_session_fds(session);
		nb_process_free(session->child);
		session->child = NULL;
		stream_cleanup(&session->stdout_stream);
		stream_cleanup(&session->stderr_stream);
	}
	nb_vec_destroy(&manager->sessions);
	process_secrets_cleanup(manager);
	(nb_cond_free(manager->condition), manager->condition = NULL);
	(nb_mutex_free(manager->mutex), manager->mutex = NULL);
	free(manager);
}

int process_spawn(struct process_manager *manager,
			  const struct process_spawn_options *options,
			  char session_id[PROCESS_SESSION_ID_MAX],
			  struct process_snapshot *snapshot)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	struct process_session *session;
	/*
	 * The index of this session, rather than its address. session_reserve()
	 * can grow manager->sessions while this call is parked in the wait
	 * loop below, which moves the array; an index survives that, a cached
	 * pointer does not.
	 */
	size_t session_index;
	int rc;

	if (!manager || !options || !options->command || !session_id)
		return NB_ERR_INVAL;
	if (snapshot && (snapshot->stdout_text || snapshot->stderr_text))
		return NB_ERR_EXISTS;
	session_id[0] = '\0';
	if (options->cancelled && options->cancelled(options->cancel_data))
		return NB_ERR_CANCELLED;
	rc = validate_workdir(options->workdir);
	if (rc != 0)
		return rc;
	if (options->sandbox) {
		rc = sandbox_prepare_temporary_paths(options->sandbox, options->temp_dir);
		if (rc != NB_OK)
			return rc;
	}
	nb_mutex_lock(manager->mutex);
	rc = nb_vec_reserve(&manager->sessions, manager->sessions.len + 1) ? 0 : NB_ERR_NOMEM;
	if (rc != 0) {
		nb_mutex_unlock(manager->mutex);
		return rc;
	}
	session = nb_vec_push(&manager->sessions);
	session_index = manager->sessions.len - 1;
	memset(session, 0, sizeof(*session));
	session->stdout_fd = -1;
	session->stderr_fd = -1;
	session->stdin_fd = -1;
	session->state = PROCESS_STARTING;
	session->timeout_ms = options->timeout_ms;
	session->started_ms = process_now_ms();
	rc = ((file_status_1 =
		       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
			: !(nb_buf_clear(&file_value_1), nb_buf_append_cstr(&file_value_1, "proc_"))
				? NB_ERR_NOMEM
				: nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
		      ? file_status_1
	      : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), session->id,
						sizeof(session->id))) == NB_ERR_RANGE
		      ? NB_ERR_NO_SPACE
		      : file_status_1);
	if (rc == 0)
		rc = stream_init(&session->stdout_stream);
	if (rc == 0)
		rc = stream_init(&session->stderr_stream);
	if (rc == 0)
		rc = process_start_child(manager, options, session);
	if (rc != 0) {
		close_fd(&session->stdout_fd);
		close_fd(&session->stderr_fd);
		close_fd(&session->stdin_fd);
		nb_process_free(session->child);
		session->child = NULL;
		stream_cleanup(&session->stdout_stream);
		stream_cleanup(&session->stderr_stream);
		nb_vec_pop(&manager->sessions);
		nb_mutex_unlock(manager->mutex);
		return rc;
	}
	session->hook_mode = options->hook_mode;
	session->kill_descendants_on_exit = options->kill_descendants_on_exit;
	session->pty = options->pty;
	session->stdout_open = 1;
	session->stderr_open = !options->pty;
	session->stdin_open = 1;
	session->state = PROCESS_RUNNING;
	snprintf(session_id, PROCESS_SESSION_ID_MAX, "%s", session->id);
	nb_cond_broadcast(manager->condition);
	if (!options->background && options->yield_time_ms > 0) {
		uint64_t deadline = process_now_ms() + options->yield_time_ms;

		for (;;) {
			uint64_t now = process_now_ms();

			/*
			 * Re-resolve the session before touching it:
			 * nb_cond_timedwait() released the mutex, so a
			 * concurrent spawn may have grown the array and freed
			 * the block this call was pointing at.
			 */
			session = nb_vec_at(&manager->sessions, session_index);
			if (options->cancelled && options->cancelled(options->cancel_data)) {
				rc = NB_ERR_CANCELLED;
				break;
			}
			if (session->state >= PROCESS_EXITED)
				break;
			if (now >= deadline)
				break;
			uint64_t remain = deadline - now;
			if (options->cancelled && remain > PROCESS_POLL_MS)
				remain = PROCESS_POLL_MS;
			int64_t duration = remain > (uint64_t)INT64_MAX / 1000000u
						   ? INT64_MAX
						   : (int64_t)remain * INT64_C(1000000);
			(void)nb_cond_timedwait(manager->condition, manager->mutex, duration);
		}
		session = nb_vec_at(&manager->sessions, session_index);
	}
	if (rc == 0 && snapshot)
		rc = snapshot_locked(manager, session, snapshot);
	nb_mutex_unlock(manager->mutex);
	return rc;
}

static int session_collect(struct process_manager *manager, const char *session_id,
			 const struct process_cursor *cursor, uint64_t wait_ms,
			 int (*cancelled)(void *), void *cancel_data,
			 struct process_snapshot *snapshot, int until_exit)
{
	uint64_t deadline;
	int rc = 0;
	if (!manager || !session_id || !snapshot)
		return NB_ERR_INVAL;
	if (snapshot->stdout_text || snapshot->stderr_text)
		return NB_ERR_EXISTS;
	uint64_t now = process_now_ms();
	if (wait_ms > UINT64_MAX - now)
		return NB_ERR_RANGE;
	deadline = now + wait_ms;
	nb_mutex_lock(manager->mutex);
	for (;;) {
		struct process_session *session = find_session(manager, session_id);
		struct process_cursor start;
		if (!session) {
			rc = NB_ERR_NOT_FOUND;
			break;
		}
		start.stdout_offset =
		    cursor ? cursor->stdout_offset : session->stdout_stream.cursor;
		start.stderr_offset =
		    cursor ? cursor->stderr_offset : session->stderr_stream.cursor;
		if (start.stdout_offset > session->stdout_stream.base +
						  nb_buf_len(&session->stdout_stream.data) ||
		    start.stderr_offset > session->stderr_stream.base +
						  nb_buf_len(&session->stderr_stream.data)) {
			rc = NB_ERR_INVAL;
			break;
		}
		if (cancelled && cancelled(cancel_data)) {
			rc = NB_ERR_CANCELLED;
			break;
		}
		if (session->state >= PROCESS_EXITED || process_now_ms() >= deadline ||
		    (!until_exit &&
		     (session->stdout_stream.base + nb_buf_len(&session->stdout_stream.data) >
			      start.stdout_offset ||
		      session->stderr_stream.base + nb_buf_len(&session->stderr_stream.data) >
			      start.stderr_offset))) {
			size_t saved_out = session->stdout_stream.cursor;
			size_t saved_err = session->stderr_stream.cursor;
			session->stdout_stream.cursor = start.stdout_offset;
			session->stderr_stream.cursor = start.stderr_offset;
			rc = snapshot_locked(manager, session, snapshot);
			if (cursor || rc != 0) {
				session->stdout_stream.cursor = saved_out;
				session->stderr_stream.cursor = saved_err;
			}
			break;
		}
		(void)nb_cond_timedwait(manager->condition, manager->mutex, INT64_C(25000000));
		/* Never hold a session pointer across a wait: the array can grow. */
	}
	nb_mutex_unlock(manager->mutex);
	return rc;
}

int process_session_poll(struct process_manager *manager, const char *session_id,
			 const struct process_cursor *cursor, uint64_t wait_ms,
			 int (*cancelled)(void *), void *cancel_data,
			 struct process_snapshot *snapshot)
{
	return session_collect(manager, session_id, cursor, wait_ms, cancelled,
			       cancel_data, snapshot, 0);
}

int process_session_wait(struct process_manager *manager, const char *session_id,
			 const struct process_cursor *cursor, uint64_t wait_ms,
			 int (*cancelled)(void *), void *cancel_data,
			 struct process_snapshot *snapshot)
{
	return session_collect(manager, session_id, cursor, wait_ms, cancelled,
			       cancel_data, snapshot, 1);
}

char *process_redact_text_copy(const struct process_manager *manager, const char *text)
{
	return manager && text ? redact_text(manager, text, strlen(text)) : NULL;
}

int process_session_inspect(struct process_manager *manager, const char *session_id,
			    int full_output, struct process_snapshot *snapshot)
{
	if (!manager || !session_id || !snapshot)
		return NB_ERR_INVAL;
	if (snapshot->stdout_text || snapshot->stderr_text)
		return NB_ERR_EXISTS;
	nb_mutex_lock(manager->mutex);
	struct process_session *session = find_session(manager, session_id);
	int rc = NB_ERR_NOT_FOUND;
	if (session) {
		size_t saved_out = session->stdout_stream.cursor;
		size_t saved_err = session->stderr_stream.cursor;
			session->stdout_stream.cursor = 0;
			session->stderr_stream.cursor = 0;
		rc = snapshot_budget_locked(session, snapshot,
			full_output ? manager->max_session_output : manager->max_inline_output);
		session->stdout_stream.cursor = saved_out;
		session->stderr_stream.cursor = saved_err;
	}
	nb_mutex_unlock(manager->mutex);
	return rc;
}

int process_session_status(struct process_manager *manager, const char *session_id,
			   struct process_snapshot *snapshot)
{
	if (!manager || !session_id || !snapshot)
		return NB_ERR_INVAL;
	if (snapshot->stdout_text || snapshot->stderr_text)
		return NB_ERR_EXISTS;
	nb_mutex_lock(manager->mutex);
	struct process_session *session = find_session(manager, session_id);
	if (session) {
		memset(snapshot, 0, sizeof(*snapshot));
		snapshot->state = session->state;
		snapshot->exit_code = session->exit_code;
		snapshot->signal_number = session->signal_number;
		snapshot->duration_ms =
			(session->ended_ms ? session->ended_ms : process_now_ms()) -
			session->started_ms;
	}
	int found = session != NULL;
	nb_mutex_unlock(manager->mutex);
	return found ? 0 : NB_ERR_NOT_FOUND;
}

int process_session_snapshot(struct process_manager *manager,
				     const char *session_id,
				     struct process_snapshot *snapshot)
{
	struct process_session *session;
	int rc;

	if (!manager || !session_id || !snapshot)
		return NB_ERR_INVAL;
	if (snapshot->stdout_text || snapshot->stderr_text)
		return NB_ERR_EXISTS;
	nb_mutex_lock(manager->mutex);
	session = find_session(manager, session_id);
	if (!session)
		rc = NB_ERR_NOT_FOUND;
	else
		rc = snapshot_locked(manager, session, snapshot);
	nb_mutex_unlock(manager->mutex);
	return rc;
}

static int signal_session(struct process_manager *manager,
				  const char *session_id, int signal_number,
				  int kill_request)
{
	struct process_session *session;
	int rc = 0;

	nb_mutex_lock(manager->mutex);
	session = find_session(manager, session_id);
	if (!session)
		rc = NB_ERR_NOT_FOUND;
	else if (session->state >= PROCESS_EXITED)
		rc = 0;
	else if ((rc = nb_process_signal(session->child, signal_number, NULL)) != NB_OK)
		rc = rc == NB_ERR_STATE ? NB_OK : rc;
	else if (kill_request && !session->kill_requested) {
		session->kill_requested = 1;
		session->kill_deadline_ms = process_now_ms() +
			manager->kill_grace_ms;
	}
	nb_mutex_unlock(manager->mutex);
	return rc;
}

int process_session_interrupt(struct process_manager *manager,
				      const char *session_id)
{
	if (!manager || !session_id)
		return NB_ERR_INVAL;
	return signal_session(manager, session_id, SIGINT, 0);
}

int process_session_kill(struct process_manager *manager,
				  const char *session_id)
{
	if (!manager || !session_id)
		return NB_ERR_INVAL;
	return signal_session(manager, session_id, SIGTERM, 1);
}

int process_session_write(struct process_manager *manager,
				  const char *session_id, const char *input,
				  size_t input_len)
{
	struct process_session *session;
	size_t offset = 0;

	if (!manager || !session_id || (!input && input_len > 0))
		return NB_ERR_INVAL;
	nb_mutex_lock(manager->mutex);
	session = find_session(manager, session_id);
	if (!session) {
		nb_mutex_unlock(manager->mutex);
		return NB_ERR_NOT_FOUND;
	}
	if (session->state >= PROCESS_EXITED || !session->stdin_open) {
		nb_mutex_unlock(manager->mutex);
		return NB_ERR_BROKEN_PIPE;
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
			nb_mutex_unlock(manager->mutex);
			return NB_ERR_AGAIN;
		}
		nb_mutex_unlock(manager->mutex);
		return nb_status_from_errno(errno, NULL);
	}
	nb_mutex_unlock(manager->mutex);
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

const char *process_manager_shell_borrow(const struct process_manager *manager)
{
	return manager && manager->shell[0] ? manager->shell : "/bin/bash";
}
