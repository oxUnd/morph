#include "agent/memory_internal.h"
#include "nb/cleanup.h"

#include "agent/memory.h"
#include "persistence/memory_snapshot.h"
#include "persistence/memory_job_store.h"
#include "persistence/memory_search.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_APPEND_INIT_CAP 1024
#define MEMORY_RENDER_CAP (1024 * 1024)

/* Session memory tree presentation. */

static void memory_emit_profile_body(nb_buf *b, const char *vbar, const char *body)
{
	const char *p = body;
	int first = 1;
	if (!body || !*body)
		return;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		if (n > 0) {
			if (first)
				memory_appendf(b, MEMORY_RENDER_CAP, "%s└── %.*s\n", vbar, (int)n,
					       p);
			else
				memory_appendf(b, MEMORY_RENDER_CAP, "%s    %.*s\n", vbar, (int)n,
					       p);
			first = 0;
		}
		if (!nl)
			break;
		p = nl + 1;
	}
}

/*
 * Print one logical sub-node value, soft-wrapping it into multiple
 * indented continuation lines if the body is wider than `wrap` bytes.
 * `wrap` is a *soft* byte cap; the actual cut is always rounded down to
 * a UTF-8 codepoint boundary so CJK never gets mojibake.
 *
 * Embedded '\n' / '\r' inside `body` are treated as hard line breaks:
 * each new logical line is re-prefixed with `prefix_cont`, so the
 * outer tree structure (│   /     guides) never gets broken by the
 * inner content.
 */
