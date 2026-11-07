#include "memory_snapshot.h"

#include "util/arena.h"
#include "util/error.h"

#include <errno.h>
#include <string.h>

static const char *snapshot_text(struct memory_snapshot *snapshot, sqlite3_stmt *stmt, int column,
				 int *error)
{
	const char *text = (const char *)sqlite3_column_text(stmt, column);
	const char *copy = text ? arena_strdup(snapshot->arena, text) : NULL;

	if (text && !copy)
		*error = -ENOMEM;
	return copy;
}

static int snapshot_facts(struct db *db, int64_t session_id,
			  const struct memory_snapshot_options *options,
			  struct memory_snapshot *snapshot)
{
	const char *sql =
		options->context_facts
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
		struct memory_fact_record *record = morph_array_push(&snapshot->facts);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		record->key = snapshot_text(snapshot, stmt, 0, &error);
		record->value = snapshot_text(snapshot, stmt, 1, &error);
		record->category = snapshot_text(snapshot, stmt, 2, &error);
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
		struct memory_rule_record *record = morph_array_push(&snapshot->rules);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		record->text = snapshot_text(snapshot, stmt, 0, &error);
		record->evidence = sqlite3_column_int(stmt, 1);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int snapshot_episodes(struct db *db, int64_t session_id, int limit,
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
		struct memory_episode_record *record = morph_array_push(&snapshot->episodes);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		record->summary = snapshot_text(snapshot, stmt, 0, &error);
		record->decisions = snapshot_text(snapshot, stmt, 1, &error);
		record->artifacts = snapshot_text(snapshot, stmt, 2, &error);
		record->tools = snapshot_text(snapshot, stmt, 3, &error);
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
		snapshot->episode_total = sqlite3_column_int(stmt, 0);
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
		struct memory_change_record *record = morph_array_push(&snapshot->changes);

		if (!record) {
			error = -ENOMEM;
			break;
		}
		record->key = snapshot_text(snapshot, stmt, 0, &error);
		record->old_value = snapshot_text(snapshot, stmt, 1, &error);
		record->new_value = snapshot_text(snapshot, stmt, 2, &error);
		record->updated_at = sqlite3_column_int64(stmt, 3);
		record->valid_to = sqlite3_column_int64(stmt, 4);
		if (error)
			break;
	}
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

static int snapshot_profile(struct db *db, int64_t session_id, struct memory_snapshot *snapshot)
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
		snapshot->profile = snapshot_text(snapshot, stmt, 0, &error);
	sqlite3_finalize(stmt);
	return error ? error : rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

void memory_snapshot_cleanup(struct memory_snapshot *snapshot)
{
	if (!snapshot)
		return;
	arena_destroy(snapshot->arena);
	memset(snapshot, 0, sizeof(*snapshot));
}

int memory_snapshot_read(struct db *db, int64_t session_id,
			 const struct memory_snapshot_options *options, struct memory_snapshot *out)
{
	struct memory_fact_record fact;
	struct memory_rule_record rule;
	struct memory_episode_record episode;
	struct memory_change_record change;
	int rc;

	if (!db || !db->handle || !options || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->arena = arena_create(0);
	if (!out->arena)
		return -ENOMEM;
	rc = morph_array_init_arena(&out->facts, out->arena, 8, sizeof(fact));
	if (rc == 0)
		rc = morph_array_init_arena(&out->rules, out->arena, 8, sizeof(rule));
	if (rc == 0)
		rc = morph_array_init_arena(&out->episodes, out->arena, 8, sizeof(episode));
	if (rc == 0)
		rc = morph_array_init_arena(&out->changes, out->arena, 8, sizeof(change));
	if (rc == 0 && options->facts_limit)
		rc = snapshot_facts(db, session_id, options, out);
	if (rc == 0 && options->include_rules)
		rc = snapshot_rules(db, session_id, out);
	if (rc == 0 && options->episodes_limit)
		rc = snapshot_episodes(db, session_id, options->episodes_limit, out);
	if (rc == 0 && options->changes_limit)
		rc = snapshot_changes(db, session_id, options->changes_limit, out);
	if (rc == 0 && options->include_profile)
		rc = snapshot_profile(db, session_id, out);
	if (rc != 0)
		memory_snapshot_cleanup(out);
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
