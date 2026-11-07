#include "agent/memory.h"
#include "agent/memory_internal.h"
#include "persistence/memory_repository.h"
#include "persistence/memory_snapshot.h"
#include "models/llm.h"
#include "session.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include "util/log.h"
#include "util/error.h"
#include "util/utf8.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEMORY_APPEND_INIT_CAP 1024
#define MEMORY_RENDER_CAP (1024 * 1024)
#define MEMORY_VALUE_MIN_BYTES 2
#define MEMORY_VALUE_MAX_BYTES 200

static int memory_stop_len(const char *p)
{
	unsigned cp;
	size_t len;

	if (!p || !*p)
		return 0;
	if (!utf8_decode_codepoint(p, strlen(p), &cp, &len))
		return 0;
	if (utf8_is_cjk_sentence_punct(cp))
		return (int)len;
	return 0;
}

static int memory_is_ascii_stop(char c)
{
	return c == '\n' || c == '\r' || c == '.' || c == '!' || c == '?' || c == ',' || c == ';';
}

/* Reject extracted values that are obviously not a fact value:
 *   - empty / too short / too long
 *   - whitespace-only
 *   - lead with a Chinese auxiliary or particle (e.g. "了一辆出租车")
 * This is the last line of defense after the substring anchors fire. */
static int memory_value_is_plausible(const char *value)
{
	static const char *const bad_prefixes[] = {
		"\xe4\xba\x86",		    /* 了 */
		"\xe7\x9a\x84",		    /* 的 */
		"\xe5\xbe\x97",		    /* 得 */
		"\xe7\x9d\x80",		    /* 着 */
		"\xe8\xbf\x87",		    /* 过 */
		"\xe6\x83\xb3",		    /* 想 */
		"\xe7\x9f\xa5\xe9\x81\x93", /* 知道 */
		"\xe4\xb8\x8d",		    /* 不 */
		"\xe6\xb2\xa1",		    /* 没 */
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
	safe = utf8_clamp_bytes(text, max_len);
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

int memory_contains_ci(const char *haystack, const char *needle)
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
		if (memory_stop_len(start + len) > 0)
			break;
		len++;
	}
	return len;
}

static int memory_extract_after_ci(const char *text, const char *needle, char *out, size_t out_sz)
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

static int memory_extract_after_raw(const char *text, const char *needle, char *out, size_t out_sz)
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

int memory_appendf(morph_buf_t *b, size_t max_len, const char *fmt, ...)
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

static int memory_refresh_profile(struct db *db, int64_t session_id)
{
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	struct memory_snapshot_options options = {
		.facts_limit = -1,
		.facts_by_recency = 1,
	};
	struct memory_fact_record *fact;
	morph_buf_t profile MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = memory_snapshot_read(db, session_id, &options, &snapshot);

	if (rc != 0)
		return rc;
	rc = morph_buf_init(&profile, MEMORY_APPEND_INIT_CAP);
	if (rc != 0) {
		memory_snapshot_cleanup(&snapshot);
		return rc;
	}
	morph_array_foreach (fact, &snapshot.facts, struct memory_fact_record) {
		const char *label = fact->key;

		if (!fact->key || !fact->value)
			continue;
		if (strcmp(fact->key, "preferred_name") == 0)
			label = "Preferred name";
		else if (strcmp(fact->key, "user_name") == 0)
			label = "User name";
		else if (strcmp(fact->key, "preferred_language") == 0)
			label = "Preferred language";
		else if (strcmp(fact->key, "response_style") == 0)
			label = "Response style";
		else if (strcmp(fact->key, "goal") == 0)
			label = "Current goal";
		else if (strcmp(fact->key, "location") == 0)
			label = "Location";
		rc = memory_appendf(&profile, 16384, "- %s: %s\n", label, fact->value);
		if (rc != 0)
			break;
	}
	if (rc == 0)
		rc = memory_store_save_profile(db, session_id, morph_buf_cstr_borrow(&profile));
	morph_buf_cleanup(&profile);
	memory_snapshot_cleanup(&snapshot);
	return rc;
}

