#include "agent/memory.h"

#include <ctype.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEMORY_APPEND_INIT_CAP   256
#define MEMORY_RENDER_CAP        16384
#define MEMORY_VALUE_MIN_BYTES   2
#define MEMORY_VALUE_MAX_BYTES   200

static int64_t memory_now_unix(void)
{
	return (int64_t)time(NULL);
}

/* CJK punctuation we treat as a hard stop when extracting a value out of
 * free-form user text. Encoded as raw UTF-8 byte sequences to avoid
 * -Wmultichar issues with literal CJK chars. */
static int memory_stop_len(const unsigned char *p)
{
	if (p[0] == 0xe3 && p[1] == 0x80 &&
	    (p[2] == 0x81 || p[2] == 0x82))
		return 3;
	if (p[0] == 0xef && p[1] == 0xbc &&
	    (p[2] == 0x8c || p[2] == 0x81 ||
	     p[2] == 0x9f || p[2] == 0x9b))
		return 3;
	return 0;
}

static int memory_is_ascii_stop(char c)
{
	return c == '\n' || c == '\r' || c == '.' || c == '!' ||
	       c == '?' || c == ',' || c == ';';
}

/* Reject extracted values that are obviously not a fact value:
 *   - empty / too short / too long
 *   - whitespace-only
 *   - lead with a Chinese auxiliary or particle (e.g. "了一辆出租车")
 * This is the last line of defense after the substring anchors fire. */
static int memory_value_is_plausible(const char *value)
{
	static const char *bad_prefixes[] = {
		"\xe4\xba\x86",          /* 了 */
		"\xe7\x9a\x84",          /* 的 */
		"\xe5\xbe\x97",          /* 得 */
		"\xe7\x9d\x80",          /* 着 */
		"\xe8\xbf\x87",          /* 过 */
		"\xe6\x83\xb3",          /* 想 */
		"\xe7\x9f\xa5\xe9\x81\x93", /* 知道 */
		"\xe4\xb8\x8d",          /* 不 */
		"\xe6\xb2\xa1",          /* 没 */
		NULL,
	};
	size_t len;
	int has_visible = 0;

	if (!value)
		return 0;
	len = strlen(value);
	if (len < MEMORY_VALUE_MIN_BYTES || len > MEMORY_VALUE_MAX_BYTES)
		return 0;
	for (size_t i = 0; i < len; i++) {
		if (!isspace((unsigned char)value[i])) {
			has_visible = 1;
			break;
		}
	}
	if (!has_visible)
		return 0;
	for (int i = 0; bad_prefixes[i]; i++) {
		size_t plen = strlen(bad_prefixes[i]);
		if (len >= plen && memcmp(value, bad_prefixes[i], plen) == 0)
			return 0;
	}
	return 1;
}

static void memory_trim_inplace(char *s)
{
	size_t len;
	size_t start = 0;

	if (!s)
		return;
	len = strlen(s);
	while (start < len && isspace((unsigned char)s[start]))
		start++;
	while (len > start && isspace((unsigned char)s[len - 1]))
		len--;
	if (start > 0)
		memmove(s, s + start, len - start);
	s[len - start] = '\0';
}

static char *memory_snippet(const char *text, size_t max_len)
{
	size_t len;
	char *out;

	if (!text)
		return strdup("");
	len = strlen(text);
	if (len > max_len)
		len = max_len;
	out = strndup(text, len);
	if (!out)
		return NULL;
	memory_trim_inplace(out);
	return out;
}

static char *memory_lower_copy(const char *s)
{
	size_t len;
	char *out;

	if (!s)
		return strdup("");
	len = strlen(s);
	out = (char *)malloc(len + 1);
	if (!out)
		return NULL;
	for (size_t i = 0; i < len; i++)
		out[i] = (char)tolower((unsigned char)s[i]);
	out[len] = '\0';
	return out;
}

static int memory_contains_ci(const char *haystack, const char *needle)
{
	char *lhs;
	char *rhs;
	int found;

	if (!haystack || !needle || !*needle)
		return 0;
	lhs = memory_lower_copy(haystack);
	rhs = memory_lower_copy(needle);
	if (!lhs || !rhs) {
		free(lhs);
		free(rhs);
		return 0;
	}
	found = strstr(lhs, rhs) != NULL;
	free(lhs);
	free(rhs);
	return found;
}

static size_t memory_scan_value(const char *start, size_t out_sz)
{
	size_t len = 0;

	while (start[len] && len + 1 < out_sz) {
		if (memory_is_ascii_stop(start[len]))
			break;
		if (memory_stop_len((const unsigned char *)(start + len)) > 0)
			break;
		len++;
	}
	return len;
}

