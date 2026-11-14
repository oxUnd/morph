#include "memory_search.h"

#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MEMORY_SEARCH_TERM_LIMIT 24
#define MEMORY_SEARCH_QUERY_MAX_BYTES 2048

static void memory_terms_cleanup(nb_vec *terms)
{
	char **term;

	for (size_t _i_ = 0; _i_ < nb_vec_len(terms) && (term = (char **)nb_vec_at(terms, _i_));
	     _i_++)
		free(*term);
	nb_vec_cleanup(terms);
}

/* The model supplies useful query words or phrases. Whitespace separates
 * alternatives; no language-specific tokenizer or stopword dictionary is used. */
static int memory_terms_create(const char *query, nb_vec *terms)
{
	char *entry;
	size_t length = query ? strlen(query) : 0;
	size_t start = 0;
	int rc = (nb_vec_init(terms, sizeof(entry)), nb_vec_reserve(terms, 8) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	if (length > MEMORY_SEARCH_QUERY_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	for (size_t position = 0; position <= length;) {
		unsigned cp = 0;
		size_t bytes = 1;

		if (position < length &&
		    !nb_utf8_read(query + position, length - position, &cp, &bytes))
			return NB_ERR_FORMAT;
		int separator = position == length || (cp < 128 && isspace((unsigned char)cp)) ||
				nb_utf8_classify_codepoint(cp) == NB_UTF8_CLASS_SEPARATOR;

		if (separator && position > start) {
			char *word NB_CLEANUP(nb_charp_cleanup) =
				strndup(query + start, position - start);

			if (!word)
				return NB_ERR_NOMEM;
			char **slot = nb_vec_push(terms);

			if (!slot)
				return NB_ERR_NOMEM;
			*slot = nb_charp_steal(&word);
			if (terms->len == MEMORY_SEARCH_TERM_LIMIT)
				break;
		}
		if (separator)
			start = position + bytes;
		position += bytes;
	}
	return 0;
}

static void memory_record_cleanup(struct memory_record *record)
{
	free(record->kind);
	free(record->key);
	free(record->value);
	free(record->source);
	free(record->details);
	free(record->session);
}

void memory_search_result_cleanup(struct memory_search_result *matches)
{
	struct memory_record *record;

	if (!matches)
		return;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&matches->records) &&
			     (record = (struct memory_record *)nb_vec_at(&matches->records, _i_));
	     _i_++)
		memory_record_cleanup(record);
	nb_vec_cleanup(&matches->records);
}

static void memory_statement_cleanup(sqlite3_stmt **stmt)
{
	sqlite3_finalize(*stmt);
}

