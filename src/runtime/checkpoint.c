#include "runtime/checkpoint.h"

#include "runtime/runtime_internal.h"
#include "agent/turn.h"
#include "agent/tokenizer.h"
#include "agent/tools/exec_jobs.h"
#include "agent/tools/skill_activate.h"
#include "persistence/checkpoint_store.h"
#include "session_history.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int checkpoint_edit_begin(void *user_data, int64_t id)
{
	struct runtime *runtime = user_data;
	int rc = nb_mutex_lock(runtime->checkpoint_lock);

	if (rc != 0)
		return rc;
	if (id <= 0 || id != runtime->context.checkpoint.id ||
	    !runtime->context.engine.turn_state.active || runtime->checkpoint_failed) {
		nb_mutex_unlock(runtime->checkpoint_lock);
		return NB_ERR_STALE;
	}
	return 0;
}

static int checkpoint_edit_capture(void *user_data, int64_t id, const char *path)
{
	struct runtime *runtime = user_data;

	return checkpoint_store_capture(&runtime->context.database, id, path);
}

static int checkpoint_edit_validate_write(void *user_data, int64_t id, const char *path,
					  size_t bytes, const char *removed_path)
{
	struct runtime *runtime = user_data;

	return checkpoint_store_validate_write(&runtime->context.database, id, path, bytes,
		removed_path);
}

static int checkpoint_edit_finish(void *user_data, int64_t id)
{
	struct runtime *runtime = user_data;
	int rc = checkpoint_store_seal(&runtime->context.database, id);

	if (rc != 0)
		runtime->checkpoint_failed = 1;
	nb_mutex_unlock(runtime->checkpoint_lock);
	return rc;
}

static int checkpoint_children_active(struct runtime *runtime)
{
	struct agent_info *tasks = NULL;
	int count = 0;
	int rc;
	int active = runtime->jobs ? exec_jobs_active(runtime->jobs) : 0;

	if (!runtime->context.agents)
		return active ? NB_ERR_BUSY : 0;
	rc = agent_control_list_copy(runtime->context.agents, 0, &tasks, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		if (tasks[i].status == AGENT_PENDING || tasks[i].status == AGENT_RUNNING)
			active = 1;
	}
	agent_control_free_list(tasks, count);
	return active ? NB_ERR_BUSY : 0;
}