static int memory_extract_after_ci(const char *text, const char *needle,
				   char *out, size_t out_sz)
{
	char *lower_text;
	char *lower_needle;
	char *match;
	size_t offset;
	const char *start;
	size_t len;

	if (!text || !needle || !out || out_sz == 0)
		return 0;
	lower_text = memory_lower_copy(text);
	lower_needle = memory_lower_copy(needle);
	if (!lower_text || !lower_needle) {
		free(lower_text);
		free(lower_needle);
		return 0;
	}
	match = strstr(lower_text, lower_needle);
	if (!match) {
		free(lower_text);
		free(lower_needle);
		return 0;
	}
	offset = (size_t)(match - lower_text);
	start = text + offset + strlen(needle);
	while (*start && isspace((unsigned char)*start))
		start++;
	len = memory_scan_value(start, out_sz);
	memcpy(out, start, len);
	out[len] = '\0';
	memory_trim_inplace(out);
	free(lower_text);
	free(lower_needle);
	return out[0] != '\0';
}

static int memory_extract_after_raw(const char *text, const char *needle,
				    char *out, size_t out_sz)
{
	const char *match;
	const char *start;
	size_t len;

	if (!text || !needle || !out || out_sz == 0)
		return 0;
	match = strstr(text, needle);
	if (!match)
		return 0;
	start = match + strlen(needle);
	while (*start && isspace((unsigned char)*start))
		start++;
	len = memory_scan_value(start, out_sz);
	memcpy(out, start, len);
	out[len] = '\0';
	memory_trim_inplace(out);
	return out[0] != '\0';
}

/* Append a formatted chunk into a growable buffer with an upper bound.
 *   - Starts with a small allocation and doubles up to max_len + 1.
 *   - Once the buffer is full, *len is pinned to max_len so subsequent
 *     calls become no-ops instead of overwriting earlier content. */
static int memory_appendf(char **buf, size_t *cap, size_t *len,
			  size_t max_len, const char *fmt, ...)
{
	va_list args;
	int needed;
	size_t cap_limit;

	if (!buf || !cap || !len || !fmt)
		return -EINVAL;
	if (max_len == 0)
		max_len = MEMORY_APPEND_INIT_CAP;
	cap_limit = max_len + 1;
	if (*len >= max_len)
		return 0;

	if (*buf == NULL || *cap == 0) {
		size_t init = MEMORY_APPEND_INIT_CAP;
		if (init > cap_limit)
			init = cap_limit;
		*buf = (char *)calloc(init, 1);
		if (!*buf)
			return -ENOMEM;
		*cap = init;
	}

	va_start(args, fmt);
	needed = vsnprintf(*buf + *len, *cap - *len, fmt, args);
	va_end(args);
	if (needed < 0)
		return -EINVAL;

	if ((size_t)needed >= *cap - *len && *cap < cap_limit) {
		size_t new_cap = *cap;
		while (new_cap < *len + (size_t)needed + 1 &&
		       new_cap < cap_limit) {
			size_t doubled = new_cap * 2;
			new_cap = doubled > cap_limit ? cap_limit : doubled;
		}
		if (new_cap > *cap) {
			char *tmp = (char *)realloc(*buf, new_cap);
			if (!tmp)
				return -ENOMEM;
			*buf = tmp;
			*cap = new_cap;
			va_start(args, fmt);
			needed = vsnprintf(*buf + *len, *cap - *len, fmt, args);
			va_end(args);
			if (needed < 0)
				return -EINVAL;
		}
	}

	if ((size_t)needed >= *cap - *len) {
		/* vsnprintf truncated because we hit the hard cap. Pin len to
		 * max_len so further appends short-circuit. */
		*len = max_len;
		(*buf)[max_len] = '\0';
		return 0;
	}
	*len += (size_t)needed;
	return 0;
}

