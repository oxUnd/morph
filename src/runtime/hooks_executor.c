#include "runtime/hooks_internal.h"

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

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int hook_matches(struct hook_entry *entry, const char *event, const struct json *input)
{
	const char *name;
	const char *alias = "";

	if (strcmp(entry->event, event) != 0)
		return 0;
	if (!entry->regex_ready || hook_event_ignores_matcher(event))
		return 1;
	name = json_str_or(input, "tool_name", "");
	if (strcmp(event, "SessionStart") == 0)
		name = json_str_or(input, "source", "");
	if (strcmp(event, "SessionEnd") == 0)
		name = json_str_or(input, "reason", "");
	if (strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0)
		name = json_str_or(input, "trigger", "");
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
	struct json *data = json_object();

	if (!data)
		return;
	json_add_string(data, "hook_id", entry->id);
	json_add_string(data, "digest", entry->digest);
	json_add_string(data, "event", entry->event);
	json_add_string(data, "state", hook_entry_state(manager, entry));
	json_add_bool(data, "blocked", blocked);
	if (run) {
		json_add_number(data, "duration_ms", (double)run->duration_ms);
		json_add_number(data, "exit_code", run->exit_code);
	}
	if (rc < 0) {
		json_add_number(data, "error_code", rc);
		json_add_string(data, "error", morph_strerror(rc));
	}
	if (strcmp(name, "hook.started") != 0 && manager->recent) {
		json_add_string(data, "name", name);
		if (json_count(manager->recent) >= 64)
			json_array_remove(manager->recent, 0);
		json_array_add(manager->recent, json_clone(data, 1));
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
	json_free(data);
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

int hook_manager_dispatch(void *opaque, const char *event, const struct json *input,
			  struct json **output)
{
	struct hook_manager *manager = opaque;
	struct process_manager *processes = NULL;
	struct process_manager_config pcfg = {
		.max_session_output = HOOK_OUTPUT_MAX,
		.max_inline_output = HOOK_OUTPUT_MAX,
		.kill_grace_ms = 100,
		.shell = "/bin/sh",
	};
	morph_array_t runs = {0};
	morph_buf_t context = {0}, reason = {0};
	struct hook_entry *entry;
	struct json *payload = NULL, *updated = NULL;
	char *wire = NULL;
	int rc = 0, blocked = 0, ask = 0, terminate = 0;
	uint64_t started = hook_now_ms();
	uint64_t elapsed;
	int progress_shown = 0;

	if (!manager || !output || !event)
		return -EINVAL;
	*output = NULL;
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
			*output = json_object();
			if (!*output ||
			    !json_add_string(*output, "context", manager->session_context))
				return -ENOMEM;
		}
		return 0;
	}
	if (strcmp(event, "Interrupt") != 0 &&
	    (morph_signal_flag_get(manager->cancelled) ||
	     morph_cancel_token_is_cancelled(manager->cancel_token)))
		return -ECANCELED;
	payload = input ? json_clone(input, 1) : json_object();
	if (!payload)
		return -ENOMEM;
	char sid[32];
	snprintf(sid, sizeof(sid), "%lld", (long long)manager->session_id);
	json_add_string(payload, "session_id", sid);
	json_add_null(payload, "transcript_path");
	json_add_string(payload, "cwd", manager->workdir);
	json_add_string(payload, "hook_event_name", event);
	json_add_string(payload, "model", manager->model ? manager->model : "");
	json_add_string(payload, "turn_id", manager->turn_id ? manager->turn_id : "");
	struct permission_policy policy;
	const char *preset = "default";

	if (manager->permissions &&
	    tool_context_get_permission_policy(manager->permissions, &policy) == 0)
		preset = runtime_permission_preset_name(&policy);
	json_add_string(payload, "permission_mode", preset);
	wire = json_stringify(payload);
	if (!wire || strlen(wire) > HOOK_INPUT_MAX) {
		rc = (wire ? -E2BIG : -ENOMEM);
		goto out_cleanup;
	}
	rc = morph_array_init(&runs, 8, sizeof(struct hook_run));
	if (rc == 0)
		rc = morph_buf_init(&context, 256);
	if (rc == 0 && strcmp(event, "UserPromptSubmit") == 0 && manager->session_context)
		rc = morph_buf_puts(&context, manager->session_context);
	if (rc == 0)
		rc = morph_buf_init(&reason, 256);
	if (rc == 0)
		rc = process_manager_create(&pcfg, &processes);
	if (rc != 0)
		goto out_cleanup;
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
		run = morph_array_push(&runs);
		if (!run) {
			rc = (-ENOMEM);
			goto out_cleanup;
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
			if (fwrite(wire, 1, strlen(wire), stdin_file) != strlen(wire) ||
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
				struct arena *permission_arena = NULL;
				struct sandbox_config sandbox;

				if (manager->permissions) {
					permission_arena = arena_create(0);
					run->rc =
						permission_arena
							? sandbox_permission_prepare(
								  manager->permissions, "hook",
								  NULL, permission_arena, &sandbox)
							: -ENOMEM;
					options.host_execution =
						permission_effective_access(&policy).host;
					options.sandbox = options.host_execution ? NULL : &sandbox;
				}
				if (run->rc == 0)
					run->rc = process_spawn(processes, &options, run->session,
								NULL);
				arena_destroy(permission_arena);
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

		morph_array_foreach (run, &runs, struct hook_run) {
			if (run->done)
				continue;
			if (cancelled) {
				(void)process_session_kill(processes, run->session);
				run->rc = -ECANCELED;
				run->done = 1;
				continue;
			}
			run->rc = collect_run(processes, run);
			if (run->rc < 0) {
				(void)process_session_kill(processes, run->session);
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

			if (runs.nelts == 1) {
				struct hook_run *only = morph_array_get(&runs, 0);
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
	morph_array_foreach (run, &runs, struct hook_run) {
		int decision = 0;
		size_t context_before = context.len, reason_before = reason.len;
		struct json *updated_before = updated;
		int ask_before = ask, terminate_before = terminate;
		int decode_rc = hook_decode_output(manager, run, event, &context, &reason, &updated,
						   &decision, &ask, &terminate);
		if (context.len + manager->context_bytes > 32768u || reason.len > 32768u)
			decode_rc = -E2BIG;
		if (decode_rc < 0) {
			(void)morph_buf_truncate(&context, context_before);
			(void)morph_buf_truncate(&reason, reason_before);
			if (updated != updated_before)
				json_free(updated);
			updated = updated_before;
			ask = ask_before;
			terminate = terminate_before;
			decision = 0;
		}
		blocked |= decision;
		if (decode_rc == 0) {
			struct json *parsed =
				json_parse_prefix(morph_buf_cstr(&run->out), NULL, NULL);
			const char *message = json_str_or(parsed, "systemMessage", "");
			if (*message)
				(void)morph_event_emit_simple(manager->event_cb,
							      manager->event_user,
							      MORPH_EVENT_BACKGROUND,
							      "hook.message", "end", message, NULL);
			json_free(parsed);
		}
		hook_emit(manager, run->entry,
			  decode_rc < 0 ? "hook.failed"
			  : decision	? "hook.blocked"
					: "hook.completed",
			  decode_rc, decision, run);
		struct json *last = json_at(manager->recent, json_count(manager->recent) - 1);
		char *diagnostic = utf8_dup_clamped(morph_buf_cstr(&run->err), 16384);
		if (diagnostic) {
			json_add_string(last, "stderr", diagnostic);
			free(diagnostic);
		}
	}
	manager->context_bytes += context.len;
	*output = json_object();
	if (!*output) {
		rc = (-ENOMEM);
		goto out_cleanup;
	}
	json_add_string(*output, "context", morph_buf_cstr(&context));
	json_add_string(*output, "reason", morph_buf_cstr(&reason));
	json_add_bool(*output, "blocked", blocked);
	json_add_bool(*output, "ask", ask);
	json_add_bool(*output, "terminate", terminate);
	if (strcmp(event, "SessionStart") == 0 &&
	    strcmp(json_str_or(input, "source", ""), "compact") == 0) {
		char *saved = strdup(morph_buf_cstr(&context));

		if (!saved) {
			rc = -ENOMEM;
			goto out_cleanup;
		}
		free(manager->session_context);
		manager->session_context = saved;
	}
	if (updated) {
		rc = json_object_add(*output, "updatedInput", updated);
		if (rc != 0)
			goto out_cleanup;
		updated = NULL;
	}

out_cleanup:
	elapsed = hook_now_ms() - started;
	manager->remaining_ms =
		elapsed < manager->remaining_ms ? manager->remaining_ms - elapsed : 0;
	process_manager_destroy(processes);
	for (size_t i = 0; i < runs.nelts; i++) {
		struct hook_run *item = morph_array_get(&runs, i);
		morph_buf_cleanup(&item->out);
		morph_buf_cleanup(&item->err);
	}
	morph_array_cleanup(&runs);
	morph_buf_cleanup(&context);
	morph_buf_cleanup(&reason);
	json_free(payload);
	json_free(updated);
	free(wire);
	return rc;
}
