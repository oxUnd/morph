#include "text_qa.h"
#include "util/log.h"
#include "util/arena.h"
#include "util/buf.h"
#include "models/llm.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/error.h"

static char *extract_param(const char *args_json, const char *param)
{
	if (!args_json || !*args_json || !param)
		return NULL;

	cJSON *root = cJSON_Parse(args_json);
	if (root) {
		cJSON *item = cJSON_GetObjectItem(root, param);
		if (cJSON_IsString(item) && item->valuestring) {
			char *result = strdup(item->valuestring);
			cJSON_Delete(root);
			return result;
		}
		cJSON_Delete(root);
	}

	const char *p = strstr(args_json, param);
	if (!p)
		return NULL;
	const char *colon = strchr(p + strlen(param), ':');
	if (!colon)
		colon = p + strlen(param);
	else
		colon++;
	while (*colon == ' ' || *colon == '\t')
		colon++;
	char quote = '\0';
	if (*colon == '"') {
		quote = '"';
		colon++;
	} else if (*colon == '\'') {
		quote = '\'';
		colon++;
	}
	const char *end;
	if (quote) {
		end = colon;
		while (*end) {
			if (*end == '\\' && *(end + 1)) {
				end += 2;
				continue;
			}
			if (*end == quote)
				break;
			end++;
		}
	} else {
		end = colon;
		while (*end && *end != ',' && *end != '}' && *end != ')')
			end++;
	}
	size_t len = (size_t)(end - colon);
	char *result = malloc(len + 1);
	if (!result)
		return NULL;
	size_t di = 0;
	for (size_t si = 0; si < len; si++) {
		if (colon[si] == '\\' && si + 1 < len) {
			switch (colon[si + 1]) {
			case 'n': result[di++] = '\n'; si++; break;
			case 't': result[di++] = '\t'; si++; break;
			case '"': result[di++] = '"'; si++; break;
			case '\'': result[di++] = '\''; si++; break;
			case '\\': result[di++] = '\\'; si++; break;
			default: result[di++] = colon[si]; break;
			}
		} else {
			result[di++] = colon[si];
		}
	}
	result[di] = '\0';
	return result;
}

static int text_qa_stream_cb(const char *token, void *user_data)
{
	return morph_buf_append_cb(token, user_data);
}

static int text_qa_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct model *llm = user_data;

	if (!result)
		return -EINVAL;

	char *prompt = extract_param(args_json, "prompt");
	if (!prompt || !*prompt) {
		free(prompt);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing or empty 'prompt' parameter. "
			"Usage: text_qa({\\\"prompt\\\": \\\"summarize this\\\", "
			"\\\"context\\\": \\\"...\\\"})\"}"));
		return -EINVAL;
	}

	if (!llm || !llm->api_key[0]) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"no LLM model configured\"}"));
		free(prompt);
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	char *context = extract_param(args_json, "context");

	char qa_prompt[4096];
	if (context && *context)
		snprintf(qa_prompt, sizeof(qa_prompt),
			 "Context: %s\n\nQuestion/Task: %s", context, prompt);
	else
		snprintf(qa_prompt, sizeof(qa_prompt), "%s", prompt);

	free(context);

	struct arena *arena = arena_create(64 * 1024);
	if (!arena) {
		free(prompt);
		(void)tool_result_take_text(result, strdup("{\"error\":\"memory allocation failed\"}"));
		return -ENOMEM;
	}

	const char *messages[] = { qa_prompt };
	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 8192);
	if (rc != 0) {
		arena_destroy(arena);
		free(prompt);
		(void)tool_result_take_text(result, strdup("{\"error\":\"buffer allocation failed\"}"));
		return rc;
	}

	int status = llm->chat(llm, arena, NULL, messages, 1,
			       text_qa_stream_cb, &buf);
	if (status < 0) {
		morph_buf_cleanup(&buf);
		arena_destroy(arena);
		free(prompt);
		(void)tool_result_take_text(result, strdup("{\"error\":\"LLM call failed\"}"));
		return status;
	}

	(void)tool_result_take_text(result, morph_buf_detach(&buf));
	if (!result->text.data) {
		arena_destroy(arena);
		free(prompt);
		(void)tool_result_take_text(result, strdup("{\"error\":\"buffer allocation failed\"}"));
		return -ENOMEM;
	}
	arena_destroy(arena);
	free(prompt);
	return 0;
}

int text_qa_init(struct tool_registry *reg, struct model *llm)
{
	if (!reg)
		return -EINVAL;
	return tool_register(reg, "text_qa",
		"Answer questions, rewrite text, or transform content based on context",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"The question or instruction to answer\"},\"context\":{\"type\":\"string\",\"description\":\"Reference context for the question\"}},\"required\":[\"prompt\"]}",
		text_qa_exec, llm, NULL);
}
