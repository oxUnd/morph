#include "runtime/hooks_internal.h"
#include "util/cleanup.h"

#include "util/signals.h"
#include "permission/context.h"
#include "sandbox/permission.h"
#include "runtime/runtime.h"
#include "exec/process.h"
#include "agent/tokenizer.h"
#include "agent/react.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/utf8.h"
#include "util/json.h"

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
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
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
		cJSON_AddStringToObject(data, "error", morph_strerror(rc));
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
		rc = -E2BIG;
	} else {
		rc = morph_buf_puts(&run->out, snapshot.stdout_text ? snapshot.stdout_text : "");
		if (rc == 0)
			rc = morph_buf_puts(&run->err,
					    snapshot.stderr_text ? snapshot.stderr_text : "");
	}
	if (snapshot.state >= PROCESS_EXITED) {
		run->done = 1;
		run->exit_code = snapshot.exit_code;
		if (snapshot.state == PROCESS_TIMED_OUT)
			rc = -ETIMEDOUT;
		else if (snapshot.state != PROCESS_EXITED)
			rc = -ECANCELED;
	}
	run->duration_ms = snapshot.duration_ms;
	process_snapshot_cleanup(&snapshot);
	return rc;
}

static uint64_t hook_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct hook_dispatch {
	struct process_manager *processes;
	morph_array_t runs;
	morph_buf_t context;
	morph_buf_t reason;
	cJSON *payload;
	cJSON *updated;
	char *wire;
};

