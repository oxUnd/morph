#include "agent/memory.h"
#include "models/llm.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/utf8.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEMORY_APPEND_INIT_CAP   1024
#define MEMORY_RENDER_CAP        (1024 * 1024)
#define MEMORY_VALUE_MIN_BYTES   2
#define MEMORY_VALUE_MAX_BYTES   200

/* LLM model used for structured extraction (optional). Set via
 * memory_set_llm(); guarded by g_memory_llm_lock to remain thread-safe
 * across CLI / FastCGI worker threads (in particular the async
 * consolidation worker reads the pointer concurrently with foreground
 * set/clear calls). */
static struct model *g_memory_llm;
static pthread_mutex_t g_memory_llm_lock = PTHREAD_MUTEX_INITIALIZER;

void memory_set_llm(struct model *llm)
{
	pthread_mutex_lock(&g_memory_llm_lock);
	g_memory_llm = llm;
	pthread_mutex_unlock(&g_memory_llm_lock);
}

static struct model *memory_get_llm(void)
{
	struct model *llm;
	pthread_mutex_lock(&g_memory_llm_lock);
	llm = g_memory_llm;
	pthread_mutex_unlock(&g_memory_llm_lock);
	return llm;
}

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
	size_t safe;
	char *out;

	if (!text)
		return strdup("");
	/*
	 * Clamp to a UTF-8 boundary so we never split a multi-byte
	 * codepoint mid-stream — otherwise CJK input gets chopped to
	 * something like "蓬松\xE2" which renders as "蓬松�".
	 */
	safe = utf8_safe_len(text, max_len);
	out = strndup(text, safe);
	if (!out)
		return NULL;
	utf8_sanitize_inplace(out);
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

static int memory_appendf(morph_buf_t *b, size_t max_len,
			  const char *fmt, ...)
{
	va_list args;
	int rc;

	if (!b || !fmt)
		return -EINVAL;
	if (max_len == 0)
		max_len = MEMORY_APPEND_INIT_CAP;
	if (b->len >= max_len)
		return 0;

	va_start(args, fmt);
	rc = morph_buf_vprintf(b, fmt, args);
	va_end(args);
	if (rc < 0)
		return rc;

	if (b->len > max_len) {
		b->len = max_len;
		b->data[max_len] = '\0';
	}
	return 0;
}

