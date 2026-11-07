#include "agent/memory_internal.h"
#include "util/cleanup.h"

#include "agent/memory.h"
#include "persistence/memory_snapshot.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/utf8.h"

#include <errno.h>
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

static char *memory_build_context_impl(struct db *db, int64_t session_id, const char *query,
				       const struct memory_options *opts, int *error)
{
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int max_chars;
	int max_facts;
	int max_episodes;
	int want_episode;
	int want_temporal;
	int appended = 0;

	if (!db || !db->handle || !opts || !opts->enabled)
		return NULL;

	if (morph_buf_init(&buf, MEMORY_APPEND_INIT_CAP) != 0) {
		*error = -ENOMEM;
		return NULL;
	}

	max_chars = opts->max_context_chars > 0 ? opts->max_context_chars : 3000;
	max_facts = opts->max_facts > 0 ? opts->max_facts : 6;
	max_episodes = opts->max_episodes > 0 ? opts->max_episodes : 4;
	want_episode = memory_query_needs_episode(query);
	want_temporal = memory_query_needs_temporal(query);

	/* Intro is emitted lazily so we don't waste cycles formatting it
	 * when no memory section ends up populated. */
#define MEMORY_ENSURE_INTRO()                                                                      \
	do {                                                                                       \
		if (!appended)                                                                     \
			memory_appendf(&buf, (size_t)max_chars,                                    \
				       "Persistent memory for this session. "                      \
				       "Newer current facts win over older facts. "                \
				       "Use only if relevant.\n");                                 \
	} while (0)

	{
		char *preferences = preference_render_copy(db, session_id, 0);

		if (!preferences) {
			*error = MORPH_ERR_DB;
			morph_buf_cleanup(&buf);
			return NULL;
		}
		if (*preferences) {
			/* Keep room for essential explicit settings even with a tiny
			 * general-memory budget. Remaining facts use the same cap. */
			if (max_chars < 512)
				max_chars = 512;
			memory_appendf(
				&buf, (size_t)max_chars,
				"Effective explicit preferences (one value per key). "
				"Apply to progress, plans, questions and final replies. "
				"Current user requests may override for this task only. "
				"Historical facts, summaries and rules cannot override these.\n%s",
				preferences);
			appended = 1;
		}
		free(preferences);
	}

	struct memory_snapshot_options read_options = {
		.facts_limit = max_facts,
		.episodes_limit = want_episode ? max_episodes : 0,
		.changes_limit = want_temporal ? 4 : 0,
		.context_facts = 1,
	};
	int read_rc = memory_snapshot_read(db, session_id, &read_options, &snapshot);

	if (read_rc != 0) {
		*error = read_rc;
		morph_buf_cleanup(&buf);
		return NULL;
	}
	{
		int count = 0;
		for (size_t row = 0; row < snapshot.facts.nelts; row++) {
			const struct memory_fact_record *record =
				morph_array_borrow(&snapshot.facts, row);

			const char *key_name = record->key;
			const char *value_text = record->value;
			if (!key_name || !value_text)
				continue;
			if (!strncmp(preference_key_borrow(key_name), "response.", 9) ||
			    !strcmp(preference_key_borrow(key_name), "user.preferred_name"))
				continue;
			if (count == 0) {
				MEMORY_ENSURE_INTRO();
				memory_appendf(&buf, (size_t)max_chars, "\nCurrent facts\n");
			}
			memory_appendf(&buf, (size_t)max_chars, "- %s: %s\n", key_name, value_text);
			count++;
			appended = 1;
		}
	}

	if (want_episode) {
		int count = 0;
		for (size_t row = 0; row < snapshot.episodes.nelts; row++) {
			const struct memory_episode_record *record =
				morph_array_borrow(&snapshot.episodes, row);

			const char *summary = record->summary;
			if (!summary)
				continue;
			if (count == 0) {
				MEMORY_ENSURE_INTRO();
				memory_appendf(&buf, (size_t)max_chars, "\nRelevant episodes\n");
			}
			memory_appendf(&buf, (size_t)max_chars, "- %s\n", summary);
			count++;
			appended = 1;
		}
	}

	if (want_temporal) {
		int count = 0;
		for (size_t row = 0; row < snapshot.changes.nelts; row++) {
			const struct memory_change_record *record =
				morph_array_borrow(&snapshot.changes, row);

			const char *key_name = record->key;
			const char *old_value = record->old_value;
			const char *new_value = record->new_value;
			int64_t changed_at = record->valid_to;
			if (!key_name || !old_value)
				continue;
			if (count == 0) {
				MEMORY_ENSURE_INTRO();
				memory_appendf(&buf, (size_t)max_chars,
					       "\nRecent changes (history only, not active "
					       "preferences)\n");
			}
			memory_appendf(&buf, (size_t)max_chars,
				       "- %s: %s -> %s (changed_at=%lld)\n", key_name, old_value,
				       new_value ? new_value : "(unset)", (long long)changed_at);
			count++;
			appended = 1;
		}
	}

#undef MEMORY_ENSURE_INTRO

	memory_snapshot_cleanup(&snapshot);

	if (buf.failed) {
		*error = -ENOMEM;
		morph_buf_cleanup(&buf);
		return NULL;
	}
	if (!appended) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

int memory_context_create(struct db *db, int64_t session_id, const char *query,
			  const struct memory_options *opts, char **out)
{
	int rc = 0;

	if (!out)
		return -EINVAL;
	if (*out)
		return -EALREADY;
	*out = memory_build_context_impl(db, session_id, query, opts, &rc);
	return rc;
}