static int memory_upsert_fact(struct db *db, int64_t session_id,
			      const char *key_name, const char *value_text,
			      const char *source_text)
{
	sqlite3_stmt *stmt = NULL;
	const char *select_sql =
		"SELECT id, value_text FROM memory_facts "
		"WHERE session_id=? AND key_name=? AND is_current=1 "
		"ORDER BY updated_at DESC LIMIT 1";
	int rc;
	int in_tx = 0;
	int64_t old_id = 0;
	char *old_value = NULL;
	int64_t now = memory_now_unix();

	if (!db || !db->handle || !key_name || !value_text || !*value_text)
		return -EINVAL;

	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		old_id = sqlite3_column_int64(stmt, 0);
		{
			const char *text = (const char *)sqlite3_column_text(stmt, 1);
			if (text)
				old_value = strdup(text);
		}
	}
	sqlite3_finalize(stmt);

	/* Same value as the current fact: refresh metadata only, no
	 * supersession needed. */
	if (old_id > 0 && old_value && strcmp(old_value, value_text) == 0) {
		const char *update_sql =
			"UPDATE memory_facts "
			"SET source_text=?, updated_at=?, valid_to=NULL, is_current=1 "
			"WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			free(old_value);
			return -EIO;
		}
		sqlite3_bind_text(stmt, 1, source_text ? source_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, now);
		sqlite3_bind_int64(stmt, 3, old_id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		free(old_value);
		return rc == SQLITE_DONE ? 0 : -EIO;
	}

	/* Supersede + insert + back-link must be atomic so we never end up
	 * with an expired old fact and no new fact to replace it. */
	if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
		free(old_value);
		return -EIO;
	}
	in_tx = 1;

	if (old_id > 0) {
		const char *expire_sql =
			"UPDATE memory_facts "
			"SET is_current=0, valid_to=?, updated_at=? "
			"WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, expire_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			goto fail;
		sqlite3_bind_int64(stmt, 1, now);
		sqlite3_bind_int64(stmt, 2, now);
		sqlite3_bind_int64(stmt, 3, old_id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
			goto fail;
	}

	{
		const char *insert_sql =
			"INSERT INTO memory_facts("
			"session_id,key_name,value_text,source_text,confidence,"
			"is_current,valid_from,valid_to,superseded_by,created_at,updated_at"
			") VALUES(?,?,?,?,1.0,1,?,NULL,NULL,?,?)";
		int64_t new_id;
		rc = sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			goto fail;
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, value_text, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, source_text ? source_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 5, now);
		sqlite3_bind_int64(stmt, 6, now);
		sqlite3_bind_int64(stmt, 7, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
			goto fail;
		new_id = sqlite3_last_insert_rowid(db->handle);
		if (old_id > 0) {
			const char *link_sql =
				"UPDATE memory_facts SET superseded_by=? WHERE id=?";
			rc = sqlite3_prepare_v2(db->handle, link_sql, -1, &stmt, NULL);
			if (rc != SQLITE_OK)
				goto fail;
			sqlite3_bind_int64(stmt, 1, new_id);
			sqlite3_bind_int64(stmt, 2, old_id);
			rc = sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			if (rc != SQLITE_DONE)
				goto fail;
		}
	}

	if (sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
		goto fail;
	free(old_value);
	return 0;

fail:
	if (in_tx)
		sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
	free(old_value);
	return -EIO;
}

static int memory_upsert_procedure(struct db *db, int64_t session_id,
				   const char *rule_text,
				   const char *trigger_text)
{
	sqlite3_stmt *stmt = NULL;
	const char *select_sql =
		"SELECT id, evidence_count FROM memory_procedures "
		"WHERE session_id=? AND rule_text=? LIMIT 1";
	int rc;
	int64_t now = memory_now_unix();
	int64_t id = 0;
	int evidence = 0;

	if (!db || !db->handle || !rule_text || !*rule_text)
		return -EINVAL;

	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, rule_text, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		id = sqlite3_column_int64(stmt, 0);
		evidence = sqlite3_column_int(stmt, 1);
	}
	sqlite3_finalize(stmt);

	if (id > 0) {
		const char *update_sql =
			"UPDATE memory_procedures "
			"SET trigger_text=?, evidence_count=?, updated_at=? "
			"WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
		sqlite3_bind_text(stmt, 1, trigger_text ? trigger_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 2, evidence + 1);
		sqlite3_bind_int64(stmt, 3, now);
		sqlite3_bind_int64(stmt, 4, id);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		return rc == SQLITE_DONE ? 0 : -EIO;
	}

	{
		const char *insert_sql =
			"INSERT INTO memory_procedures("
			"session_id,rule_text,trigger_text,evidence_count,updated_at"
			") VALUES(?,?,?,?,?)";
		rc = sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, rule_text, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, trigger_text ? trigger_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 4, 1);
		sqlite3_bind_int64(stmt, 5, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		return rc == SQLITE_DONE ? 0 : -EIO;
	}
}

static int memory_refresh_profile(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	const char *select_sql =
		"SELECT key_name, value_text FROM memory_facts "
		"WHERE session_id=? AND is_current=1 "
		"ORDER BY updated_at DESC";
	char *profile = NULL;
	size_t cap = 0;
	size_t len = 0;
	int rc;
	int64_t now = memory_now_unix();

	if (!db || !db->handle)
		return -EINVAL;

	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *key_name = (const char *)sqlite3_column_text(stmt, 0);
		const char *value_text = (const char *)sqlite3_column_text(stmt, 1);
		const char *label = key_name;
		if (!key_name || !value_text)
			continue;
		if (strcmp(key_name, "preferred_name") == 0)
			label = "Preferred name";
		else if (strcmp(key_name, "user_name") == 0)
			label = "User name";
		else if (strcmp(key_name, "preferred_language") == 0)
			label = "Preferred language";
		else if (strcmp(key_name, "response_style") == 0)
			label = "Response style";
		else if (strcmp(key_name, "goal") == 0)
			label = "Current goal";
		else if (strcmp(key_name, "location") == 0)
			label = "Location";
		memory_appendf(&profile, &cap, &len, 2048, "- %s: %s\n",
			       label, value_text);
	}
	sqlite3_finalize(stmt);

	if (!profile)
		profile = strdup("");
	if (!profile)
		return -ENOMEM;

	{
		const char *upsert_sql =
			"INSERT INTO memory_profiles(session_id, profile_text, updated_at) "
			"VALUES(?,?,?) "
			"ON CONFLICT(session_id) DO UPDATE SET "
			"profile_text=excluded.profile_text, updated_at=excluded.updated_at";
		rc = sqlite3_prepare_v2(db->handle, upsert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			free(profile);
			return -EIO;
		}
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, profile, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		free(profile);
		return rc == SQLITE_DONE ? 0 : -EIO;
	}
}