static void memory_emit_wrapped(nb_buf *b, const char *prefix_first, const char *prefix_cont,
				const char *label, const char *body, size_t wrap)
{
	const char *p;
	int line = 0;

	if (!body || !*body)
		return;
	p = body;
	while (*p) {
		const char *nl;
		size_t logical;
		size_t take;

		/* Skip empty logical lines. */
		while (*p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;

		/* The next line break ends this logical line. */
		nl = strpbrk(p, "\n\r");
		logical = nl ? (size_t)(nl - p) : strlen(p);
		if (logical == 0) {
			p = nl ? nl + 1 : p + strlen(p);
			continue;
		}
		take = logical > wrap ? nb_utf8_clamp_bytes(p, wrap) : logical;
		if (take == 0)
			take = logical;

		if (line == 0) {
			if (label && *label)
				memory_appendf(b, MEMORY_RENDER_CAP, "%s%s: %.*s\n", prefix_first,
					       label, (int)take, p);
			else
				memory_appendf(b, MEMORY_RENDER_CAP, "%s%.*s\n", prefix_first,
					       (int)take, p);
		} else {
			memory_appendf(b, MEMORY_RENDER_CAP, "%s%.*s\n", prefix_cont, (int)take, p);
		}
		p += take;
		line++;
		if (line > 4) /* hard stop after a few wraps */
			break;
	}
}

/*
 * Pull "Task: ...", "Outcome: ...", "Tools: ...", "Result: ..." from a
 * legacy-format summary built by memory_insert_episode(). Each output
 * pointer is non-NULL only if the field is present; substring is into
 * `summary` (NUL-terminated via static buffer reuse not needed — caller
 * can copy if it must outlive the original buffer).
 */
struct memory_task_summary {
	char task[512];
	char outcome[64];
	char tools[256];
	char result[1024];
	int has_task;
	int has_outcome;
	int has_tools;
	int has_result;
	int shortened;
};

static int memory_copy_field(char *dst, size_t cap, const char *src, size_t n)
{
	size_t take = n;
	if (cap < sizeof("…"))
		return 1;
	int shortened = take >= cap;
	if (shortened)
		take = cap - sizeof("…");
	/* Trim trailing whitespace before the boundary. */
	while (take > 0 && (src[take - 1] == ' ' || src[take - 1] == '\t' || src[take - 1] == '\n'))
		take--;
	/* Snap to UTF-8 boundary. */
	take = nb_utf8_clamp_bytes(src, take);
	memcpy(dst, src, take);
	dst[take] = '\0';
	if (shortened)
		memcpy(dst + take, "…", sizeof("…"));
	nb_utf8_sanitize_inplace(dst);
	return shortened;
}

static void memory_parse_task_summary(const char *summary, struct memory_task_summary *v)
{
	const char *fields[4];
	const char *labels[4] = {"Task: ", "Outcome: ", "Tools: ", "Result: "};
	const char *next;
	int i;

	memset(v, 0, sizeof(*v));
	if (!summary || !*summary)
		return;

	for (i = 0; i < 4; i++)
		fields[i] = strstr(summary, labels[i]);

	for (i = 0; i < 4; i++) {
		size_t off;
		size_t end;
		const char *body;
		if (!fields[i])
			continue;
		off = strlen(labels[i]);
		body = fields[i] + off;
		/*
		 * Find the next " | <Label>: " separator, whichever
		 * comes first; otherwise consume to end of string.
		 */
		next = NULL;
		{
			int j;
			for (j = 0; j < 4; j++) {
				if (!fields[j] || j == i)
					continue;
				if (fields[j] <= fields[i])
					continue;
				if (!next || fields[j] < next)
					next = fields[j];
			}
		}
		if (next) {
			/* Walk back over " | " separator (3 bytes). */
			end = (size_t)(next - body);
			while (end > 0 && (body[end - 1] == ' ' || body[end - 1] == '|'))
				end--;
		} else {
			end = strlen(body);
		}
		switch (i) {
		case 0:
			v->shortened |= memory_copy_field(v->task, sizeof(v->task), body, end);
			v->has_task = v->task[0] != '\0';
			break;
		case 1:
			v->shortened |= memory_copy_field(v->outcome, sizeof(v->outcome), body,
			end);
			v->has_outcome = v->outcome[0] != '\0';
			break;
		case 2:
			v->shortened |= memory_copy_field(v->tools, sizeof(v->tools), body, end);
			v->has_tools = v->tools[0] != '\0';
			break;
		case 3:
			v->shortened |= memory_copy_field(v->result, sizeof(v->result), body, end);
			v->has_result = v->result[0] != '\0';
			break;
		}
	}
	/* Fallback: no labels at all → treat the whole thing as task. */
	if (!v->has_task && !v->has_outcome && !v->has_tools && !v->has_result) {
		v->shortened |= memory_copy_field(v->task, sizeof(v->task), summary,
		strlen(summary));
		v->has_task = v->task[0] != '\0';
	}
}

static char *memory_render_archive(struct db *db, int64_t session_id, int max_past_tasks)
{
	struct memory_snapshot snapshot NB_CLEANUP(memory_snapshot_cleanup) = {0};
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	/*
	 * max_past_tasks is a soft cap: <= 0 means "render everything".
	 * The /mem command intentionally passes 0 to show the full record.
	 */
	int task_limit = max_past_tasks > 0 ? max_past_tasks : -1;

	int has_user_summary = 0;
	int n_facts = 0;
	int n_rules = 0;
	int task_total = 0;
	int task_count = 0;
	int n_changes = 0;
	int sections_total = 0;
	int sections_seen = 0;

	if (!db || !db->handle)
		return NULL;

	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, MEMORY_APPEND_INIT_CAP) ? 0 : NB_ERR_NOMEM) !=
	    0)
		return NULL;

	struct memory_snapshot_options read_options = {
		.facts_limit = -1,
		.tasks_limit = task_limit,
		.changes_limit = -1,
		.include_user_summary = 1,
		.include_rules = 1,
	};

	if (memory_snapshot_read(db, session_id, &read_options, &snapshot) != 0) {
		nb_buf_cleanup(&buf);
		return NULL;
	}
	has_user_summary = snapshot.user_summary && snapshot.user_summary[0];
	n_facts = (int)snapshot.facts.len;
	n_rules = (int)snapshot.rules.len;
	task_total = snapshot.task_count;
	task_count = (int)snapshot.tasks.len;
	n_changes = (int)snapshot.changes.len;

	sections_total = (has_user_summary ? 1 : 0) + (n_facts > 0 ? 1 : 0) +
			 (n_rules > 0 ? 1 : 0) + (task_count > 0 ? 1 : 0) + (n_changes > 0 ? 1 : 0);

	if (sections_total == 0) {
		memory_snapshot_cleanup(&snapshot);
		nb_buf_cleanup(&buf);
		return strdup("No long-term memory stored for this session.");
	}

	memory_appendf(&buf, MEMORY_RENDER_CAP, "memory\n");

	/* Profile */
	if (has_user_summary) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Profile\n", branch);
		memory_emit_profile_body(&buf, vbar, snapshot.user_summary);
	}

	/* Current facts */
	if (n_facts > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Current facts (%d)\n", branch, n_facts);
		for (size_t row = 0; row < snapshot.facts.len; row++) {
			const struct memory_fact_record *record = nb_vec_at(&snapshot.facts, row);

			const char *key_name = record->key;
			const char *value_text = record->value;
			const char *category = record->category;
			double importance = record->importance;
			const char *cb;
			if (!key_name || !value_text)
				continue;
			cb = (idx == n_facts - 1) ? "└──" : "├──";
			memory_appendf(&buf, MEMORY_RENDER_CAP, "%s%s [%s|imp=%.2f] %s: %s\n", vbar,
				       cb, category && *category ? category : "general", importance,
				       key_name, value_text);
			idx++;
		}
	}

	/* Unapplied rules */
	if (n_rules > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Unapplied rules (%d)\n", branch,
			       n_rules);
		for (size_t row = 0; row < snapshot.rules.len; row++) {
			const struct memory_rule_record *record = nb_vec_at(&snapshot.rules, row);

			const char *rule_text = record->text;
			int evidence = record->evidence;
			const char *cb;
			if (!rule_text)
				continue;
			cb = (idx == n_rules - 1) ? "└──" : "├──";
			memory_appendf(&buf, MEMORY_RENDER_CAP, "%s%s %s (evidence=%d)\n", vbar, cb,
				       rule_text, evidence);
			idx++;
		}
	}

	/* Past tasks */
	if (task_count > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Past tasks (%d/%d)\n", branch,
			       task_count, task_total);
		for (size_t row = 0; row < snapshot.tasks.len; row++) {
			const struct memory_task_record *record = nb_vec_at(&snapshot.tasks, row);

			const char *summary = record->summary;
			const char *decisions = record->decisions;
			const char *artifacts = record->artifacts;
			const char *tools_col = record->tools;
			double importance = record->importance;
			long long created_at = (long long)record->created_at;
			const char *cb;
			const char *vbar2;
			struct memory_task_summary ev;
			/*
			 * Each rendered child line lives under
			 * `vbar + vbar2 + ...`. We assemble the
			 * concrete prefixes lazily per sub-node so
			 * we always know if the next sibling is
			 * "last" (└──) or "middle" (├──).
			 */
			char prefix_first[64];
			char prefix_cont[64];
			int total_subs;
			int sub_idx;

			if (!summary)
				continue;
			if (idx >= task_count)
				break;
			cb = (idx == task_count - 1) ? "└──" : "├──";
			vbar2 = (idx == task_count - 1) ? "    " : "│   ";

			/* Episode header is short and structured: just
			 * the importance/timestamp + outcome flag. */
			memory_parse_task_summary(summary, &ev);
			memory_appendf(&buf, MEMORY_RENDER_CAP, "%s%s [imp=%.2f t=%lld]%s%s\n",
				       vbar, cb, importance, created_at, ev.has_outcome ? " " : "",
				       ev.has_outcome ? ev.outcome : "");

			total_subs = (ev.has_task ? 1 : 0) + (ev.has_tools ? 1 : 0) +
				     ((tools_col && *tools_col && !ev.has_tools) ? 1 : 0) +
				     (ev.has_result ? 1 : 0) + ((decisions && *decisions) ? 1 : 0) +
				     ((artifacts && *artifacts) ? 1 : 0) + (ev.shortened ? 1 : 0);
			sub_idx = 0;

#define MEMORY_PFX(is_last)                                                                        \
	do {                                                                                       \
		snprintf(prefix_first, sizeof(prefix_first), "%s%s%s ", vbar, vbar2,               \
			 (is_last) ? "└──" : "├──");                                               \
		snprintf(prefix_cont, sizeof(prefix_cont), "%s%s    ", vbar, vbar2);               \
	} while (0)

			if (ev.has_task) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "task",
						    ev.task, 200);
			}
			if (ev.has_tools) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "tools",
						    ev.tools, 200);
			} else if (tools_col && *tools_col) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "tools",
						    tools_col, 200);
			}
			if (decisions && *decisions) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "decisions",
						    decisions, 200);
			}
			if (artifacts && *artifacts) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "artifacts",
						    artifacts, 200);
			}
			if (ev.has_result) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(&buf, prefix_first, prefix_cont, "result",
						    ev.result, 200);
			}

			if (ev.shortened) {
				MEMORY_PFX(++sub_idx == total_subs);
				memory_emit_wrapped(
					&buf, prefix_first, prefix_cont, "details",
					"Display shortened; use memory type=past_tasks for the "
					"complete record.",
					200);
			}

