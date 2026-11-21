#include "runtime/protocol_dispatch.h"

#include "runtime/runtime_internal.h"
#include "runtime/checkpoint.h"
#include "runtime/model.h"
#include "runtime/protocol.h"
#include "runtime/protocol_inspect.h"
#include "agent/tools/exec_jobs.h"
#include "permission/context.h"
#include "permission/permission.h"
#include "persistence/history_store.h"
#include "persistence/preferences.h"
#include "errors.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <nb/random.h>
#include <nb/str.h>
#include <nb/utf8.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

static void protocol_json_text_cleanup(char **text)
{
	cJSON_free(*text);
	*text = NULL;
}

static const char *protocol_string(const cJSON *params, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);

	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int protocol_optional_string(const cJSON *params, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, key);
	return !value || cJSON_IsString(value);
}

static int64_t protocol_integer(const cJSON *params, const char *key, int64_t fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
	double number = cJSON_GetNumberValue(item);

	if (!cJSON_IsNumber(item) || !isfinite(number) || number < 0 ||
	    number > 9007199254740991.0 || floor(number) != number)
		return fallback;

	return (int64_t)number;
}

static int protocol_commit(cJSON **out, cJSON **value)
{
	if (!*value)
		return NB_ERR_NOMEM;

	*out = *value;
	*value = NULL;
	return 0;
}

static cJSON *protocol_session_json(const struct session *session)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!json || !cJSON_AddNumberToObject(json, "id", (double)session->id) ||
	    !cJSON_AddNumberToObject(json, "session_id", (double)session->id) ||
	    !cJSON_AddStringToObject(json, "display_id", session->display_id) ||
	    !cJSON_AddStringToObject(json, "name", session->name) ||
	    !cJSON_AddStringToObject(json, "model", session->model) ||
	    !cJSON_AddStringToObject(json, "workdir", session->workdir) ||
	    !cJSON_AddNumberToObject(json, "created_at", (double)session->created_at) ||
	    !cJSON_AddNumberToObject(json, "updated_at", (double)session->updated_at) ||
	    !cJSON_AddNumberToObject(json, "token_used", (double)session->token_used))
		return NULL;

	cJSON *result = json;

	json = NULL;
	return result;
}