static int memory_upsert_fact(struct db *db, int64_t session_id,
			      const char *key_name, const char *value_text,
			      const char *source_text,
			      const char *category, double importance)
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
	if (!category || !*category)
		category = "general";
	if (importance < 0.0 || importance > 1.0)
		importance = 0.5;

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
			"SET source_text=?, updated_at=?, valid_to=NULL, is_current=1, "
			"category=?, importance=? "
			"WHERE id=?";
		rc = sqlite3_prepare_v2(db->handle, update_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			free(old_value);
			return -EIO;
		}
		sqlite3_bind_text(stmt, 1, source_text ? source_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 2, now);
		sqlite3_bind_text(stmt, 3, category, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 4, importance);
		sqlite3_bind_int64(stmt, 5, old_id);
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
			"category,importance,"
			"is_current,valid_from,valid_to,superseded_by,created_at,updated_at"
			") VALUES(?,?,?,?,1.0,?,?,1,?,NULL,NULL,?,?)";
		int64_t new_id;
		rc = sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK)
			goto fail;
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, value_text, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, source_text ? source_text : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, category, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 6, importance);
		sqlite3_bind_int64(stmt, 7, now);
		sqlite3_bind_int64(stmt, 8, now);
		sqlite3_bind_int64(stmt, 9, now);
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
	morph_buf_t profile;
	int rc;
	int64_t now = memory_now_unix();

	if (!db || !db->handle)
		return -EINVAL;

	rc = morph_buf_init(&profile, MEMORY_APPEND_INIT_CAP);
	if (rc != 0)
		return rc;

	rc = sqlite3_prepare_v2(db->handle, select_sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		morph_buf_cleanup(&profile);
		return -EIO;
	}
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
		memory_appendf(&profile, 16384, "- %s: %s\n",
			       label, value_text);
	}
	sqlite3_finalize(stmt);

	{
		const char *upsert_sql =
			"INSERT INTO memory_profiles(session_id, profile_text, updated_at) "
			"VALUES(?,?,?) "
			"ON CONFLICT(session_id) DO UPDATE SET "
			"profile_text=excluded.profile_text, updated_at=excluded.updated_at";
		rc = sqlite3_prepare_v2(db->handle, upsert_sql, -1, &stmt, NULL);
		if (rc != SQLITE_OK) {
			morph_buf_cleanup(&profile);
			return -EIO;
		}
		sqlite3_bind_int64(stmt, 1, session_id);
		sqlite3_bind_text(stmt, 2, profile.data, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, now);
		rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		morph_buf_cleanup(&profile);
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

#define TRY_FACT(key, ci, raw, cat, importance)				\
	do {								\
		if (memory_try_anchors(user_input, value, sizeof(value),\
				       ci, raw)) {			\
			rc = memory_upsert_fact(db, session_id, key,	\
						value, user_input,	\
						cat, importance);	\
			if (rc != 0 && worst == 0)			\
				worst = rc;				\
			else if (rc == 0)				\
				dirty = 1;				\
		}							\
	} while (0)

	TRY_FACT("preferred_name", preferred_name_ci, preferred_name_raw,
		 "identity", 0.9);
	/* user_name comes after location-style anchors so "I am in Tokyo"
	 * never lands here; user_name_ci itself uses strict anchors. */
	TRY_FACT("user_name", user_name_ci, user_name_raw, "identity", 0.85);
	TRY_FACT("goal", goal_ci, goal_raw, "goal", 0.7);
	TRY_FACT("location", location_ci, location_raw, "context", 0.6);
#undef TRY_FACT

#define APPLY_PREF(value_text, rule_text)				\
	do {								\
		rc = memory_upsert_fact(db, session_id,			\
					"preferred_language",		\
					value_text, user_input,		\
					"preference", 0.8);		\
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
					value_text, user_input,		\
					"preference", 0.75);		\
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
				 int success,
				 const char *entities,
				 const char *key_decisions,
				 const char *artifacts,
				 double importance)
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
	if (importance < 0.0 || importance > 1.0)
		importance = 0.5;

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
			"session_id,task_type,summary_text,outcome_text,success,"
			"entities,key_decisions,artifacts,tools_used,importance,"
			"created_at"
			") VALUES(?,?,?,?,?,?,?,?,?,?,?)";
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
		sqlite3_bind_text(stmt, 6,
				  entities ? entities : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 7,
				  key_decisions ? key_decisions : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 8,
				  artifacts ? artifacts : "", -1,
				  SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 9, tools, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 10, importance);
		sqlite3_bind_int64(stmt, 11, memory_now_unix());
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
	morph_buf_t buf;
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

	if (morph_buf_init(&buf, MEMORY_APPEND_INIT_CAP) != 0)
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
			memory_appendf(&buf,		\
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
					memory_appendf(&buf, (size_t)max_chars,
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
					memory_appendf(&buf,
						       (size_t)max_chars,
						       "\nCurrent facts\n");
				}
				memory_appendf(&buf, (size_t)max_chars,
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
					memory_appendf(&buf,
						       (size_t)max_chars,
						       "\nStanding rules\n");
				}
				memory_appendf(&buf, (size_t)max_chars,
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
					memory_appendf(&buf,
						       (size_t)max_chars,
						       "\nRelevant episodes\n");
				}
				memory_appendf(&buf, (size_t)max_chars,
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
					memory_appendf(&buf,
						       (size_t)max_chars,
						       "\nRecent changes\n");
				}
				memory_appendf(&buf, (size_t)max_chars,
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
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

/* --- /mem 树形渲染辅助 ------------------------------------------------ */

static int memory_count_rows(struct db *db, const char *sql,
			     int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int n = 0;
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

static void memory_emit_profile_body(morph_buf_t *b, const char *vbar,
				     const char *body)
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
				memory_appendf(b, MEMORY_RENDER_CAP,
					       "%s└── %.*s\n",
					       vbar, (int)n, p);
			else
				memory_appendf(b, MEMORY_RENDER_CAP,
					       "%s    %.*s\n",
					       vbar, (int)n, p);
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
static void memory_emit_wrapped(morph_buf_t *b,
				const char *prefix_first,
				const char *prefix_cont,
				const char *label,
				const char *body,
				size_t wrap)
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

		/* 跳过领头的 '\r' / '\n' / 多余空白，避免输出空行 */
		while (*p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;

		/* 当前 logical line 在下一个换行符处终止 */
		nl = strpbrk(p, "\n\r");
		logical = nl ? (size_t)(nl - p) : strlen(p);
		if (logical == 0) {
			p = nl ? nl + 1 : p + strlen(p);
			continue;
		}
		take = logical > wrap ? utf8_safe_len(p, wrap) : logical;
		if (take == 0)
			take = logical;

		if (line == 0) {
			if (label && *label)
				memory_appendf(b, MEMORY_RENDER_CAP,
					       "%s%s: %.*s\n",
					       prefix_first, label,
					       (int)take, p);
			else
				memory_appendf(b, MEMORY_RENDER_CAP,
					       "%s%.*s\n",
					       prefix_first,
					       (int)take, p);
		} else {
			memory_appendf(b, MEMORY_RENDER_CAP,
				       "%s%.*s\n",
				       prefix_cont, (int)take, p);
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

static void memory_copy_field(char *dst, size_t cap, const char *src,
			      size_t n)
{
	size_t take = n;
	if (cap == 0)
		return;
	if (take >= cap)
		take = cap - 1;
	/* Trim trailing whitespace before the boundary. */
	while (take > 0 && (src[take - 1] == ' ' ||
			    src[take - 1] == '\t' ||
			    src[take - 1] == '\n'))
		take--;
	/* Snap to UTF-8 boundary. */
	take = utf8_safe_len(src, take);
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
			while (end > 0 && (body[end - 1] == ' ' ||
					   body[end - 1] == '|'))
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
			memory_copy_field(v->outcome, sizeof(v->outcome),
					  body, end);
			v->has_outcome = v->outcome[0] != '\0';
			break;
		case 2:
			memory_copy_field(v->tools, sizeof(v->tools), body, end);
			v->has_tools = v->tools[0] != '\0';
			break;
		case 3:
			memory_copy_field(v->result, sizeof(v->result),
					  body, end);
			v->has_result = v->result[0] != '\0';
			break;
		}
	}
	/* Fallback: no labels at all → treat the whole thing as task. */
	if (!v->has_task && !v->has_outcome && !v->has_tools &&
	    !v->has_result) {
		memory_copy_field(v->task, sizeof(v->task), summary,
				  strlen(summary));
		v->has_task = v->task[0] != '\0';
	}
}

char *memory_render_session(struct db *db, int64_t session_id,
			    int max_episodes)
{
	sqlite3_stmt *stmt = NULL;
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

	/* Detect section presence and counts up-front so we know which
	 * branch is the last one (├── vs └──) without a two-pass render. */
	if (sqlite3_prepare_v2(db->handle,
		"SELECT profile_text FROM memory_profiles WHERE session_id=?",
		-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *p =
				(const char *)sqlite3_column_text(stmt, 0);
			if (p && *p)
				has_profile = 1;
		}
		sqlite3_finalize(stmt);
	}

	n_facts = memory_count_rows(db,
		"SELECT COUNT(*) FROM memory_facts "
		"WHERE session_id=? AND is_current=1", session_id);
	n_rules = memory_count_rows(db,
		"SELECT COUNT(*) FROM memory_procedures WHERE session_id=?",
		session_id);
	n_episodes_total = memory_count_rows(db,
		"SELECT COUNT(*) FROM memory_episodes WHERE session_id=?",
		session_id);
	n_episodes = (episode_cap >= 0 && n_episodes_total > episode_cap)
		? episode_cap : n_episodes_total;
	n_changes = memory_count_rows(db,
		"SELECT COUNT(*) FROM memory_facts "
		"WHERE session_id=? AND is_current=0", session_id);

	sections_total =
		(has_profile ? 1 : 0) +
		(n_facts > 0 ? 1 : 0) +
		(n_rules > 0 ? 1 : 0) +
		(n_episodes > 0 ? 1 : 0) +
		(n_changes > 0 ? 1 : 0);

	if (sections_total == 0) {
		morph_buf_cleanup(&buf);
		return strdup("No long-term memory stored for this session.");
	}

	memory_appendf(&buf, MEMORY_RENDER_CAP, "memory\n");

	/* Profile */
	if (has_profile) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar   = last ? "    " : "│   ";
		memory_appendf(&buf, MEMORY_RENDER_CAP,
			       "%s Profile\n", branch);
		if (sqlite3_prepare_v2(db->handle,
			"SELECT profile_text FROM memory_profiles "
			"WHERE session_id=?",
			-1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *p = (const char *)
					sqlite3_column_text(stmt, 0);
				memory_emit_profile_body(&buf,
							 vbar, p);
			}
			sqlite3_finalize(stmt);
		}
	}

	/* Current facts */
	if (n_facts > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar   = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP,
			       "%s Current facts (%d)\n", branch, n_facts);
		if (sqlite3_prepare_v2(db->handle,
			"SELECT key_name, value_text, category, importance "
			"FROM memory_facts "
			"WHERE session_id=? AND is_current=1 "
			"ORDER BY importance DESC, updated_at DESC",
			-1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name = (const char *)
					sqlite3_column_text(stmt, 0);
				const char *value_text = (const char *)
					sqlite3_column_text(stmt, 1);
				const char *category = (const char *)
					sqlite3_column_text(stmt, 2);
				double importance =
					sqlite3_column_double(stmt, 3);
				const char *cb;
				if (!key_name || !value_text)
					continue;
				cb = (idx == n_facts - 1) ? "└──" : "├──";
				memory_appendf(&buf,
					       MEMORY_RENDER_CAP,
					       "%s%s [%s|imp=%.2f] %s: %s\n",
					       vbar, cb,
					       category && *category ?
						       category : "general",
					       importance,
					       key_name, value_text);
				idx++;
			}
			sqlite3_finalize(stmt);
		}
	}

	/* Standing rules */
	if (n_rules > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar   = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP,
			       "%s Standing rules (%d)\n", branch, n_rules);
		if (sqlite3_prepare_v2(db->handle,
			"SELECT rule_text, evidence_count "
			"FROM memory_procedures "
			"WHERE session_id=? "
			"ORDER BY evidence_count DESC, updated_at DESC",
			-1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *rule_text = (const char *)
					sqlite3_column_text(stmt, 0);
				int evidence = sqlite3_column_int(stmt, 1);
				const char *cb;
				if (!rule_text)
					continue;
				cb = (idx == n_rules - 1) ? "└──" : "├──";
				memory_appendf(&buf,
					       MEMORY_RENDER_CAP,
					       "%s%s %s (evidence=%d)\n",
					       vbar, cb, rule_text, evidence);
				idx++;
			}
			sqlite3_finalize(stmt);
		}
	}

	/* Recent episodes */
	if (n_episodes > 0) {
		sections_seen++;
		int last = (sections_seen == sections_total);
		const char *branch = last ? "└──" : "├──";
		const char *vbar   = last ? "    " : "│   ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP,
			       "%s Recent episodes (%d/%d)\n",
			       branch, n_episodes, n_episodes_total);
		if (sqlite3_prepare_v2(db->handle,
			"SELECT summary_text, key_decisions, artifacts, "
			"tools_used, importance, created_at "
			"FROM memory_episodes "
			"WHERE session_id=? "
			"ORDER BY created_at DESC",
			-1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *summary = (const char *)
					sqlite3_column_text(stmt, 0);
				const char *decisions = (const char *)
					sqlite3_column_text(stmt, 1);
				const char *artifacts = (const char *)
					sqlite3_column_text(stmt, 2);
				const char *tools_col = (const char *)
					sqlite3_column_text(stmt, 3);
				double importance =
					sqlite3_column_double(stmt, 4);
				long long created_at = (long long)
					sqlite3_column_int64(stmt, 5);
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
				vbar2 = (idx == n_episodes - 1) ?
					"    " : "│   ";

				/* Episode header is short and structured: just
				 * the importance/timestamp + outcome flag. */
				memory_parse_episode(summary, &ev);
				memory_appendf(&buf,
					       MEMORY_RENDER_CAP,
					       "%s%s [imp=%.2f t=%lld]%s%s\n",
					       vbar, cb,
					       importance, created_at,
					       ev.has_outcome ? " " : "",
					       ev.has_outcome ?
						       ev.outcome : "");

				total_subs =
					(ev.has_task ? 1 : 0) +
					(ev.has_tools ? 1 : 0) +
					((tools_col && *tools_col &&
					  !ev.has_tools) ? 1 : 0) +
					(ev.has_result ? 1 : 0) +
					((decisions && *decisions) ? 1 : 0) +
					((artifacts && *artifacts) ? 1 : 0);
				sub_idx = 0;

				#define MEMORY_PFX(is_last)                          \
					do {                                         \
						snprintf(prefix_first,               \
							 sizeof(prefix_first),       \
							 "%s%s%s ", vbar, vbar2,     \
							 (is_last) ? "└──"           \
								   : "├──");        \
						snprintf(prefix_cont,                \
							 sizeof(prefix_cont),        \
							 "%s%s    ", vbar, vbar2);   \
					} while (0)

				if (ev.has_task) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"task", ev.task, 200);
				}
				if (ev.has_tools) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"tools", ev.tools, 200);
				} else if (tools_col && *tools_col) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"tools", tools_col, 200);
				}
				if (decisions && *decisions) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"decisions", decisions, 200);
				}
				if (artifacts && *artifacts) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"artifacts", artifacts, 200);
				}
				if (ev.has_result) {
					MEMORY_PFX(++sub_idx == total_subs);
					memory_emit_wrapped(&buf,
						prefix_first, prefix_cont,
						"result", ev.result, 200);
				}

				#undef MEMORY_PFX
				idx++;
			}
			sqlite3_finalize(stmt);
		}
	}

	/* Recent changes */
	if (n_changes > 0) {
		const char *vbar = "    ";
		int idx = 0;
		memory_appendf(&buf, MEMORY_RENDER_CAP,
			       "└── Recent changes (%d)\n", n_changes);
		if (sqlite3_prepare_v2(db->handle,
			"SELECT old.key_name, old.value_text, "
			"cur.value_text, old.updated_at "
			"FROM memory_facts old "
			"LEFT JOIN memory_facts cur "
			"  ON cur.id = old.superseded_by "
			"WHERE old.session_id=? AND old.is_current=0 "
			"ORDER BY old.updated_at DESC",
			-1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(stmt, 1, session_id);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key_name = (const char *)
					sqlite3_column_text(stmt, 0);
				const char *old_value = (const char *)
					sqlite3_column_text(stmt, 1);
				const char *new_value = (const char *)
					sqlite3_column_text(stmt, 2);
				long long changed_at = (long long)
					sqlite3_column_int64(stmt, 3);
				const char *cb;
				if (!key_name || !old_value)
					continue;
				cb = (idx == n_changes - 1) ? "└──" : "├──";
				memory_appendf(&buf,
					       MEMORY_RENDER_CAP,
					       "%s%s %s: %s -> %s (t=%lld)\n",
					       vbar, cb,
					       key_name, old_value,
					       new_value ? new_value :
							   "(unset)",
					       changed_at);
				idx++;
			}
			sqlite3_finalize(stmt);
		}
	}

	return morph_buf_detach(&buf);
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

