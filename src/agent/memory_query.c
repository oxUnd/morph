#include "agent/memory.h"
#include "util/cleanup.h"

#include "persistence/memory_snapshot.h"
#include "persistence/memory_search.h"
#include "session.h"
#include "util/buf.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_STORE_APPEND_INIT_CAP 1024

static int memory_query_append_session_header(morph_buf_t *buf, const struct session *session)
{
	return morph_buf_printf(buf, "session %lld", (long long)(session ? session->id : 0));
}

static int memory_query_append_type(struct db *db, int64_t session_id,
				    const struct memory_query_options *options, morph_buf_t *buf)
{
	struct memory_snapshot_options read = {0};
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	struct memory_fact_record *fact;
	struct memory_rule_record *rule;
	struct memory_task_record *task;
	struct memory_change_record *change;
	int rc;

	switch (options->type) {
	case MEMORY_QUERY_USER_SUMMARY:
		read.include_user_summary = 1;
		break;
	case MEMORY_QUERY_FACTS:
		read.facts_limit = -1;
		break;
	case MEMORY_QUERY_UNAPPLIED_RULES:
		read.include_rules = 1;
		break;
	case MEMORY_QUERY_TASKS:
		read.tasks_limit = options->max_past_tasks > 0 ? options->max_past_tasks : -1;
		break;
	case MEMORY_QUERY_CHANGES:
		read.changes_limit = -1;
		break;
	case MEMORY_QUERY_ALL:
	case MEMORY_QUERY_PREFERENCES:
	case MEMORY_QUERY_TIPS:
	case MEMORY_QUERY_NOTES:
	case MEMORY_QUERY_SUMMARY:
	case MEMORY_QUERY_UNAPPLIED_SETTINGS:
	case MEMORY_QUERY_LEARNED_FACTS:
		return 0;
	}
	rc = memory_snapshot_read(db, session_id, &read, &snapshot);
	if (rc != 0)
		return rc;
	if (snapshot.user_summary && snapshot.user_summary[0])
		rc = morph_buf_printf(buf, "profile\n%s\n", snapshot.user_summary);
	if (snapshot.facts.nelts && rc == 0)
		rc = morph_buf_puts(buf, "facts\n");
	morph_array_foreach (fact, &snapshot.facts, struct memory_fact_record) {
		if (rc != 0)
			break;
		if (!fact->key || !fact->value)
			continue;
		rc = morph_buf_printf(buf, "- [%s|imp=%.2f] %s: %s\n",
				      fact->category && fact->category[0] ? fact->category
									  : "general",
				      fact->importance, fact->key, fact->value);
	}
	if (snapshot.rules.nelts && rc == 0)
		rc = morph_buf_puts(buf, "Unapplied rules\n");
	morph_array_foreach (rule, &snapshot.rules, struct memory_rule_record) {
		if (rc != 0)
			break;
		if (rule->text)
			rc = morph_buf_printf(buf, "- %s (evidence=%d)\n", rule->text,
					      rule->evidence);
	}
	if (snapshot.tasks.nelts && rc == 0)
		rc = morph_buf_puts(buf, "Past tasks\n");
	morph_array_foreach (task, &snapshot.tasks, struct memory_task_record) {
		if (rc != 0)
			break;
		if (!task->summary)
			continue;
		rc = morph_buf_printf(buf, "- [imp=%.2f t=%lld] %s\n", task->importance,
				      (long long)task->created_at, task->summary);
		if (rc == 0 && task->tools && task->tools[0])
			rc = morph_buf_printf(buf, "  tools: %s\n", task->tools);
	}
	if (snapshot.changes.nelts && rc == 0)
		rc = morph_buf_puts(buf, "changes\n");
	morph_array_foreach (change, &snapshot.changes, struct memory_change_record) {
		if (rc != 0)
			break;
		if (!change->key || !change->old_value)
			continue;
		rc = morph_buf_printf(buf, "- %s: %s -> %s (t=%lld)\n", change->key,
				      change->old_value,
				      change->new_value ? change->new_value : "(unset)",
				      (long long)change->updated_at);
	}
	memory_snapshot_cleanup(&snapshot);
	return rc;
}

static int memory_query_session_is_visible(const struct memory_query_options *opts,
					   const struct session *s)
{
	if (!opts || !opts->visible_fn)
		return 1;
	if (!s || !s->display_id[0])
		return 0;
	return opts->visible_fn(s->display_id, opts->user_id, opts->visible_user_data);
}

static int memory_query_append_one_session(struct db *db, const struct session *s,
					   const struct memory_query_options *opts,
					   morph_buf_t *buf, int *written)
{
	char *rendered;
	size_t start_len;
	size_t before;
	int rc;

	if (!memory_query_session_is_visible(opts, s) || !memory_store_session_has_rows(db, s->id))
		return 0;

	start_len = buf->len;
	if (*written > 0) {
		rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
	}
	rc = memory_query_append_session_header(buf, s);
	if (rc == 0 && s->name[0])
		rc = morph_buf_printf(buf, " %s", s->name);
	if (rc == 0)
		rc = morph_buf_putc(buf, '\n');
	if (rc != 0)
		return rc;

	before = buf->len;
	if (opts->type == MEMORY_QUERY_PREFERENCES || opts->type == MEMORY_QUERY_USER_SUMMARY ||
	    opts->type == MEMORY_QUERY_FACTS || opts->type == MEMORY_QUERY_CHANGES) {
		char *preferences =
			preference_render_copy(db, s->id, opts->type == MEMORY_QUERY_CHANGES);

		if (!preferences)
			return MORPH_ERR_DB;
		if (*preferences)
			rc = morph_buf_printf(buf, "Effective preferences\n%s", preferences);
		free(preferences);
		if (rc != 0)
			return rc;
	}
	if (opts->type == MEMORY_QUERY_ALL) {
		rendered = memory_session_text_create(db, s->id, opts->max_past_tasks);
		if (!rendered)
			return -ENOMEM;
		rc = morph_buf_puts(buf, rendered);
		rc = rc == 0 ? morph_buf_putc(buf, '\n') : rc;
		free(rendered);
	} else {
		rc = memory_query_append_type(db, s->id, opts, buf);
	}
	if (rc != 0)
		return rc;
	if (buf->len == before) {
		morph_buf_truncate(buf, start_len);
	} else {
		(*written)++;
	}
	return 0;
}