#undef MEMORY_PFX
			idx++;
		}
	}

	/* Recent changes */
	if (n_changes > 0) {
		const char *vbar = "    ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "└── Recent changes (%d)\n", n_changes);
		for (size_t row = 0; row < snapshot.changes.len; row++) {
			const struct memory_change_record *record =
				nb_vec_at(&snapshot.changes, row);

			const char *key_name = record->key;
			const char *old_value = record->old_value;
			const char *new_value = record->new_value;
			long long changed_at = (long long)record->updated_at;
			const char *cb;
			if (!key_name || !old_value)
				continue;
			cb = (idx == n_changes - 1) ? "└──" : "├──";
			memory_appendf(&buf, MEMORY_RENDER_CAP, "%s%s %s: %s -> %s (t=%lld)\n",
				       vbar, cb, key_name, old_value,
				       new_value ? new_value : "(unset)", changed_at);
			idx++;
		}
	}

	memory_snapshot_cleanup(&snapshot);
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

char *memory_session_text_create(struct db *db, int64_t session_id, int max_past_tasks)
{
	char *effective NB_CLEANUP(nb_charp_cleanup) = preference_render_copy(db, session_id, 0);
	char *archive NB_CLEANUP(nb_charp_cleanup) =
		memory_render_archive(db, session_id, max_past_tasks);
	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {0};
	struct memory_search_options options = {.kind = "learned_notes"};
	struct memory_record *record;
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = memory_search_records(db, session_id, &options, &matches);

	if (rc != 0 || !effective || !archive)
		return NULL;
	if (!*effective && !matches.records.len)
		return nb_charp_steal(&archive);
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 512) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && *effective)
		rc = (nb_buf_printf(&buf, "Effective preferences\n%s\n", effective) ? 0
										    : NB_ERR_NOMEM);
	if (rc == 0 && (!matches.records.len ||
			strcmp(archive, "No long-term memory stored for this session.")))
		rc = (nb_buf_printf(&buf, "Session archive (not preference instructions)\n%s",
				    archive)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0 && matches.records.len)
		rc = (nb_buf_append_cstr(
			      &buf, "\nLearned memory (inferred; suggested settings are not active "
				    "preferences)\n")
			      ? 0
			      : NB_ERR_NOMEM);
	for (size_t _i_ = 0; _i_ < nb_vec_len(&matches.records) &&
			     (record = (struct memory_record *)nb_vec_at(&matches.records, _i_));
	     _i_++) {
		if (rc != 0)
			return NULL;
		rc = memory_appendf(&buf, MEMORY_RENDER_CAP, "[%s#%lld] %s: %s\nSource: %s\n",
				    record->kind, (long long)record->id, record->key, record->value,
				    record->source);
	}
	if (rc != 0)
		return NULL;
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