/* ---- LLM-driven structured extraction ---------------------------------
 *
 * On each consolidated turn we ask the LLM to produce a single JSON
 * envelope with three sections:
 *   { "facts":[{key,value,category,importance}],
 *     "rules":[{rule_text,trigger}],
 *     "episode":{key_decisions,artifacts,importance} }
 *
 * facts/rules are merged into memory_facts / memory_procedures using the
 * same dedup/upsert pipeline as the anchor path. The episode block
 * augments the structured fields written into memory_episodes.
 *
 * Failures (LLM unconfigured, network error, malformed JSON) degrade
 * silently — the anchor path still runs, so memory never gets worse than
 * before this hook existed.
 * --------------------------------------------------------------------- */

static const char MEMORY_LLM_SYSTEM[] =
	"You distill a single user/assistant turn into durable memory. "
	"Be conservative: only emit items that will still be true and useful "
	"in a future, unrelated turn. Drop pleasantries, error chatter, and "
	"anything bound to the current question. "
	"Output STRICT JSON matching this schema and nothing else:\n"
	"{\n"
	"  \"facts\":[{\"key\":string, \"value\":string,"
	" \"category\":string, \"importance\":number}],\n"
	"  \"rules\":[{\"rule_text\":string, \"trigger\":string}],\n"
	"  \"episode\":{\"entities\":string, \"key_decisions\":string,"
	" \"artifacts\":string, \"importance\":number}\n"
	"}\n"
	"Rules:\n"
	"- key uses snake_case; common keys: user_name, preferred_name, "
	"preferred_language, response_style, goal, location, project, "
	"tech_stack, constraint, deadline, contact.\n"
	"- category is one of: identity, preference, goal, context, project, "
	"constraint, relationship, general.\n"
	"- importance is between 0 and 1; >=0.8 only for stable identity / "
	"explicit standing instructions.\n"
	"- rule_text is an imperative directive the assistant should obey "
	"in future turns.\n"
	"- entities is a comma-separated list of salient nouns the turn "
	"discusses (people, products, files, places).\n"
	"- artifacts is a comma-separated list of file paths, URLs, or IDs "
	"produced this turn.\n"
	"- Use empty arrays / strings when there is nothing to record. "
	"Never invent facts not grounded in the turn.\n"
	"- Output JSON only — no markdown, no commentary.";