/* Try a list of anchor phrases; succeed only when the captured value passes
 * the plausibility filter. Stricter anchors come first to win over the
 * looser ones (e.g. "i live in" before any future "i live ..." anchor). */
static int memory_try_anchors(const char *user_input, char *value,
			      size_t value_sz,
			      const char *const *ci_needles,
			      const char *const *raw_needles)
{
	if (ci_needles) {
		for (int i = 0; ci_needles[i]; i++) {
			if (memory_extract_after_ci(user_input, ci_needles[i],
						    value, value_sz) &&
			    memory_value_is_plausible(value))
				return 1;
		}
	}
	if (raw_needles) {
		for (int i = 0; raw_needles[i]; i++) {
			if (memory_extract_after_raw(user_input, raw_needles[i],
						     value, value_sz) &&
			    memory_value_is_plausible(value))
				return 1;
		}
	}
	value[0] = '\0';
	return 0;
}

static int memory_capture_hot_path(struct db *db, int64_t session_id,
				   const char *user_input)
{
	/* Anchors are deliberately conservative: we'd rather miss a fact than
	 * record a wrong one. Phrases like "i am " or "我想" were dropped
	 * because they fire on neutral statements ("I am tired", "我想知道
	 * 天气"). */
	static const char *preferred_name_ci[] = {
		"call me ", "please call me ", NULL,
	};
	static const char *preferred_name_raw[] = {
		"\xe5\x8f\xab\xe6\x88\x91",  /* 叫我 */
		NULL,
	};
	static const char *user_name_ci[] = {
		"my name is ", "i am called ", "i'm called ", NULL,
	};
	static const char *user_name_raw[] = {
		"\xe6\x88\x91\xe5\x8f\xab",  /* 我叫 */
		"\xe6\x88\x91\xe7\x9a\x84\xe5\x90\x8d\xe5\xad\x97\xe6\x98\xaf", /* 我的名字是 */
		NULL,
	};
	static const char *goal_ci[] = {
		"my goal is ", "i want to ", "i'd like to ", NULL,
	};
	static const char *goal_raw[] = {
		"\xe6\x88\x91\xe7\x9a\x84\xe7\x9b\xae\xe6\xa0\x87\xe6\x98\xaf", /* 我的目标是 */
		NULL,
	};
	static const char *location_ci[] = {
		"i live in ", "i am in ", "i'm in ", NULL,
	};
	static const char *location_raw[] = {
		"\xe6\x88\x91\xe4\xbd\x8f\xe5\x9c\xa8", /* 我住在 */
		NULL,
	};

	char value[256];
	char rule[512];
	int dirty = 0;
	int worst = 0;
	int rc = 0;

	if (!db || !db->handle || !user_input)
		return -EINVAL;

#define TRY_FACT(key, ci, raw)						\
	do {								\
		if (memory_try_anchors(user_input, value, sizeof(value),\
				       ci, raw)) {			\
			rc = memory_upsert_fact(db, session_id, key,	\
						value, user_input);	\
			if (rc != 0 && worst == 0)			\
				worst = rc;				\
			else if (rc == 0)				\
				dirty = 1;				\
		}							\
	} while (0)

	TRY_FACT("preferred_name", preferred_name_ci, preferred_name_raw);
	/* user_name comes after location-style anchors so "I am in Tokyo"
	 * never lands here; user_name_ci itself uses strict anchors. */
	TRY_FACT("user_name", user_name_ci, user_name_raw);
	TRY_FACT("goal", goal_ci, goal_raw);
	TRY_FACT("location", location_ci, location_raw);
#undef TRY_FACT

#define APPLY_PREF(value_text, rule_text)				\
	do {								\
		rc = memory_upsert_fact(db, session_id,			\
					"preferred_language",		\
					value_text, user_input);	\
		if (rc != 0 && worst == 0) worst = rc;			\
		else if (rc == 0) dirty = 1;				\
		rc = memory_upsert_procedure(db, session_id,		\
					     rule_text, user_input);	\
		if (rc != 0 && worst == 0) worst = rc;			\
	} while (0)

	if (memory_contains_ci(user_input, "in chinese") ||
	    memory_contains_ci(user_input, "use chinese") ||
	    memory_contains_ci(user_input, "reply in chinese") ||
	    strstr(user_input, "\xe4\xb8\xad\xe6\x96\x87") != NULL /* 中文 */) {
		APPLY_PREF("Chinese",
			   "Respond in Chinese by default unless the user requests another language.");
	}
	if (memory_contains_ci(user_input, "in english") ||
	    memory_contains_ci(user_input, "use english") ||
	    memory_contains_ci(user_input, "reply in english") ||
	    strstr(user_input, "\xe8\x8b\xb1\xe6\x96\x87") != NULL /* 英文 */) {
		APPLY_PREF("English",
			   "Respond in English by default unless the user requests another language.");
	}
#undef APPLY_PREF

#define APPLY_STYLE(value_text, rule_text)				\
	do {								\
		rc = memory_upsert_fact(db, session_id,			\
					"response_style",		\
					value_text, user_input);	\
		if (rc != 0 && worst == 0) worst = rc;			\
		else if (rc == 0) dirty = 1;				\
		rc = memory_upsert_procedure(db, session_id,		\
					     rule_text, user_input);	\
		if (rc != 0 && worst == 0) worst = rc;			\
	} while (0)

	if (memory_contains_ci(user_input, "be concise") ||
	    memory_contains_ci(user_input, "keep it concise") ||
	    memory_contains_ci(user_input, "be brief") ||
	    strstr(user_input, "\xe7\xae\x80\xe6\xb4\x81") != NULL /* 简洁 */ ||
	    strstr(user_input, "\xe7\xae\x80\xe7\x9f\xad") != NULL /* 简短 */) {
		APPLY_STYLE("concise",
			    "Prefer concise responses unless the user explicitly asks for detail.");
	}
	if (memory_contains_ci(user_input, "be detailed") ||
	    memory_contains_ci(user_input, "more detail") ||
	    strstr(user_input, "\xe8\xaf\xa6\xe7\xbb\x86") != NULL /* 详细 */) {
		APPLY_STYLE("detailed",
			    "Prefer detailed responses with more explanation unless brevity is requested.");
	}
#undef APPLY_STYLE

	if ((memory_contains_ci(user_input, "always") ||
	     strstr(user_input, "\xe4\xbb\xa5\xe5\x90\x8e\xe9\x83\xbd") != NULL /* 以后都 */ ||
	     strstr(user_input, "\xe8\xaf\xb7\xe4\xb8\x80\xe7\x9b\xb4") != NULL /* 请一直 */ ||
	     strstr(user_input, "\xe6\x80\xbb\xe6\x98\xaf") != NULL /* 总是 */) &&
	    !memory_contains_ci(user_input, "always use") &&
	    !memory_contains_ci(user_input, "always answer")) {
		char *snippet = memory_snippet(user_input, 360);
		if (snippet) {
			snprintf(rule, sizeof(rule),
				 "Standing user instruction: %s", snippet);
			rc = memory_upsert_procedure(db, session_id, rule,
						     user_input);
			if (rc != 0 && worst == 0)
				worst = rc;
			free(snippet);
		}
	}

	if (dirty) {
		rc = memory_refresh_profile(db, session_id);
		if (rc != 0 && worst == 0)
			worst = rc;
	}
	return worst;
}

