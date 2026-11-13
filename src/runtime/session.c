#include "runtime/engine.h"
#include "runtime/session.h"

#include "agent/context.h"
#include "session_history.h"
#include "permission/context.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>

int runtime_session_get_or_create(struct runtime_engine *engine,
				  const char *name,
				  const char *model,
				  struct session *out,
				  int *created)
{
	const char *workdir = engine && engine->tool_context
		? tool_context_workdir_borrow(engine->tool_context)
		: NULL;
	int rc;

	if (!engine || !engine->db || !name || !model || !out)
		return -EINVAL;
	rc = session_create_with_workdir(engine->db, name, model, workdir, out);
	if (rc == -EEXIST) {
		rc = session_get_by_name(engine->db, name, out);
		if (rc == 0) {
			(void)session_update_model(engine->db, out->id, model);
			strncpy(out->model, model, sizeof(out->model) - 1);
			if (!out->workdir[0] && workdir && workdir[0]) {
				(void)session_update_workdir(engine->db, out->id, workdir);
				strncpy(out->workdir, workdir, sizeof(out->workdir) - 1);
			}
		}
		if (created)
			*created = 0;
	} else if (rc == 0 && created) {
		*created = 1;
	}
	if (rc == 0)
		(void)session_ensure_display_id(engine->db, out);
	return rc;
}

int runtime_session_create(struct runtime_engine *engine,
			   const char *name,
			   const char *model,
			   struct session *out)
{
	const char *workdir = engine && engine->tool_context
		? tool_context_workdir_borrow(engine->tool_context)
		: NULL;
	int rc;

	if (!engine || !engine->db || !name || !model || !out)
		return -EINVAL;
	rc = session_create_with_workdir(engine->db, name, model, workdir, out);
	if (rc == 0)
		(void)session_ensure_display_id(engine->db, out);
	return rc;
}

int runtime_session_switch(struct runtime_engine *engine,
			   const char *name,
			   const char *model,
			   struct session *out,
			   int *created)
{
	return runtime_session_get_or_create(engine, name, model, out, created);
}

int runtime_session_rename(struct runtime_engine *engine,
			   int64_t id,
			   const char *new_name)
{
	if (!engine || !engine->db || id <= 0 || !new_name)
		return -EINVAL;
	return session_rename(engine->db, id, new_name);
}

int runtime_session_delete(struct runtime_engine *engine, int64_t id)
{
	if (!engine || !engine->db || id <= 0)
		return -EINVAL;
	return session_delete(engine->db, id);
}

void runtime_session_load_history(struct runtime_engine *engine,
				  int64_t session_id)
{
	struct agent_session_runtime runtime;

	if (!engine || !engine->db || !engine->react || session_id <= 0)
		return;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = engine->db;
	runtime.session_id = session_id;
	runtime.react = engine->react;
	agent_session_load_history(&runtime);
	log_info(engine ? engine->logger : NULL, "runtime: loaded messages for session id=%lld",
		 (long long)session_id);
}

void runtime_session_clear_history(struct react_context *react)
{
	if (!react)
		return;
	msg_list_destroy(react->messages);
	react->messages = NULL;
	model_history_free_list(react->history_items);
	react->history_items = NULL;
}