/* Try a list of anchor phrases; succeed only when the captured value passes
 * the plausibility filter. Stricter anchors come first to win over the
 * looser ones (e.g. "i live in" before any future "i live ..." anchor). */
static int memory_try_anchors(const char *user_input, char *value, size_t value_sz,
			      const char *const *ci_needles, const char *const *raw_needles)
{
	if (ci_needles) {
		for (int i = 0; ci_needles[i]; i++) {
			if (memory_extract_after_ci(user_input, ci_needles[i], value, value_sz) &&
			    memory_value_is_plausible(value))
				return 1;
		}
	}
	if (raw_needles) {
		for (int i = 0; raw_needles[i]; i++) {
			if (memory_extract_after_raw(user_input, raw_needles[i], value, value_sz) &&
			    memory_value_is_plausible(value))
				return 1;
		}
	}
	value[0] = '\0';
	return 0;
}

/* Language mentions are not preferences. Only capture a positive directive;
 * questions, negations, quotations and translation tasks must not change it. */
static const char *memory_language_directive(const char *clause)
{
	static const char *const reject[] = {
		"why",	  "how",  "don't", "do not", "not ",   "never ", "translate",
		"为什么", "为啥", "怎么",  "不是",   "不要",   "别",	 "不用",
		"无需",	  "翻译", "是否",  "吗",     "英文版", "中文版", "\"",
		"“",	  "”",	  "`",	   NULL,
	};
	static const char *const prefixes[] = {
		"please ",
		"always ",
		"reply ",
		"respond ",
		"answer ",
		"use ",
		"from now on ",
		"i prefer ",
		"my preferred language ",
		"本会话",
		"这个会话",
		"本项目",
		"这个项目",
		"for this session",
		"for this project",
		"请",
		"以后",
		"后续",
		"之后",
		"从现在起",
		"默认",
		"用",
		"使用",
		"改成",
		"切换到",
		"回答",
		"回复",
		"我的",
		"我希望",
		"语言",
		NULL,
	};
	static const char *const chinese[] = {
		"reply in chinese", "respond in chinese", "answer in chinese",
		"use chinese",	    "prefer chinese",	  "preferred language is chinese",
		"用中文",	    "中文回答",		  "中文回复",
		"改成中文",	    "切换到中文",	  NULL,
	};
	static const char *const english[] = {
		"reply in english", "respond in english", "answer in english",
		"use english",	    "prefer english",	  "preferred language is english",
		"用英文",	    "英文回答",		  "英文回复",
		"改成英文",	    "切换到英文",	  NULL,
	};
	int directive = 0;
	int wants_chinese = 0;
	int wants_english = 0;

	while (isspace((unsigned char)*clause))
		clause++;
	for (int i = 0; reject[i]; i++) {
		if (memory_contains_ci(clause, reject[i]))
			return NULL;
	}
	for (int i = 0; prefixes[i]; i++) {
		if (strncasecmp(clause, prefixes[i], strlen(prefixes[i])) == 0)
			directive = 1;
	}
	if (!directive)
		return NULL;
	for (int i = 0; chinese[i]; i++)
		wants_chinese |= memory_contains_ci(clause, chinese[i]);
	for (int i = 0; english[i]; i++)
		wants_english |= memory_contains_ci(clause, english[i]);
	if (wants_chinese == wants_english)
		return NULL;
	return wants_chinese ? "Chinese" : "English";
}

