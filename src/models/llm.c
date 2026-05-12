#include "llm.h"
#include "util/log.h"
#include "util/file.h"
#include "http/client.h"
#include "http/sse.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *escape_json_string(const char *s)
{
	if (!s)
		return strdup("");
	size_t len = strlen(s);
	size_t cap = len * 2 + 1;
	char *out = malloc(cap);
	if (!out)
		return NULL;
	size_t j = 0;
	for (size_t i = 0; i < len; i++) {
		if (j + 3 >= cap) {
			cap *= 2;
			char *new_out = realloc(out, cap);
			if (!new_out) {
				free(out);
				return NULL;
			}
			out = new_out;
		}
		switch (s[i]) {
		case '"':  out[j++] = '\\'; out[j++] = '"'; break;
		case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
		case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
		case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
		case '\t': out[j++] = '\\'; out[j++] = 't'; break;
		default:   out[j++] = s[i]; break;
		}
	}
	out[j] = '\0';
	return out;
}

static int build_messages_json(const char *system_prompt,
				const char **messages, int n,
				char **out_json)
{
	size_t cap = 8192;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, cap - len, "[");
	int first = 1;

	if (system_prompt && *system_prompt) {
		char *esc = escape_json_string(system_prompt);
		if (!esc) { free(buf); return -ENOMEM; }
		len += snprintf(buf + len, cap - len,
				"{\"role\":\"system\",\"content\":\"%s\"}", esc);
		free(esc);
		first = 0;
	}

	for (int i = 0; i < n; i++) {
		if (!first)
			len += snprintf(buf + len, cap - len, ",");
		const char *role = (i % 2 == 0) ? "user" : "assistant";
		char *esc = escape_json_string(messages[i]);
		if (!esc) { free(buf); return -ENOMEM; }
		size_t needed = len + strlen(esc) + 64;
		if (needed > cap) {
			cap = needed * 2;
			char *new_buf = realloc(buf, cap);
			if (!new_buf) { free(buf); free(esc); return -ENOMEM; }
			buf = new_buf;
		}
		len += snprintf(buf + len, cap - len,
				"{\"role\":\"%s\",\"content\":\"%s\"}",
				role, esc);
		free(esc);
		first = 0;
	}
	len += snprintf(buf + len, cap - len, "]");
	*out_json = buf;
	return 0;
}

struct llm_stream_ctx {
	sse_callback user_cb;
	void *user_data;
	char *accumulated;
	size_t acc_len;
};

static void llm_stream_init(struct llm_stream_ctx *ctx,
			     sse_callback cb, void *user_data)
{
	ctx->user_cb = cb;
	ctx->user_data = user_data;
	ctx->accumulated = malloc(8192);
	ctx->acc_len = 0;
	if (ctx->accumulated)
		ctx->accumulated[0] = '\0';
}

static void llm_stream_append(struct llm_stream_ctx *ctx, const char *text)
{
	size_t tlen = strlen(text);
	char *new_acc = realloc(ctx->accumulated, ctx->acc_len + tlen + 1);
	if (!new_acc)
		return;
	ctx->accumulated = new_acc;
	memcpy(ctx->accumulated + ctx->acc_len, text, tlen);
	ctx->acc_len += tlen;
	ctx->accumulated[ctx->acc_len] = '\0';
}

static void llm_stream_free(struct llm_stream_ctx *ctx)
{
	free(ctx->accumulated);
	ctx->accumulated = NULL;
}

static int llm_sse_event_cb(const char *event, const char *data, void *ud)
{
	struct llm_stream_ctx *ctx = ud;
	(void)event;
	if (!data || !*data)
		return 0;

	const char *json_str = data;
	if (strcmp(json_str, "[DONE]") == 0)
		return 0;

	const char *content_key = strstr(json_str, "\"content\"");
	if (!content_key)
		return 0;
	const char *val_start = strchr(content_key + 9, '"');
	if (!val_start)
		return 0;
	val_start++;
	const char *val_end = strchr(val_start, '"');
	if (!val_end)
		return 0;

	size_t clen = (size_t)(val_end - val_start);
	char *content = malloc(clen + 1);
	if (!content)
		return -ENOMEM;
	memcpy(content, val_start, clen);
	content[clen] = '\0';

	char *unescaped = malloc(clen * 2 + 1);
	if (unescaped) {
		size_t j = 0;
		for (size_t i = 0; i < clen; i++) {
			if (content[i] == '\\' && i + 1 < clen) {
				switch (content[i + 1]) {
				case 'n': unescaped[j++] = '\n'; i++; break;
				case 'r': unescaped[j++] = '\r'; i++; break;
				case 't': unescaped[j++] = '\t'; i++; break;
				case '"': unescaped[j++] = '"'; i++; break;
				case '\\': unescaped[j++] = '\\'; i++; break;
				default: unescaped[j++] = content[i]; break;
				}
			} else {
				unescaped[j++] = content[i];
			}
		}
		unescaped[j] = '\0';
	}

	llm_stream_append(ctx, unescaped ? unescaped : content);
	if (ctx->user_cb)
		ctx->user_cb(unescaped ? unescaped : content, ctx->user_data);

	free(unescaped);
	free(content);
	return 0;
}

