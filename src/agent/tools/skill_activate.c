#include "skill_activate.h"
#include "agent/tool_runtime.h"
#include "db/database.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/utf8.h"
#include "nb/log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "errors.h"

static void skill_statement_cleanup(sqlite3_stmt **statement)
{
	if (statement && *statement)
		sqlite3_finalize(*statement);
}

int skill_activate_session(struct skill_registry *skills, struct db *db, int64_t session,
			   const char *name, int active)
{
	struct skill_entry *entry = skill_borrow(skills, name);
	if (!entry)
		return NB_ERR_NOT_FOUND;
	if (active && !entry->enabled)
		return NB_ERR_PERMISSION;
	int before = entry->activated;
	int rc = active ? skill_activate(entry) : 0;
	if (!active)
		skill_deactivate(entry);
	if (rc != 0 || !db || session <= 0)
		return rc;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(skill_statement_cleanup) = NULL;
	rc = db_lock_acquire(db, &lock);
	const char *sql = active ? "INSERT OR IGNORE INTO "
				   "session_skill_activations(session_id,skill_name) VALUES(?,?)"
				 : "DELETE FROM session_skill_activations WHERE session_id=? AND "
				   "skill_name=?";
	if (rc == 0 && sqlite3_prepare_v2(db->handle, sql, -1, &statement, NULL) != SQLITE_OK)
		rc = MORPH_ERR_DB;
	if (rc == 0) {
		sqlite3_bind_int64(statement, 1, session);
		sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(statement) != SQLITE_DONE)
			rc = MORPH_ERR_DB;
	}
	if (rc != 0)
		entry->activated = before;
	return rc;
}

int skill_restore_session(struct skill_registry *skills, struct db *db, int64_t session)
{
	if (!skills || !db)
		return NB_ERR_INVAL;
	nb_vec previous NB_CLEANUP(nb_vec_cleanup) = {0};
	int rc = (nb_vec_init(&previous, sizeof(skills->count)),
		  nb_vec_reserve(&previous, (size_t)skills->count) ? 0 : NB_ERR_NOMEM);
	for (int i = 0; rc == 0 && i < skills->count; i++) {
		int *active = nb_vec_push(&previous);
		if (!active)
			rc = NB_ERR_NOMEM;
		else
			*active = skill_entry_borrow_at(skills, (size_t)i)->activated;
	}
	if (rc != 0)
		return rc;
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	sqlite3_stmt *statement NB_CLEANUP(skill_statement_cleanup) = NULL;
	rc = db_lock_acquire(db, &lock);
	if (rc == 0 && sqlite3_prepare_v2(db->handle,
					  "SELECT skill_name FROM session_skill_activations WHERE "
					  "session_id=? ORDER BY skill_name",
					  -1, &statement, NULL) != SQLITE_OK)
		rc = MORPH_ERR_DB;
	if (rc != 0)
		return rc;
	skill_deactivate_all(skills);
	sqlite3_bind_int64(statement, 1, session);
	int step;
	while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(statement, 0);
		struct skill_entry *entry = skill_borrow(skills, name);
		if (entry && entry->enabled) {
			rc = skill_activate(entry);
			if (rc != 0)
				break;
		}
	}
	if (rc == 0 && step != SQLITE_DONE)
		rc = MORPH_ERR_DB;
	if (rc != 0) {
		for (int i = 0; i < skills->count; i++)
			skill_entry_borrow_at(skills, (size_t)i)->activated =
				*(const int *)nb_vec_at(&previous, (size_t)i);
	}
	return rc;
}

static int skill_activate_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct skill_registry *skills = user_data;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const cJSON *name_item;
	const char *name;
	struct skill_entry *skill;
	int rc;

	if (!result)
		return NB_ERR_INVAL;
	if (!skills) {
		(void)tool_result_error_copy(result, "not_initialized",
					     "Skill system is unavailable.");
		return MORPH_ERR_NOT_INITIALIZED;
	}
	root = args_json ? cJSON_Parse(args_json) : NULL;
	name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	name = cJSON_GetStringValue(name_item);
	if (!name || !*name || nb_utf8_validate(nb_str_from_cstr(name), NULL) ||
	    nb_utf8_count(nb_str_from_cstr(name)) > SKILL_NAME_MAX_CHARS) {
		(void)tool_result_error_format(
			result, "invalid_name",
			"Provide a valid UTF-8 skill name of at most %d characters.",
			SKILL_NAME_MAX_CHARS);
		return NB_ERR_INVAL;
	}
	skill = skill_borrow(skills, name);
	if (!skill) {
		(void)tool_result_error_format(result, "skill_not_found",
					       "Skill '%s' was not found.", name);
		return NB_ERR_NOT_FOUND;
	}
	if (!skill->enabled) {
		(void)tool_result_error_format(result, "skill_disabled", "Skill '%s' is disabled.",
					       name);
		return NB_ERR_PERMISSION;
	}
	int already_active = skill->activated && skill->body_loaded;
	const struct tool_runtime_context *runtime = result->runtime;
	rc = skill_activate_session(skills, runtime ? runtime->db : NULL,
				    runtime ? runtime->memory_session_id : 0, name, 1);
	if (rc < 0) {
		(void)tool_result_error_format(
			result, "activation_failed", "Failed to activate skill '%s': %s", name,
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}
	if (already_active)
		return tool_result_success_format(
			result, "Skill '%s' is already active. Its instructions are in context.",
			name);
	if (!skill->body || !skill->body[0])
		return tool_result_success_text_copy(
			result, "Skill activated but contains no instructions.");
	nb_log_write(result->runtime ? result->runtime->http.logger : NULL, NB_LOG_INFO,
		     "skill_activate: '%s' activated", name);
	return tool_result_success_text_copy(
		result,
		"Skill activated. Its full instructions are supplied in the active skill context "
		"on the next model request.");
}

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return NB_ERR_INVAL;
	return tool_register_borrow(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "activate_skill",
			.description =
				"Activate a skill by name to load its specialized instructions "
				"into context. Use when a task matches a skill's description.",
			.input_schema = "{\"type\":\"object\",\"properties\":{"
					"\"name\":{\"type\":\"string\","
					"\"description\":\"The skill name to activate\"}"
					"},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = skill_activate_exec,
			.user_data = skills,
		});
}

int skill_activate_rebind(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return NB_ERR_INVAL;
	struct tool_entry *entry = tool_borrow(reg, "activate_skill");
	unsigned int flags = entry ? entry->flags : TOOL_FLAG_READONLY;
	int timeout = entry ? entry->timeout_seconds : 0;
	int disabled = entry ? tool_is_disabled(reg, "activate_skill") : 0;
	int rc = entry ? tool_unregister(reg, "activate_skill") : 0;
	if (rc == 0)
		rc = skill_activate_init(reg, skills);
	if (rc != 0)
		return rc;
	entry = tool_borrow(reg, "activate_skill");
	entry->flags = flags;
	entry->timeout_seconds = timeout;
	return disabled ? tool_disable(reg, "activate_skill") : 0;
}