int runtime_protocol_session_copy(struct runtime *runtime, int64_t id, cJSON **out)
{
	struct session session = {0};
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	int rc;

	if (!runtime || !out || id <= 0)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	rc = db_lock_acquire(&runtime->context.database, &lock);
	if (rc == 0)
		rc = session_get_by_id(&runtime->context.database, id, &session);

	if (rc != 0)
		return rc;

	*out = protocol_session_json(&session);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int protocol_name_valid(const char *name, size_t capacity)
{
	return name && name[0] && strlen(name) < capacity &&
	       nb_utf8_validate(nb_str_from_cstr(name), NULL) == NB_UTF8_OK;
}

static int protocol_workdir(const char *path, nb_buf *out)
{
	struct stat info;
	int rc = nb_path_expand(out, path, NULL);

	if (rc == 0)
		rc = nb_path_canonical_missing(out, nb_buf_cstr(out), NULL);

	if (rc != 0)
		return rc;

	if (stat(nb_buf_cstr(out), &info) != 0)
		return nb_status_from_errno(errno, NULL);

	return S_ISDIR(info.st_mode) ? 0 : NB_ERR_INVAL;
}

static int protocol_session_create(struct runtime *runtime, const cJSON *params, cJSON **out)
{
	struct session session = {0};
	nb_buf generated NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf workdir NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *name = protocol_string(params, "name");
	const char *model = protocol_string(params, "model");
	const char *path = protocol_string(params, "workdir");
	int rc;

	if ((cJSON_GetObjectItemCaseSensitive(params, "name") && !name) ||
	    (cJSON_GetObjectItemCaseSensitive(params, "model") && !model) ||
	    (cJSON_GetObjectItemCaseSensitive(params, "workdir") && !path))
		return NB_ERR_INVAL;

	nb_buf_init(&generated);
	nb_buf_init(&workdir);
	if (!name) {
		if (!nb_buf_append_cstr(&generated, "session_"))
			return NB_ERR_NOMEM;

		rc = nb_random_hex(&generated, 8, NULL);
		if (rc != 0)
			return rc;

		name = nb_buf_cstr(&generated);
	}

	if (!model)
		model = runtime->context.config.models.text.model;

	if (!protocol_name_valid(name, sizeof(session.name)) ||
	    !protocol_name_valid(model, sizeof(session.model)))
		return NB_ERR_INVAL;

	rc = protocol_workdir(path ? path : runtime->context.workdir, &workdir);
	if (rc != 0)
		return rc;

	rc = session_create_with_workdir(&runtime->context.database, name, model,
		nb_buf_cstr(&workdir), &session);
	if (rc == 0)
		rc = session_ensure_display_id(&runtime->context.database, &session);

	if (rc != 0)
		return rc;

	*out = protocol_session_json(&session);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int protocol_session_list(struct runtime *runtime, const cJSON *params, cJSON **out)
{
	struct session *sessions = NULL;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *items = json ? cJSON_AddArrayToObject(json, "sessions") : NULL;
	int count = 0;
	int64_t limit = protocol_integer(params, "limit", 100);
	int rc;

	if (!items)
		return NB_ERR_NOMEM;

	if (limit < 1 || limit > 10000)
		return NB_ERR_RANGE;

	rc = db_lock_acquire(&runtime->context.database, &lock);
	if (rc == 0)
		rc = runtime_session_list_query(runtime, &sessions, &count, (int)limit,
			protocol_string(params, "filter"));

	for (int i = 0; rc == 0 && i < count; i++) {
		cJSON *item = protocol_session_json(&sessions[i]);

		if (!item || !cJSON_AddItemToArray(items, item)) {
			cJSON_Delete(item);
			rc = NB_ERR_NOMEM;
		}
	}

	runtime_session_list_free(sessions);
	if (rc != 0)
		return rc;

	if (!cJSON_AddNumberToObject(json, "count", count))
		return NB_ERR_NOMEM;

	return protocol_commit(out, &json);
}

static int protocol_session_update(struct runtime *runtime, const cJSON *params, cJSON **out)
{
	struct db *db = &runtime->context.database;
	struct session session = {0};
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	nb_buf workdir NB_CLEANUP(nb_buf_cleanup) = {0};
	int64_t id = protocol_integer(params, "session_id", 0);
	const char *name = protocol_string(params, "name");
	const char *model = protocol_string(params, "model");
	const char *path = protocol_string(params, "workdir");
	int rc;

	if (id <= 0 || (!name && !model && !path) ||
	    (cJSON_GetObjectItemCaseSensitive(params, "name") && !name) ||
	    (cJSON_GetObjectItemCaseSensitive(params, "model") && !model) ||
	    (cJSON_GetObjectItemCaseSensitive(params, "workdir") && !path))
		return NB_ERR_INVAL;

	if ((name && !protocol_name_valid(name, sizeof(session.name))) ||
	    (model && !protocol_name_valid(model, sizeof(session.model))))
		return NB_ERR_INVAL;

	nb_buf_init(&workdir);
	if (path) {
		rc = protocol_workdir(path, &workdir);
		if (rc != 0)
			return rc;
	}

	rc = db_lock_acquire(db, &lock);
	if (rc == 0)
		rc = session_get_by_id(db, id, &session);

	if (rc != 0)
		return rc;

	const cJSON *expected = cJSON_GetObjectItemCaseSensitive(params, "expected_updated_at");

	if (expected && protocol_integer(params, "expected_updated_at", -1) != session.updated_at)
		return NB_ERR_BUSY;

	rc = db_exec(db, "SAVEPOINT runtime_protocol_session_update");
	if (rc != 0)
		return rc;

	if (name)
		rc = session_rename(db, id, name);

	if (rc == 0 && model)
		rc = session_update_model(db, id, model);

	if (rc == 0 && path)
		rc = preference_rebind_project(db, id, session.workdir, nb_buf_cstr(&workdir));

	if (rc == 0 && path)
		rc = session_update_workdir(db, id, nb_buf_cstr(&workdir));

	if (rc != 0)
		(void)db_exec(db, "ROLLBACK TO runtime_protocol_session_update");

	int finish_rc = db_exec(db, "RELEASE runtime_protocol_session_update");

	if (rc == 0)
		rc = finish_rc;

	return rc == 0 ? runtime_protocol_session_copy(runtime, id, out) : rc;
}

struct protocol_history_page {
	cJSON *items;
	int count;
	int limit;
	int more;
	int64_t next;
};

static int protocol_history_item(const struct model_history_item *item, void *data)
{
	struct protocol_history_page *page = data;
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (page->count >= page->limit) {
		page->more = 1;
		return 1;
	}

	if (!json || !cJSON_AddNumberToObject(json, "sequence_no", (double)item->sequence_no) ||
	    !cJSON_AddStringToObject(json, "turn_id", item->turn_id ? item->turn_id : "") ||
	    !cJSON_AddStringToObject(json, "kind", item->kind) ||
	    !cJSON_AddStringToObject(json, "role", item->role) ||
	    !cJSON_AddStringToObject(json, "content", item->content ? item->content : "") ||
	    !cJSON_AddStringToObject(json, "payload_json",
		item->payload_json ? item->payload_json : "") ||
	    !cJSON_AddStringToObject(json, "tool_name", item->tool_name) ||
	    !cJSON_AddStringToObject(json, "tool_call_id",
		item->tool_call_id ? item->tool_call_id : "") ||
	    !cJSON_AddBoolToObject(json, "active", item->active) ||
	    !cJSON_AddBoolToObject(json, "truncated", item->truncated) ||
	    !cJSON_AddNumberToObject(json, "created_at", (double)item->created_at) ||
	    !cJSON_AddItemToArray(page->items, json))
		return NB_ERR_NOMEM;

	json = NULL;
	page->count++;
	page->next = item->sequence_no;
	return 0;
}

static int protocol_history(struct runtime *runtime, const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	int64_t id = protocol_integer(params, "session_id", 0);
	int64_t limit = protocol_integer(params, "limit", 100);
	int64_t before = protocol_integer(params, "before_sequence", 0);
	struct session session;
	struct protocol_history_page page = {0};
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};

	if (id <= 0 || limit < 1 || limit > 1000)
		return NB_ERR_INVAL;

	int rc = db_lock_acquire(&runtime->context.database, &lock);
	if (rc == 0)
		rc = session_get_by_id(&runtime->context.database, id, &session);

	if (rc != 0)
		return rc;

	page.items = json ? cJSON_AddArrayToObject(json, "items") : NULL;
	page.limit = (int)limit;
	if (!page.items)
		return NB_ERR_NOMEM;

	rc = history_store_visit_page(&runtime->context.database, id, before,
		(int)limit + 1, protocol_history_item, &page);
	if (rc != 0)
		return rc;

	if (!cJSON_AddNumberToObject(json, "session_id", (double)id) ||
	    !cJSON_AddNumberToObject(json, "next_before_sequence", (double)page.next) ||
	    !cJSON_AddBoolToObject(json, "has_more", page.more) ||
	    !cJSON_AddStringToObject(json, "order", "newest_first"))
		return NB_ERR_NOMEM;

	return protocol_commit(out, &json);
}

static int protocol_permission(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	struct permission_policy policy;
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	int rc;

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "permission.update")) {
		const char *preset = protocol_string(params, "preset");
		const char *scope = protocol_string(params, "scope");
		if (!protocol_optional_string(params, "scope") ||
		    !protocol_optional_string(params, "preset"))
			return NB_ERR_INVAL;

		if (scope && !strcmp(scope, "session_and_defaults") && !runtime->client_parent)
			return NB_ERR_INVAL;

		rc = preset ? runtime_permission_preset(preset, &policy) :
			permission_policy_parse(protocol_string(params, "access"),
				protocol_string(params, "approval"),
				protocol_string(params, "reviewer"), &policy);
		if (rc != 0)
			return rc;

		if (scope && !strcmp(scope, "defaults")) {
			rc = runtime_permission_set_policy(runtime, &policy);
		} else if (!scope || !strcmp(scope, "session") ||
			   !strcmp(scope, "session_and_defaults")) {
			rc = nb_mutex_trylock(runtime->context.execution_lock);
			if (rc != 0)
				return NB_ERR_BUSY;

			if (agent_control_active(runtime->context.agents, 0) ||
			    exec_jobs_active(runtime->jobs)) {
				rc = NB_ERR_BUSY;
			} else {
				if (scope && !strcmp(scope, "session_and_defaults"))
					rc = runtime_permission_set_policy(
						runtime->client_parent ?
							runtime->client_parent : runtime,
						&policy);

				if (rc == 0)
					rc = tool_context_set_permission_policy(
						runtime->context.tctx, &policy);
				if (rc == 0) {
					runtime->context.config.permissions = policy;
					tool_context_clear_turn_grants(runtime->context.tctx);
				}
			}

			nb_mutex_unlock(runtime->context.execution_lock);
		} else {
			return NB_ERR_INVAL;
		}
	} else if (!strcmp(method, "permission.get")) {
		rc = tool_context_get_permission_policy(runtime->context.tctx, &policy);
	} else {
		return NB_ERR_UNSUPPORTED;
	}

	if (rc != 0)
		return rc;

	if (!cJSON_AddStringToObject(json, "access", permission_access_name(policy.access)) ||
	    !cJSON_AddStringToObject(json, "approval", permission_approval_name(policy.approval)) ||
	    !cJSON_AddStringToObject(json, "reviewer", permission_reviewer_name(policy.reviewer)))
		return NB_ERR_NOMEM;

	return protocol_commit(out, &json);
}