static char *memory_llm_extract_json(const char *user_input,
				     const char *assistant_output,
				     const char *tool_names,
				     int success);

static cJSON *memory_llm_parse_envelope(const char *raw)
{
	const char *p;
	const char *end;
	cJSON *root;

	if (!raw)
		return NULL;
	p = strchr(raw, '{');
	if (!p)
		return NULL;
	end = strrchr(p, '}');
	if (!end || end <= p)
		return NULL;
	{
		size_t len = (size_t)(end - p) + 1;
		char *trimmed = (char *)malloc(len + 1);
		if (!trimmed)
			return NULL;
		memcpy(trimmed, p, len);
		trimmed[len] = '\0';
		root = cJSON_Parse(trimmed);
		free(trimmed);
	}
	return root;
}

static const char *memory_json_string(const cJSON *obj, const char *field,
				      const char *fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
	if (cJSON_IsString(item) && item->valuestring && *item->valuestring)
		return item->valuestring;
	return fallback;
}

static double memory_json_number(const cJSON *obj, const char *field,
				 double fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
	if (cJSON_IsNumber(item))
		return item->valuedouble;
	return fallback;
}

static int memory_apply_llm_envelope(struct db *db, int64_t session_id,
				     const char *user_input,
				     cJSON *root,
				     char **out_entities,
				     char **out_decisions,
				     char **out_artifacts,
				     double *out_importance)
{
	cJSON *facts;
	cJSON *rules;
	cJSON *episode;
	cJSON *item;
	int dirty = 0;
	int worst = 0;
	int rc;

	if (!root)
		return -EINVAL;

	facts = cJSON_GetObjectItemCaseSensitive(root, "facts");
	if (cJSON_IsArray(facts)) {
		cJSON_ArrayForEach(item, facts) {
			const char *key = memory_json_string(item, "key", NULL);
			const char *value =
				memory_json_string(item, "value", NULL);
			const char *category =
				memory_json_string(item, "category",
						   "general");
			double importance =
				memory_json_number(item, "importance", 0.5);
			if (!key || !value)
				continue;
			if (!*key || !*value)
				continue;
			if (strlen(value) > MEMORY_VALUE_MAX_BYTES)
				continue;
			rc = memory_upsert_fact(db, session_id, key, value,
						user_input, category,
						importance);
			if (rc != 0 && worst == 0)
				worst = rc;
			else if (rc == 0)
				dirty = 1;
		}
	}

	rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
	if (cJSON_IsArray(rules)) {
		cJSON_ArrayForEach(item, rules) {
			const char *rule =
				memory_json_string(item, "rule_text", NULL);
			const char *trigger =
				memory_json_string(item, "trigger",
						   user_input);
			if (!rule || !*rule)
				continue;
			rc = memory_upsert_procedure(db, session_id, rule,
						     trigger);
			if (rc != 0 && worst == 0)
				worst = rc;
		}
	}

	episode = cJSON_GetObjectItemCaseSensitive(root, "episode");
	if (cJSON_IsObject(episode)) {
		const char *ent =
			memory_json_string(episode, "entities", "");
		const char *kd =
			memory_json_string(episode, "key_decisions", "");
		const char *ar =
			memory_json_string(episode, "artifacts", "");
		double imp = memory_json_number(episode, "importance", 0.5);
		if (out_entities)
			*out_entities = strdup(ent);
		if (out_decisions)
			*out_decisions = strdup(kd);
		if (out_artifacts)
			*out_artifacts = strdup(ar);
		if (out_importance)
			*out_importance = imp;
	}

	if (dirty) {
		rc = memory_refresh_profile(db, session_id);
		if (rc != 0 && worst == 0)
			worst = rc;
	}
	return worst;
}

