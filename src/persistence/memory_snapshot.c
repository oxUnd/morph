#include "memory_snapshot.h"

#include "util/cleanup.h"
#include "util/error.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

static char *snapshot_text(sqlite3_stmt *stmt, int column, int *error)
{
	const char *text = (const char *)sqlite3_column_text(stmt, column);
	char *copy = text ? strdup(text) : NULL;

	if (text && !copy)
		*error = -ENOMEM;
	return copy;
}

static int snapshot_facts(struct db *db, int64_t session_id,
			  const struct memory_snapshot_options *options,
			  struct memory_snapshot *snapshot)
{
	const char *sql =
		options->for_reply
			? "SELECT key_name,value_text,category,importance FROM memory_facts "
			  "WHERE session_id=? AND is_current=1 AND category!='preference' "
			  "AND key_name NOT IN "
			  "('preferred_language','response_style','preferred_name') "
			  "ORDER BY updated_at DESC LIMIT ?"
		: options->facts_by_recency
			? "SELECT key_name,value_text,category,importance FROM memory_facts "
			  "WHERE session_id=? AND is_current=1 ORDER BY updated_at DESC LIMIT ?"
			: "SELECT key_name,value_text,category,importance FROM memory_facts "
			  "WHERE session_id=? AND is_current=1 "
			  "ORDER BY importance DESC,updated_at DESC LIMIT ?";
	sqlite3_stmt *stmt = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, options->facts_limit);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct memory_fact_record *record = morph_array_push_borrow(&snapshot->facts);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		memset(record, 0, sizeof(*record));
		record->key = snapshot_text(stmt, 0, &error);
		record->value = snapshot_text(stmt, 1, &error);
		record->category = snapshot_text(stmt, 2, &error);
		record->importance = sqlite3_column_double(stmt, 3);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int snapshot_rules(struct db *db, int64_t session_id, struct memory_snapshot *snapshot)
{
	sqlite3_stmt *stmt = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT rule_text,evidence_count FROM memory_procedures "
		"WHERE session_id=? ORDER BY evidence_count DESC,updated_at DESC",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct memory_rule_record *record = morph_array_push_borrow(&snapshot->rules);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		memset(record, 0, sizeof(*record));
		record->text = snapshot_text(stmt, 0, &error);
		record->evidence = sqlite3_column_int(stmt, 1);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int snapshot_tasks(struct db *db, int64_t session_id, int limit,
			  struct memory_snapshot *snapshot)
{
	sqlite3_stmt *stmt = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT summary_text,key_decisions,artifacts,tools_used,importance,created_at "
		"FROM memory_episodes WHERE session_id=? ORDER BY created_at DESC LIMIT ?",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, limit);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct memory_task_record *record = morph_array_push_borrow(&snapshot->tasks);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		memset(record, 0, sizeof(*record));
		record->summary = snapshot_text(stmt, 0, &error);
		record->decisions = snapshot_text(stmt, 1, &error);
		record->artifacts = snapshot_text(stmt, 2, &error);
		record->tools = snapshot_text(stmt, 3, &error);
		record->importance = sqlite3_column_double(stmt, 4);
		record->created_at = sqlite3_column_int64(stmt, 5);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	if (error || rc != SQLITE_DONE)
		return error ? error : MORPH_ERR_DB;
	rc = sqlite3_prepare_v2(db->handle,
				"SELECT COUNT(*) FROM memory_episodes WHERE session_id=?", -1,
				&stmt, NULL);
	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		snapshot->task_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return rc == SQLITE_ROW ? 0 : MORPH_ERR_DB;
}