static void hook_dispatch_cleanup(struct hook_dispatch *dispatch)
{
	process_manager_destroy(dispatch->processes);
	dispatch->processes = NULL;
	for (size_t i = 0; i < dispatch->runs.nelts; i++) {
		struct hook_run *item = morph_array_borrow(&dispatch->runs, i);
		morph_buf_cleanup(&item->out);
		morph_buf_cleanup(&item->err);
	}
	morph_array_cleanup(&dispatch->runs);
	morph_buf_cleanup(&dispatch->context);
	morph_buf_cleanup(&dispatch->reason);
	morph_json_cleanup(&dispatch->payload);
	morph_json_cleanup(&dispatch->updated);
	morph_json_text_cleanup(&dispatch->wire);
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
	cJSON *result MORPH_CLEANUP(morph_json_cleanup) = NULL;
	int rc = 0, blocked = 0, ask = 0, terminate = 0;
	int progress_shown = 0;

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
		rc = -ENOMEM;
		return rc;
	}
	struct permission_policy policy;
	const char *preset = "default";

	if (manager->permissions &&
	    tool_context_get_permission_policy(manager->permissions, &policy) == 0)
		preset = runtime_permission_preset_name(&policy);
	if (!cJSON_AddStringToObject(dispatch->payload, "permission_mode", preset)) {
		rc = -ENOMEM;
		return rc;
	}
	dispatch->wire = cJSON_PrintUnformatted(dispatch->payload);
	if (!dispatch->wire || strlen(dispatch->wire) > HOOK_INPUT_MAX) {
		rc = (dispatch->wire ? -E2BIG : -ENOMEM);
		return rc;
	}
	rc = morph_array_init(&dispatch->runs, 8, sizeof(struct hook_run));
	if (rc == 0)
		rc = morph_buf_init(&dispatch->context, 256);
	if (rc == 0 && strcmp(event, "UserPromptSubmit") == 0 && manager->session_context)
		rc = morph_buf_puts(&dispatch->context, manager->session_context);
	if (rc == 0)
		rc = morph_buf_init(&dispatch->reason, 256);
	if (rc == 0)
		rc = process_manager_create(&pcfg, &dispatch->processes);
	if (rc != 0)
		return rc;
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
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
			hook_emit(manager, entry, "hook.failed", -ETIMEDOUT, 0, NULL);
			continue;
		}
		run = morph_array_push_borrow(&dispatch->runs);
		if (!run) {
			rc = (-ENOMEM);
			return rc;
		}
		memset(run, 0, sizeof(*run));
		run->entry = entry;
		run->rc = morph_buf_init(&run->out, 256);
		if (run->rc == 0)
			run->rc = morph_buf_init(&run->err, 256);
		stdin_file = tmpfile();
		if (!stdin_file) {
			run->rc = -errno;
		} else if (run->rc == 0) {
			if (fwrite(dispatch->wire, 1, strlen(dispatch->wire), stdin_file) !=
				    strlen(dispatch->wire) ||
			    fflush(stdin_file) != 0 || fseek(stdin_file, 0, SEEK_SET) != 0)
				run->rc = -EIO;
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
				struct sandbox_config sandbox MORPH_CLEANUP(
					sandbox_config_cleanup) = {0};

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
				(morph_signal_flag_get(manager->cancelled) ||
				 morph_cancel_token_is_cancelled(manager->cancel_token));

		morph_array_foreach (run, &dispatch->runs, struct hook_run) {
			if (run->done)
				continue;
			if (cancelled) {
				(void)process_session_kill(dispatch->processes, run->session);
				run->rc = -ECANCELED;
				run->done = 1;
				continue;
			}
			run->rc = collect_run(dispatch->processes, run);
			if (run->rc < 0) {
				(void)process_session_kill(dispatch->processes, run->session);
				run->done = 1;
			}
			pending += !run->done;
		}
		if (cancelled) {
			rc = (-ECANCELED);
			break;
		}
		if (!pending)
			break;
		if (!progress_shown && hook_now_ms() - started >= 300) {
			const char *message = "Running hooks";

			if (dispatch->runs.nelts == 1) {
				struct hook_run *only = morph_array_borrow(&dispatch->runs, 0);
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
	morph_array_foreach (run, &dispatch->runs, struct hook_run) {
		int decision = 0;
		size_t context_before = dispatch->context.len, reason_before = dispatch->reason.len;
		cJSON *updated_before = dispatch->updated;
		int ask_before = ask, terminate_before = terminate;
		int decode_rc = hook_decode_output(
			manager, run, event, &dispatch->context, &dispatch->reason,
			&dispatch->updated, &decision, &ask, &terminate);
		if (decode_rc == 0) {
			decode_rc = hook_context_bound(manager, &dispatch->context);
			if (decode_rc != 0)
				run->context_error = decode_rc;
		}
		if (decode_rc < 0) {
			(void)morph_buf_truncate(&dispatch->context, context_before);
			if (!run->context_error)
				(void)morph_buf_truncate(&dispatch->reason, reason_before);
			if (dispatch->updated != updated_before)
				cJSON_Delete(dispatch->updated);
			dispatch->updated = updated_before;
			if (run->context_error) {
				decision = 1;
				(void)morph_buf_printf(&dispatch->reason,
					"Hook context could not be preserved: %s. Processing is "
					"blocked.\n",
					morph_strerror(run->context_error));
			} else {
				ask = ask_before;
				terminate = terminate_before;
				decision = 0;
			}
		}
		blocked |= decision;
		if (decode_rc == 0) {
			cJSON *parsed = cJSON_Parse(morph_buf_cstr_borrow(&run->out));
			const char *message = hook_json_string_borrow(parsed, "systemMessage");
			if (*message)
				(void)morph_event_emit_simple(manager->event_cb,
							      manager->event_user,
							      MORPH_EVENT_BACKGROUND,
							      "hook.message", "end", message, NULL);
			cJSON_Delete(parsed);
		}
		hook_emit(manager, run->entry,
			  decode_rc < 0 ? "hook.failed"
			  : decision	? "hook.blocked"
					: "hook.completed",
			  decode_rc, decision, run);
		cJSON *last = cJSON_GetArrayItem(manager->recent,
						 cJSON_GetArraySize(manager->recent) - 1);
		char *diagnostic = utf8_dup_clamped(morph_buf_cstr_borrow(&run->err), 16384);
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
		rc = (-ENOMEM);
		return rc;
	}
	if (!cJSON_AddStringToObject(result, "context",
				   morph_buf_cstr_borrow(&dispatch->context)) ||
	    !cJSON_AddStringToObject(result, "reason", morph_buf_cstr_borrow(&dispatch->reason)) ||
	    !cJSON_AddBoolToObject(result, "blocked", blocked) ||
	    !cJSON_AddBoolToObject(result, "ask", ask) ||
	    !cJSON_AddBoolToObject(result, "terminate", terminate)) {
		rc = -ENOMEM;
		return rc;
	}
	if (strcmp(event, "SessionStart") == 0 &&
	    strcmp(hook_json_string_borrow(input, "source"), "compact") == 0) {
		char *saved = strdup(morph_buf_cstr_borrow(&dispatch->context));

		if (!saved) {
			rc = -ENOMEM;
			return rc;
		}
		free(manager->session_context);
		manager->session_context = saved;
	}
	if (dispatch->updated) {
		rc = morph_json_object_take(result, "updatedInput", &dispatch->updated);
		if (rc != 0)
			return rc;
	}

	*output = morph_json_steal(&result);
	return 0;
}

int hook_manager_dispatch(void *opaque, const char *event, const cJSON *input, cJSON **output)
{
	struct hook_manager *manager = opaque;
	struct hook_dispatch dispatch MORPH_CLEANUP(hook_dispatch_cleanup) = {0};
	struct hook_entry *entry;
	cJSON *result MORPH_CLEANUP(morph_json_cleanup) = NULL;
	int rc;
	uint64_t started = hook_now_ms();
	uint64_t elapsed;

	if (!manager || !output || !event)
		return -EINVAL;
	if (*output)
		return -EEXIST;
	if (!manager->ready || !manager->enabled)
		return 0;
	int matching = 0;
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
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
				return -ENOMEM;
			*output = result;
			result = NULL;
		}
		return 0;
	}
	if (strcmp(event, "Interrupt") != 0 &&
	    (morph_signal_flag_get(manager->cancelled) ||
	     morph_cancel_token_is_cancelled(manager->cancel_token)))
		return -ECANCELED;
	dispatch.payload = input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject();
	if (!dispatch.payload)
		return -ENOMEM;
	rc = hook_dispatch_execute(manager, event, input, &dispatch, started, &result);
	elapsed = hook_now_ms() - started;
	manager->remaining_ms =
		elapsed < manager->remaining_ms ? manager->remaining_ms - elapsed : 0;
	hook_dispatch_cleanup(&dispatch);
	if (rc == 0)
		*output = morph_json_steal(&result);
	return rc;
}
