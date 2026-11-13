#include "runtime/hooks_internal.h"
#include "nb/cleanup.h"

#include "nb/signal.h"
#include "permission/context.h"
#include "sandbox/permission.h"
#include "runtime/runtime.h"
#include "exec/process.h"
#include "agent/tokenizer.h"
#include "agent/react.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/hash.h"
#include "nb/utf8.h"
#include "cJSON.h"
#include "nb/time.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int hook_matches(struct hook_entry *entry, const char *event, const cJSON *input)
{
	const char *name;
	const char *alias = "";

	if (strcmp(entry->event, event) != 0)
		return 0;
	if (!entry->regex_ready || hook_event_ignores_matcher(event))
		return 1;
	name = hook_json_string_borrow(input, "tool_name");
	if (strcmp(event, "SessionStart") == 0)
		name = hook_json_string_borrow(input, "source");
	if (strcmp(event, "SessionEnd") == 0)
		name = hook_json_string_borrow(input, "reason");
	if (strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0)
		name = hook_json_string_borrow(input, "trigger");
	if (strcmp(name, "exec") == 0)
		alias = "Bash";
	if (regexec(&entry->regex, name, 0, NULL, 0) == 0 ||
	    (*alias && regexec(&entry->regex, alias, 0, NULL, 0) == 0))
		return 1;
	return strcmp(name, "apply_patch") == 0 &&
	       (regexec(&entry->regex, "Edit", 0, NULL, 0) == 0 ||
		regexec(&entry->regex, "Write", 0, NULL, 0) == 0);
}

int hook_manager_has(struct hook_manager *manager, const char *event)
{
	struct hook_entry *entry;

	if (!manager || !manager->ready || !manager->enabled)
		return 0;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		if (entry->supported && strcmp(entry->event, event) == 0 &&
		    hook_refresh_trust(manager, entry) == 0 &&
		    strcmp(hook_entry_state(manager, entry), "trusted") == 0)
			return 1;
	}
	return 0;
}

