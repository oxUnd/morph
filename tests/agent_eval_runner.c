#include "exec/process.h"

#include "sandbox/sandbox.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void eval_manager_cleanup(struct process_manager **owner)
{
	process_manager_destroy(*owner);
	*owner = NULL;
}

static int eval_json(int rc, const struct process_snapshot *snapshot)
{
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!report)
		return 2;
	(void)cJSON_AddBoolToObject(report, "ok",
				    rc == 0 && snapshot && snapshot->state == PROCESS_EXITED &&
					    snapshot->exit_code == 0);
	(void)cJSON_AddNumberToObject(report, "error_code", rc);
	if (rc != 0)
		(void)cJSON_AddStringToObject(report, "error", morph_strerror(rc));
	if (snapshot) {
		(void)cJSON_AddStringToObject(report, "status",
					      process_state_name(snapshot->state));
		(void)cJSON_AddNumberToObject(report, "exit_code", snapshot->exit_code);
		(void)cJSON_AddNumberToObject(report, "signal", snapshot->signal_number);
		(void)cJSON_AddNumberToObject(report, "duration_ms", (double)snapshot->duration_ms);
		(void)cJSON_AddBoolToObject(report, "timed_out",
					    snapshot->state == PROCESS_TIMED_OUT);
		(void)cJSON_AddBoolToObject(report, "setup_failed",
					    snapshot->state == PROCESS_EXITED &&
						    snapshot->exit_code == 126);
		(void)cJSON_AddStringToObject(report, "stdout",
					      snapshot->stdout_text ? snapshot->stdout_text : "");
		(void)cJSON_AddStringToObject(report, "stderr",
					      snapshot->stderr_text ? snapshot->stderr_text : "");
		(void)cJSON_AddBoolToObject(report, "output_truncated",
					    snapshot->stdout_truncated ||
						    snapshot->stderr_truncated);
		(void)cJSON_AddBoolToObject(report, "capture_failed",
					    snapshot->stdout_capture_error ||
						    snapshot->stderr_capture_error);
	}
	char *text MORPH_CLEANUP(morph_json_text_cleanup) = morph_json_print_copy(report);
	if (!text)
		return 2;
	puts(text);
	return rc == 0 && snapshot && snapshot->state == PROCESS_EXITED && snapshot->exit_code == 0
		       ? 0
		       : 1;
}

static int eval_directory(const char *path, char **out)
{
	if (!path)
		return -EINVAL;
	*out = realpath(path, NULL);
	if (!*out)
		return morph_error_from_errno(errno);
	struct stat info;
	if (stat(*out, &info) != 0)
		return morph_error_from_errno(errno);
	if (!S_ISDIR(info.st_mode) || !strcmp(*out, "/") || info.st_uid != getuid() ||
	    (info.st_mode & 0077) != 0)
		return -EACCES;
	return 0;
}

static int eval_argument(morph_buf_t *command, const char *argument)
{
	int rc = command->len ? morph_buf_putc(command, ' ') : 0;
	if (rc == 0)
		rc = morph_buf_putc(command, '\'');
	for (const char *at = argument; rc == 0 && *at; at++)
		rc = *at == '\'' ? morph_buf_puts(command, "'\\''") : morph_buf_putc(command, *at);
	return rc == 0 ? morph_buf_putc(command, '\'') : rc;
}

static int eval_execute(const char *workdir, const char *temporary, uint64_t timeout,
			const char *command)
{
	struct process_manager *manager MORPH_CLEANUP(eval_manager_cleanup) = NULL;
	struct process_snapshot snapshot MORPH_CLEANUP(process_snapshot_cleanup) = {0};
	struct process_manager_config config = {
		.shell = "/bin/sh",
		.max_session_output = 65536,
		.max_inline_output = 65536,
		.kill_grace_ms = 100,
	};
	char *paths[] = {(char *)workdir, (char *)temporary};
	struct sandbox_config sandbox = {
		.path_policy_enabled = 1,
		.read_all = 1,
		.process_exec = 1,
		.write_paths = paths,
		.write_paths_count = 2,
		.delete_paths = paths,
		.delete_paths_count = 2,
		.max_cpu_seconds = (int)(timeout / 1000u + 2u),
		.max_file_size_mb = 32,
		/* RLIMIT_NPROC counts all processes of the host user, not this child group. */
		.max_open_files = 128,
	};
	struct process_spawn_options options = {
		.command = command,
		.workdir = workdir,
		.temp_dir = temporary,
		.timeout_ms = timeout,
		.yield_time_ms = 0,
		.sandbox = &sandbox,
		.hook_mode = 1,
		.kill_descendants_on_exit = 1,
	};
	int rc = process_manager_create(&config, &manager);
	char session[PROCESS_SESSION_ID_MAX] = {0};
	if (rc == 0)
		rc = process_spawn(manager, &options, session, &snapshot);
	while (rc == 0 && snapshot.state < PROCESS_EXITED) {
		process_snapshot_cleanup(&snapshot);
		rc = process_session_wait(manager, session, NULL, 1000, NULL, NULL, &snapshot);
	}
	if (rc == 0) {
		process_snapshot_cleanup(&snapshot);
		rc = process_session_inspect(manager, session, 1, &snapshot);
	}
	return eval_json(rc, rc == 0 ? &snapshot : NULL);
}

int main(int argc, char **argv)
{
	const char *workdir = NULL, *temporary = NULL, *shell = NULL;
	uint64_t timeout = 20000;
	int first = argc;
	int rc = 0;
	for (int i = 1; i < argc && rc == 0; i++) {
		if (!strcmp(argv[i], "--")) {
			first = i + 1;
			break;
		}
		if (i + 1 >= argc) {
			rc = -EINVAL;
			break;
		}
		if (!strcmp(argv[i], "--workdir"))
			workdir = argv[++i];
		else if (!strcmp(argv[i], "--tmpdir"))
			temporary = argv[++i];
		else if (!strcmp(argv[i], "--shell"))
			shell = argv[++i];
		else if (!strcmp(argv[i], "--timeout-ms")) {
			char *end = NULL;
			errno = 0;
			unsigned long long parsed = strtoull(argv[++i], &end, 10);
			if (errno || !end || *end || !parsed || parsed > 600000)
				rc = -EINVAL;
			else
				timeout = (uint64_t)parsed;
		} else
			rc = -EINVAL;
	}
	char *work MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *temp MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_buf_t command MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (rc == 0)
		rc = eval_directory(workdir, &work);
	if (rc == 0)
		rc = eval_directory(temporary, &temp);
	if (rc == 0)
		rc = morph_buf_init(&command, 128);
	if (rc == 0 && shell && first != argc)
		rc = -EINVAL;
	if (rc == 0 && shell)
		rc = morph_buf_puts(&command, shell);
	for (int i = first; rc == 0 && !shell && i < argc; i++)
		rc = eval_argument(&command, argv[i]);
	if (rc == 0 && (!command.len || command.len >= PROCESS_COMMAND_MAX))
		rc = -EINVAL;
	return rc != 0 ? eval_json(rc, NULL)
		       : eval_execute(work, temp, timeout, morph_buf_cstr_borrow(&command));
}
