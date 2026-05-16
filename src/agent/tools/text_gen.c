#include "text_gen.h"
#include "util/log.h"
#include "util/arena.h"
#include "models/llm.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static struct model *g_llm;

static char *extract_prompt(const char *args_json)
{
	if (!args_json || !*args_json)
		return NULL;

	cJSON *root = cJSON_Parse(args_json);
	if (root) {
		cJSON *prompt = cJSON_GetObjectItem(root, "prompt");
		if (cJSON_IsString(prompt) && prompt->valuestring) {
			char *result = strdup(prompt->valuestring);
			cJSON_Delete(root);
			return result;
		}
		cJSON_Delete(root);
	}

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

	return strdup(args_json);
}

struct text_gen_stream_ctx {
	char *response;
	size_t len;
	size_t cap;
};

static int text_gen_stream_cb(const char *token, void *user_data)
{
	struct text_gen_stream_ctx *ctx = user_data;
	log_dbg("text_gen_stream_cb: token=\"%s\", len=%zu", token, ctx->len);
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
		*result_json = strdup(
			"{\"error\":\"missing or empty 'prompt' parameter. "
			"Usage: text_gen({\\\"prompt\\\": \\\"tell me a story\\\"})\"}");
		return -EINVAL;
	}

	if (!g_llm || !g_llm->api_key[0]) {
		*result_json = strdup("{\"error\":\"no LLM model configured\"}");
		free(prompt);
		return -ENOSYS;
	}

	struct arena *arena = arena_create(64 * 1024);
	if (!arena) {
		free(prompt);
		*result_json = strdup("{\"error\":\"memory allocation failed\"}");
		return -ENOMEM;
	}

	const char *messages[] = { prompt };
	struct text_gen_stream_ctx ctx = {
		.response = malloc(8192),
		.len = 0,
		.cap = 8192,
	};
	if (!ctx.response) {
		arena_destroy(arena);
		free(prompt);
		return -ENOMEM;
	}
	ctx.response[0] = '\0';

	int status = g_llm->chat(g_llm, arena, NULL, messages, 1,
				 text_gen_stream_cb, &ctx);
	if (status < 0) {
		free(ctx.response);
		arena_destroy(arena);
		free(prompt);
		*result_json = strdup("{\"error\":\"LLM call failed\"}");
		return status;
	}

	*result_json = ctx.response;
	arena_destroy(arena);
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
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"The text prompt to generate content from\"},\"style\":{\"type\":\"string\",\"description\":\"Writing style (e.g. formal, casual, creative)\"},\"length\":{\"type\":\"string\",\"description\":\"Desired length (e.g. short, medium, long)\"}},\"required\":[\"prompt\"]}",
		text_gen_exec, NULL);
}