static const char *memory_explicit_language(const char *input)
{
	const char *start = input;
	const char *cursor = input;
	const char *language = NULL;

	if (strchr(input, '?') || strstr(input, "？") || strchr(input, '"') || strstr(input, "“") ||
	    strchr(input, '`'))
		return NULL;
	while (*cursor) {
		utf8_int32_t cp;
		const char *next = utf8codepoint(cursor, &cp);

		if ((cp < 128 && memory_is_ascii_stop((char)cp)) ||
		    utf8_is_cjk_sentence_punct((unsigned)cp)) {
			char *clause = strndup(start, (size_t)(cursor - start));
			const char *found = clause ? memory_language_directive(clause) : NULL;

			if (found)
				language = found;
			free(clause);
			start = next;
		}
		cursor = next;
	}
	{
		const char *found = memory_language_directive(start);

		if (found)
			language = found;
	}
	return language;
}

const char *memory_preference_request_scope(const char *input)
{
	static const char *const temporary[] = {
		"这次",	  "本次",      "这封",	     "这篇",	   "这段",	  "这个回答",
		"这一轮", "this time", "this email", "this reply", "this answer", NULL,
	};
	static const char *const standing[] = {
		"以后",	    "后续",	   "之后",	 "从现在起", "默认",   "一直",
		"我的偏好", "我希望",	   "记住",	 "偏好改",   "偏好设", "remember",
		"always",   "from now on", "by default", "i prefer", NULL,
	};

	if (!input || strchr(input, '?') || strstr(input, "？") || strstr(input, "为什么") ||
	    strstr(input, "为啥") || memory_contains_ci(input, "why ") ||
	    memory_contains_ci(input, "translate") || strstr(input, "翻译") || strchr(input, '"') ||
	    strchr(input, '`') || strstr(input, "“") || strstr(input, "‘"))
		return NULL;
	for (int i = 0; temporary[i]; i++) {
		if (memory_contains_ci(input, temporary[i]))
			return NULL;
	}
	if (strstr(input, "本会话") || strstr(input, "这个会话") ||
	    memory_contains_ci(input, "this session"))
		return "session";
	if (strstr(input, "本项目") || strstr(input, "这个项目") ||
	    memory_contains_ci(input, "this project"))
		return "project";
	for (int i = 0; standing[i]; i++) {
		if (memory_contains_ci(input, standing[i]))
			return "personal";
	}
	return NULL;
}

static int memory_verify_legacy_preference(const char *key, const char *value, const char *source)
{
	const char *language = memory_explicit_language(source);

	return !strcmp(preference_key_borrow(key), "response.language") && language &&
	       !strcmp(language, value) && memory_preference_request_scope(source);
}

static int memory_capture_hot_path(struct db *db, int64_t session_id, const char *user_input);

static int memory_accept_preferences(struct db *db, int64_t session_id, const char *input,
				     const char *event_token, const struct memory_options *opts)
{
	const char *scope;
	const char *language;
	int rc;

	if (!opts || !opts->enabled || !opts->hot_path_enabled || !input)
		return 0;
	rc = memory_prepare_session(db, session_id);
	if (rc != 0)
		return rc;
	scope = memory_preference_request_scope(input);
	if (!strncasecmp(input, "call me ", 8) || !strncasecmp(input, "please call me ", 15) ||
	    !strncmp(input, "叫我", strlen("叫我"))) {
		static const char *const ci[] = {"please call me ", "call me ", NULL};
		static const char *const raw[] = {"叫我", NULL};
		char value[256];

		if (memory_try_anchors(input, value, sizeof(value), ci, raw)) {
			rc = preference_set(db, session_id, scope ? scope : "personal",
					    "user.preferred_name", value, input, event_token);
			if (rc != 0)
				return rc;
		}
	}
	if (!scope)
		return 0;
	language = memory_explicit_language(input);
	if (language) {
		rc = preference_set(db, session_id, scope, "response.language", language, input,
				    event_token);
		if (rc != 0)
			return rc;
	}
	/* A complaint or a quoted example cannot change response detail. */
	if (!strchr(input, '?') && !strstr(input, "？") && !strstr(input, "为什么") &&
	    !strstr(input, "为啥") && !strstr(input, "不要") && !strstr(input, "别") &&
	    !strchr(input, '"') && !strstr(input, "“") && !strchr(input, '`') &&
	    !memory_contains_ci(input, "why") && !memory_contains_ci(input, "not ")) {
		int concise = memory_contains_ci(input, "be concise") ||
			      memory_contains_ci(input, "be brief") || strstr(input, "简洁") ||
			      strstr(input, "简短");
		int detailed = memory_contains_ci(input, "be detailed") || strstr(input, "详细");

		if (concise != detailed)
			return preference_set(db, session_id, scope, "response.detail",
					      concise ? "concise" : "detailed", input, event_token);
	}
	return 0;
}

