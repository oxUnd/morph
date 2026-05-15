#include "text_qa.h"
#include "util/log.h"
#include "models/llm.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct model *g_qa_llm;

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

struct text_qa_stream_ctx {
	char *response;
	size_t len;
	size_t cap;
};

static int text_qa_stream_cb(const char *token, void *user_data)
{
	struct text_qa_stream_ctx *ctx = user_data;
	size_t tlen = strlen(token);
	if (ctx->len + tlen + 1 >= ctx->cap) {
		ctx->cap = (ctx->len + tlen + 1) * 2;
		char *new_resp = realloc(ctx->response, ctx->cap);
		if (!new_resp)
			return -ENOMEM;
		ctx->response = new_resp;
	}
	memcpy(ctx->response + ctx->len, token, tlen);
	ctx->len += tlen;
	ctx->response[ctx->len] = '\0';
	return 0;
}

static int text_qa_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	char *prompt = extract_param(args_json, "prompt");
	if (!prompt || !*prompt) {
		free(prompt);
		*result_json = strdup(
			"{\"error\":\"missing or empty 'prompt' parameter. "
			"Usage: text_qa({\\\"prompt\\\": \\\"summarize this\\\", "
			"\\\"context\\\": \\\"...\\\"})\"}");
		return -EINVAL;
	}

	if (!g_qa_llm || !g_qa_llm->api_key[0]) {
		*result_json = strdup("{\"error\":\"no LLM model configured\"}");
		free(prompt);
		return -ENOSYS;
	}

	char *context = extract_param(args_json, "context");

	char qa_prompt[4096];
	if (context && *context)
		snprintf(qa_prompt, sizeof(qa_prompt),
			 "Context: %s\n\nQuestion/Task: %s", context, prompt);
	else
		snprintf(qa_prompt, sizeof(qa_prompt), "%s", prompt);

	free(context);

	const char *messages[] = { qa_prompt };
	struct text_qa_stream_ctx ctx = {
		.response = malloc(8192),
		.len = 0,
		.cap = 8192,
	};
	if (!ctx.response) {
		free(prompt);
		return -ENOMEM;
	}
	ctx.response[0] = '\0';

	int status = g_qa_llm->chat(g_qa_llm, NULL, messages, 1,
				    text_qa_stream_cb, &ctx);
	if (status < 0) {
		free(ctx.response);
		free(prompt);
		*result_json = strdup("{\"error\":\"LLM call failed\"}");
		return status;
	}

	*result_json = ctx.response;
	free(prompt);
	return 0;
}

int text_qa_init(struct tool_registry *reg, struct model *llm)
{
	if (!reg)
		return -EINVAL;
	g_qa_llm = llm;
	return tool_register(reg, "text_qa",
		"Answer questions, rewrite text, or transform content based on context",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"The question or instruction to answer\"},\"context\":{\"type\":\"string\",\"description\":\"Reference context for the question\"}},\"required\":[\"prompt\"]}",
		text_qa_exec, NULL);
}