char *memory_background_text_create(struct db *db, int64_t session_id)
{
	struct memory_job_records records NB_CLEANUP(memory_job_records_cleanup) = {0};
	struct memory_job_record *record;
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = memory_job_store_recent(db, session_id, 20, &records);

	if (rc)
		return NULL;
	rc = (nb_buf_init(&text), nb_buf_reserve(&text, 512) ? 0 : NB_ERR_NOMEM);
	if (rc) {
		memory_job_records_cleanup(&records);
		return NULL;
	}
	for (size_t _i_ = 0; _i_ < nb_vec_len(&records.rows) &&
			     (record = (struct memory_job_record *)nb_vec_at(&records.rows, _i_));
	     _i_++) {
		rc = (nb_buf_printf(&text, "job #%lld: %s — %s (attempts=%d; %s)\n",
				    (long long)record->id, record->state,
				    !strcmp(record->stage, "organize") ? "updating saved summary"
								       : "reading recent work",
				    record->attempts,
				    record->error_code
					    ? nb_err_str(morph_error_descriptions(),
							 MORPH_ERROR_COUNT, record->error_code)
					    : "ok")
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc)
			break;
	}
	memory_job_records_cleanup(&records);
	if (rc) {
		nb_buf_cleanup(&text);
		return NULL;
	}
	return (nb_buf_ok(&text) ? strdup(nb_buf_cstr(&text)) : NULL);
}
