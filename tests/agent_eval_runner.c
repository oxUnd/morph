#include "exec/process.h"

#include "sandbox/sandbox.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"

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
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!report)
		return 2;
	(void)cJSON_AddBoolToObject(report, "ok",
				    rc == 0 && snapshot && snapshot->state == PROCESS_EXITED &&
					    snapshot->exit_code == 0);
	(void)cJSON_AddNumberToObject(report, "error_code", rc);
	if (rc != 0)
		(void)cJSON_AddStringToObject(
			report, "error",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
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
	char *text NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(report);
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
		return NB_ERR_INVAL;
	*out = realpath(path, NULL);
	if (!*out)
		return nb_status_from_errno(errno, NULL);
	struct stat info;
	if (stat(*out, &info) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISDIR(info.st_mode) || !strcmp(*out, "/") || info.st_uid != getuid() ||
	    (info.st_mode & 0077) != 0)
		return NB_ERR_PERMISSION;
	return 0;
}

static int eval_argument(nb_buf *command, const char *argument)
{
	int rc = command->len ? (nb_buf_append_char(command, ' ') ? 0 : NB_ERR_NOMEM) : 0;
	if (rc == 0)
		rc = (nb_buf_append_char(command, '\'') ? 0 : NB_ERR_NOMEM);
	for (const char *at = argument; rc == 0 && *at; at++)
		rc = *at == '\'' ? (nb_buf_append_cstr(command, "'\\''") ? 0 : NB_ERR_NOMEM)
				 : (nb_buf_append_char(command, *at) ? 0 : NB_ERR_NOMEM);
	return rc == 0 ? (nb_buf_append_char(command, '\'') ? 0 : NB_ERR_NOMEM) : rc;
}

static int eval_execute(const char *workdir, const char *temporary, uint64_t timeout,
			const char *command)
{
	struct process_manager *manager NB_CLEANUP(eval_manager_cleanup) = NULL;
	struct process_snapshot snapshot NB_CLEANUP(process_snapshot_cleanup) = {0};
	struct process_manager_config config = {
		.shell = "/bin/sh",
		.max_session_output = 65536,
		.max_inline_output = 65536,
		.kill_grace_ms = 100,
	};
	char *paths[] = {(char *)workdir, (char *)temporary};
	struct sandbox_config sandbox NB_CLEANUP(sandbox_config_cleanup) = {
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
			rc = NB_ERR_INVAL;
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
				rc = NB_ERR_INVAL;
			else
				timeout = (uint64_t)parsed;
		} else
			rc = NB_ERR_INVAL;
	}
	char *work NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *temp NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf command NB_CLEANUP(nb_buf_cleanup) = {0};
	if (rc == 0)
		rc = eval_directory(workdir, &work);
	if (rc == 0)
		rc = eval_directory(temporary, &temp);
	if (rc == 0)
		rc = (nb_buf_init(&command), nb_buf_reserve(&command, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && shell && first != argc)
		rc = NB_ERR_INVAL;
	if (rc == 0 && shell)
		rc = (nb_buf_append_cstr(&command, shell) ? 0 : NB_ERR_NOMEM);
	for (int i = first; rc == 0 && !shell && i < argc; i++)
		rc = eval_argument(&command, argv[i]);
	if (rc == 0 && (!command.len || command.len >= PROCESS_COMMAND_MAX))
		rc = NB_ERR_INVAL;
	return rc != 0 ? eval_json(rc, NULL)
		       : eval_execute(work, temp, timeout, nb_buf_cstr(&command));
}