static void memory_collect_tools(const struct react_step *steps,
				 char *buf, size_t buf_sz)
{
	int first = 1;

	if (!buf || buf_sz == 0)
		return;
	buf[0] = '\0';
	for (const struct react_step *cur = steps; cur; cur = cur->next) {
		size_t used;
		size_t left;
		int exists = 0;
		if (cur->type != REACT_STEP_ACTION || !cur->tool_name ||
		    !cur->tool_name[0])
			continue;
		if (strstr(buf, cur->tool_name) != NULL)
			exists = 1;
		if (exists)
			continue;
		used = strlen(buf);
		left = buf_sz - used;
		if (left <= 1)
			break;
		snprintf(buf + used, left, "%s%s", first ? "" : ", ",
			 cur->tool_name);
		first = 0;
	}
}

static const char *memory_infer_task_type(const char *user_input,
					  const char *tool_names)
{
	if ((tool_names && strstr(tool_names, "img_")) ||
	    (user_input && (memory_contains_ci(user_input, "image") ||
			    memory_contains_ci(user_input, "photo") ||
			    strstr(user_input, "图片") != NULL ||
			    strstr(user_input, "图像") != NULL)))
		return "image";
	if ((tool_names && strstr(tool_names, "vid_")) ||
	    (user_input && (memory_contains_ci(user_input, "video") ||
			    strstr(user_input, "视频") != NULL)))
		return "video";
	if ((tool_names && strstr(tool_names, "file_")) ||
	    (tool_names && strstr(tool_names, "bash_exec")) ||
	    (user_input && (memory_contains_ci(user_input, "code") ||
			    memory_contains_ci(user_input, "repo") ||
			    strstr(user_input, "代码") != NULL ||
			    strstr(user_input, "仓库") != NULL)))
		return "coding";
	if (tool_names && *tool_names)
		return "tooling";
	return "conversation";
}