static int memory_query_append_all_sessions(struct db *db, const struct memory_query_options *opts,
					    morph_buf_t *buf)
{
	struct session *sessions = NULL;
	int count = 0;
	int written = 0;
	int rc;

	rc = session_list(db, &sessions, &count, 0, NULL);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		rc = memory_query_append_one_session(db, &sessions[i], opts, buf, &written);
		if (rc != 0)
			break;
	}
	free(sessions);
	if (rc != 0)
		return rc;
	if (written == 0)
		return morph_buf_puts(buf, "No long-term memory stored.");
	return 0;
}

static int memory_query_append_current_session(struct db *db, int64_t current_session_id,
					       const struct memory_query_options *opts,
					       morph_buf_t *buf)
{
	struct session s;
	int written = 0;
	int rc;

	if (current_session_id <= 0)
		return morph_buf_puts(buf, "No current memory session is available.");
	rc = session_get_by_id(db, current_session_id, &s);
	if (rc != 0)
		return rc;
	rc = memory_query_append_one_session(db, &s, opts, buf, &written);
	if (rc != 0)
		return rc;
	if (written == 0)
		return morph_buf_puts(buf, "No long-term memory stored for this session.");
	return 0;
}

static int memory_query_search(struct db *db, int64_t session_id,
			       const struct memory_query_options *opts, morph_buf_t *buf)
{
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {0};
	const char *kind = opts->type == MEMORY_QUERY_FACTS		   ? "facts"
			   : opts->type == MEMORY_QUERY_TASKS		   ? "past_tasks"
			   : opts->type == MEMORY_QUERY_TIPS		   ? "tips"
			   : opts->type == MEMORY_QUERY_NOTES		   ? "notes"
			   : opts->type == MEMORY_QUERY_SUMMARY		   ? "summary"
			   : opts->type == MEMORY_QUERY_LEARNED_FACTS	   ? "learned_facts"
			   : opts->type == MEMORY_QUERY_UNAPPLIED_SETTINGS ? "unapplied_settings"
			   : opts->type == MEMORY_QUERY_UNAPPLIED_RULES	   ? "unapplied_rules"
									   : NULL;
	struct memory_search_options options = {
		.query = opts->query,
		.record_id = opts->record_id,
		.kind = kind,
		.project = opts->scope_project,
		.all = opts->scope_all,
		.visible_fn = opts->visible_fn,
		.user_id = opts->user_id,
		.visible_data = opts->visible_user_data,
		.limit = opts->record_id		    ? 0
			 : opts->max_results > 0	    ? opts->max_results
			 : opts->type == MEMORY_QUERY_TASKS ? opts->max_past_tasks
							    : 0,
	};
	struct memory_record *record;
	int rc = memory_search_records(db, session_id, &options, &matches);

	if (rc != 0)
		return rc;
	rc = morph_buf_puts(buf, "Memory search (learned facts are inferred; "
				 "explicit preferences remain authoritative)\n");
	morph_array_foreach (record, &matches.records, struct memory_record) {
		if (rc != 0)
			return rc;
		rc = morph_buf_printf(
			buf,
			"[%s#%lld session=%s time=%lld score=%d version=%lld] %s: %s\n"
			"Source: %s\nDetails: %s\n\n",
			record->kind, (long long)record->id, record->session,
			(long long)record->updated_at, record->score,
			(long long)record->memory_version, record->key, record->value,
			record->source, record->details);
	}
	if (rc == 0 && !matches.records.nelts)
		rc = morph_buf_puts(buf, "No matching memories.\n");
	return rc;
}

char *memory_query_text_create(struct db *db, int64_t current_session_id,
			       const struct memory_query_options *opts)
{
	struct memory_query_options local_opts;
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!db || !db->handle)
		return NULL;
	memset(&local_opts, 0, sizeof(local_opts));
	if (opts)
		local_opts = *opts;
	if (morph_buf_init(&buf, MEMORY_STORE_APPEND_INIT_CAP) != 0)
		return NULL;
	if ((local_opts.query || local_opts.scope_project || local_opts.record_id ||
	     local_opts.max_results || local_opts.type == MEMORY_QUERY_TIPS ||
	     local_opts.type == MEMORY_QUERY_NOTES || local_opts.type == MEMORY_QUERY_SUMMARY ||
	     local_opts.type == MEMORY_QUERY_LEARNED_FACTS ||
	     local_opts.type == MEMORY_QUERY_UNAPPLIED_SETTINGS) &&
	    local_opts.type != MEMORY_QUERY_PREFERENCES)
		rc = memory_query_search(db, current_session_id, &local_opts, &buf);
	else if (local_opts.scope_all)
		rc = memory_query_append_all_sessions(db, &local_opts, &buf);
	else
		rc = memory_query_append_current_session(db, current_session_id, &local_opts, &buf);
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}