static int protocol_grants(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	int rc = 0;

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "permission.grants.list")) {
		struct runtime_permission_grant *grants = NULL;
		int count = 0;
		cJSON *items = cJSON_AddArrayToObject(json, "grants");

		if (!items)
			return NB_ERR_NOMEM;

		rc = runtime_permission_list(runtime, &grants, &count);
		for (int i = 0; rc == 0 && i < count; i++) {
			cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

			if (!item || !cJSON_AddNumberToObject(item, "id", (double)grants[i].id) ||
			    !cJSON_AddStringToObject(item, "subject", grants[i].subject) ||
			    !cJSON_AddStringToObject(item, "resource_kind",
				grants[i].resource_kind) ||
			    !cJSON_AddStringToObject(item, "resource", grants[i].resource) ||
			    !cJSON_AddStringToObject(item, "project_root",
				grants[i].project_root) ||
			    !cJSON_AddNumberToObject(item, "created_at",
				(double)grants[i].created_at) ||
			    !cJSON_AddItemToArray(items, item))
				rc = NB_ERR_NOMEM;
			else
				item = NULL;
		}

		runtime_permission_list_free(grants);
	} else if (!strcmp(method, "permission.grants.revoke")) {
		int deleted = 0;
		int64_t id = protocol_integer(params, "grant_id", 0);
		const char *subject = protocol_string(params, "subject");

		if (id > 0)
			rc = runtime_permission_revoke_id(runtime, id, &deleted);
		else if (subject && subject[0])
			rc = runtime_permission_revoke_subject(runtime, subject, &deleted);
		else
			return NB_ERR_INVAL;

		if (rc == 0 && !cJSON_AddNumberToObject(json, "deleted", deleted))
			rc = NB_ERR_NOMEM;
	} else if (!strcmp(method, "permission.grants.clear")) {
		int deleted = 0;
		const cJSON *all = cJSON_GetObjectItemCaseSensitive(params, "all_projects");

		if (all && !cJSON_IsBool(all))
			return NB_ERR_INVAL;

		rc = runtime_permission_clear(runtime, cJSON_IsTrue(all), &deleted);
		if (rc == 0 && !cJSON_AddNumberToObject(json, "deleted", deleted))
			rc = NB_ERR_NOMEM;
	} else {
		return NB_ERR_UNSUPPORTED;
	}

	return rc == 0 ? protocol_commit(out, &json) : rc;
}