static int checkpoint_authorize_restore(struct runtime *runtime, const cJSON *preview)
{
	const cJSON *files = cJSON_GetObjectItemCaseSensitive(preview, "files");
	const cJSON *item;

	if (!cJSON_IsArray(files))
		return NB_ERR_FORMAT;
	cJSON_ArrayForEach (item, files) {
		const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
		const cJSON *exists = cJSON_GetObjectItemCaseSensitive(item, "restore_exists");
		nb_buf full_path NB_CLEANUP(nb_buf_cleanup) = {0};
		struct tool_operation operation = {
			.kind = cJSON_IsFalse(exists) ? TOOL_OP_PATH_DELETE : TOOL_OP_PATH_WRITE,
			.tool_name = "checkpoint",
			.principal = "tool:*",
			.action = "restore_code",
			.scope = runtime->context.workdir,
			.reason = "Restore tracked files to the selected turn checkpoint",
		};
		int rc;

		if (!cJSON_IsString(path) || !path->valuestring[0])
			return NB_ERR_FORMAT;
		if (!cJSON_IsBool(exists))
			return NB_ERR_FORMAT;
		rc = (nb_buf_init(&full_path),
		      nb_buf_reserve(&full_path, strlen(runtime->context.workdir) +
							 strlen(path->valuestring) + 2)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&full_path, "%s/%s", runtime->context.workdir,
					    path->valuestring)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		operation.target = nb_buf_cstr(&full_path);
		rc = tool_context_check_operation(runtime->context.tctx, &operation);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int checkpoint_preview_locked(struct runtime *runtime, int64_t session_id, int64_t id,
				     cJSON **out)
{
	cJSON *info NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const cJSON *workspace;
	char root[PATH_MAX];
	int rc = checkpoint_store_info_copy(&runtime->context.database, session_id, id, &info);

	if (rc != 0)
		return rc;
	if (!realpath(runtime->context.workdir, root))
		return nb_status_from_errno(errno, NULL);
	workspace = cJSON_GetObjectItemCaseSensitive(info, "workspace_root");
	if (!cJSON_IsString(workspace) || strcmp(root, workspace->valuestring) != 0)
		return NB_ERR_CROSS_DEVICE;
	return checkpoint_store_preview_copy(&runtime->context.database, session_id, id, out);
}

static int checkpoint_record_rewind(struct runtime *runtime, int64_t session_id, int64_t id,
				    enum runtime_rewind_mode mode)
{
	nb_buf note NB_CLEANUP(nb_buf_cleanup) = {0};
	struct model_history_insert item = {
		.session_id = session_id,
		.kind = "background_receipt",
		.role = "system",
		.active = 1,
	};
	int rc = (nb_buf_init(&note), nb_buf_reserve(&note, 384) ? 0 : NB_ERR_NOMEM);

	if (rc == 0)
		rc = (nb_buf_printf(
			      &note,
			      "[Workspace rewind] The user restored %s to checkpoint %lld. %s "
			      "Re-read relevant files before continuing; earlier edit and test "
			      "results "
			      "may describe a different file state.",
			      mode == RUNTIME_REWIND_CODE ? "tracked code" : "the conversation",
			      (long long)id,
			      mode == RUNTIME_REWIND_CODE
				      ? "The conversation was retained."
				      : "Files were retained, including edits made "
					"after that conversation point.")
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	item.content = nb_buf_cstr(&note);
	item.token_count = tokenizer_count(runtime->context.tokenizer, item.content);
	rc = model_history_migrate_messages(&runtime->context.database, session_id);
	return rc != 0 ? rc : model_history_add(&runtime->context.database, &item, NULL);
}

static int checkpoint_restore_locked(struct runtime *runtime, int64_t session_id, int64_t id,
				     enum runtime_rewind_mode mode, cJSON **out)
{
	cJSON *preview NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	struct agent_session_runtime session_runtime = {0};
	int rc;

	rc = checkpoint_children_active(runtime);
	if (rc != 0)
		return rc;
	if (mode & RUNTIME_REWIND_CODE) {
		rc = checkpoint_preview_locked(runtime, session_id, id, &preview);
		if (rc != 0)
			return rc;
		if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(preview, "conflicts")) >
		    0) {
			(void)cJSON_AddBoolToObject(preview, "code_restored", 0);
			(void)cJSON_AddBoolToObject(preview, "conversation_restored", 0);
			(void)cJSON_AddNumberToObject(preview, "session_id", (double)session_id);
			*out = preview;
			preview = NULL;
			return NB_ERR_AGAIN;
		}
		rc = checkpoint_authorize_restore(runtime, preview);
		if (rc != 0)
			return rc;
	}
	if (mode & RUNTIME_REWIND_CONVERSATION) {
		runtime_title_stop(runtime);
		runtime_recap_stop(runtime);
	}
	rc = checkpoint_store_restore(&runtime->context.database, session_id, id,
				      (enum checkpoint_restore_mode)mode, out);
	if (*out)
		(void)cJSON_AddNumberToObject(*out, "session_id", (double)session_id);
	if (rc != 0)
		return rc;
	if (!runtime->context.engine.turn_state.active && mode != RUNTIME_REWIND_BOTH) {
		rc = checkpoint_record_rewind(runtime, session_id, id, mode);
		if (rc != 0) {
			if (*out)
				(void)cJSON_AddStringToObject(*out, "history_receipt_error",
							      nb_err_str(morph_error_descriptions(),
									 MORPH_ERROR_COUNT, rc));
			return rc;
		}
	}
	if (!(mode & RUNTIME_REWIND_CONVERSATION))
		return 0;
	runtime_recap_invalidate(runtime);
	react_reset(runtime->context.react);
	(void)react_set_memory_reply_text_copy(runtime->context.react, NULL);
	runtime_context_forget_plan_session(&runtime->context, session_id);
	runtime_context_select_plan_session(&runtime->context, session_id);
	runtime->context.active_skill_session_id = 0;
	if (runtime->context.skills) {
		rc = skill_restore_session(runtime->context.skills, &runtime->context.database,
					   session_id);
		if (rc != 0) {
			if (*out)
				(void)cJSON_AddStringToObject(*out, "skill_reload_error",
							      nb_err_str(morph_error_descriptions(),
									 MORPH_ERROR_COUNT, rc));
			return rc;
		}
		runtime->context.active_skill_session_id = session_id;
	}
	session_runtime.db = &runtime->context.database;
	session_runtime.session_id = session_id;
	session_runtime.react = runtime->context.react;
	rc = agent_session_load_history(&session_runtime);
	if (rc != 0 && *out)
		(void)cJSON_AddStringToObject(
			*out, "history_reload_error",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc;
}

static int checkpoint_request(void *user_data, const char *action, int64_t id, cJSON **out)
{
	struct runtime *runtime = user_data;
	int64_t session_id = runtime->context.engine.turn_state.session_id;
	int rc;

	if (!out || !action)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (session_id <= 0)
		return NB_ERR_STALE;
	nb_mutex_lock(runtime->checkpoint_lock);
	if (strcmp(action, "list") == 0)
		rc = checkpoint_store_list_copy(&runtime->context.database, session_id, out);
	else if (strcmp(action, "preview") == 0)
		rc = checkpoint_preview_locked(runtime, session_id, id, out);
	else if (strcmp(action, "restore_code") == 0)
		rc = checkpoint_restore_locked(runtime, session_id, id, RUNTIME_REWIND_CODE, out);
	else
		rc = NB_ERR_INVAL;
	nb_mutex_unlock(runtime->checkpoint_lock);
	return rc;
}

int runtime_checkpoints_init(struct runtime *runtime)
{
	int rc = ((runtime->checkpoint_lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	runtime->checkpoint_lock_ready = 1;
	runtime->context.checkpoint = (struct tool_checkpoint_context){
		.user_data = runtime,
		.begin = checkpoint_edit_begin,
		.capture = checkpoint_edit_capture,
		.finish = checkpoint_edit_finish,
		.request = checkpoint_request,
		.max_file_bytes = CHECKPOINT_FILE_MAX_BYTES,
		.validate_write = checkpoint_edit_validate_write,
	};
	return 0;
}

void runtime_checkpoints_cleanup(struct runtime *runtime)
{
	if (runtime->checkpoint_lock_ready) {
		(nb_mutex_free(runtime->checkpoint_lock), runtime->checkpoint_lock = NULL);
		runtime->checkpoint_lock_ready = 0;
	}
}

int runtime_checkpoint_begin_turn(struct runtime *runtime, const struct runtime_request *request)
{
	int64_t id = 0;
	int rc;

	nb_mutex_lock(runtime->checkpoint_lock);
	rc = checkpoint_store_create(&runtime->context.database, request->session_id,
				     runtime->context.workdir, request->turn_id,
				     request->stored_user_input ? request->stored_user_input
								: request->model_input,
				     &id);
	if (rc == 0) {
		runtime->checkpoint_failed = 0;
		runtime->context.checkpoint.id = id;
		runtime->context.react->tool_runtime.checkpoint = runtime->context.checkpoint;
	}
	nb_mutex_unlock(runtime->checkpoint_lock);
	return rc;
}

void runtime_checkpoint_end_turn(struct runtime *runtime)
{
	runtime->context.checkpoint.id = 0;
	if (runtime->context.react)
		runtime->context.react->tool_runtime.checkpoint.id = 0;
}

static int checkpoint_lock_idle(struct runtime *runtime, cJSON **out)
{
	int rc;

	if (!runtime || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	rc = nb_mutex_trylock(runtime->context.execution_lock);
	if (rc != 0)
		return rc;
	nb_mutex_lock(runtime->checkpoint_lock);
	return 0;
}

static void checkpoint_unlock_idle(struct runtime *runtime)
{
	nb_mutex_unlock(runtime->checkpoint_lock);
	nb_mutex_unlock(runtime->context.execution_lock);
}

int runtime_checkpoint_list_copy(struct runtime *runtime, cJSON **out)
{
	int rc = checkpoint_lock_idle(runtime, out);

	if (rc != 0)
		return rc;
	if (runtime->context.current_session.id > 0) {
		rc = checkpoint_store_list_copy(&runtime->context.database,
						runtime->context.current_session.id, out);
	} else {
		cJSON *empty NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

		if (!empty || !cJSON_AddArrayToObject(empty, "checkpoints"))
			rc = NB_ERR_NOMEM;
		else {
			*out = empty;
			empty = NULL;
		}
	}
	checkpoint_unlock_idle(runtime);
	return rc;
}

int runtime_checkpoint_preview_copy(struct runtime *runtime, int64_t id, cJSON **out)
{
	int rc;

	if (id <= 0)
		return NB_ERR_INVAL;
	rc = checkpoint_lock_idle(runtime, out);
	if (rc != 0)
		return rc;
	rc = checkpoint_preview_locked(runtime, runtime->context.current_session.id, id, out);
	checkpoint_unlock_idle(runtime);
	return rc;
}

int runtime_checkpoint_restore(struct runtime *runtime, int64_t id, enum runtime_rewind_mode mode,
			       cJSON **out)
{
	int rc;

	if (id <= 0 || mode < RUNTIME_REWIND_CODE || mode > RUNTIME_REWIND_BOTH)
		return NB_ERR_INVAL;
	rc = checkpoint_lock_idle(runtime, out);
	if (rc != 0)
		return rc;
	rc = checkpoint_restore_locked(runtime, runtime->context.current_session.id, id, mode, out);
	checkpoint_unlock_idle(runtime);
	return rc;
}
