#include "history_internal.h"

#include "prompt_context.h"
#include "react.h"
#include "tokenizer.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/utf8.h"
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http/client.h"

#define HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS 8000

static char *history_json_text_copy(const cJSON *value)
{
	char *printed NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);

	return printed ? strdup(printed) : NULL;
}

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
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *cur;
	int rc;

	if (!text)
		return NULL;
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, strlen(text) + 1) ? 0 : NB_ERR_NOMEM);
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
				rc = (nb_buf_append_cstr(&buf, "[binary data omitted]")
					      ? 0
					      : NB_ERR_NOMEM);
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
			rc = (nb_buf_append_char(&buf, *cur++) ? 0 : NB_ERR_NOMEM);
			if (rc != 0)
				break;
			continue;
		}
		if (preserve_prefix)
			rc = (nb_buf_append(&buf, cur, prefix_len) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&buf, "[REDACTED]") ? 0 : NB_ERR_NOMEM);
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
		nb_buf_cleanup(&buf);
		return NULL;
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static char *history_redact_exact(const char *text, const char *secret)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *cur;
	const char *match;
	int rc;

	if (!text)
		return NULL;
	if (!secret || strlen(secret) < 4)
		return strdup(text);
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, strlen(text) + 1) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	cur = text;
	while ((match = strstr(cur, secret)) != NULL) {
		rc = (nb_buf_append(&buf, cur, (size_t)(match - cur)) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&buf, "[REDACTED]") ? 0 : NB_ERR_NOMEM);
		if (rc != 0) {
			nb_buf_cleanup(&buf);
			return NULL;
		}
		cur = match + strlen(secret);
	}
	if ((nb_buf_append_cstr(&buf, cur) ? 0 : NB_ERR_NOMEM) != 0) {
		nb_buf_cleanup(&buf);
		return NULL;
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

char *history_redacted_text_create(struct react_context *ctx, const char *text)
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

static int history_redact_json_values(struct react_context *ctx, cJSON *value, int *changed)
{
	if (cJSON_IsString(value)) {
		const char *key = value->string;
		size_t key_len = key ? strlen(key) : 0;
		int credential = key && (!strcmp(key, "api_key") || !strcmp(key, "apiKey") ||
					 (key_len >= 8 && !strcmp(key + key_len - 8, "_API_KEY")));
		char *safe = credential && value->valuestring[0]
				     ? strdup("[REDACTED]")
				     : history_redacted_json_create(ctx, value->valuestring);
		int rc;

		if (!safe)
			return NB_ERR_NOMEM;
		if (strcmp(value->valuestring, safe) != 0)
			*changed = 1;
		rc = cJSON_SetValuestring(value, safe) ? 0 : NB_ERR_NOMEM;

		free(safe);
		return rc;
	}
	for (cJSON *child = value->child; child; child = child->next) {
		int rc = history_redact_json_values(ctx, child, changed);

		if (rc != 0)
			return rc;
	}
	return 0;
}

char *history_redacted_json_create(struct react_context *ctx, const char *text)
{
	cJSON *root;
	char *safe = NULL;
	int changed = 0;

	if (!text)
		return NULL;
	root = cJSON_ParseWithOpts(text, NULL, 1);
	if (!cJSON_IsObject(root) && !cJSON_IsArray(root)) {
		cJSON_Delete(root);
		return history_redacted_text_create(ctx, text);
	}
	if (history_redact_json_values(ctx, root, &changed) == 0)
		safe = changed ? history_json_text_copy(root) : strdup(text);
	cJSON_Delete(root);
	return safe;
}

static char *history_truncate_result(struct tokenizer *tokenizer, const char *content,
				     int max_tokens, int *truncated)
{
	const char *marker = "\n…(tool output truncated)…\n";
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
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
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return NULL;
	for (;;) {
		size_t head = nb_utf8_clamp_bytes(content, budget * 3 / 5);
		size_t tail = nb_utf8_clamp_bytes(content, len - (budget - budget * 3 / 5));
		size_t next;

		nb_buf_clear(&buf);
		rc = (nb_buf_append(&buf, content, head) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&buf, marker) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&buf, content + tail) ? 0 : NB_ERR_NOMEM);
		if (rc != 0) {
			nb_buf_cleanup(&buf);
			return NULL;
		}
		tokens = tokenizer_count(tokenizer, nb_buf_cstr(&buf));
		if (tokens <= max_tokens)
			break;
		next = (size_t)((double)budget * (double)max_tokens / (double)tokens);
		budget = next < budget ? next : budget - 1;
	}
	*truncated = 1;
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

