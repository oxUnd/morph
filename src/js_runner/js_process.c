#include "js_process.h"
#include "runner_api.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/process.h"
#include "errors.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int capture_command(JSContext *ctx, const char *command, nb_buf *output, nb_buf *error,
			   int *status, int *signal_number)
{
	struct runner_state *state = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
	int timeout_ms = 0;
	size_t capture_limit = runner_capture_limit(ctx);
	if (state && state->deadline > 0) {
		time_t remaining = state->deadline - time(NULL);
		if (remaining <= 0)
			return NB_ERR_TIMEOUT;
		timeout_ms = remaining > INT_MAX / 1000 ? INT_MAX : (int)remaining * 1000;
	}
	const char *argv[] = {"/bin/sh", "-c", command, NULL};
	nb_process_options options = {
		.argv = argv,
		.new_group = true,
		.close_fds = true,
		.stdin_mode = NB_PROCESS_NULL,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	bool truncated_out = false, truncated_err = false;
	nb_process_exchange_options exchange = {
		.timeout_ms = timeout_ms,
		.max_output = capture_limit,
		.stdout_truncated = &truncated_out,
		.stderr_truncated = &truncated_err,
	};
	rc = nb_process_exchange(child, "", 0, &exchange, output, error, status, NULL);
	*signal_number = nb_process_exit_signal(child);
	if (rc == NB_OK && (truncated_out || truncated_err))
		return NB_ERR_TOO_BIG;
	return rc;
}

static JSValue js_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf error NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *command;
	size_t command_len;
	JSValue value;
	int status = 0;
	int signal_number = 0;
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
	rc = capture_command(ctx, command, &output, &error, &status, &signal_number);
	JS_FreeCString(ctx, command);
	if (rc == NB_ERR_TOO_BIG)
		value = JS_ThrowRangeError(ctx, "command failed: output exceeded the configured "
					 "capture limit of %zu bytes", runner_capture_limit(ctx));
	else if (rc != 0)
		value = JS_ThrowInternalError(
			ctx, "command failed: %s",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	else if (signal_number != 0)
		value = JS_ThrowInternalError(ctx, "command terminated by signal %d",
					      signal_number);
	else if (status != 0)
		value = JS_ThrowInternalError(ctx, "command exited with status %d", status);
	else
		value = JS_NewStringLen(ctx, nb_buf_cstr(&output), output.len);
	nb_buf_cleanup(&error);
	nb_buf_cleanup(&output);
	return value;
}

void js_process_install(JSContext *ctx, JSValueConst target)
{
	JS_SetPropertyStr(ctx, target, "exec", JS_NewCFunction(ctx, js_exec, "exec", 1));
}