static char *memory_llm_extract_json(const char *user_input,
				     const char *assistant_output,
				     const char *tool_names,
				     int success)
{
	struct model *llm = memory_get_llm();
	struct arena *arena;
	char *prompt;
	size_t prompt_cap;
	const char *messages[1];
	morph_buf_t buf;
	int rc;

	if (!llm || !llm->chat || !llm->api_key[0] || !user_input)
		return NULL;

	prompt_cap = strlen(user_input) +
		     (assistant_output ? strlen(assistant_output) : 0) +
		     (tool_names ? strlen(tool_names) : 0) + 1024;
	prompt = (char *)malloc(prompt_cap);
	if (!prompt)
		return NULL;
	snprintf(prompt, prompt_cap,
		 "Turn outcome: %s\n"
		 "Tools used: %s\n"
		 "User said:\n%s\n\n"
		 "Assistant said:\n%s\n",
		 success ? "completed" : "aborted",
		 (tool_names && *tool_names) ? tool_names : "(none)",
		 user_input,
		 (assistant_output && *assistant_output) ? assistant_output
							 : "(no answer)");

	arena = arena_create(64 * 1024);
	if (!arena) {
		free(prompt);
		return NULL;
	}

	rc = morph_buf_init(&buf, 4096);
	if (rc != 0) {
		arena_destroy(arena);
		free(prompt);
		return NULL;
	}

	messages[0] = prompt;
	rc = llm->chat(llm, arena, MEMORY_LLM_SYSTEM, messages, 1,
		       morph_buf_append_cb, &buf);
	arena_destroy(arena);
	free(prompt);

	if (rc != 0 || buf.len == 0) {
		log_dbg("memory: LLM extraction failed (rc=%d, len=%zu)",
			rc, buf.len);
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static int memory_capture_llm_path(struct db *db, int64_t session_id,
				   const char *user_input,
				   const char *assistant_output,
				   const struct react_step *steps,
				   int success,
				   char **out_entities,
				   char **out_decisions,
				   char **out_artifacts,
				   double *out_importance)
{
	char tools[256];
	char *raw;
	cJSON *root;
	int rc;

	memory_collect_tools(steps, tools, sizeof(tools));
	raw = memory_llm_extract_json(user_input, assistant_output, tools,
				      success);
	if (!raw)
		return -ENODATA;
	root = memory_llm_parse_envelope(raw);
	free(raw);
	if (!root)
		return -EINVAL;
	rc = memory_apply_llm_envelope(db, session_id, user_input, root,
				       out_entities, out_decisions,
				       out_artifacts, out_importance);
	cJSON_Delete(root);
	return rc;
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
	int llm_ran = 0;
	char *llm_entities = NULL;
	char *llm_decisions = NULL;
	char *llm_artifacts = NULL;
	double llm_importance = 0.5;

	if (!db || !db->handle || !opts || !opts->enabled || !user_input)
		return 0;

	/* LLM path runs first so its facts win recency tiebreaks against
	 * the heuristic anchors. The anchor path always runs as a safety
	 * net for offline / rate-limited scenarios. */
	if (opts->llm_extract_enabled) {
		rc = memory_capture_llm_path(db, session_id, user_input,
					     assistant_output, steps,
					     success, &llm_entities,
					     &llm_decisions,
					     &llm_artifacts, &llm_importance);
		if (rc == 0)
			llm_ran = 1;
		else if (rc != -ENODATA && worst == 0)
			worst = rc;
	}

	if (opts->hot_path_enabled) {
		rc = memory_capture_hot_path(db, session_id, user_input);
		if (rc != 0 && worst == 0)
			worst = rc;
	}

	if (opts->cold_path_enabled) {
		rc = memory_insert_episode(db, session_id, user_input,
					   assistant_output, steps, success,
					   llm_ran ? llm_entities : NULL,
					   llm_ran ? llm_decisions : NULL,
					   llm_ran ? llm_artifacts : NULL,
					   llm_ran ? llm_importance : 0.5);
		if (rc != 0 && worst == 0)
			worst = rc;
	}

	free(llm_entities);
	free(llm_decisions);
	free(llm_artifacts);
	return worst;
}

/* ----------------------------------------------------------------------
 * Async consolidate worker.
 *
 * Background:
 *   memory_capture_llm_path() makes a blocking HTTP SSE call that can
 *   take 1-3 seconds. When invoked from the CLI on the main thread it
 *   stalls the readline prompt right after the assistant finishes.
 *
 * Design:
 *   A single long-lived worker thread owns its own SQLite connection
 *   (opened against the same db->path) and drains a FIFO queue of jobs
 *   posted by memory_consolidate_turn_async(). The caller deep-copies
 *   user_input / assistant_output / react_step list into the job, so
 *   the foreground may free or reuse those buffers immediately.
 *
 *   We pin to one worker (not a pool) on purpose: SQLite + LLM API
 *   serialisation is fine, and one writer keeps the WAL contention low.
 *
 * Lifecycle:
 *   The worker is started lazily on the first async submission and is
 *   torn down by memory_async_shutdown() (CLI calls this just before
 *   db_close so all in-flight jobs flush first).
 * ---------------------------------------------------------------------- */

struct memory_job {
	char *db_path;
	int64_t session_id;
	char *user_input;
	char *assistant_output;
	struct react_step *steps;
	int success;
	struct memory_options opts;
	struct memory_job *next;
};

static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_async_cv = PTHREAD_COND_INITIALIZER;
static struct memory_job *g_async_head;
static struct memory_job *g_async_tail;
static pthread_t g_async_thread;
static int g_async_running;
static int g_async_stop;
static int g_async_active;

static void memory_steps_free(struct react_step *s)
{
	while (s) {
		struct react_step *n = s->next;
		free(s->content);
		free(s->tool_name);
		free(s->tool_args);
		free(s->tool_call_id);
		free(s);
		s = n;
	}
}

static struct react_step *memory_steps_dup(const struct react_step *src)
{
	struct react_step *head = NULL;
	struct react_step *tail = NULL;

	for (const struct react_step *cur = src; cur; cur = cur->next) {
		struct react_step *node = calloc(1, sizeof(*node));
		if (!node) {
			memory_steps_free(head);
			return NULL;
		}
		node->type = cur->type;
		if (cur->content) {
			node->content = strdup(cur->content);
			if (!node->content)
				goto oom;
		}
		if (cur->tool_name) {
			node->tool_name = strdup(cur->tool_name);
			if (!node->tool_name)
				goto oom;
		}
		if (cur->tool_args) {
			node->tool_args = strdup(cur->tool_args);
			if (!node->tool_args)
				goto oom;
		}
		if (cur->tool_call_id) {
			node->tool_call_id = strdup(cur->tool_call_id);
			if (!node->tool_call_id)
				goto oom;
		}
		if (!head)
			head = node;
		else
			tail->next = node;
		tail = node;
		continue;
oom:
		free(node->content);
		free(node->tool_name);
		free(node->tool_args);
		free(node->tool_call_id);
		free(node);
		memory_steps_free(head);
		return NULL;
	}
	return head;
}

static void memory_job_free(struct memory_job *j)
{
	if (!j)
		return;
	free(j->db_path);
	free(j->user_input);
	free(j->assistant_output);
	memory_steps_free(j->steps);
	free(j);
}

static void *memory_async_worker(void *arg)
{
	(void)arg;
	for (;;) {
		struct memory_job *job = NULL;

		pthread_mutex_lock(&g_async_lock);
		while (!g_async_stop && !g_async_head)
			pthread_cond_wait(&g_async_cv, &g_async_lock);
		if (g_async_stop && !g_async_head) {
			pthread_mutex_unlock(&g_async_lock);
			break;
		}
		job = g_async_head;
		g_async_head = job->next;
		if (!g_async_head)
			g_async_tail = NULL;
		g_async_active = 1;
		pthread_mutex_unlock(&g_async_lock);

		if (!job)
			continue;

		/* Worker owns its own SQLite handle so it never collides
		 * with the foreground db on the main thread. WAL keeps
		 * concurrent reads/writes consistent. */
		struct db worker_db = {0};
		int rc = db_open(&worker_db, job->db_path);
		if (rc == 0) {
			memory_consolidate_turn(&worker_db, job->session_id,
						job->user_input,
						job->assistant_output,
						job->steps,
						job->success,
						&job->opts);
			db_close(&worker_db);
		} else {
			log_dbg("memory: async worker failed to open db: %d",
				rc);
		}
		memory_job_free(job);

		pthread_mutex_lock(&g_async_lock);
		g_async_active = 0;
		pthread_mutex_unlock(&g_async_lock);
	}
	return NULL;
}

static int memory_async_ensure_worker(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_async_lock);
	if (!g_async_running) {
		g_async_stop = 0;
		if (pthread_create(&g_async_thread, NULL,
				   memory_async_worker, NULL) != 0)
			rc = -errno;
		else
			g_async_running = 1;
	}
	pthread_mutex_unlock(&g_async_lock);
	return rc;
}

int memory_consolidate_turn_async(struct db *db, int64_t session_id,
				  const char *user_input,
				  const char *assistant_output,
				  const struct react_step *steps,
				  int success,
				  const struct memory_options *opts)
{
	struct memory_job *job;

	if (!db || !db->path[0] || !opts || !opts->enabled || !user_input)
		return 0;

	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->db_path = strdup(db->path);
	job->user_input = strdup(user_input);
	if (assistant_output)
		job->assistant_output = strdup(assistant_output);
	job->steps = memory_steps_dup(steps);
	job->session_id = session_id;
	job->success = success;
	job->opts = *opts;
	if (!job->db_path || !job->user_input ||
	    (assistant_output && !job->assistant_output) ||
	    (steps && !job->steps)) {
		memory_job_free(job);
		return -ENOMEM;
	}

	if (memory_async_ensure_worker() != 0) {
		memory_job_free(job);
		return -EAGAIN;
	}

	pthread_mutex_lock(&g_async_lock);
	if (g_async_tail)
		g_async_tail->next = job;
	else
		g_async_head = job;
	g_async_tail = job;
	pthread_cond_signal(&g_async_cv);
	pthread_mutex_unlock(&g_async_lock);
	return 0;
}

void memory_async_shutdown(void)
{
	pthread_t th;
	int running;

	pthread_mutex_lock(&g_async_lock);
	running = g_async_running;
	if (running) {
		g_async_stop = 1;
		th = g_async_thread;
		pthread_cond_broadcast(&g_async_cv);
	}
	pthread_mutex_unlock(&g_async_lock);

	if (running)
		pthread_join(th, NULL);

	pthread_mutex_lock(&g_async_lock);
	g_async_running = 0;
	g_async_active = 0;
	pthread_mutex_unlock(&g_async_lock);
}

int memory_async_pending(void)
{
	int pending;

	pthread_mutex_lock(&g_async_lock);
	pending = g_async_running && (g_async_active || g_async_head != NULL);
	pthread_mutex_unlock(&g_async_lock);
	return pending;
}