static int history_agent_status_array(const cJSON *array)
{
	if (!cJSON_IsArray(array) || !cJSON_GetArraySize(array))
		return 0;
	const cJSON *entry;
	cJSON_ArrayForEach (entry, array) {
		if (!cJSON_IsObject(entry) ||
		    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "agent_id")) ||
		    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "status")))
			return 0;
	}
	return 1;
}

/* Only payload fields are expendable; execution and agent control fields stay intact. */
static cJSON *history_largest_payload(cJSON *node, size_t *size)
{
	cJSON *best = NULL;
	cJSON *child;

	cJSON_ArrayForEach (child, node) {
		const char *key = child->string;
		if (key &&
		    (!strcmp(key, "ok") || !strcmp(key, "error") || !strcmp(key, "exit_code") ||
		     !strcmp(key, "status") || !strcmp(key, "session_id") ||
		     !strcmp(key, "agent_id") || !strcmp(key, "parent_agent_id") ||
		     !strcmp(key, "cursor") || !strcmp(key, "next_cursor") ||
		     !strcmp(key, "artifacts") || !strcmp(key, "ui")))
			continue;
		if (cJSON_IsString(child) && strlen(child->valuestring) > *size) {
			*size = strlen(child->valuestring);
			best = child;
		} else if (cJSON_IsArray(child) && cJSON_GetArraySize(child) > 0) {
			if (history_agent_status_array(child)) {
				cJSON *entry;
				cJSON_ArrayForEach (entry, child) {
					cJSON *nested = history_largest_payload(entry, size);
					if (nested)
						best = nested;
				}
				continue;
			}
			char *json = history_json_text_copy(child);
			if (json && strlen(json) > *size) {
				*size = strlen(json);
				best = child;
			}
			free(json);
		} else if (cJSON_IsObject(child)) {
			cJSON *nested = history_largest_payload(child, size);
			if (nested)
				best = nested;
		}
	}
	return best;
}

/* A report page remains a contiguous prefix so its cursor never skips evidence. */
static int history_shorten_agent_page(cJSON *data, cJSON *payload)
{
	cJSON *agent = cJSON_GetObjectItemCaseSensitive(data, "agent");
	cJSON *offset = cJSON_GetObjectItemCaseSensitive(data, "offset");
	cJSON *total = cJSON_GetObjectItemCaseSensitive(data, "total_bytes");
	if (!data || payload != cJSON_GetObjectItemCaseSensitive(data, "result") ||
	    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(agent, "agent_id")) ||
	    !cJSON_IsNumber(offset) || offset->valuedouble < 0 ||
	    offset->valuedouble > INT_MAX || offset->valuedouble != (double)offset->valueint ||
	    !cJSON_IsNumber(total) || total->valuedouble < offset->valuedouble)
		return 0;
	size_t length = strlen(payload->valuestring);
	size_t retained = nb_utf8_clamp_bytes(payload->valuestring, length / 2);
	if (!retained)
		return NB_ERR_TOO_BIG;
	char *prefix NB_CLEANUP(nb_charp_cleanup) = strndup(payload->valuestring, retained);
	if (!prefix || !cJSON_SetValuestring(payload, prefix))
		return NB_ERR_NOMEM;
	cJSON *next NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_CreateNumber(offset->valuedouble + (double)retained);
	cJSON *more NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateBool(1);
	if (!next || !more)
		return NB_ERR_NOMEM;
	cJSON_DeleteItemFromObjectCaseSensitive(data, "next_offset");
	cJSON_DeleteItemFromObjectCaseSensitive(data, "has_more");
	if ((cJSON_AddItemToObject(data, "next_offset", next) ? (next = NULL, NB_OK)
							      : NB_ERR_NOMEM) != 0 ||
	    (cJSON_AddItemToObject(data, "has_more", more) ? (more = NULL, NB_OK) : NB_ERR_NOMEM) !=
		    0)
		return NB_ERR_NOMEM;
	return 1;
}