static int snapshot_changes(struct db *db, int64_t session_id, int limit,
			    struct memory_snapshot *snapshot)
{
	sqlite3_stmt *stmt = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT old.key_name,old.value_text,cur.value_text,old.updated_at,old.valid_to "
		"FROM memory_facts old LEFT JOIN memory_facts cur ON cur.id=old.superseded_by "
		"WHERE old.session_id=? AND old.is_current=0 ORDER BY old.updated_at DESC LIMIT ?",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, limit);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct memory_change_record *record = morph_array_push_borrow(&snapshot->changes);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		memset(record, 0, sizeof(*record));
		record->key = snapshot_text(stmt, 0, &error);
		record->old_value = snapshot_text(stmt, 1, &error);
		record->new_value = snapshot_text(stmt, 2, &error);
		record->updated_at = sqlite3_column_int64(stmt, 3);
		record->valid_to = sqlite3_column_int64(stmt, 4);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int snapshot_user_summary(struct db *db, int64_t session_id,
				 struct memory_snapshot *snapshot)
{
	sqlite3_stmt *stmt = NULL;
	int error = 0;
	int rc = sqlite3_prepare_v2(db->handle,
				    "SELECT profile_text FROM memory_profiles WHERE session_id=?",
				    -1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
		snapshot->user_summary = snapshot_text(stmt, 0, &error);
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

void memory_snapshot_cleanup(struct memory_snapshot *snapshot)
{
	if (!snapshot)
		return;
	for (size_t i = 0; i < snapshot->facts.nelts; i++) {
		struct memory_fact_record *record = morph_array_borrow(&snapshot->facts, i);
		free(record->key);
		free(record->value);
		free(record->category);
	}
	for (size_t i = 0; i < snapshot->rules.nelts; i++) {
		struct memory_rule_record *record = morph_array_borrow(&snapshot->rules, i);
		free(record->text);
	}
	for (size_t i = 0; i < snapshot->tasks.nelts; i++) {
		struct memory_task_record *record = morph_array_borrow(&snapshot->tasks, i);
		free(record->summary);
		free(record->decisions);
		free(record->artifacts);
		free(record->tools);
	}
	for (size_t i = 0; i < snapshot->changes.nelts; i++) {
		struct memory_change_record *record = morph_array_borrow(&snapshot->changes, i);
		free(record->key);
		free(record->old_value);
		free(record->new_value);
	}
	free(snapshot->user_summary);
	morph_array_cleanup(&snapshot->facts);
	morph_array_cleanup(&snapshot->rules);
	morph_array_cleanup(&snapshot->tasks);
	morph_array_cleanup(&snapshot->changes);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void memory_snapshot_move(struct memory_snapshot *destination,
				 struct memory_snapshot *source)
{
	*destination = *source;
	memset(source, 0, sizeof(*source));
}

int memory_snapshot_read(struct db *db, int64_t session_id,
			 const struct memory_snapshot_options *options, struct memory_snapshot *out)
{
	struct memory_snapshot value MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	struct memory_fact_record fact;
	struct memory_rule_record rule;
	struct memory_task_record task;
	struct memory_change_record change;
	int rc;

	if (!db || !db->handle || !options || !out)
		return -EINVAL;
	if (out->user_summary || out->facts.elts || out->rules.elts || out->tasks.elts ||
	    out->changes.elts)
		return -EEXIST;
	rc = morph_array_init(&value.facts, 8, sizeof(fact));
	if (rc == 0)
		rc = morph_array_init(&value.rules, 8, sizeof(rule));
	if (rc == 0)
		rc = morph_array_init(&value.tasks, 8, sizeof(task));
	if (rc == 0)
		rc = morph_array_init(&value.changes, 8, sizeof(change));
	if (rc == 0 && options->facts_limit)
		rc = snapshot_facts(db, session_id, options, &value);
	if (rc == 0 && options->include_rules)
		rc = snapshot_rules(db, session_id, &value);
	if (rc == 0 && options->tasks_limit)
		rc = snapshot_tasks(db, session_id, options->tasks_limit, &value);
	if (rc == 0 && options->changes_limit)
		rc = snapshot_changes(db, session_id, options->changes_limit, &value);
	if (rc == 0 && options->include_user_summary)
		rc = snapshot_user_summary(db, session_id, &value);
	if (rc == 0)
		memory_snapshot_move(out, &value);
	return rc;
}

int memory_store_session_has_rows(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(
		db->handle,
		"SELECT 1 FROM memory_profiles WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_facts WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_procedures WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_episodes WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_scopes WHERE session_id=? LIMIT 1",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		return 0;
	for (int i = 1; i <= 5; i++)
		sqlite3_bind_int64(stmt, i, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_ROW;
}