static int protocol_process(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	const char *id = protocol_string(params, "process_id");

	if (!strcmp(method, "process.list"))
		return runtime_process_list(runtime, out);

	if (!id || !id[0])
		return NB_ERR_INVAL;

	if (!strcmp(method, "process.read"))
		return runtime_process_read(runtime, id, out);

	if (!strcmp(method, "process.stop")) {
		int rc = runtime_process_stop(runtime, id);

		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out ? 0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "process.write")) {
		const char *text = protocol_string(params, "text");
		int64_t owner = runtime->context.current_session.id;
		int rc = text && runtime->jobs ? exec_jobs_write(runtime->jobs, owner,
			id, text, strlen(text)) : NB_ERR_INVAL;

		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out ? 0 : NB_ERR_NOMEM;
	}

	return NB_ERR_UNSUPPORTED;
}

static int protocol_model(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	int ultra = 0;
	int rc;

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "model.update")) {
		const char *model = protocol_string(params, "model");
		const char *scope = protocol_string(params, "scope");
		const cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "ultra");
		if (!protocol_optional_string(params, "scope") ||
		    !protocol_optional_string(params, "model"))
			return NB_ERR_INVAL;

		if ((!model && !mode) || (mode && !cJSON_IsBool(mode)) ||
		    (model && !protocol_name_valid(model,
			sizeof(runtime->context.current_session.model))))
			return NB_ERR_INVAL;

		if (scope && !strcmp(scope, "defaults")) {
			if (model || !mode)
				return NB_ERR_INVAL;

			rc = runtime_model_set_ultra(runtime, cJSON_IsTrue(mode));
		} else if (!scope || !strcmp(scope, "session") ||
			   !strcmp(scope, "session_and_defaults")) {
			rc = nb_mutex_trylock(runtime->context.execution_lock);
			if (rc != 0)
				return NB_ERR_BUSY;

			if (agent_control_active(runtime->context.agents, 0))
				rc = NB_ERR_BUSY;

			if (rc == 0 && cJSON_IsTrue(mode) && (!runtime->context.agents ||
			    !runtime->context.config.agents.enabled))
				rc = NB_ERR_UNSUPPORTED;

			if (rc == 0 && scope && !strcmp(scope, "session_and_defaults")) {
				if (model || !mode || !runtime->client_parent)
					rc = NB_ERR_INVAL;
				else
					rc = runtime_model_set_ultra(runtime->client_parent,
						cJSON_IsTrue(mode));
			}

			if (rc == 0 && model)
				rc = runtime_session_set_model(runtime, model);

			if (rc == 0 && mode) {
				int enabled = cJSON_IsTrue(mode);

				if (enabled && (!runtime->context.agents ||
				    !runtime->context.config.agents.enabled)) {
					rc = NB_ERR_UNSUPPORTED;
				} else {
					snprintf(
						runtime->context.config.agents.multi_agent_mode,
						sizeof(runtime->context.config.agents.multi_agent_mode),
						"%s", enabled ? "proactive" : "explicit");
					agent_control_set_proactive(runtime->context.agents,
						enabled);
				}
			}

			nb_mutex_unlock(runtime->context.execution_lock);
		} else {
			return NB_ERR_INVAL;
		}

		if (rc != 0)
			return rc;
	} else if (strcmp(method, "model.get")) {
		return NB_ERR_UNSUPPORTED;
	}

	rc = runtime_model_get_ultra(runtime, &ultra);
	if (rc != 0)
		return rc;

	if (!cJSON_AddBoolToObject(json, "ultra", ultra) ||
	    !cJSON_AddStringToObject(json, "model", runtime->context.config.models.text.model))
		return NB_ERR_NOMEM;

	return protocol_commit(out, &json);
}