static int memory_search_visible_sessions_sql(struct db *db,
					      const struct memory_search_options *options,
					      nb_buf *sql)
{
	sqlite3_stmt *stmt NB_CLEANUP(memory_statement_cleanup) = NULL;
	int rc;
	int step;

	if (!options->visible_fn)
		return 0;
	rc = (nb_buf_append_cstr(sql, " AND r.session_id IN (-1") ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (sqlite3_prepare_v2(db->handle, "SELECT id,COALESCE(display_id,'') FROM sessions", -1,
			       &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *display = (const char *)sqlite3_column_text(stmt, 1);

		if (!options->visible_fn(display, options->user_id, options->visible_data))
			continue;
		rc = (nb_buf_printf(sql, ",%lld", (long long)sqlite3_column_int64(stmt, 0))
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	if (step != SQLITE_DONE)
		return MORPH_ERR_DB;
	return (nb_buf_append_char(sql, ')') ? 0 : NB_ERR_NOMEM);
}

/* Stored table names and old note tags are retained for existing databases. */
static int memory_search_build_sql(nb_buf *sql, size_t terms, int include_unmatched,
				   const char *visibility)
{
	int rc =
		(nb_buf_append_cstr(
			 sql,
			 "WITH "
			 "raw(kind,id,session_id,key,value,source,details,updated_at,importance) "
			 "AS ("
			 "SELECT "
			 "'facts',id,session_id,key_name,value_text,COALESCE(source_text,''),"
			 "category,updated_at,importance FROM memory_facts WHERE is_current=1 "
			 "AND category!='preference' AND key_name NOT IN "
			 "('language','preferred_language','response_language','language_"
			 "preference',"
			 "'preferred_response_language','response_style','style','verbosity','"
			 "detail_level',"
			 "'preferred_name','user.preferred_name') AND key_name NOT LIKE "
			 "'response.%' "
			 "UNION ALL SELECT CASE WHEN kind IN ('preference','rule','legacy') "
			 "OR key IN "
			 "('language','preferred_language','response_style','style','preferred_"
			 "name') "
			 "OR key IN "
			 "('user.preferred_name','response_language','language_preference',"
			 "'preferred_response_language','verbosity','detail_level') "
			 "OR key LIKE 'response.%' THEN 'unapplied_settings' "
			 "WHEN kind IN ('tip','lesson') THEN 'tips' ELSE 'learned_facts' END,"
			 "id,session_id,key,value,source,kind,created_at,0.5 FROM "
			 "memory_candidates "
			 "UNION ALL SELECT 'past_tasks',id,session_id,COALESCE(task_type,'task'),"
			 "summary_text,summary_text,COALESCE(key_decisions,'')||char(10)||"
			 "COALESCE(entities,'')||char(10)||COALESCE(artifacts,'')||char(10)||"
			 "COALESCE(tools_used,'')||char(10)||COALESCE(outcome_text,''),"
			 "created_at,importance FROM memory_episodes "
			 "UNION ALL SELECT 'unapplied_rules',id,session_id,'legacy_rule',rule_text,"
			 "COALESCE(trigger_text,''),'Legacy rule; not an active "
			 "preference',updated_at,"
			 "0.5 FROM memory_procedures "
			 "UNION ALL SELECT 'notes',id,session_id,title,text,source,'Explicit user "
			 "note: "
			 "'||scope,"
			 "updated_at,1.0 FROM memory_notes WHERE scope='project' OR session_id=?1 "
			 "OR ?2=1 "
			 "UNION ALL SELECT 'summary',m.session_id,m.session_id,m.title,m.text,"
			 "'Model-organized historical context',m.sources,m.updated_at,0.5 "
			 "FROM memory_summaries m JOIN memory_scopes scope ON "
			 "scope.session_id=m.session_id "
			 "WHERE m.text!='' AND m.memory_version=scope.generation AND "
			 "m.content_revision=scope.content_revision "
			 "AND (m.session_id=?1 OR ?2=1 OR NOT EXISTS (SELECT 1 FROM memory_notes "
			 "private "
			 "WHERE private.session_id=m.session_id AND private.scope='session'))"
			 "), scoped AS ("
			 "SELECT r.*,COALESCE(NULLIF(s.display_id,''),s.name) AS session,"
			 "COALESCE(origin.generation,0) AS memory_version,"
			 "ROW_NUMBER() OVER (PARTITION BY r.kind,"
			 "CASE WHEN r.kind IN "
			 "('past_tasks','unapplied_settings','unapplied_rules','notes','summary') "
			 "THEN "
			 "r.value "
			 "ELSE r.key END ORDER BY r.updated_at DESC,r.id DESC) AS revision "
			 "FROM raw r JOIN sessions s ON s.id=r.session_id "
			 "LEFT JOIN memory_scopes origin ON origin.session_id=r.session_id "
			 "LEFT JOIN memory_scopes current ON current.session_id=?1 "
			 "WHERE (r.session_id=?1 OR (origin.owner=current.owner AND "
			 "(?2=1 OR (?3=1 AND current.project!='' AND "
			 "origin.project=current.project))))"
			 " AND (?5=0 OR r.id=?5) AND (?4='' OR r.kind=?4 OR (?4='learned_notes' "
			 "AND r.kind "
			 "IN "
			 "('learned_facts','tips','notes','unapplied_settings')))")
			 ? 0
			 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append_cstr(sql, visibility) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append_cstr(sql, "), ranked AS (SELECT *, (0") ? 0 : NB_ERR_NOMEM);

	for (size_t i = 0; i < terms && rc == 0; i++) {
		rc = (nb_buf_printf(
			      sql,
			      "+4*morph_text_contains(key,?%zu)+2*morph_text_contains(value,?%zu)"
			      "+morph_text_contains(details,?%zu)+morph_text_contains(source,?%zu)",
			      i + 6, i + 6, i + 6, i + 6)
			      ? 0
			      : NB_ERR_NOMEM);
	}
	if (rc == 0)
		rc = (nb_buf_printf(
			      sql,
			      ") AS score FROM scoped WHERE revision=1) "
			      "SELECT id,session_id,updated_at,kind,key,value,source,details,"
			      "COALESCE(session,''),importance,score,memory_version FROM ranked %s "
			      "ORDER BY score DESC,updated_at DESC,importance DESC,id DESC",
			      terms && !include_unmatched ? "WHERE score>0" : "")
			      ? 0
			      : NB_ERR_NOMEM);
	return rc;
}

static int memory_search_copy_record(sqlite3_stmt *stmt, struct memory_search_result *out)
{
	struct memory_record *record = nb_vec_push(&out->records);

	if (!record)
		return NB_ERR_NOMEM;
	memset(record, 0, sizeof(*record));
	record->id = sqlite3_column_int64(stmt, 0);
	record->session_id = sqlite3_column_int64(stmt, 1);
	record->updated_at = sqlite3_column_int64(stmt, 2);
	record->kind = strdup((const char *)sqlite3_column_text(stmt, 3));
	record->key = strdup((const char *)sqlite3_column_text(stmt, 4));
	record->value = strdup((const char *)sqlite3_column_text(stmt, 5));
	record->source = strdup((const char *)sqlite3_column_text(stmt, 6));
	const char *details = (const char *)sqlite3_column_text(stmt, 7);
	record->details = strdup(details ? details : "");
	record->session = strdup((const char *)sqlite3_column_text(stmt, 8));
	record->importance = sqlite3_column_double(stmt, 9);
	record->score = sqlite3_column_int(stmt, 10);
	record->memory_version = sqlite3_column_int64(stmt, 11);
	if (!record->kind || !record->key || !record->value || !record->source ||
	    !record->details || !record->session)
		return NB_ERR_NOMEM;
	return 0;
}

int memory_search_records(struct db *db, int64_t session_id,
			  const struct memory_search_options *options,
			  struct memory_search_result *out)
{
	struct memory_search_result result NB_CLEANUP(memory_search_result_cleanup) = {0};
	nb_vec terms NB_CLEANUP(memory_terms_cleanup) = {0};
	nb_buf sql NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf visibility NB_CLEANUP(nb_buf_cleanup) = {0};
	sqlite3_stmt *stmt NB_CLEANUP(memory_statement_cleanup) = NULL;
	struct memory_record record;
	int rc;

	if (!db || !db->handle || !options || !out || session_id <= 0)
		return NB_ERR_INVAL;
	if (((void *)out->records.data))
		return NB_ERR_EXISTS;
	rc = memory_terms_create(options->query, &terms);
	if (rc == 0)
		rc = (nb_buf_init(&sql), nb_buf_reserve(&sql, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&visibility),
		      nb_buf_reserve(&visibility, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = memory_search_visible_sessions_sql(db, options, &visibility);
	if (rc == 0)
		rc = memory_search_build_sql(&sql, terms.len, options->include_unmatched,
					     nb_buf_cstr(&visibility));
	if (rc == 0)
		rc = (nb_vec_init(&result.records, sizeof(record)),
		      nb_vec_reserve(&result.records, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (options->query && *options->query && !terms.len && !options->include_unmatched) {
		return (nb_vec_move(&out->records, &result.records), 0);
	}
	if (sqlite3_prepare_v2(db->handle, nb_buf_cstr(&sql), -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_int(stmt, 2, options->all);
	sqlite3_bind_int(stmt, 3, options->project);
	sqlite3_bind_text(stmt, 4, options->kind ? options->kind : "", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 5, options->record_id);
	for (size_t i = 0; i < terms.len; i++) {
		char **term = nb_vec_at(&terms, i);

		sqlite3_bind_text(stmt, (int)i + 6, *term, -1, SQLITE_TRANSIENT);
	}
	int step;
	while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
		rc = memory_search_copy_record(stmt, &result);
		if (rc != 0)
			return rc;
		if (options->limit > 0 && result.records.len >= (size_t)options->limit)
			break;
	}
	if (step != SQLITE_ROW && step != SQLITE_DONE)
		return MORPH_ERR_DB;
	return (nb_vec_move(&out->records, &result.records), 0);
}
