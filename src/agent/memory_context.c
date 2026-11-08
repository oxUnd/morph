#include "agent/memory_internal.h"
#include "util/cleanup.h"

#include "agent/memory.h"
#include "persistence/memory_snapshot.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/utf8.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_APPEND_INIT_CAP 1024
#define MEMORY_RENDER_CAP (1024 * 1024)

static int memory_query_needs_episode(const char *query)
{
	if (!query)
		return 0;
	return memory_contains_ci(query, "last time") || memory_contains_ci(query, "previous") ||
	       memory_contains_ci(query, "before") || memory_contains_ci(query, "how did") ||
	       strstr(query, "上次") != NULL || strstr(query, "之前") != NULL;
}

static int memory_query_needs_temporal(const char *query)
{
	if (!query)
		return 0;
	return memory_contains_ci(query, "current") || memory_contains_ci(query, "still") ||
	       memory_contains_ci(query, "changed") || memory_contains_ci(query, "when") ||
	       strstr(query, "现在") != NULL || strstr(query, "还") != NULL ||
	       strstr(query, "变化") != NULL || strstr(query, "何时") != NULL;
}

struct memory_context_writer {
	morph_buf_t *buffer;
	size_t limit;
	size_t characters;
	const char *notice;
	size_t omitted;
	int counted_notice;
};

/* A context budget selects complete records; it never changes their meaning by
 * cutting a value. The notice reserves a recoverable path to omitted records. */
static int memory_context_append(struct memory_context_writer *writer, const char *heading,
				 const char *format, ...)
{
	morph_buf_t entry MORPH_CLEANUP(morph_buf_cleanup) = {0};
	va_list args;
	int rc = morph_buf_init(&entry, 256);

	if (rc == 0 && heading)
		rc = morph_buf_puts(&entry, heading);
	if (rc != 0)
		return rc;
	va_start(args, format);
	rc = morph_buf_vprintf(&entry, format, args);
	va_end(args);
	if (rc != 0)
		return rc;
	const char *text = morph_buf_cstr_borrow(&entry);
	if (utf8valid(text))
		return MORPH_ERR_FORMAT;
	size_t characters = utf8len(text);
	size_t notice = utf8len(writer->notice) + (writer->counted_notice ? 20u : 0u);
	if (characters > writer->limit - writer->characters ||
	    notice > writer->limit - writer->characters - characters) {
		writer->omitted++;
		return 0;
	}
	rc = morph_buf_puts(writer->buffer, text);
	if (rc != 0)
		return rc;
	writer->characters += characters;
	return 1;
}

static void memory_context_notice_configure(struct memory_context_writer *writer)
{
	writer->notice = writer->limit >= 128
		? "[Memory context is selective; at least %zu omitted records. "
		  "Recover via memory with scope=session.]\n"
		: writer->limit >= 29 ? "[Memory omitted; use memory.]\n"
		: writer->limit >= 10 ? "[omitted]\n" : "…";
	writer->counted_notice = writer->limit >= 128;
}

static int memory_context_preference_visit(const char *key, const char *value,
					   const char *scope, int64_t revision, void *data)
{
	static const char heading[] =
		"Effective explicit preferences (one value per key). "
		"Apply to progress, plans, questions and final replies. "
		"Current user requests may override for this task only. "
		"Historical facts, summaries and rules cannot override these.\n";
	struct memory_context_writer *writer = data;
	int rc;

	/* Essential settings precede optional records and retain their minimum budget. */
	if (writer->limit < 512) {
		writer->limit = 512;
		memory_context_notice_configure(writer);
	}
	rc = memory_context_append(writer, writer->characters ? NULL : heading,
		"- %s: %s [%s; revision=%lld]\n", key, value, scope, (long long)revision);
	return rc < 0 ? rc : 0;
}