static int memory_insert_episode(struct db *db, int64_t session_id,
				 const char *user_input,
				 const char *assistant_output,
				 const struct react_step *steps,
				 int success)
{
	sqlite3_stmt *stmt = NULL;
	char tools[256];
	char *user_snip = NULL;
	char *assistant_snip = NULL;
	char summary[1024];
	const char *task_type;
	const char *outcome;
	int rc;

	if (!db || !db->handle || !user_input)
		return -EINVAL;

	memory_collect_tools(steps, tools, sizeof(tools));
	user_snip = memory_snippet(user_input, 220);
	assistant_snip = memory_snippet(assistant_output ? assistant_output : "",
					220);
	if (!user_snip || !assistant_snip) {
		free(user_snip);
		free(assistant_snip);
		return -ENOMEM;
	}
	task_type = memory_infer_task_type(user_input, tools);
	outcome = success ? "completed" : "aborted";
	if (tools[0]) {
		snprintf(summary, sizeof(summary),
			 "Task: %s | Outcome: %s | Tools: %s | Result: %s",
			 user_snip, outcome, tools,
			 assistant_snip[0] ? assistant_snip : "(no final answer)");
	} else {
		snprintf(summary, sizeof(summary),
			 "Task: %s | Outcome: %s | Result: %s",
			 user_snip, outcome,
			 assistant_snip[0] ? assistant_snip : "(no final answer)");
	}

	{
		const char *insert_sql =
			"INSERT INTO memory_episodes("
			"session_id,task_type,summary_text,outcome_text,success,entities,created_at"
			") VALUES(?,?,?,?,?,?,?)";
		rc = sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			free(user_snip);
			free(assistant_snip);
			return -EIO;
		}
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, task_type, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, assistant_snip, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 5, success ? 1 : 0);
		sqlite3_bind_text(stmt, 6, tools, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 7, memory_now_unix());
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	free(user_snip);
	free(assistant_snip);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

static int memory_query_needs_episode(const char *query)
{
	if (!query)
		return 0;
	return memory_contains_ci(query, "last time") ||
	       memory_contains_ci(query, "previous") ||
	       memory_contains_ci(query, "before") ||
	       memory_contains_ci(query, "how did") ||
	       strstr(query, "上次") != NULL ||
	       strstr(query, "之前") != NULL;
}

static int memory_query_needs_temporal(const char *query)
{
	if (!query)
		return 0;
	return memory_contains_ci(query, "current") ||
	       memory_contains_ci(query, "still") ||
	       memory_contains_ci(query, "changed") ||
	       memory_contains_ci(query, "when") ||
	       strstr(query, "现在") != NULL ||
	       strstr(query, "还") != NULL ||
	       strstr(query, "变化") != NULL ||
	       strstr(query, "何时") != NULL;
}

static int memory_query_needs_procedure(const char *query)
{
	if (!query)
		return 0;
	return memory_contains_ci(query, "always") ||
	       memory_contains_ci(query, "style") ||
	       memory_contains_ci(query, "prefer") ||
	       memory_contains_ci(query, "how should you") ||
	       strstr(query, "以后") != NULL ||
	       strstr(query, "风格") != NULL;
}

static int memory_exec_delete(struct db *db, const char *sql, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !sql)
		return -EINVAL;
	rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : -EIO;
}