int memory_accept_input(struct db *db, int64_t session_id, const char *input,
			const char *event_token, const struct memory_options *opts)
{
	int rc;
	char canonical_token[64];

	if (!opts || !opts->enabled || !input)
		return 0;
	rc = memory_prepare_session(db, session_id);
	if (rc != 0)
		return rc;
	rc = memory_store_begin_write(db);
	if (rc != 0)
		return rc;
	/* Use the persisted user-event identity for both deterministic ingestion
	 * and the model's explicit preference tool. A second interpretation of
	 * the same event cannot silently replace its already committed value. */
	if (event_token) {
		rc = memory_store_event_token(db, session_id, event_token, canonical_token,
					      sizeof(canonical_token));
		if (rc < 0)
			return memory_store_finish_write(db, rc);
		if (rc > 0)
			event_token = canonical_token;
	}
	rc = memory_store_migrate_preferences(db, session_id, memory_verify_legacy_preference);
	if (rc == 0)
		rc = memory_accept_preferences(db, session_id, input, event_token, opts);
	rc = memory_store_finish_write(db, rc);
	if (rc != 0)
		return rc;
	rc = memory_async_resume(db, opts);
	if (rc != 0)
		log_err(db ? db->logger : NULL, "memory: recovering background jobs failed: %s",
			morph_strerror(rc));
	if (opts->hot_path_enabled)
		return memory_capture_hot_path(db, session_id, input);
	return 0;
}

static int memory_capture_hot_path(struct db *db, int64_t session_id, const char *user_input)
{
	/* Anchors are deliberately conservative: we'd rather miss a fact than
	 * record a wrong one. Phrases like "i am " or "我想" were dropped
	 * because they fire on neutral statements ("I am tired", "我想知道
	 * 天气"). */
	static const char *const user_name_ci[] = {
		"my name is ",
		"i am called ",
		"i'm called ",
		NULL,
	};
	static const char *const user_name_raw[] = {
		"\xe6\x88\x91\xe5\x8f\xab",					/* 我叫 */
		"\xe6\x88\x91\xe7\x9a\x84\xe5\x90\x8d\xe5\xad\x97\xe6\x98\xaf", /* 我的名字是 */
		NULL,
	};
	static const char *const goal_ci[] = {
		"my goal is ",
		"i want to ",
		"i'd like to ",
		NULL,
	};
	static const char *const goal_raw[] = {
		"\xe6\x88\x91\xe7\x9a\x84\xe7\x9b\xae\xe6\xa0\x87\xe6\x98\xaf", /* 我的目标是 */
		NULL,
	};
	static const char *const location_ci[] = {
		"i live in ",
		"i am in ",
		"i'm in ",
		NULL,
	};
	static const char *const location_raw[] = {
		"\xe6\x88\x91\xe4\xbd\x8f\xe5\x9c\xa8", /* 我住在 */
		NULL,
	};

	char value[256];
	int dirty = 0;
	int worst = 0;
	int rc = 0;

	if (!db || !db->handle || !user_input)
		return -EINVAL;

#define TRY_FACT(key, ci, raw, cat, importance)                                                    \
	do {                                                                                       \
		if (memory_try_anchors(user_input, value, sizeof(value), ci, raw)) {               \
			rc = memory_store_upsert_fact(db, session_id, key, value, user_input, cat, \
						      importance);                                 \
			if (rc != 0 && worst == 0)                                                 \
				worst = rc;                                                        \
			else if (rc == 0)                                                          \
				dirty = 1;                                                         \
		}                                                                                  \
	} while (0)

	/* user_name comes after location-style anchors so "I am in Tokyo"
	 * never lands here; user_name_ci itself uses strict anchors. */
	TRY_FACT("user_name", user_name_ci, user_name_raw, "identity", 0.85);
	TRY_FACT("goal", goal_ci, goal_raw, "goal", 0.7);
	TRY_FACT("location", location_ci, location_raw, "context", 0.6);
#undef TRY_FACT

	if (dirty) {
		rc = memory_refresh_profile(db, session_id);
		if (rc != 0 && worst == 0)
			worst = rc;
	}
	return worst;
}

