#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include "util/utf8.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http/client.h"

#define HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS 8000

static int history_secret_prefix(const char *start, const char *text, size_t *prefix_len,
				 int *preserve_prefix)
{
	static const char *const prefixes[] = {
		"Bearer ",	  "sk-",	   "sk_live_",
		"sk_test_",	  "api_key=",	   "api_key:",
		"\"api_key\":\"", "\"apiKey\":\"", "OPENAI_API_KEY="};

	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t len = strlen(prefixes[i]);

		/* A key-like prefix inside a package, identifier or path is not a key. */
		if (i >= 1 && i <= 3 && text > start &&
		    (isalnum((unsigned char)text[-1]) || strchr("_-/.", text[-1])))
			continue;
		if (strncmp(text, prefixes[i], len) == 0) {
			*prefix_len = len;
			*preserve_prefix = i == 0 || i >= 4;
			return 1;
		}
	}
	return 0;
}

static char *history_redact(const char *text)
{
	morph_buf_t buf;
	const char *cur;
	int rc;

	if (!text)
		return NULL;
	rc = morph_buf_init(&buf, strlen(text) + 1);
	if (rc != 0)
		return NULL;
	cur = text;
	while (*cur) {
		size_t prefix_len = 0;
		int preserve_prefix = 0;
		if (strncmp(cur, "data:", 5) == 0) {
			const char *base64 = strstr(cur, ";base64,");
			const char *header = cur + 5;

			while (*header &&
			       (isalnum((unsigned char)*header) || strchr("/;=.+-", *header)))
				header++;
			if (base64 && (size_t)(base64 - cur) < 128 &&
			    header == base64 + strlen(";base64")) {
				rc = morph_buf_puts(&buf, "[binary data omitted]");
				cur = base64 + strlen(";base64,");
				while (*cur &&
				       (isalnum((unsigned char)*cur) || strchr("+/=", *cur)))
					cur++;
				if (rc != 0)
					break;
				continue;
			}
		}

		if (!history_secret_prefix(text, cur, &prefix_len, &preserve_prefix)) {
			rc = morph_buf_putc(&buf, *cur++);
			if (rc != 0)
				break;
			continue;
		}
		if (preserve_prefix)
			rc = morph_buf_append(&buf, cur, prefix_len);
		if (rc == 0)
			rc = morph_buf_puts(&buf, "[REDACTED]");
		cur += prefix_len;
		while (*cur && isspace((unsigned char)*cur))
			cur++;
		while (*cur && !isspace((unsigned char)*cur) && *cur != '"' && *cur != '\'' &&
		       *cur != ',' && *cur != '}')
			cur++;
		if (rc != 0)
			break;
	}
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static char *history_redact_exact(const char *text, const char *secret)
{
	morph_buf_t buf;
	const char *cur;
	const char *match;
	int rc;

	if (!text)
		return NULL;
	if (!secret || strlen(secret) < 4)
		return strdup(text);
	rc = morph_buf_init(&buf, strlen(text) + 1);
	if (rc != 0)
		return NULL;
	cur = text;
	while ((match = strstr(cur, secret)) != NULL) {
		rc = morph_buf_append(&buf, cur, (size_t)(match - cur));
		if (rc == 0)
			rc = morph_buf_puts(&buf, "[REDACTED]");
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
		cur = match + strlen(secret);
	}
	if (morph_buf_puts(&buf, cur) != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

char *history_redact_all(struct react_context *ctx, const char *text)
{
	char *safe = history_redact(text);

	if (!safe)
		return NULL;
	for (int i = 0; ctx && i < ctx->history_secret_count; i++) {
		char *next = history_redact_exact(safe, ctx->history_secrets[i]);

		free(safe);
		safe = next;
		if (!safe)
			return NULL;
	}
	return safe;
}

/* Redact decoded values so replacements cannot corrupt JSON escaping.
 * Tool arguments may themselves contain a serialized JSON object. */

static int history_redact_json_values(struct react_context *ctx, struct json *value, int *changed)
{
	if (json_is_string_value(value)) {
		const char *key = json_key(value);
		size_t key_len = key ? strlen(key) : 0;
		int credential = key && (!strcmp(key, "api_key") || !strcmp(key, "apiKey") ||
					 (key_len >= 8 && !strcmp(key + key_len - 8, "_API_KEY")));
		char *safe = credential && json_string_value(value)[0]
				     ? strdup("[REDACTED]")
				     : history_redact_json(ctx, json_string_value(value));
		int rc;

		if (!safe)
			return -ENOMEM;
		if (strcmp(json_string_value(value), safe) != 0)
			*changed = 1;
		rc = json_string_update(value, safe) ? 0 : -ENOMEM;

		free(safe);
		return rc;
	}
	for (struct json *child = json_first(value); child; child = json_next(child)) {
		int rc = history_redact_json_values(ctx, child, changed);

		if (rc != 0)
			return rc;
	}
	return 0;
}

char *history_redact_json(struct react_context *ctx, const char *text)
{
	struct json *root;
	char *safe = NULL;
	int changed = 0;

	if (!text)
		return NULL;
	root = json_parse_ex(text, NULL, 1, NULL);
	if (!json_is_object_value(root) && !json_is_array_value(root)) {
		json_free(root);
		return history_redact_all(ctx, text);
	}
	if (history_redact_json_values(ctx, root, &changed) == 0)
		safe = changed ? json_stringify(root) : strdup(text);
	json_free(root);
	return safe;
}

static char *history_truncate_result(struct tokenizer *tokenizer, const char *content,
				     int max_tokens, int *truncated)
{
	const char *marker = "\n…(tool output truncated)…\n";
	morph_buf_t buf;
	size_t len = strlen(content);
	size_t budget;
	int tokens;
	int rc;

	if (max_tokens <= 0)
		max_tokens = HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;
	tokens = tokenizer_count(tokenizer, content);
	if (tokens <= max_tokens) {
		*truncated = 0;
		return strdup(content);
	}
	/* Very small budgets still have a bounded, recognizable omission marker. */
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = "...";
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = ".";
	if (tokenizer_count(tokenizer, marker) > max_tokens)
		marker = "";
	budget = (size_t)((double)len * (double)max_tokens / (double)tokens);
	if (budget >= len)
		budget = len - 1;
	rc = morph_buf_init(&buf, 128);
	if (rc != 0)
		return NULL;
	for (;;) {
		size_t head = utf8_clamp_bytes(content, budget * 3 / 5);
		size_t tail = utf8_clamp_bytes(content, len - (budget - budget * 3 / 5));
		size_t next;

		morph_buf_reset(&buf);
		rc = morph_buf_append(&buf, content, head);
		if (rc == 0)
			rc = morph_buf_puts(&buf, marker);
		if (rc == 0)
			rc = morph_buf_puts(&buf, content + tail);
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
		tokens = tokenizer_count(tokenizer, morph_buf_cstr(&buf));
		if (tokens <= max_tokens)
			break;
		next = (size_t)((double)budget * (double)max_tokens / (double)tokens);
		budget = next < budget ? next : budget - 1;
	}
	*truncated = 1;
	return morph_buf_detach(&buf);
}

/* Only payload fields are expendable; execution control fields stay intact. */
static struct json *history_largest_payload(struct json *node, size_t *size)
{
	struct json *best = NULL;
	struct json *child;

	json_foreach (child, node) {
		const char *key = json_key(child);
		if (key &&
		    (!strcmp(key, "ok") || !strcmp(key, "error") || !strcmp(key, "exit_code") ||
		     !strcmp(key, "status") || !strcmp(key, "session_id") ||
		     !strcmp(key, "artifacts") || !strcmp(key, "ui")))
			continue;
		if (json_is_string_value(child) && strlen(json_string_value(child)) > *size) {
			*size = strlen(json_string_value(child));
			best = child;
		} else if (json_is_array_value(child) && json_count(child) > 0) {
			char *json = json_stringify(child);
			if (json && strlen(json) > *size) {
				*size = strlen(json);
				best = child;
			}
			free(json);
		} else if (json_is_object_value(child)) {
			struct json *nested = history_largest_payload(child, size);
			if (nested)
				best = nested;
		}
	}
	return best;
}

static char *history_prepare_json(struct tokenizer *tokenizer, struct json *root, int budget,
				  int *truncated)
{
	struct json *data = json_member(root, "data");
	struct json *ok = json_member(root, "ok");
	struct json *child;
	struct json *next;

	if (!(json_is_bool_value(ok) && json_is_object_value(data))) {
		char *json = json_stringify(root);
		struct json *wrapper;
		char *result;

		if (!json || tokenizer_count(tokenizer, json) <= budget)
			return json;
		wrapper = json_object();
		if (!wrapper) {
			free(json);
			return NULL;
		}
		if (!json_add_bool(wrapper, "history_truncated", 1) ||
		    !json_add_string(wrapper, "preview", json)) {
			free(json);
			json_free(wrapper);
			return NULL;
		}
		free(json);
		for (int preview_budget = budget; preview_budget > 0; preview_budget /= 2) {
			int ignored;
			struct json *preview = json_member(wrapper, "preview");
			char *snippet = history_truncate_result(
				tokenizer, json_string_value(preview), preview_budget, &ignored);
			if (!snippet)
				break;
			if (!json_string_update(preview, snippet)) {
				free(snippet);
				break;
			}
			free(snippet);
			result = json_stringify(wrapper);
			if (result && tokenizer_count(tokenizer, result) <= budget) {
				*truncated = 1;
				json_free(wrapper);
				return result;
			}
			free(result);
		}
		result = NULL;
		json_free(wrapper);
		return result;
	}
	if (json_is_bool_value(ok) && json_is_object_value(data)) {
		for (child = json_first(root); child; child = next) {
			struct json *original;

			next = json_next(child);
			if (child == data || !json_key(child))
				continue;
			original = json_member(data, json_key(child));
			if (original && json_equal(original, child, 1))
				json_object_remove(root, json_key(child));
		}
	}
	for (;;) {
		char *json = json_stringify(root);
		struct json *largest;
		size_t size = 0;
		char *snippet;
		int ignored;
		int total_tokens;

		if (!json)
			return NULL;
		if (*truncated && json_is_object_value(root) &&
		    !json_member(root, "history_truncated")) {
			free(json);
			if (!json_add_bool(root, "history_truncated", 1))
				return NULL;
			continue;
		}
		total_tokens = tokenizer_count(tokenizer, json);
		if (total_tokens <= budget)
			return json;
		free(json);
		largest = history_largest_payload(root, &size);
		if (!largest || size < 16)
			return NULL;
		if (json_is_array_value(largest)) {
			json_array_remove(largest, json_count(largest) - 1);
			*truncated = 1;
			continue;
		}
		int text_tokens = tokenizer_count(tokenizer, json_string_value(largest));
		int text_budget = text_tokens - (total_tokens - budget) - 16;
		snippet = history_truncate_result(tokenizer, json_string_value(largest),
						  text_budget > 0 ? text_budget : 1, &ignored);
		if (!snippet)
			return NULL;
		if (!json_string_update(largest, snippet)) {
			free(snippet);
			return NULL;
		}
		free(snippet);
		*truncated = 1;
	}
}

int agent_history_prepare_tool_content(struct react_context *ctx, const char *content,
				       char **prepared, int *truncated)
{
	char *limited;
	char *safe;
	int was_truncated = 0;

	if (!ctx || !prepared)
		return -EINVAL;
	/* Repair malformed tool bytes before redaction or UTF-8 boundary selection. */
	limited = strdup(content ? content : "");
	if (!limited)
		return -ENOMEM;
	utf8_sanitize_inplace(limited);
	/* Redaction may expand short secrets into longer replacement strings. */
	safe = history_redact_json(ctx, limited);
	free(limited);
	if (!safe)
		return -ENOMEM;
	{
		struct json *root = json_parse_ex(safe, NULL, 1, NULL);
		int budget = ctx->history_tool_result_tokens > 0
				     ? ctx->history_tool_result_tokens
				     : HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;

		if (root) {
			limited =
				history_prepare_json(ctx->tokenizer, root, budget, &was_truncated);
			json_free(root);
			if (!limited) {
				free(safe);
				return -E2BIG;
			}
		} else {
			limited = history_truncate_result(ctx->tokenizer, safe, budget,
							  &was_truncated);
		}
	}
	free(safe);
	if (!limited)
		return -ENOMEM;
	log_dbg(ctx ? ctx->logger : NULL, "history tool result: %zu -> %zu bytes, truncated=%d",
		content ? strlen(content) : 0, strlen(limited), was_truncated);
	*prepared = limited;
	if (truncated)
		*truncated = was_truncated;
	return 0;
}