char *memory_build_context(struct db *db, int64_t session_id,
			   const char *query,
			   const struct memory_options *opts)
{
	sqlite3_stmt *stmt = NULL;
	char *buf = NULL;
	size_t cap = 0;
	size_t len = 0;
	int max_chars;
	int max_facts;
	int max_episodes;
	int max_procedures;
	int want_episode;
	int want_temporal;
	int want_procedure;
	int appended = 0;

	if (!db || !db->handle || !opts || !opts->enabled)
		return NULL;

	max_chars = opts->max_context_chars > 0 ? opts->max_context_chars : 3000;
	max_facts = opts->max_facts > 0 ? opts->max_facts : 6;
	max_episodes = opts->max_episodes > 0 ? opts->max_episodes : 4;
	max_procedures = opts->max_procedures > 0 ? opts->max_procedures : 4;
	want_episode = memory_query_needs_episode(query);
	want_temporal = memory_query_needs_temporal(query);
	want_procedure = memory_query_needs_procedure(query);

	/* Intro is emitted lazily so we don't waste cycles formatting it
	 * when no memory section ends up populated. */
#define MEMORY_ENSURE_INTRO()						\
	do {								\
		if (!appended)						\
			memory_appendf(&buf, &cap, &len,		\
				       (size_t)max_chars,		\
				       "Persistent memory for this session. " \
				       "Newer current facts win over older facts. " \
				       "Use only if relevant.\n");	\
	} while (0)

	{
		const char *profile_sql =
			"SELECT profile_text FROM memory_profiles WHERE session_id=?";
		if (sqlite3_prepare_v2(db->handle, profile_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *profile =
					(const char *)sqlite3_column_text(stmt, 0);
				if (profile && *profile) {
					MEMORY_ENSURE_INTRO();
					memory_appendf(&buf, &cap, &len, (size_t)max_chars,
						       "\nProfile\n%s", profile);
					appended = 1;
				}
			}
			sqlite3_finalize(stmt);
		}
	}

	{
		const char *facts_sql =
			"SELECT key_name, value_text, updated_at "
			"FROM memory_facts "
			"WHERE session_id=? AND is_current=1 "
			"ORDER BY updated_at DESC LIMIT ?";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, facts_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			sqlite3_bind_int(stmt, 2, max_facts);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name =
					(const char *)sqlite3_column_text(stmt, 0);
				const char *value_text =
					(const char *)sqlite3_column_text(stmt, 1);
				if (!key_name || !value_text)
					continue;
				if (count == 0) {
					MEMORY_ENSURE_INTRO();
					memory_appendf(&buf, &cap, &len,
						       (size_t)max_chars,
						       "\nCurrent facts\n");
				}
				memory_appendf(&buf, &cap, &len, (size_t)max_chars,
					       "- %s: %s\n", key_name, value_text);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (want_procedure) {
		const char *proc_sql =
			"SELECT rule_text, evidence_count "
			"FROM memory_procedures "
			"WHERE session_id=? "
			"ORDER BY evidence_count DESC, updated_at DESC LIMIT ?";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, proc_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			sqlite3_bind_int(stmt, 2, max_procedures);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *rule_text =
					(const char *)sqlite3_column_text(stmt, 0);
				int evidence = sqlite3_column_int(stmt, 1);
				if (!rule_text)
					continue;
				if (count == 0) {
					MEMORY_ENSURE_INTRO();
					memory_appendf(&buf, &cap, &len,
						       (size_t)max_chars,
						       "\nStanding rules\n");
				}
				memory_appendf(&buf, &cap, &len, (size_t)max_chars,
					       "- %s (evidence=%d)\n",
					       rule_text, evidence);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (want_episode) {
		const char *episode_sql =
			"SELECT summary_text FROM memory_episodes "
			"WHERE session_id=? "
			"ORDER BY created_at DESC LIMIT ?";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, episode_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			sqlite3_bind_int(stmt, 2, max_episodes);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *summary =
					(const char *)sqlite3_column_text(stmt, 0);
				if (!summary)
					continue;
				if (count == 0) {
					MEMORY_ENSURE_INTRO();
					memory_appendf(&buf, &cap, &len,
						       (size_t)max_chars,
						       "\nRelevant episodes\n");
				}
				memory_appendf(&buf, &cap, &len, (size_t)max_chars,
					       "- %s\n", summary);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (want_temporal) {
		const char *temporal_sql =
			"SELECT old.key_name, old.value_text, cur.value_text, old.valid_to "
			"FROM memory_facts old "
			"LEFT JOIN memory_facts cur ON cur.id = old.superseded_by "
			"WHERE old.session_id=? AND old.is_current=0 "
			"ORDER BY old.updated_at DESC LIMIT 4";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, temporal_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name =
					(const char *)sqlite3_column_text(stmt, 0);
				const char *old_value =
					(const char *)sqlite3_column_text(stmt, 1);
				const char *new_value =
					(const char *)sqlite3_column_text(stmt, 2);
				int64_t changed_at = sqlite3_column_int64(stmt, 3);
				if (!key_name || !old_value)
					continue;
				if (count == 0) {
					MEMORY_ENSURE_INTRO();
					memory_appendf(&buf, &cap, &len,
						       (size_t)max_chars,
						       "\nRecent changes\n");
				}
				memory_appendf(&buf, &cap, &len, (size_t)max_chars,
					       "- %s: %s -> %s (changed_at=%lld)\n",
					       key_name, old_value,
					       new_value ? new_value : "(unset)",
					       (long long)changed_at);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

#undef MEMORY_ENSURE_INTRO

	if (!appended) {
		free(buf);
		return NULL;
	}
	return buf;
}

char *memory_render_session(struct db *db, int64_t session_id,
			    int max_episodes)
{
	sqlite3_stmt *stmt = NULL;
	char *buf = NULL;
	size_t cap = 0;
	size_t len = 0;
	int appended = 0;
	int episode_limit = max_episodes > 0 ? max_episodes : 8;

	if (!db || !db->handle)
		return NULL;

	{
		const char *profile_sql =
			"SELECT profile_text FROM memory_profiles WHERE session_id=?";
		if (sqlite3_prepare_v2(db->handle, profile_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *profile =
					(const char *)sqlite3_column_text(stmt, 0);
				if (profile && *profile) {
					memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
						       "Profile\n%s",
						       profile);
					if (profile[strlen(profile) - 1] != '\n')
						memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
							       "\n");
					appended = 1;
				}
			}
			sqlite3_finalize(stmt);
		}
	}

	{
		const char *facts_sql =
			"SELECT key_name, value_text FROM memory_facts "
			"WHERE session_id=? AND is_current=1 "
			"ORDER BY updated_at DESC";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, facts_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name =
					(const char *)sqlite3_column_text(stmt, 0);
				const char *value_text =
					(const char *)sqlite3_column_text(stmt, 1);
				if (!key_name || !value_text)
					continue;
				if (count == 0) {
					if (appended)
						memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
							       "\n");
					memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
						       "Current facts\n");
				}
				memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
					       "- %s: %s\n",
					       key_name, value_text);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	{
		const char *proc_sql =
			"SELECT rule_text, evidence_count "
			"FROM memory_procedures "
			"WHERE session_id=? "
			"ORDER BY evidence_count DESC, updated_at DESC";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, proc_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *rule_text =
					(const char *)sqlite3_column_text(stmt, 0);
				int evidence = sqlite3_column_int(stmt, 1);
				if (!rule_text)
					continue;
				if (count == 0) {
					if (appended)
						memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
							       "\n");
					memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
						       "Standing rules\n");
				}
				memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
					       "- %s (evidence=%d)\n",
					       rule_text, evidence);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	{
		const char *episodes_sql =
			"SELECT summary_text FROM memory_episodes "
			"WHERE session_id=? "
			"ORDER BY created_at DESC LIMIT ?";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, episodes_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			sqlite3_bind_int(stmt, 2, episode_limit);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *summary =
					(const char *)sqlite3_column_text(stmt, 0);
				if (!summary)
					continue;
				if (count == 0) {
					if (appended)
						memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
							       "\n");
					memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
						       "Recent episodes\n");
				}
				memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
					       "- %s\n", summary);
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	{
		const char *changes_sql =
			"SELECT old.key_name, old.value_text, cur.value_text "
			"FROM memory_facts old "
			"LEFT JOIN memory_facts cur ON cur.id = old.superseded_by "
			"WHERE old.session_id=? AND old.is_current=0 "
			"ORDER BY old.updated_at DESC LIMIT 6";
		int count = 0;
		if (sqlite3_prepare_v2(db->handle, changes_sql, -1, &stmt, NULL) ==
		    SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name =
					(const char *)sqlite3_column_text(stmt, 0);
				const char *old_value =
					(const char *)sqlite3_column_text(stmt, 1);
				const char *new_value =
					(const char *)sqlite3_column_text(stmt, 2);
				if (!key_name || !old_value)
					continue;
				if (count == 0) {
					if (appended)
						memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
							       "\n");
					memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
						       "Recent changes\n");
				}
				memory_appendf(&buf, &cap, &len, MEMORY_RENDER_CAP,
					       "- %s: %s -> %s\n",
					       key_name, old_value,
					       new_value ? new_value : "(unset)");
				count++;
				appended = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (!appended)
		return strdup("No long-term memory stored for this session.");
	return buf;
}