static void memory_collect_tools(const struct react_step *steps, char *buf, size_t buf_sz)
{
	int first = 1;

	if (!buf || buf_sz == 0)
		return;
	buf[0] = '\0';
	for (const struct react_step *cur = steps; cur; cur = cur->next) {
		size_t used;
		size_t left;
		int exists = 0;
		if (cur->type != REACT_STEP_ACTION || !cur->tool_name || !cur->tool_name[0])
			continue;
		if (strstr(buf, cur->tool_name) != NULL)
			exists = 1;
		if (exists)
			continue;
		used = strlen(buf);
		left = buf_sz - used;
		if (left <= 1)
			break;
		snprintf(buf + used, left, "%s%s", first ? "" : ", ", cur->tool_name);
		first = 0;
	}
}

static const char *memory_infer_task_type(const char *user_input, const char *tool_names)
{
	if ((tool_names && strstr(tool_names, "img_")) ||
	    (user_input &&
	     (memory_contains_ci(user_input, "image") || memory_contains_ci(user_input, "photo") ||
	      strstr(user_input, "图片") != NULL || strstr(user_input, "图像") != NULL)))
		return "image";
	if ((tool_names && strstr(tool_names, "vid_")) ||
	    (user_input &&
	     (memory_contains_ci(user_input, "video") || strstr(user_input, "视频") != NULL)))
		return "video";
	if ((tool_names && strstr(tool_names, "file_")) ||
	    (tool_names && strstr(tool_names, "exec")) ||
	    (user_input &&
	     (memory_contains_ci(user_input, "code") || memory_contains_ci(user_input, "repo") ||
	      strstr(user_input, "代码") != NULL || strstr(user_input, "仓库") != NULL)))
		return "coding";
	if (tool_names && *tool_names)
		return "tooling";
	return "conversation";
}