static int llm_sse_http_cb(const char *data, size_t len, void *ud)
{
	struct sse_parser *parser = ud;
	sse_parser_feed(parser, data, len);
	return 0;
}

static int llm_chat(struct model *self, const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data)
{
	if (!self || !self->api_key[0]) {
		log_err("llm_chat: no API key configured");
		return -EINVAL;
	}

	char *msgs_json = NULL;
	int rc = build_messages_json(system_prompt, messages, n, &msgs_json);
	if (rc < 0)
		return rc;

	size_t body_cap = strlen(msgs_json) + 512;
	char *body = malloc(body_cap);
	if (!body) {
		free(msgs_json);
		return -ENOMEM;
	}

	int body_len = snprintf(body, body_cap,
		"{\"model\":\"%s\","
		"\"messages\":%s,"
		"\"stream\":true,"
		"\"max_tokens\":4096}",
		self->model_id, msgs_json);
	free(msgs_json);

	if (body_len < 0 || (size_t)body_len >= body_cap) {
		free(body);
		return -EIO;
	}

	struct llm_stream_ctx ctx;
	llm_stream_init(&ctx, cb, user_data);

	struct sse_parser parser;
	sse_parser_init(&parser, llm_sse_event_cb, &ctx);

	char url[512];
	if (strcmp(self->provider, "anthropic") == 0)
		snprintf(url, sizeof(url), "%s/messages", self->api_base);
	else
		snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);

	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };
	int status = http_post_sse_ex(url, body, (size_t)body_len,
				       "application/json",
				       extra_headers, 1,
				       llm_sse_http_cb, &parser);

	sse_parser_free(&parser);
	free(body);

	if (status < 0) {
		log_err("llm_chat: SSE request failed: %d", status);
		llm_stream_free(&ctx);
		return status;
	}

	llm_stream_free(&ctx);
	return status;
}

static int llm_generate(struct model *self, const char *prompt,
			const char *out_path)
{
	if (!self || !prompt)
		return -EINVAL;

	const char *messages[] = { prompt };
	char *msgs_json = NULL;
	int rc = build_messages_json(NULL, messages, 1, &msgs_json);
	if (rc < 0)
		return rc;

	size_t body_cap = strlen(msgs_json) + 256;
	char *body = malloc(body_cap);
	if (!body) {
		free(msgs_json);
		return -ENOMEM;
	}

	int body_len = snprintf(body, body_cap,
		"{\"model\":\"%s\","
		"\"messages\":%s,"
		"\"max_tokens\":4096}",
		self->model_id, msgs_json);
	free(msgs_json);

	if (body_len < 0 || (size_t)body_len >= body_cap) {
		free(body);
		return -EIO;
	}

	struct http_response resp = {0};
	char url[512];
	if (strcmp(self->provider, "anthropic") == 0)
		snprintf(url, sizeof(url), "%s/messages", self->api_base);
	else
		snprintf(url, sizeof(url), "%s/chat/completions", self->api_base);
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", self->api_key);
	const char *extra_headers[] = { auth_header };
	rc = http_post_ex(url, body, (size_t)body_len,
			  "application/json", extra_headers, 1, &resp);
	free(body);

	if (rc < 0) {
		http_response_free(&resp);
		return rc;
	}

	if (resp.status_code != 200) {
		log_err("llm_generate: API returned %d", resp.status_code);
		http_response_free(&resp);
		return -EIO;
	}

	rc = 0;
	if (out_path && resp.body)
		rc = file_write_all(out_path, resp.body, resp.body_len);

	http_response_free(&resp);
	return rc;
}

static void llm_destroy(struct model *self)
{
	if (self)
		free(self);
}

struct model *model_llm_create(const char *provider, const char *model_id,
			      const char *api_base, const char *api_key)
{
	struct model *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	strncpy(m->provider, provider ? provider : "openai", sizeof(m->provider) - 1);
	strncpy(m->model_id, model_id ? model_id : "gpt-4o", sizeof(m->model_id) - 1);
	if (api_base)
		strncpy(m->api_base, api_base, sizeof(m->api_base) - 1);
	else
		strncpy(m->api_base, "https://api.openai.com/v1", sizeof(m->api_base) - 1);
	if (api_key)
		strncpy(m->api_key, api_key, sizeof(m->api_key) - 1);
	m->context_limit = 128000;
	m->chat = llm_chat;
	m->generate = llm_generate;
	m->destroy = llm_destroy;
	return m;
}

void model_destroy(struct model *m)
{
	if (m && m->destroy)
		m->destroy(m);
}