static int memory_context_render(struct db *db, int64_t session_id, const char *query,
				 const struct memory_options *opts, morph_buf_t *buffer)
{
	static const char intro[] =
		"Persistent memory for this session. Newer current facts win over older facts. "
		"Use only if relevant.\n";
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	int max_chars = opts->max_context_chars > 0 ? opts->max_context_chars : 3000;
	int max_facts = opts->max_facts > 0 ? opts->max_facts : 6;
	int max_episodes = opts->max_episodes > 0 ? opts->max_episodes : 4;
	int want_episode = memory_query_needs_episode(query);
	int want_temporal = memory_query_needs_temporal(query);
	int appended = 0;
	int rc;

	struct memory_context_writer writer = {
		.buffer = buffer,
		.limit = (size_t)max_chars,
	};
	memory_context_notice_configure(&writer);
	rc = preference_visit_effective(db, session_id, memory_context_preference_visit, &writer);
	if (rc != 0)
		return rc;
	appended = writer.characters > 0;
	struct memory_snapshot_options read_options = {
		/* Read one extra record so a selection limit is observable. */
		.facts_limit = max_facts < INT_MAX ? max_facts + 1 : max_facts,
		.episodes_limit = want_episode
				  ? (max_episodes < INT_MAX ? max_episodes + 1 : max_episodes) : 0,
		.changes_limit = want_temporal ? 5 : 0,
		.context_facts = 1,
	};
	rc = memory_snapshot_read(db, session_id, &read_options, &snapshot);
	if (rc != 0)
		return rc;
	const char *heading = appended ? "\nCurrent facts\n" : intro;
	int facts = 0;
	for (size_t row = 0; row < snapshot.facts.nelts; row++) {
		const struct memory_fact_record *record = morph_array_borrow(&snapshot.facts, row);
		if (!record->key || !record->value)
			continue;
		if (!strncmp(preference_key_borrow(record->key), "response.", 9) ||
		    !strcmp(preference_key_borrow(record->key), "user.preferred_name"))
			continue;
		if (facts++ >= max_facts) {
			writer.omitted++;
			continue;
		}
		rc = memory_context_append(&writer, heading, "- %s: %s\n", record->key,
					   record->value);
		if (rc < 0)
			return rc;
		if (rc > 0) {
			heading = NULL;
			appended = 1;
		}
	}
	heading = appended ? "\nRelevant episodes\n" : intro;
	for (size_t row = 0; row < snapshot.episodes.nelts; row++) {
		const struct memory_episode_record *record =
			morph_array_borrow(&snapshot.episodes, row);
		if (!record->summary)
			continue;
		if (row >= (size_t)max_episodes) {
			writer.omitted++;
			continue;
		}
		rc = memory_context_append(&writer, heading, "- %s\n", record->summary);
		if (rc < 0)
			return rc;
		if (rc > 0) {
			heading = NULL;
			appended = 1;
		}
	}
	heading = appended ? "\nRecent changes (history only, not active preferences)\n" : intro;
	for (size_t row = 0; row < snapshot.changes.nelts; row++) {
		const struct memory_change_record *record = morph_array_borrow(&snapshot.changes,
		row);
		if (!record->key || !record->old_value)
			continue;
		if (row >= 4) {
			writer.omitted++;
			continue;
		}
		rc = memory_context_append(&writer, heading,
			"- %s: %s -> %s (changed_at=%lld)\n", record->key, record->old_value,
			record->new_value ? record->new_value : "(unset)", (long
			long)record->valid_to);
		if (rc < 0)
			return rc;
		if (rc > 0) {
			heading = NULL;
			appended = 1;
		}
	}
	if (writer.omitted)
		return writer.counted_notice
			 ? morph_buf_printf(buffer, writer.notice, writer.omitted)
			 : morph_buf_puts(buffer, writer.notice);
	return 0;
}

int memory_context_create(struct db *db, int64_t session_id, const char *query,
			  const struct memory_options *opts, char **out)
{
	morph_buf_t context MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!out)
		return -EINVAL;
	if (*out)
		return -EALREADY;
	if (!db || !db->handle || !opts || !opts->enabled)
		return 0;
	rc = morph_buf_init(&context, MEMORY_APPEND_INIT_CAP);
	if (rc == 0)
		rc = memory_context_render(db, session_id, query, opts, &context);
	if (rc == 0 && context.len)
		*out = morph_buf_detach(&context);
	return rc;
}