static int memory_insert_episode(struct db *db, int64_t session_id, const char *user_input,
				 const char *assistant_output, const struct react_step *steps,
				 int success, const char *entities, const char *key_decisions,
				 const char *artifacts, double importance, int64_t job_id)
{
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
	assistant_snip = memory_snippet(assistant_output ? assistant_output : "", 220);
	if (!user_snip || !assistant_snip) {
		free(user_snip);
		free(assistant_snip);
		return -ENOMEM;
	}
	task_type = memory_infer_task_type(user_input, tools);
	outcome = success ? "completed" : "aborted";
	if (tools[0]) {
		snprintf(summary, sizeof(summary),
			 "Task: %s | Outcome: %s | Tools: %s | Result: %s", user_snip, outcome,
			 tools, assistant_snip[0] ? assistant_snip : "(no final answer)");
	} else {
		snprintf(summary, sizeof(summary), "Task: %s | Outcome: %s | Result: %s", user_snip,
			 outcome, assistant_snip[0] ? assistant_snip : "(no final answer)");
	}

	struct memory_episode_input input = {
		.task_type = task_type,
		.summary = summary,
		.outcome = assistant_snip,
		.success = success,
		.entities = entities,
		.decisions = key_decisions,
		.artifacts = artifacts,
		.tools = tools,
		.importance = importance,
		.job_id = job_id,
	};

	rc = memory_store_insert_episode(db, session_id, &input);

	free(user_snip);
	free(assistant_snip);
	return rc;
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
	"- A language mention, complaint (for example, 'why are you answering in "
	"English?'), translation request, or negated instruction is not a preferred "
	"language. Record preferred_language or a language rule only for an explicit "
	"positive instruction about how the assistant should communicate.\n"
	"- rule_text is an imperative directive the assistant should obey "
	"in future turns.\n"
	"- entities is a comma-separated list of salient nouns the turn "
	"discusses (people, products, files, places).\n"
	"- artifacts is a comma-separated list of file paths, URLs, or IDs "
	"produced this turn.\n"
	"- Use empty arrays / strings when there is nothing to record. "
	"Never invent facts not grounded in the turn.\n"
	"- Output JSON only — no markdown, no commentary.";

static char *memory_llm_extract_json(struct model *llm, const char *user_input,
				     const char *assistant_output, const char *tool_names,
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

static const char *memory_json_string(const cJSON *obj, const char *field, const char *fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
	if (cJSON_IsString(item) && item->valuestring && *item->valuestring)
		return item->valuestring;
	return fallback;
}

static double memory_json_number(const cJSON *obj, const char *field, double fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
	if (cJSON_IsNumber(item))
		return item->valuedouble;
	return fallback;
}

static int memory_apply_llm_envelope(struct db *db, int64_t session_id, const char *user_input,
				     cJSON *root, char **out_entities, char **out_decisions,
				     char **out_artifacts, double *out_importance)
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
		cJSON_ArrayForEach (item, facts) {
			const char *key = memory_json_string(item, "key", NULL);
			const char *value = memory_json_string(item, "value", NULL);
			const char *category = memory_json_string(item, "category", "general");
			double importance = memory_json_number(item, "importance", 0.5);
			if (!key || !value)
				continue;
			if (!*key || !*value)
				continue;
			if (strlen(value) > MEMORY_VALUE_MAX_BYTES)
				continue;
			rc = preference_candidate(db, session_id, category, key, value, user_input);
			(void)importance;
			if (rc != 0 && worst == 0)
				worst = rc;
			else if (rc == 0)
				dirty = 1;
		}
	}

	rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
	if (cJSON_IsArray(rules)) {
		cJSON_ArrayForEach (item, rules) {
			const char *rule = memory_json_string(item, "rule_text", NULL);
			const char *trigger = memory_json_string(item, "trigger", user_input);
			if (!rule || !*rule)
				continue;
			rc = preference_candidate(db, session_id, "rule", "procedure", rule,
						  trigger);
			if (rc != 0 && worst == 0)
				worst = rc;
		}
	}

	episode = cJSON_GetObjectItemCaseSensitive(root, "episode");
	if (cJSON_IsObject(episode)) {
		const char *ent = memory_json_string(episode, "entities", "");
		const char *kd = memory_json_string(episode, "key_decisions", "");
		const char *ar = memory_json_string(episode, "artifacts", "");
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

static char *memory_llm_extract_json(struct model *llm, const char *user_input,
				     const char *assistant_output, const char *tool_names,
				     int success)
{
	char *prompt;
	size_t prompt_cap;
	const char *messages[1];
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!llm || !llm->chat || !llm->api_key[0] || !user_input)
		return NULL;

	prompt_cap = strlen(user_input) + (assistant_output ? strlen(assistant_output) : 0) +
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
		 (tool_names && *tool_names) ? tool_names : "(none)", user_input,
		 (assistant_output && *assistant_output) ? assistant_output : "(no answer)");

	rc = morph_buf_init(&buf, 4096);
	if (rc != 0) {
		free(prompt);
		return NULL;
	}

	messages[0] = prompt;
	rc = llm->chat(llm, MEMORY_LLM_SYSTEM, messages, 1, NULL, morph_buf_append_cb, &buf);
	free(prompt);

	if (rc != 0 || buf.len == 0) {
		log_dbg(llm ? llm->http.logger : NULL,
			"memory: LLM extraction failed (rc=%d, len=%zu)", rc, buf.len);
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static int memory_capture_llm_path(struct db *db, int64_t session_id, const char *user_input,
				   const char *assistant_output, const struct react_step *steps,
				   int success, char **out_entities, char **out_decisions,
				   char **out_artifacts, double *out_importance, int64_t generation,
				   struct model *llm)
{
	char tools[256];
	char *raw;
	cJSON *root;
	int rc;

	memory_collect_tools(steps, tools, sizeof(tools));
	raw = memory_llm_extract_json(llm, user_input, assistant_output, tools, success);
	if (!raw)
		return -ENODATA;
	root = memory_llm_parse_envelope(raw);
	free(raw);
	if (!root)
		return -EINVAL;
	rc = memory_store_begin_generation(db, session_id, generation);
	if (rc != 0) {
		cJSON_Delete(root);
		return rc;
	}
	rc = memory_apply_llm_envelope(db, session_id, user_input, root, out_entities,
				       out_decisions, out_artifacts, out_importance);
	cJSON_Delete(root);
	return memory_store_finish_write(db, rc);
}

int memory_consolidate_turn(struct db *db, int64_t session_id, const char *user_input,
			    const char *assistant_output, const struct react_step *steps,
			    int success, const struct memory_options *opts)
{
	int worst = 0;
	int rc;
	int llm_ran = 0;
	int64_t generation = 0;
	char *llm_entities = NULL;
	char *llm_decisions = NULL;
	char *llm_artifacts = NULL;
	double llm_importance = 0.5;

	if (!db || !db->handle || !opts || !opts->enabled || !user_input)
		return 0;

	rc = memory_prepare_session(db, session_id);
	if (rc != 0)
		return rc;
	if (opts->generation_bound)
		generation = opts->generation;
	else {
		rc = memory_generation(db, session_id, &generation);
		if (rc != 0)
			return rc;
	}

	/* Extraction is advisory. Only memory_accept_input or explicit
	 * preference commands may write authoritative values. */
	if (opts->llm_extract_enabled) {
		rc = memory_capture_llm_path(db, session_id, user_input, assistant_output, steps,
					     success, &llm_entities, &llm_decisions, &llm_artifacts,
					     &llm_importance, generation, opts->llm);
		if (rc == 0)
			llm_ran = 1;
		else if (rc != -ENODATA && worst == 0)
			worst = rc;
	}

	if (opts->cold_path_enabled) {
		rc = memory_store_begin_generation(db, session_id, generation);
		if (rc == 0) {
			rc = memory_insert_episode(
				db, session_id, user_input, assistant_output, steps, success,
				llm_ran ? llm_entities : NULL, llm_ran ? llm_decisions : NULL,
				llm_ran ? llm_artifacts : NULL, llm_ran ? llm_importance : 0.5,
				opts->background_job_id);
			rc = memory_store_finish_write(db, rc);
		}
		if (rc != 0 && worst == 0)
			worst = rc;
	}

	free(llm_entities);
	free(llm_decisions);
	free(llm_artifacts);
	return worst;
}

int memory_prepare_session(struct db *db, int64_t session_id)
{
	return memory_store_prepare_session(db, session_id);
}

int memory_generation(struct db *db, int64_t session_id, int64_t *generation)
{
	return memory_store_generation(db, session_id, generation);
}

int memory_clear(struct db *db, int64_t session_id, enum memory_clear_scope scope)
{
	return memory_store_clear(db, session_id, scope);
}
