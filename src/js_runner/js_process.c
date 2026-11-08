#include "js_process.h"
#include "runner_api.h"
#include "util/buf.h"
#include "util/child_io.h"
#include "util/error.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int spawn_command(const char *command, pid_t *pid, int *output, int *error)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	int output_pipe[2] = {-1, -1};
	int error_pipe[2] = {-1, -1};
	char *const arguments[] = {"sh", "-c", (char *)command, NULL};
	short flags = POSIX_SPAWN_SETPGROUP;
	int rc;

	*output = -1;
	*error = -1;
	rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0)
		return -rc;
	rc = posix_spawnattr_init(&attributes);
	if (rc != 0) {
		posix_spawn_file_actions_destroy(&actions);
		return -rc;
	}
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
	rc = -posix_spawnattr_setflags(&attributes, flags);
	if (rc == 0)
		rc = -posix_spawnattr_setpgroup(&attributes, 0);
	if (rc == 0)
		rc = morph_io_pipe(output_pipe);
	if (rc == 0)
		rc = morph_io_pipe(error_pipe);
	if (rc == 0)
		rc = -posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
						       O_RDONLY, 0);
	if (rc == 0)
		rc = -posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
	if (rc == 0)
		rc = -posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
	for (size_t i = 0; rc == 0 && i < sizeof(output_pipe) / sizeof(output_pipe[0]); i++) {
		rc = -posix_spawn_file_actions_addclose(&actions, output_pipe[i]);
		if (rc == 0)
			rc = -posix_spawn_file_actions_addclose(&actions, error_pipe[i]);
	}
	if (rc == 0)
		rc = -posix_spawn(pid, "/bin/sh", &actions, &attributes, arguments, environ);
	if (rc == 0) {
		*output = output_pipe[0];
		*error = error_pipe[0];
		output_pipe[0] = -1;
		error_pipe[0] = -1;
	}
	for (size_t i = 0; i < sizeof(output_pipe) / sizeof(output_pipe[0]); i++) {
		if (output_pipe[i] >= 0)
			close(output_pipe[i]);
		if (error_pipe[i] >= 0)
			close(error_pipe[i]);
	}
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	return rc;
}

static int capture_command(JSContext *ctx, const char *command, morph_buf_t *output,
			   morph_buf_t *error, int *status)
{
	struct runner_state *state = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
	int output_fd = -1;
	int error_fd = -1;
	int timeout_ms = 0;
	size_t capture_limit = runner_capture_limit(ctx);
	pid_t pid;
	int rc;

	if (state && state->deadline > 0) {
		time_t remaining = state->deadline - time(NULL);

		if (remaining <= 0)
			return -ETIMEDOUT;
		timeout_ms = remaining > INT_MAX / 1000 ? INT_MAX : (int)remaining * 1000;
	}
	rc = morph_buf_init(output, BUFSIZ);
	if (rc == 0)
		rc = morph_buf_init(error, BUFSIZ);
	if (rc == 0)
		rc = spawn_command(command, &pid, &output_fd, &error_fd);
	if (rc != 0)
		return rc;
	rc = morph_child_exchange(pid, -1, output_fd, error_fd, NULL, 0, timeout_ms,
				  capture_limit + 1, NULL, NULL, output, error, status);
	close(output_fd);
	close(error_fd);
	if (rc == 0 && (output->len > capture_limit || error->len > capture_limit))
		rc = -EFBIG;
	return rc;
}

static JSValue js_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	morph_buf_t output = {0};
	morph_buf_t error = {0};
	const char *command;
	size_t command_len;
	JSValue value;
	int status = 0;
	int rc;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "exec(command) requires command");
	if (!runner_has_cap("shell") && !runner_has_cap("process"))
		return runner_throw_cap(ctx, "shell");
	command = JS_ToCStringLen(ctx, &command_len, argv[0]);
	if (!command)
		return JS_EXCEPTION;
	if (memchr(command, '\0', command_len)) {
		JS_FreeCString(ctx, command);
		return JS_ThrowTypeError(ctx, "exec command must not contain NUL bytes");
	}
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_COMMANDS", command)) {
		JS_FreeCString(ctx, command);
		return JS_ThrowTypeError(ctx, "command denied");
	}
	rc = capture_command(ctx, command, &output, &error, &status);
	JS_FreeCString(ctx, command);
	if (rc == -EFBIG)
		value = JS_ThrowRangeError(ctx, "command failed: output exceeded the configured "
					 "capture limit of %zu bytes", runner_capture_limit(ctx));
	else if (rc != 0)
		value = JS_ThrowInternalError(ctx, "command failed: %s", morph_strerror(rc));
	else if (WIFSIGNALED(status))
		value = JS_ThrowInternalError(ctx, "command terminated by signal %d",
					      WTERMSIG(status));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		value = JS_ThrowInternalError(ctx, "command exited with status %d",
					      WIFEXITED(status) ? WEXITSTATUS(status) : 1);
	else
		value = JS_NewStringLen(ctx, morph_buf_cstr_borrow(&output), output.len);
	morph_buf_cleanup(&error);
	morph_buf_cleanup(&output);
	return value;
}

void js_process_install(JSContext *ctx, JSValueConst target)
{
	JS_SetPropertyStr(ctx, target, "exec", JS_NewCFunction(ctx, js_exec, "exec", 1));
}
