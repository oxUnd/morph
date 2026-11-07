#include "agent/memory_internal.h"

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

/* Session memory tree presentation. */

static void memory_emit_profile_body(morph_buf_t *b, const char *vbar, const char *body)
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
static void memory_emit_wrapped(morph_buf_t *b, const char *prefix_first, const char *prefix_cont,
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
		take = logical > wrap ? utf8_clamp_bytes(p, wrap) : logical;
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
struct episode_view {
	char task[512];
	char outcome[64];
	char tools[256];
	char result[1024];
	int has_task;
	int has_outcome;
	int has_tools;
	int has_result;
};

static void memory_copy_field(char *dst, size_t cap, const char *src, size_t n)
{
	size_t take = n;
	if (cap == 0)
		return;
	if (take >= cap)
		take = cap - 1;
	/* Trim trailing whitespace before the boundary. */
	while (take > 0 && (src[take - 1] == ' ' || src[take - 1] == '\t' || src[take - 1] == '\n'))
		take--;
	/* Snap to UTF-8 boundary. */
	take = utf8_clamp_bytes(src, take);
	memcpy(dst, src, take);
	dst[take] = '\0';
	utf8_sanitize_inplace(dst);
}

static void memory_parse_episode(const char *summary, struct episode_view *v)
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
			memory_copy_field(v->task, sizeof(v->task), body, end);
			v->has_task = v->task[0] != '\0';
			break;
		case 1:
			memory_copy_field(v->outcome, sizeof(v->outcome), body, end);
			v->has_outcome = v->outcome[0] != '\0';
			break;
		case 2:
			memory_copy_field(v->tools, sizeof(v->tools), body, end);
			v->has_tools = v->tools[0] != '\0';
			break;
		case 3:
			memory_copy_field(v->result, sizeof(v->result), body, end);
			v->has_result = v->result[0] != '\0';
			break;
		}
	}
	/* Fallback: no labels at all → treat the whole thing as task. */
	if (!v->has_task && !v->has_outcome && !v->has_tools && !v->has_result) {
		memory_copy_field(v->task, sizeof(v->task), summary, strlen(summary));
		v->has_task = v->task[0] != '\0';
	}
}

static char *memory_render_archive(struct db *db, int64_t session_id, int max_episodes)
{
	struct memory_snapshot snapshot;
	morph_buf_t buf;
	/*
	 * max_episodes is a soft cap: <= 0 means "render everything".
	 * The /mem command intentionally passes 0 to show the full record.
	 */
	int episode_cap = max_episodes > 0 ? max_episodes : -1;

	int has_profile = 0;
	int n_facts = 0;
	int n_rules = 0;
	int n_episodes_total = 0;
	int n_episodes = 0;
	int n_changes = 0;
	int sections_total = 0;
	int sections_seen = 0;

	if (!db || !db->handle)
		return NULL;

	if (morph_buf_init(&buf, MEMORY_APPEND_INIT_CAP) != 0)
		return NULL;

	struct memory_snapshot_options read_options = {
		.facts_limit = -1,
		.episodes_limit = episode_cap,
		.changes_limit = -1,
		.include_profile = 1,
		.include_rules = 1,
	};

	if (memory_snapshot_read(db, session_id, &read_options, &snapshot) != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	has_profile = snapshot.profile && snapshot.profile[0];
	n_facts = (int)snapshot.facts.nelts;
	n_rules = (int)snapshot.rules.nelts;
	n_episodes_total = snapshot.episode_total;
	n_episodes = (int)snapshot.episodes.nelts;
	n_changes = (int)snapshot.changes.nelts;

	sections_total = (has_profile ? 1 : 0) + (n_facts > 0 ? 1 : 0) + (n_rules > 0 ? 1 : 0) +
			 (n_episodes > 0 ? 1 : 0) + (n_changes > 0 ? 1 : 0);

	if (sections_total == 0) {
		memory_snapshot_cleanup(&snapshot);
		morph_buf_cleanup(&buf);
		return strdup("No long-term memory stored for this session.");
	}

	memory_appendf(&buf, MEMORY_RENDER_CAP, "memory\n");

	/* Profile */
	if (has_profile) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Profile\n", branch);
		memory_emit_profile_body(&buf, vbar, snapshot.profile);
	}

	/* Current facts */
	if (n_facts > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Current facts (%d)\n", branch, n_facts);
		for (size_t row = 0; row < snapshot.facts.nelts; row++) {
			const struct memory_fact_record *record =
				morph_array_get(&snapshot.facts, row);

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

	/* Standing rules */
	if (n_rules > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Standing rules (%d)\n", branch,
			       n_rules);
		for (size_t row = 0; row < snapshot.rules.nelts; row++) {
			const struct memory_rule_record *record =
				morph_array_get(&snapshot.rules, row);

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

	/* Recent episodes */
	if (n_episodes > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "%s Recent episodes (%d/%d)\n", branch,
			       n_episodes, n_episodes_total);
		for (size_t row = 0; row < snapshot.episodes.nelts; row++) {
			const struct memory_episode_record *record =
				morph_array_get(&snapshot.episodes, row);

			const char *summary = record->summary;
			const char *decisions = record->decisions;
			const char *artifacts = record->artifacts;
			const char *tools_col = record->tools;
			double importance = record->importance;
			long long created_at = (long long)record->created_at;
			const char *cb;
			const char *vbar2;
			struct episode_view ev;
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
			if (idx >= n_episodes)
				break;
			cb = (idx == n_episodes - 1) ? "└──" : "├──";
			vbar2 = (idx == n_episodes - 1) ? "    " : "│   ";

			/* Episode header is short and structured: just
			 * the importance/timestamp + outcome flag. */
			memory_parse_episode(summary, &ev);
			memory_appendf(&buf, MEMORY_RENDER_CAP, "%s%s [imp=%.2f t=%lld]%s%s\n",
				       vbar, cb, importance, created_at, ev.has_outcome ? " " : "",
				       ev.has_outcome ? ev.outcome : "");

			total_subs = (ev.has_task ? 1 : 0) + (ev.has_tools ? 1 : 0) +
				     ((tools_col && *tools_col && !ev.has_tools) ? 1 : 0) +
				     (ev.has_result ? 1 : 0) + ((decisions && *decisions) ? 1 : 0) +
				     ((artifacts && *artifacts) ? 1 : 0);
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

#undef MEMORY_PFX
			idx++;
		}
	}

	/* Recent changes */
	if (n_changes > 0) {
		const char *vbar = "    ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP, "└── Recent changes (%d)\n", n_changes);
		for (size_t row = 0; row < snapshot.changes.nelts; row++) {
			const struct memory_change_record *record =
				morph_array_get(&snapshot.changes, row);

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
	return morph_buf_detach(&buf);
}

char *memory_render_session(struct db *db, int64_t session_id, int max_episodes)
{
	char *effective = preference_render(db, session_id, 0);
	char *archive = memory_render_archive(db, session_id, max_episodes);
	morph_buf_t buf;

	if (!effective || !*effective) {
		free(effective);
		return archive;
	}
	if (morph_buf_init(&buf, 512) != 0) {
		free(effective);
		free(archive);
		return NULL;
	}
	if (morph_buf_printf(&buf,
			     "Effective preferences\n%s\n"
			     "Session archive (not preference instructions)\n%s",
			     effective, archive ? archive : "") != 0) {
		morph_buf_cleanup(&buf);
		free(effective);
		free(archive);
		return NULL;
	}
	free(effective);
	free(archive);
	return morph_buf_detach(&buf);
}
