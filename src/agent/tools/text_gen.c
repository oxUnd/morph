#include "text_gen.h"
#include "util/log.h"
#include "models/llm.h"
#include <stdlib.h>
#include <string.h>

static struct model *g_llm;

static char *extract_prompt(const char *args_json)
{
	if (!args_json || !*args_json)
		return NULL;
	const char *p = strstr(args_json, "\"prompt\"");
	if (p) {
		const char *colon = strchr(p + 7, ':');
		if (!colon)
			colon = p + 7;
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
		memcpy(result, colon, len);
		result[len] = '\0';
		char *dst = result;
		char *src = result;
		while (*src) {
			if (*src == '\\' && *(src + 1)) {
				char next = *(src + 1);
				switch (next) {
				case 'n': *dst++ = '\n'; break;
				case 't': *dst++ = '\t'; break;
				case '"': *dst++ = '"'; break;
				case '\'': *dst++ = '\''; break;
				case '\\': *dst++ = '\\'; break;
				default: *dst++ = next; break;
				}
				src += 2;
			} else {
				*dst++ = *src++;
			}
		}
		*dst = '\0';
		return result;
	}

	size_t len = strlen(args_json);
	const char *start = args_json;
	while (*start == ' ' || *start == '\t')
		start++;
	char *result = malloc(len + 1);
	if (!result)
		return NULL;
	size_t i = 0;
	while (*start && *start != ')') {
		if (*start == '\\' && *(start + 1)) {
			result[i++] = *(start + 1);
			start += 2;
		} else {
			result[i++] = *start++;
		}
	}
	result[i] = '\0';
	return result;
}

struct text_gen_stream_ctx {
	char *response;
	size_t len;
	size_t cap;
};

static int text_gen_stream_cb(const char *token, void *user_data)
{
	struct text_gen_stream_ctx *ctx = user_data;
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

static int text_gen_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	char *prompt = extract_prompt(args_json);
	if (!prompt || !*prompt) {
		free(prompt);
		*result_json = strdup("{\"error\":\"missing or empty prompt\"}");
		return -EINVAL;
	}

	if (!g_llm || !g_llm->api_key[0]) {
		*result_json = strdup("{\"error\":\"no LLM model configured\"}");
		free(prompt);
		return -ENOSYS;
	}

	const char *messages[] = { prompt };
	struct text_gen_stream_ctx ctx = {
		.response = malloc(8192),
		.len = 0,
		.cap = 8192,
	};
	if (!ctx.response) {
		free(prompt);
		return -ENOMEM;
	}
	ctx.response[0] = '\0';

	int status = g_llm->chat(g_llm, NULL, messages, 1,
				 text_gen_stream_cb, &ctx);
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

int text_gen_init(struct tool_registry *reg, struct model *llm)
{
	if (!reg)
		return -EINVAL;
	g_llm = llm;
	return tool_register(reg, "text_gen",
		"Generate text content based on a prompt",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"style\":{\"type\":\"string\"},\"length\":{\"type\":\"string\"}}}",
		text_gen_exec, NULL);
}