int memory_clear(struct db *db, int64_t session_id,
		 enum memory_clear_scope scope)
{
	int rc = 0;

	if (!db || !db->handle)
		return -EINVAL;

	switch (scope) {
	case MEMORY_CLEAR_ALL:
		rc = memory_exec_delete(db,
					"DELETE FROM memory_profiles WHERE session_id=?",
					session_id);
		if (rc != 0)
			return rc;
		rc = memory_exec_delete(db,
					"DELETE FROM memory_facts WHERE session_id=?",
					session_id);
		if (rc != 0)
			return rc;
		rc = memory_exec_delete(db,
					"DELETE FROM memory_episodes WHERE session_id=?",
					session_id);
		if (rc != 0)
			return rc;
		return memory_exec_delete(
			db,
			"DELETE FROM memory_procedures WHERE session_id=?",
			session_id);
	case MEMORY_CLEAR_FACTS:
		rc = memory_exec_delete(db,
					"DELETE FROM memory_profiles WHERE session_id=?",
					session_id);
		if (rc != 0)
			return rc;
		return memory_exec_delete(
			db,
			"DELETE FROM memory_facts WHERE session_id=?",
			session_id);
	case MEMORY_CLEAR_EPISODES:
		return memory_exec_delete(
			db,
			"DELETE FROM memory_episodes WHERE session_id=?",
			session_id);
	case MEMORY_CLEAR_PROCEDURES:
		return memory_exec_delete(
			db,
			"DELETE FROM memory_procedures WHERE session_id=?",
			session_id);
	default:
		return -EINVAL;
	}
}

int memory_consolidate_turn(struct db *db, int64_t session_id,
			    const char *user_input,
			    const char *assistant_output,
			    const struct react_step *steps,
			    int success,
			    const struct memory_options *opts)
{
	int worst = 0;
	int rc;

	if (!db || !db->handle || !opts || !opts->enabled || !user_input)
		return 0;

	if (opts->hot_path_enabled) {
		rc = memory_capture_hot_path(db, session_id, user_input);
		if (rc != 0)
			worst = rc;
	}
	if (opts->cold_path_enabled) {
		rc = memory_insert_episode(db, session_id, user_input,
					   assistant_output, steps, success);
		if (rc != 0 && worst == 0)
			worst = rc;
	}
	return worst;
}