static int protocol_checkpoint(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!strcmp(method, "checkpoint.list"))
		return runtime_checkpoint_list_copy(runtime, out);

	int64_t id = protocol_integer(params, "checkpoint_id", 0);

	if (id <= 0)
		return NB_ERR_INVAL;

	if (!strcmp(method, "checkpoint.preview"))
		return runtime_checkpoint_preview_copy(runtime, id, out);

	if (!strcmp(method, "checkpoint.restore")) {
		if (!protocol_optional_string(params, "mode"))
			return NB_ERR_INVAL;

		const char *mode = protocol_string(params, "mode");
		enum runtime_rewind_mode value = RUNTIME_REWIND_BOTH;

		if (mode && !strcmp(mode, "code"))
			value = RUNTIME_REWIND_CODE;
		else if (mode && !strcmp(mode, "conversation"))
			value = RUNTIME_REWIND_CONVERSATION;
		else if (mode && strcmp(mode, "both"))
			return NB_ERR_INVAL;

		return runtime_checkpoint_restore(runtime, id, value, out);
	}

	return NB_ERR_UNSUPPORTED;
}

static cJSON *protocol_agent_json(const struct agent_info *agent)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	const char *status = agent->status == AGENT_PENDING ? "queued" :
		agent->status == AGENT_RUNNING ? "running" :
		agent->status == AGENT_COMPLETED ? "completed" :
		agent->status == AGENT_CANCELLED ? "interrupted" : "failed";

	if (!json || !cJSON_AddStringToObject(json, "agent_id", agent->id) ||
	    !cJSON_AddStringToObject(json, "name", agent->agent_name) ||
	    !cJSON_AddStringToObject(json, "description",
		agent->description ? agent->description : "") ||
	    !cJSON_AddStringToObject(json, "activity", agent->activity ? agent->activity : "") ||
	    !cJSON_AddStringToObject(json, "result", agent->result ? agent->result : "") ||
	    !cJSON_AddStringToObject(json, "status", status) ||
	    !cJSON_AddStringToObject(json, "mode", agent->mode) ||
	    !cJSON_AddNumberToObject(json, "session_id", (double)agent->child_session_id) ||
	    !cJSON_AddNumberToObject(json, "root_session_id", (double)agent->parent_session_id) ||
	    !cJSON_AddNumberToObject(json, "iteration_count", agent->iteration_count) ||
	    !cJSON_AddNumberToObject(json, "started_at_ms", (double)agent->started_at_ms) ||
	    !cJSON_AddNumberToObject(json, "ended_at_ms", (double)agent->ended_at_ms) ||
	    !cJSON_AddNumberToObject(json, "error_code", agent->error_code))
		return NULL;

	cJSON *result = json;

	json = NULL;
	return result;
}

static int protocol_agent(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!strcmp(method, "agent.budget")) {
		struct agent_budget budget = {0};
		int rc = runtime_agent_budget(runtime, &budget);
		if (rc != 0)
			return rc;

		cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!json || !cJSON_AddNumberToObject(json, "used", (double)budget.used) ||
		    !cJSON_AddNumberToObject(json, "limit", (double)budget.limit) ||
		    !cJSON_AddNumberToObject(json, "request_tokens",
			(double)budget.request_tokens) ||
		    !cJSON_AddNumberToObject(json, "input_tokens", (double)budget.input_tokens) ||
		    !cJSON_AddNumberToObject(json, "cached_tokens", (double)budget.cached_tokens) ||
		    !cJSON_AddNumberToObject(json, "output_tokens", (double)budget.output_tokens) ||
		    !cJSON_AddNumberToObject(json, "reserved", (double)budget.reserved) ||
		    !cJSON_AddNumberToObject(json, "reminder_level", budget.reminder_level) ||
		    !cJSON_AddBoolToObject(json, "exhausted", budget.exhausted) ||
		    !cJSON_AddNumberToObject(json, "persistence_error", budget.persistence_error))
			return NB_ERR_NOMEM;

		return protocol_commit(out, &json);
	}

	struct agent_info *agents = NULL;
	const char *id = protocol_string(params, "agent_id");
	int count = 0;
	int rc;

	if (!strcmp(method, "agent.followup")) {
		const char *message = protocol_string(params, "message");

		if (!id || !message)
			return NB_ERR_INVAL;

		rc = runtime_agent_followup(runtime, id, message);
	} else if (!strcmp(method, "agent.interrupt")) {
		rc = id ? runtime_agent_interrupt(runtime, id) : NB_ERR_INVAL;
	} else if (!strcmp(method, "agent.select")) {
		rc = id ? runtime_agent_select(runtime, id) : NB_ERR_INVAL;
	} else if (!strcmp(method, "agent.subscribe") || !strcmp(method, "agent.events")) {
		char **events = NULL;
		int64_t sequence = 0;
		cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *items = json ? cJSON_AddArrayToObject(json, "events") : NULL;

		if (!id)
			return NB_ERR_INVAL;

		if (!items)
			return NB_ERR_NOMEM;

		rc = !strcmp(method, "agent.subscribe") ?
			runtime_agent_subscribe(runtime, id, &events, &count, &sequence) :
			runtime_agent_events(runtime, id, &events, &count);
		for (int i = 0; rc == 0 && i < count; i++) {
			cJSON *event = cJSON_CreateString(events[i]);

			if (!event || !cJSON_AddItemToArray(items, event)) {
				cJSON_Delete(event);
				rc = NB_ERR_NOMEM;
			}
		}

		runtime_agent_events_free(events, count);
		if (rc == 0 && !cJSON_AddNumberToObject(json, "sequence", (double)sequence))
			rc = NB_ERR_NOMEM;

		return rc == 0 ? protocol_commit(out, &json) : rc;
	} else {
		if (!strcmp(method, "agent.read")) {
			rc = id ? runtime_agent_read(runtime, id, &agents) : NB_ERR_INVAL;
			count = rc == 0 ? 1 : 0;
		} else if (!strcmp(method, "agent.list")) {
			rc = runtime_agent_list(runtime,
				protocol_integer(params, "session_id", 0), &agents, &count);
		} else if (!strcmp(method, "agent.activity")) {
			rc = runtime_agent_activity(runtime,
				protocol_integer(params, "session_id", 0), &agents, &count);
		} else {
			return NB_ERR_UNSUPPORTED;
		}

		cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *items = json ? cJSON_AddArrayToObject(json, "agents") : NULL;

		if (!items && rc == 0)
			rc = NB_ERR_NOMEM;

		for (int i = 0; rc == 0 && i < count; i++) {
			cJSON *item = protocol_agent_json(&agents[i]);

			if (!item || !cJSON_AddItemToArray(items, item)) {
				cJSON_Delete(item);
				rc = NB_ERR_NOMEM;
			}
		}

		runtime_agent_list_free(agents, count);
		return rc == 0 ? protocol_commit(out, &json) : rc;
	}

	if (rc != 0)
		return rc;

	*out = cJSON_CreateObject();
	return *out ? 0 : NB_ERR_NOMEM;
}