static char *history_prepare_json(struct tokenizer *tokenizer, cJSON *root, int budget,
				  int *truncated)
{
	cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
	cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
	cJSON *child;
	cJSON *next;

	if (!(cJSON_IsBool(ok) && cJSON_IsObject(data))) {
		char *json = history_json_text_copy(root);
		cJSON *wrapper;
		char *result;

		if (!json || tokenizer_count(tokenizer, json) <= budget)
			return json;
		wrapper = cJSON_CreateObject();
		if (!wrapper) {
			free(json);
			return NULL;
		}
		if (!cJSON_AddBoolToObject(wrapper, "history_truncated", 1) ||
		    !cJSON_AddStringToObject(wrapper, "preview", json)) {
			free(json);
			cJSON_Delete(wrapper);
			return NULL;
		}
		free(json);
		for (int preview_budget = budget; preview_budget > 0; preview_budget /= 2) {
			int ignored;
			cJSON *preview = cJSON_GetObjectItemCaseSensitive(wrapper, "preview");
			char *snippet = history_truncate_result(tokenizer, preview->valuestring,
								preview_budget, &ignored);
			if (!snippet)
				break;
			if (!cJSON_SetValuestring(preview, snippet)) {
				free(snippet);
				break;
			}
			free(snippet);
			result = history_json_text_copy(wrapper);
			if (result && tokenizer_count(tokenizer, result) <= budget) {
				*truncated = 1;
				cJSON_Delete(wrapper);
				return result;
			}
			free(result);
		}
		result = NULL;
		cJSON_Delete(wrapper);
		return result;
	}
	if (cJSON_IsBool(ok) && cJSON_IsObject(data)) {
		for (child = root->child; child; child = next) {
			cJSON *original;

			next = child->next;
			if (child == data || !child->string)
				continue;
			original = cJSON_GetObjectItemCaseSensitive(data, child->string);
			if (original && cJSON_Compare(original, child, 1))
				cJSON_DeleteItemFromObjectCaseSensitive(root, child->string);
		}
	}
	for (;;) {
		char *json = history_json_text_copy(root);
		cJSON *largest;
		size_t size = 0;
		char *snippet;
		int ignored;
		int total_tokens;

		if (!json)
			return NULL;
		if (*truncated && cJSON_IsObject(root) &&
		    !cJSON_GetObjectItemCaseSensitive(root, "history_truncated")) {
			free(json);
			if (!cJSON_AddBoolToObject(root, "history_truncated", 1))
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
		int page_rc = history_shorten_agent_page(data, largest);
		if (page_rc < 0)
			return NULL;
		if (page_rc > 0) {
			*truncated = 1;
			continue;
		}
		if (cJSON_IsArray(largest)) {
			cJSON_DeleteItemFromArray(largest, cJSON_GetArraySize(largest) - 1);
			*truncated = 1;
			continue;
		}
		int text_tokens = tokenizer_count(tokenizer, largest->valuestring);
		int text_budget = text_tokens - (total_tokens - budget) - 16;
		snippet = history_truncate_result(tokenizer, largest->valuestring,
						  text_budget > 0 ? text_budget : 1, &ignored);
		if (!snippet)
			return NULL;
		if (!cJSON_SetValuestring(largest, snippet)) {
			free(snippet);
			return NULL;
		}
		free(snippet);
		*truncated = 1;
	}
}

int agent_history_tool_content_copy(struct react_context *ctx, const char *content, char **prepared,
				    int *truncated)
{
	char *limited;
	char *safe;
	int was_truncated = 0;

	if (!ctx || !prepared)
		return NB_ERR_INVAL;
	if (*prepared)
		return NB_ERR_STATE;
	/* Repair malformed tool bytes before redaction or UTF-8 boundary selection. */
	limited = strdup(content ? content : "");
	if (!limited)
		return NB_ERR_NOMEM;
	nb_utf8_sanitize_inplace(limited);
	/* Redaction may expand short secrets into longer replacement strings. */
	safe = history_redacted_json_create(ctx, limited);
	free(limited);
	if (!safe)
		return NB_ERR_NOMEM;
	{
		cJSON *root = cJSON_ParseWithOpts(safe, NULL, 1);
		int budget = ctx->history_tool_result_tokens > 0
				     ? ctx->history_tool_result_tokens
				     : HISTORY_DEFAULT_TOOL_RESULT_MAX_TOKENS;

		if (root) {
			limited =
				history_prepare_json(ctx->tokenizer, root, budget, &was_truncated);
			cJSON_Delete(root);
			if (!limited) {
				free(safe);
				return NB_ERR_TOO_BIG;
			}
		} else {
			limited = history_truncate_result(ctx->tokenizer, safe, budget,
							  &was_truncated);
		}
	}
	free(safe);
	if (!limited)
		return NB_ERR_NOMEM;
	nb_log_write(ctx ? ctx->logger : NULL, NB_LOG_DEBUG,
		     "history tool result: %zu -> %zu bytes, truncated=%d",
		     content ? strlen(content) : 0, strlen(limited), was_truncated);
	*prepared = limited;
	if (truncated)
		*truncated = was_truncated;
	return 0;
}