static void hook_emit(struct hook_manager *manager, struct hook_entry *entry, const char *name,
		      int rc, int blocked, const struct hook_run *run)
{
	cJSON *data = cJSON_CreateObject();

	if (!data)
		return;
	cJSON_AddStringToObject(data, "hook_id", entry->id);
	cJSON_AddStringToObject(data, "digest", entry->digest);
	cJSON_AddStringToObject(data, "event", entry->event);
	cJSON_AddStringToObject(data, "state", hook_entry_state(manager, entry));
	cJSON_AddBoolToObject(data, "blocked", blocked);
	if (run) {
		cJSON_AddNumberToObject(data, "duration_ms", (double)run->duration_ms);
		cJSON_AddNumberToObject(data, "exit_code", run->exit_code);
	}
	if (rc < 0) {
		cJSON_AddNumberToObject(data, "error_code", rc);
		cJSON_AddStringToObject(
			data, "error",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
	if (strcmp(name, "hook.started") != 0 && manager->recent) {
		cJSON_AddStringToObject(data, "name", name);
		if (cJSON_GetArraySize(manager->recent) >= 64)
			cJSON_DeleteItemFromArray(manager->recent, 0);
		cJSON_AddItemToArray(manager->recent, cJSON_Duplicate(data, 1));
	}
	(void)morph_event_emit_simple(
		manager->event_cb, manager->event_user, MORPH_EVENT_BACKGROUND, name,
		rc < 0				    ? "failed"
		: strcmp(name, "hook.started") == 0 ? "begin"
						    : "end",
		strcmp(name, "hook.skipped") == 0 ? "Hook skipped; inspect /hooks"
		: rc < 0			  ? "Hook failed; inspect /hooks"
		: blocked			  ? "Hook blocked processing; inspect /hooks"
		: strcmp(name, "hook.started") == 0 && *entry->status_message
			? entry->status_message
			: "Hook lifecycle",
		data);
	cJSON_Delete(data);
}

static int collect_run(struct process_manager *processes, struct hook_run *run)
{
	struct process_snapshot snapshot = {0};
	int rc = process_session_snapshot(processes, run->session, &snapshot);

	if (rc != 0)
		return rc;
	if (snapshot.stdout_truncated || snapshot.stderr_truncated ||
	    run->out.len + strlen(snapshot.stdout_text ? snapshot.stdout_text : "") >
		    HOOK_OUTPUT_MAX ||
	    run->err.len + strlen(snapshot.stderr_text ? snapshot.stderr_text : "") >
		    HOOK_OUTPUT_MAX) {
		rc = NB_ERR_TOO_BIG;
	} else {
		rc = (nb_buf_append_cstr(&run->out,
					 snapshot.stdout_text ? snapshot.stdout_text : "")
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&run->err,
						 snapshot.stderr_text ? snapshot.stderr_text : "")
				      ? 0
				      : NB_ERR_NOMEM);
	}
	if (snapshot.state >= PROCESS_EXITED) {
		run->done = 1;
		run->exit_code = snapshot.exit_code;
		if (snapshot.state == PROCESS_TIMED_OUT)
			rc = NB_ERR_TIMEOUT;
		else if (snapshot.state != PROCESS_EXITED)
			rc = NB_ERR_CANCELLED;
	}
	run->duration_ms = snapshot.duration_ms;
	process_snapshot_cleanup(&snapshot);
	return rc;
}

static uint64_t hook_now_ms(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? (uint64_t)now / 1000000u : 0u;
}

struct hook_dispatch {
	struct process_manager *processes;
	nb_vec runs;
	nb_buf context;
	nb_buf reason;
	cJSON *payload;
	cJSON *updated;
	char *wire;
};

static void hook_dispatch_cleanup(struct hook_dispatch *dispatch)
{
	process_manager_destroy(dispatch->processes);
	dispatch->processes = NULL;
	for (size_t i = 0; i < dispatch->runs.len; i++) {
		struct hook_run *item = nb_vec_at(&dispatch->runs, i);
		nb_buf_cleanup(&item->out);
		nb_buf_cleanup(&item->err);
		free(item->system_message);
	}
	nb_vec_cleanup(&dispatch->runs);
	nb_buf_cleanup(&dispatch->context);
	nb_buf_cleanup(&dispatch->reason);
	cJSON_DeleteOwner(&dispatch->payload);
	cJSON_DeleteOwner(&dispatch->updated);
	cJSON_FreeTextOwner(&dispatch->wire);
}

static int hook_dispatch_execute(struct hook_manager *manager, const char *event,
	const cJSON *input, struct hook_dispatch *dispatch, uint64_t started,
	cJSON **output)
{
	struct process_manager_config pcfg = {
		.max_session_output = HOOK_OUTPUT_MAX,
		.max_inline_output = HOOK_OUTPUT_MAX,
		.kill_grace_ms = 100,
		.shell = "/bin/sh",
	};
	struct hook_entry *entry;
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = 0, blocked = 0, ask = 0, terminate = 0;
	int progress_shown = 0;
	size_t completed_runs = 0, updated_order = 0;

	char sid[32];
	snprintf(sid, sizeof(sid), "%lld", (long long)manager->session_id);
	if (!cJSON_AddStringToObject(dispatch->payload, "session_id", sid) ||
	    !cJSON_AddNullToObject(dispatch->payload, "transcript_path") ||
	    !cJSON_AddStringToObject(dispatch->payload, "cwd", manager->workdir) ||
	    !cJSON_AddStringToObject(dispatch->payload, "hook_event_name", event) ||
	    !cJSON_AddStringToObject(dispatch->payload, "model",
				     manager->model ? manager->model : "") ||
	    !cJSON_AddStringToObject(dispatch->payload, "turn_id",
				     manager->turn_id ? manager->turn_id : "")) {
		rc = NB_ERR_NOMEM;
		return rc;
	}
	struct permission_policy policy;
	const char *preset = "default";

	if (manager->permissions &&
	    tool_context_get_permission_policy(manager->permissions, &policy) == 0)
		preset = runtime_permission_preset_name(&policy);
	if (!cJSON_AddStringToObject(dispatch->payload, "permission_mode", preset)) {
		rc = NB_ERR_NOMEM;
		return rc;
	}
	dispatch->wire = cJSON_PrintUnformatted(dispatch->payload);
	if (!dispatch->wire || strlen(dispatch->wire) > HOOK_INPUT_MAX) {
		rc = (dispatch->wire ? NB_ERR_TOO_BIG : NB_ERR_NOMEM);
		return rc;
	}
	rc = (nb_vec_init(&dispatch->runs, sizeof(struct hook_run)),
	      nb_vec_reserve(&dispatch->runs, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&dispatch->context),
		      nb_buf_reserve(&dispatch->context, 256) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && strcmp(event, "UserPromptSubmit") == 0 && manager->session_context)
		rc = (nb_buf_append_cstr(&dispatch->context, manager->session_context)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&dispatch->reason),
		      nb_buf_reserve(&dispatch->reason, 256) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = process_manager_create(&pcfg, &dispatch->processes);
	if (rc != 0)
		return rc;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		struct hook_run *run;
		FILE *stdin_file;
		struct process_spawn_options options = {.logger = manager->logger};

		if (!hook_matches(entry, event, input))
			continue;
		if (hook_refresh_trust(manager, entry) != 0 || !entry->supported ||
		    strcmp(hook_entry_state(manager, entry), "trusted") != 0) {
			continue;
		}
		if (manager->remaining_ms == 0 && strcmp(event, "Interrupt") != 0) {
			hook_emit(manager, entry, "hook.failed", NB_ERR_TIMEOUT, 0, NULL);
			continue;
		}
		run = nb_vec_push(&dispatch->runs);
		if (!run) {
			rc = (NB_ERR_NOMEM);
			return rc;
		}
		memset(run, 0, sizeof(*run));
		run->entry = entry;
		run->rc =
			(nb_buf_init(&run->out), nb_buf_reserve(&run->out, 256) ? 0 : NB_ERR_NOMEM);
		if (run->rc == 0)
			run->rc = (nb_buf_init(&run->err),
				   nb_buf_reserve(&run->err, 256) ? 0 : NB_ERR_NOMEM);
		stdin_file = tmpfile();
		if (!stdin_file) {
			run->rc = nb_status_from_errno(errno, NULL);
		} else if (run->rc == 0) {
			if (fwrite(dispatch->wire, 1, strlen(dispatch->wire), stdin_file) !=
				    strlen(dispatch->wire) ||
			    fflush(stdin_file) != 0 || fseek(stdin_file, 0, SEEK_SET) != 0)
				run->rc = NB_ERR_IO;
			if (run->rc == 0) {
				options.command = entry->command;
				options.workdir = manager->workdir;
				options.timeout_ms = (uint64_t)entry->timeout * 1000u;
				if (strcmp(event, "Interrupt") != 0 &&
				    options.timeout_ms > manager->remaining_ms)
					options.timeout_ms = manager->remaining_ms;
				if (strcmp(event, "SessionEnd") == 0 && options.timeout_ms > 1000)
					options.timeout_ms = 1000;
				options.background = 1;
				options.hook_mode = 1;
				options.use_stdin_fd = 1;
				options.stdin_fd = fileno(stdin_file);
				hook_emit(manager, entry, "hook.started", 0, 0, NULL);
				struct sandbox_config sandbox NB_CLEANUP(sandbox_config_cleanup) = {
					0};

				if (manager->permissions) {
					run->rc = sandbox_permission_prepare(
						manager->permissions, "hook", NULL, &sandbox);
					options.host_execution =
						permission_effective_access(&policy).host;
					options.sandbox = options.host_execution ? NULL : &sandbox;
				}
				if (run->rc == 0)
					run->rc = process_spawn(dispatch->processes, &options,
							run->session, NULL);
			}
		}
		if (stdin_file)
			fclose(stdin_file);
		if (run->rc != 0)
			run->done = 1;
	}
	for (;;) {
		int pending = 0;
		struct hook_run *run;
		int cancelled = strcmp(event, "Interrupt") != 0 &&
				(nb_atomic_int_load(manager->cancelled) ||
				 nb_cancel_requested(manager->cancel_token));

		for (size_t _i_ = 0; _i_ < nb_vec_len(&dispatch->runs) &&
				     (run = (struct hook_run *)nb_vec_at(&dispatch->runs, _i_));
		     _i_++) {
			if (run->done)
				continue;
			if (cancelled) {
				(void)process_session_kill(dispatch->processes, run->session);
				run->rc = NB_ERR_CANCELLED;
				run->done = 1;
				continue;
			}
			run->rc = collect_run(dispatch->processes, run);
			if (run->rc < 0) {
				(void)process_session_kill(dispatch->processes, run->session);
				run->done = 1;
			}
			if (run->done)
				run->completion_order = ++completed_runs;
			pending += !run->done;
		}
		if (cancelled) {
			rc = (NB_ERR_CANCELLED);
			break;
		}
		if (!pending)
			break;
		if (!progress_shown && hook_now_ms() - started >= 300) {
			const char *message = "Running hooks";

			if (dispatch->runs.len == 1) {
				struct hook_run *only = nb_vec_at(&dispatch->runs, 0);
				message = *only->entry->status_message ? only->entry->status_message
								       : "Running hook";
			}
			(void)morph_event_emit_simple(manager->event_cb, manager->event_user,
						      MORPH_EVENT_BACKGROUND, "hook.progress",
						      "progress", message, NULL);
			progress_shown = 1;
		}
		struct timespec delay = {.tv_nsec = 10000000};
		nanosleep(&delay, NULL);
	}
	struct hook_run *run;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&dispatch->runs) &&
			     (run = (struct hook_run *)nb_vec_at(&dispatch->runs, _i_));
	     _i_++) {
		int decision = 0;
		size_t context_before = dispatch->context.len, reason_before = dispatch->reason.len;
		cJSON *run_updated NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		int ask_before = ask, terminate_before = terminate;
		int decode_rc = hook_decode_output(
			manager, run, event, &dispatch->context, &dispatch->reason,
			&run_updated, &decision, &ask, &terminate);
		if (decode_rc < 0) {
			if (!run->context_valid)
				(void)nb_buf_truncate(&dispatch->context, context_before);
			if (!run->context_error)
				(void)nb_buf_truncate(&dispatch->reason, reason_before);
			if (run->context_error) {
				decision = 1;
				(void)nb_buf_printf(
					&dispatch->reason,
					"Hook context could not be preserved: %s. Processing is "
					"blocked.\n",
					nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
						   run->context_error));
			} else {
				ask = ask_before;
				terminate = terminate_before;
				decision = 0;
			}
		}
		if (decode_rc == 0 && run_updated && run->completion_order >= updated_order) {
			cJSON_Delete(dispatch->updated);
			dispatch->updated = cJSON_StealOwner(&run_updated);
			updated_order = run->completion_order;
		}
		blocked |= decision;
		if (run->system_message)
			(void)morph_event_emit_simple(manager->event_cb, manager->event_user,
				MORPH_EVENT_BACKGROUND, "hook.message", "end",
				run->system_message, NULL);
		hook_emit(manager, run->entry,
			  decode_rc < 0 ? "hook.failed"
			  : decision	? "hook.blocked"
					: "hook.completed",
			  decode_rc, decision, run);
		cJSON *last = cJSON_GetArrayItem(manager->recent,
						 cJSON_GetArraySize(manager->recent) - 1);
		char *diagnostic = nb_utf8_ellipsize_bytes(nb_buf_cstr(&run->err), 16384);
		if (diagnostic) {
			cJSON_AddStringToObject(last, "stderr", diagnostic);
			free(diagnostic);
		}
	}
	if (rc != 0)
		return rc;
	manager->context_bytes += dispatch->context.len;
	result = cJSON_CreateObject();
	if (!result) {
		rc = (NB_ERR_NOMEM);
		return rc;
	}
	if (strcmp(event, "Stop") == 0 && terminate)
		blocked = 0;
	if (blocked)
		cJSON_DeleteOwner(&dispatch->updated);
	if (!cJSON_AddStringToObject(result, "context", nb_buf_cstr(&dispatch->context)) ||
	    !cJSON_AddStringToObject(result, "reason", nb_buf_cstr(&dispatch->reason)) ||
	    !cJSON_AddBoolToObject(result, "blocked", blocked) ||
	    !cJSON_AddBoolToObject(result, "ask", ask) ||
	    !cJSON_AddBoolToObject(result, "terminate", terminate)) {
		rc = NB_ERR_NOMEM;
		return rc;
	}
	if (strcmp(event, "SessionStart") == 0 &&
	    strcmp(hook_json_string_borrow(input, "source"), "compact") == 0) {
		char *saved = strdup(nb_buf_cstr(&dispatch->context));

		if (!saved) {
			rc = NB_ERR_NOMEM;
			return rc;
		}
		free(manager->session_context);
		manager->session_context = saved;
	}
	if (dispatch->updated) {
		rc = (cJSON_AddItemToObject(result, "updatedInput", dispatch->updated)
			      ? (dispatch->updated = NULL, NB_OK)
			      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}

	*output = cJSON_StealOwner(&result);
	return 0;
}