static int protocol_skill(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "skill.update")) {
		const char *name = protocol_string(params, "name");
		const cJSON *active = cJSON_GetObjectItemCaseSensitive(params, "active");
		int changed = 0;
		int rc;

		if (!name || !cJSON_IsBool(active))
			return NB_ERR_INVAL;

		rc = runtime_skill_set_active(runtime, name, cJSON_IsTrue(active), &changed);
		if (rc != 0)
			return rc;

		if (!cJSON_AddBoolToObject(json, "changed", changed))
			return NB_ERR_NOMEM;
	} else if (!strcmp(method, "skill.list")) {
		cJSON *items = cJSON_AddArrayToObject(json, "skills");
		int count = runtime_skill_count(runtime);

		if (!items)
			return NB_ERR_NOMEM;

		for (int i = 0; i < count; i++) {
			struct skill_entry skill = {0};
			int rc = runtime_skill_info_borrow(runtime, i, &skill);
			cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

			if (rc != 0)
				return rc;

			if (!item || !cJSON_AddStringToObject(item, "name", skill.fm.name) ||
			    !cJSON_AddStringToObject(item, "description", skill.fm.description) ||
			    !cJSON_AddBoolToObject(item, "active", skill.activated) ||
			    !cJSON_AddBoolToObject(item, "enabled", skill.enabled) ||
			    !cJSON_AddItemToArray(items, item))
				return NB_ERR_NOMEM;

			item = NULL;
		}
	} else {
		return NB_ERR_UNSUPPORTED;
	}

	return protocol_commit(out, &json);
}

static int protocol_mcp(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	const char *name = protocol_string(params, "name");
	int rc = 0;

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "mcp.connect"))
		rc = name ? runtime_mcp_connect(runtime, name) : NB_ERR_INVAL;
	else if (!strcmp(method, "mcp.disconnect"))
		rc = name ? runtime_mcp_disconnect(runtime, name) : NB_ERR_INVAL;
	else if (!strcmp(method, "mcp.discover")) {
		int tools = 0, resources = 0, prompts = 0;

		rc = name ? runtime_mcp_discover(runtime, name, &tools, &resources, &prompts) :
			NB_ERR_INVAL;
		if (rc == 0 && (!cJSON_AddNumberToObject(json, "tools", tools) ||
		    !cJSON_AddNumberToObject(json, "resources", resources) ||
		    !cJSON_AddNumberToObject(json, "prompts", prompts)))
			rc = NB_ERR_NOMEM;
	} else if (!strcmp(method, "mcp.list")) {
		cJSON *items = cJSON_AddArrayToObject(json, "servers");
		int count = runtime_mcp_count(runtime);

		if (!items)
			return NB_ERR_NOMEM;

		for (int i = 0; rc == 0 && i < count; i++) {
			struct runtime_mcp_status status;
			cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

			rc = runtime_mcp_info(runtime, i, &status);
			if (rc == 0 && (!item ||
			    !cJSON_AddStringToObject(item, "name", status.config.name) ||
			    !cJSON_AddNumberToObject(item, "state", status.state) ||
			    !cJSON_AddBoolToObject(item, "connected", status.connected) ||
			    !cJSON_AddNumberToObject(item, "error_code", status.error_code) ||
			    !cJSON_AddNumberToObject(item, "elapsed_ms",
				(double)status.elapsed_ms) ||
			    !cJSON_AddItemToArray(items, item)))
				rc = NB_ERR_NOMEM;

			if (rc == 0)
				item = NULL;
		}
	} else {
		return NB_ERR_UNSUPPORTED;
	}

	return rc == 0 ? protocol_commit(out, &json) : rc;
}

static int protocol_info(struct runtime *runtime, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *methods = json ? cJSON_AddArrayToObject(json, "methods") : NULL;
	const char *const names[] = {
		"runtime.info", "runtime.capabilities", "session.create", "session.list",
		"session.get", "session.update", "session.delete", "session.load", "session.unload",
		"session.subscribe", "session.unsubscribe", "turn.start", "turn.steer",
		"turn.interrupt", "turn.get", "turn.expedite", "history.list", "history.read",
		"context.get", "history.compress", "history.repair", "history.diagnose",
		"turn.inspect", "interaction.progress",
		"permission.get", "permission.update", "process.list", "process.read",
		"process.stop",
		"permission.grants.list", "permission.grants.revoke", "permission.grants.clear",
		"model.get", "model.update",
		"session.claim_interactions",
		"process.write",
		"agent.list", "agent.read", "agent.budget", "agent.followup", "agent.interrupt",
		"skill.list",
		"agent.activity", "agent.subscribe", "agent.events", "agent.select",
		"skill.update", "checkpoint.list", "checkpoint.preview", "checkpoint.restore",
		"mcp.list", "mcp.connect", "mcp.disconnect", "mcp.discover", "task.execute",
		"recap.request", "recap.cancel", "recap.progress", "recap.is_current",
		"memory.read", "memory.report", "memory.retry", "memory.jobs", "memory.clear",
		"preference.read", "preference.update",
		"session.import", "hooks.list", "hooks.update"
	};

	(void)runtime;
	if (!methods ||
	    !cJSON_AddNumberToObject(json, "protocol_version", RUNTIME_PROTOCOL_VERSION) ||
	    !cJSON_AddBoolToObject(json, "multi_session", 1))
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		cJSON *item = cJSON_CreateString(names[i]);

		if (!item || !cJSON_AddItemToArray(methods, item)) {
			cJSON_Delete(item);
			return NB_ERR_NOMEM;
		}
	}

	return protocol_commit(out, &json);
}

static int protocol_recap(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	struct runtime_recap_progress progress = {0};
	int rc = 0;

	if (!json)
		return NB_ERR_NOMEM;

	if (!strcmp(method, "recap.request")) {
		int automatic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "automatic"));

		rc = runtime_recap_request(runtime, automatic, &progress);
	} else if (!strcmp(method, "recap.progress")) {
		rc = runtime_recap_progress(runtime, &progress);
	} else if (!strcmp(method, "recap.cancel")) {
		runtime_recap_cancel(runtime);
		return protocol_commit(out, &json);
	} else if (!strcmp(method, "recap.is_current")) {
		progress.session_id = protocol_integer(params, "session_id", 0);
		progress.history_id = protocol_integer(params, "history_id", 0);
		progress.revision = (uint64_t)protocol_integer(params, "revision", 0);
		int64_t completed = protocol_integer(params, "completed_turns", 0);

		if (completed > INT_MAX)
			return NB_ERR_RANGE;

		progress.completed_turns = (int)completed;
		if (!cJSON_AddBoolToObject(json, "current",
			runtime_recap_is_current(runtime, &progress)))
			return NB_ERR_NOMEM;

		return protocol_commit(out, &json);
	} else {
		return NB_ERR_UNSUPPORTED;
	}

	if (rc != 0)
		return rc;

	if (!cJSON_AddNumberToObject(json, "session_id", (double)progress.session_id) ||
	    !cJSON_AddNumberToObject(json, "history_id", (double)progress.history_id) ||
	    !cJSON_AddNumberToObject(json, "revision", (double)progress.revision) ||
	    !cJSON_AddNumberToObject(json, "completed_turns", progress.completed_turns))
		return NB_ERR_NOMEM;

	return protocol_commit(out, &json);
}