int hook_manager_dispatch(void *opaque, const char *event, const cJSON *input, cJSON **output)
{
	struct hook_manager *manager = opaque;
	struct hook_dispatch dispatch NB_CLEANUP(hook_dispatch_cleanup) = {0};
	struct hook_entry *entry;
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc;
	uint64_t started = hook_now_ms();
	uint64_t elapsed;

	if (!manager || !output || !event)
		return NB_ERR_INVAL;
	if (*output)
		return NB_ERR_EXISTS;
	if (!manager->ready || !manager->enabled)
		return 0;
	int matching = 0;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		if (!hook_matches(entry, event, input))
			continue;
		if (hook_refresh_trust(manager, entry) == 0 && entry->supported &&
		    strcmp(hook_entry_state(manager, entry), "trusted") == 0)
			matching++;
		else
			hook_emit(manager, entry, "hook.skipped", 0, 0, NULL);
	}
	if (!matching) {
		if (strcmp(event, "UserPromptSubmit") == 0 && manager->session_context) {
			result = cJSON_CreateObject();
			if (!result ||
			    !cJSON_AddStringToObject(result, "context", manager->session_context))
				return NB_ERR_NOMEM;
			*output = result;
			result = NULL;
		}
		return 0;
	}
	if (strcmp(event, "Interrupt") != 0 &&
	    (nb_atomic_int_load(manager->cancelled) || nb_cancel_requested(manager->cancel_token)))
		return NB_ERR_CANCELLED;
	dispatch.payload = input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject();
	if (!dispatch.payload)
		return NB_ERR_NOMEM;
	rc = hook_dispatch_execute(manager, event, input, &dispatch, started, &result);
	elapsed = hook_now_ms() - started;
	manager->remaining_ms =
		elapsed < manager->remaining_ms ? manager->remaining_ms - elapsed : 0;
	hook_dispatch_cleanup(&dispatch);
	if (rc == 0)
		*output = cJSON_StealOwner(&result);
	return rc;
}