static int protocol_misc(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!strcmp(method, "task.execute")) {
		char *args NB_CLEANUP(protocol_json_text_cleanup) = cJSON_PrintUnformatted(params);
		char *result NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = args ? runtime_tasks_execute(runtime, args, (int64_t)time(NULL), &result) :
			NB_ERR_NOMEM;

		if (rc != 0)
			return rc;

		*out = cJSON_Parse(result);
		return *out ? 0 : NB_ERR_PARSE;
	}

	if (!strcmp(method, "hooks.list"))
		return runtime_hooks_snapshot_copy(runtime, out);

	if (!strcmp(method, "hooks.update")) {
		int rc = runtime_hooks_manage(runtime, protocol_string(params, "action"),
			protocol_string(params, "hook_id"),
			protocol_string(params, "expected_digest"));

		if (rc != 0)
			return rc;

		return runtime_hooks_snapshot_copy(runtime, out);
	}

	if (!strcmp(method, "memory.report"))
		return runtime_memory_report_copy(runtime, protocol_string(params, "query"), out);

	if (!strcmp(method, "memory.read")) {
		int64_t limit = protocol_integer(params, "limit", 0);
		if (limit > INT_MAX)
			return NB_ERR_RANGE;

		char *text NB_CLEANUP(nb_charp_cleanup) =
			runtime_memory_render_current_copy(runtime, (int)limit);

		*out = cJSON_CreateObject();
		return *out && cJSON_AddStringToObject(*out, "text", text ? text : "") ?
			0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "memory.jobs") || !strcmp(method, "preference.read")) {
		char *text NB_CLEANUP(nb_charp_cleanup) = !strcmp(method, "memory.jobs")
			? runtime_memory_background_render_copy(runtime)
			: runtime_preferences_render_copy(runtime,
				cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "history")));
		*out = cJSON_CreateObject();
		return text && *out && cJSON_AddStringToObject(*out, "text", text) ?
			0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "memory.retry") || !strcmp(method, "memory.clear")) {
		const cJSON *field = cJSON_GetObjectItemCaseSensitive(params, "scope");
		if (field && (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
		    field->valuedouble < (double)MEMORY_CLEAR_ALL ||
		    field->valuedouble > (double)MEMORY_CLEAR_TIPS ||
		    floor(field->valuedouble) != field->valuedouble))
			return NB_ERR_INVAL;

		int64_t scope = protocol_integer(params, "scope", MEMORY_CLEAR_ALL);
		if (scope > MEMORY_CLEAR_TIPS)
			return NB_ERR_INVAL;

		int rc = !strcmp(method, "memory.retry") ? runtime_memory_retry_current(runtime)
			: runtime_memory_clear_current(runtime, (enum memory_clear_scope)scope);
		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out ? 0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "preference.update")) {
		const char *scope = protocol_string(params, "scope");
		const char *key = protocol_string(params, "key");
		const char *value = protocol_string(params, "value");
		const cJSON *value_field = cJSON_GetObjectItemCaseSensitive(params, "value");
		int rc = scope && key &&
			(cJSON_IsString(value_field) || cJSON_IsNull(value_field)) ?
			runtime_preference_set(runtime, scope, key, value) : NB_ERR_INVAL;

		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out ? 0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "session.import")) {
		struct runtime_session_import_result result;
		const char *path = protocol_string(params, "path");
		int rc = path ? runtime_session_import_file(runtime, path,
			protocol_string(params, "origin"), &result) : NB_ERR_INVAL;

		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out && cJSON_AddNumberToObject(*out, "imported", result.imported) &&
			cJSON_AddNumberToObject(*out, "unchanged", result.unchanged) ?
			0 : NB_ERR_NOMEM;
	}

	return NB_ERR_UNSUPPORTED;
}

int runtime_protocol_dispatch(struct runtime *runtime, const char *method,
	const cJSON *params, cJSON **out)
{
	if (!runtime || !method || !out || (params && !cJSON_IsObject(params)))
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	if (!strcmp(method, "runtime.info") || !strcmp(method, "runtime.capabilities"))
		return protocol_info(runtime, out);

	if (!strcmp(method, "turn.inspect"))
		return runtime_protocol_turn_inspect(runtime, out);

	if (!strcmp(method, "context.get") || !strcmp(method, "history.stats"))
		return runtime_protocol_context_get(runtime, out);

	if (!strcmp(method, "history.compress") || !strcmp(method, "history.repair") ||
	    !strcmp(method, "history.diagnose"))
		return runtime_protocol_history_manage(runtime, method, out);

	if (!strcmp(method, "session.create"))
		return protocol_session_create(runtime, params, out);

	if (!strcmp(method, "session.list"))
		return protocol_session_list(runtime, params, out);

	if (!strcmp(method, "session.get"))
		return runtime_protocol_session_copy(runtime,
			protocol_integer(params, "session_id", 0), out);

	if (!strcmp(method, "session.update"))
		return protocol_session_update(runtime, params, out);

	if (!strcmp(method, "session.delete")) {
		int64_t id = protocol_integer(params, "session_id", 0);
		struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
		int rc = id > 0 ? db_lock_acquire(&runtime->context.database, &lock) : NB_ERR_INVAL;
		if (rc == 0)
			rc = session_delete(&runtime->context.database, id);


		if (rc != 0)
			return rc;

		*out = cJSON_CreateObject();
		return *out ? 0 : NB_ERR_NOMEM;
	}

	if (!strcmp(method, "history.list") || !strcmp(method, "history.read"))
		return protocol_history(runtime, params, out);

	if (!strncmp(method, "permission.grants.", 18))
		return protocol_grants(runtime, method, params, out);

	if (!strncmp(method, "permission.", 11))
		return protocol_permission(runtime, method, params, out);

	if (!strncmp(method, "process.", 8))
		return protocol_process(runtime, method, params, out);

	if (!strncmp(method, "model.", 6))
		return protocol_model(runtime, method, params, out);

	if (!strncmp(method, "checkpoint.", 11))
		return protocol_checkpoint(runtime, method, params, out);

	if (!strncmp(method, "agent.", 6))
		return protocol_agent(runtime, method, params, out);

	if (!strncmp(method, "skill.", 6))
		return protocol_skill(runtime, method, params, out);

	if (!strncmp(method, "mcp.", 4))
		return protocol_mcp(runtime, method, params, out);

	if (!strncmp(method, "recap.", 6))
		return protocol_recap(runtime, method, params, out);

	return protocol_misc(runtime, method, params, out);
}
