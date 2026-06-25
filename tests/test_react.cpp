#include <gtest/gtest.h>
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "util/arena.h"
#include "http/client.h"
#include "http/sse.h"
#include "config.h"
#include "util/error.h"
#include <string.h>
#include <signal.h>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>

struct sse_test_info {
	int count;
	std::string last_data;
};

static int global_sse_write_adapter(const char *data, size_t len, void *user_data)
{
	struct sse_parser *parser = (struct sse_parser *)user_data;
	sse_parser_feed(parser, data, len);
	return 0;
}

/* ---- mock tools ---- */

static int test_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_take_text(result, strdup("{\"result\":\"test\"}"));
	return 0;
}

static int failing_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_take_text(result, strdup("tool failed"));
	return -EIO;
}

static int call_count_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	int *count = (int *)user_data;
	(*count)++;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"calls\":%d}", *count);
	(void)tool_result_take_text(result, strdup(buf));
	return 0;
}

static int artifact_tool_fn(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_take_text(result,
		strdup("{\"output_path\":\"/tmp/morph-event-test.png\"}"));
	return 0;
}

/* ---- mock LLM helpers ---- */

struct mock_collect_data {
	char *buf;
	size_t len;
	size_t cap;
};

static int mock_collect_cb(const char *token, void *user_data)
{
	struct mock_collect_data *cd = (struct mock_collect_data *)user_data;
	size_t tlen = strlen(token);
	if (cd->len + tlen + 1 >= cd->cap) {
		cd->cap = (cd->len + tlen + 1) * 2;
		char *new_b = (char *)realloc(cd->buf, cd->cap);
		if (!new_b) return -ENOMEM;
		cd->buf = new_b;
	}
	memcpy(cd->buf + cd->len, token, tlen);
	cd->len += tlen;
	cd->buf[cd->len] = '\0';
	return 0;
}

/* ---- mock LLM ---- */

struct mock_llm_data {
	const char *response;
	int call_count;
	int fail_after;
	int should_fail;
	int sleep_ms;
};

static int mock_llm_chat(struct model *self, struct arena *arena,
			  const char *system_prompt,
			  const char **messages, int n,
			  sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (data->sleep_ms > 0)
		std::this_thread::sleep_for(
			std::chrono::milliseconds(data->sleep_ms));
	if (cb && data->response)
		cb(data->response, user_data);
	return 200;
}

static int mock_llm_streaming_chat(struct model *self, struct arena *arena,
				    const char *system_prompt,
				    const char **messages, int n,
				    sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (data->sleep_ms > 0)
		std::this_thread::sleep_for(
			std::chrono::milliseconds(data->sleep_ms));
	if (cb && data->response) {
		const char *r = data->response;
		size_t len = strlen(r);
		size_t chunk = len > 20 ? 20 : len;
		for (size_t i = 0; i < len; i += chunk) {
			size_t remaining = len - i;
			size_t this_chunk = remaining < chunk ? remaining : chunk;
			char *piece = (char *)malloc(this_chunk + 1);
			memcpy(piece, r + i, this_chunk);
			piece[this_chunk] = '\0';
			cb(piece, user_data);
			free(piece);
		}
	}
	return 200;
}

static char *strcasestr_local(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	while (*haystack) {
		if (strncasecmp(haystack, needle, nlen) == 0)
			return (char *)haystack;
		haystack++;
	}
	return nullptr;
}

static int mock_chat_with_tools(struct model *self, struct arena *arena,
				const char *system_prompt,
				struct chat_message *messages, int msg_count,
				struct tool_desc *tools, int tool_count,
				struct chat_response *response,
				sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;

	struct mock_collect_data cd = {nullptr, 0, 0};
	cd.buf = (char *)malloc(8192);
	cd.cap = 8192;
	cd.buf[0] = '\0';

	int status = self->chat(self, arena, nullptr, nullptr, 0, mock_collect_cb, &cd);
	if (status < 0) {
		free(cd.buf);
		return status;
	}

	memset(response, 0, sizeof(*response));

	char *action_pos = strcasestr_local(cd.buf, "Action:");
	if (action_pos && tool_count > 0) {
		if (action_pos > cd.buf) {
			size_t tlen = action_pos - cd.buf;
			char *thought = (char *)malloc(tlen + 1);
			memcpy(thought, cd.buf, tlen);
			thought[tlen] = '\0';
			while (tlen > 0 && isspace((unsigned char)thought[tlen-1]))
				thought[--tlen] = '\0';
			char *t = thought;
			if (strncasecmp(t, "Thought:", 8) == 0) {
				t += 8;
				while (*t == ' ') t++;
			}
			if (*t) {
				response->content = strdup(t);
				if (thought_cb)
					thought_cb(t, thought_ud);
			}
			free(thought);
		}

		const char *ap = action_pos + 7;
		while (*ap == ' ') ap++;
		char tool_name[64] = {0};
		int ni = 0;
		while (*ap && *ap != '(' && *ap != '\n' && ni < 63)
			tool_name[ni++] = *ap++;
		char *args = strdup("{}");
		if (*ap == '(') {
			ap++;
			const char *args_start = ap;
			int depth = 1;
			while (*ap && depth > 0) {
				if (*ap == '(') depth++;
				else if (*ap == ')') depth--;
				ap++;
			}
			size_t alen = (size_t)((ap - 1) - args_start);
			free(args);
			args = (char *)malloc(alen + 1);
			memcpy(args, args_start, alen);
			args[alen] = '\0';
		}
		response->tool_calls = (struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		snprintf(response->tool_calls[0].id, sizeof(response->tool_calls[0].id),
			 "call_mock_%d", 0);
		strncpy(response->tool_calls[0].name, tool_name,
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = args;
	} else {
		const char *content = cd.buf;
		char *final_pos = strcasestr_local(cd.buf, "Final:");
		if (final_pos) {
			final_pos += 6;
			while (*final_pos == ' ') final_pos++;
			content = final_pos;
		} else {
			char *thought_pos = strcasestr_local(cd.buf, "Thought:");
			if (thought_pos) {
				thought_pos += 8;
				while (*thought_pos == ' ') thought_pos++;
				content = thought_pos;
			}
		}
		response->content = strdup(content);
		if (thought_cb && response->content && *response->content)
			thought_cb(response->content, thought_ud);
	}

	free(cd.buf);
	return 200;
}

static void mock_llm_destroy(struct model *self)
{
	if (self && self->handle) {
		free(self->handle);
		self->handle = NULL;
	}
	free(self);
}

static struct model *create_mock_llm(const char *response)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "mock-model", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	strncpy(m->api_base, "http://localhost:1", sizeof(m->api_base) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = mock_llm_chat;
	m->chat_with_tools = mock_chat_with_tools;
	m->generate = NULL;
	m->destroy = mock_llm_destroy;
	struct mock_llm_data *data = (struct mock_llm_data *)calloc(1, sizeof(*data));
	data->response = response;
	data->call_count = 0;
	data->fail_after = 0;
	data->should_fail = 0;
	data->sleep_ms = 0;
	m->handle = data;
	return m;
}

static struct model *create_mock_streaming_llm(const char *response)
{
	struct model *m = create_mock_llm(response);
	m->chat = mock_llm_streaming_chat;
	return m;
}

/* ---- Multi-response mock: returns different responses per call ---- */

struct multi_mock_data {
	const char **responses;
	int count;
	int call_count;
	int should_fail;
};

static int multi_mock_chat(struct model *self, struct arena *arena,
			   const char *system_prompt,
			   const char **messages, int n,
			   sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	struct multi_mock_data *data = (struct multi_mock_data *)self->handle;
	int idx = data->call_count;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (idx < data->count && cb && data->responses[idx])
		cb(data->responses[idx], user_data);
	return 200;
}

static int multi_mock_chat_with_tools(struct model *self, struct arena *arena,
				      const char *system_prompt,
				      struct chat_message *messages, int msg_count,
				      struct tool_desc *tools, int tool_count,
				      struct chat_response *response,
				      sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;

	struct multi_mock_data *data = (struct multi_mock_data *)self->handle;
	int idx = data->call_count;

	struct mock_collect_data cd = {nullptr, 0, 0};
	cd.buf = (char *)malloc(8192);
	cd.cap = 8192;
	cd.buf[0] = '\0';

	if (data->should_fail) {
		free(cd.buf);
		return -EIO;
	}
	if (idx < data->count && data->responses[idx])
		mock_collect_cb(data->responses[idx], &cd);
	data->call_count++;

	memset(response, 0, sizeof(*response));

	char *action_pos = strcasestr_local(cd.buf, "Action:");
	if (action_pos && tool_count > 0) {
		if (action_pos > cd.buf) {
			size_t tlen = action_pos - cd.buf;
			char *thought = (char *)malloc(tlen + 1);
			memcpy(thought, cd.buf, tlen);
			thought[tlen] = '\0';
			while (tlen > 0 && isspace((unsigned char)thought[tlen-1]))
				thought[--tlen] = '\0';
			char *t = thought;
			if (strncasecmp(t, "Thought:", 8) == 0) {
				t += 8;
				while (*t == ' ') t++;
			}
			if (*t) {
				response->content = strdup(t);
				if (thought_cb)
					thought_cb(t, thought_ud);
			}
			free(thought);
		}

		const char *ap = action_pos + 7;
		while (*ap == ' ') ap++;
		char tool_name[64] = {0};
		int ni = 0;
		while (*ap && *ap != '(' && *ap != '\n' && ni < 63)
			tool_name[ni++] = *ap++;
		char *args = strdup("{}");
		if (*ap == '(') {
			ap++;
			const char *args_start = ap;
			int depth = 1;
			while (*ap && depth > 0) {
				if (*ap == '(') depth++;
				else if (*ap == ')') depth--;
				ap++;
			}
			size_t alen = (size_t)((ap - 1) - args_start);
			free(args);
			args = (char *)malloc(alen + 1);
			memcpy(args, args_start, alen);
			args[alen] = '\0';
		}
		response->tool_calls = (struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		snprintf(response->tool_calls[0].id, sizeof(response->tool_calls[0].id),
			 "call_mock_%d", idx);
		strncpy(response->tool_calls[0].name, tool_name,
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = args;
	} else {
		const char *content = cd.buf;
		char *final_pos = strcasestr_local(cd.buf, "Final:");
		if (final_pos) {
			final_pos += 6;
			while (*final_pos == ' ') final_pos++;
			content = final_pos;
		}
		response->content = strdup(content);
		if (thought_cb && response->content && *response->content)
			thought_cb(response->content, thought_ud);
	}

	free(cd.buf);
	return 200;
}

static void multi_mock_destroy(struct model *self)
{
	if (self && self->handle) {
		free(self->handle);
		self->handle = NULL;
	}
	free(self);
}

static struct model *create_multi_mock_llm(const char **responses, int count)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "mock-model", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	strncpy(m->api_base, "http://localhost:1", sizeof(m->api_base) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = multi_mock_chat;
	m->chat_with_tools = multi_mock_chat_with_tools;
	m->generate = NULL;
	m->destroy = multi_mock_destroy;
	struct multi_mock_data *d = (struct multi_mock_data *)calloc(1, sizeof(*d));
	d->responses = responses;
	d->count = count;
	d->call_count = 0;
	d->should_fail = 0;
	m->handle = d;
	return m;
}

struct slot_mock_data {
	int call_count;
	int last_msg_count;
};

static int slot_mock_chat(struct model *self, struct arena *arena,
			  const char *system_prompt,
			  const char **messages, int n,
			  sse_callback cb, void *user_data)
{
	(void)self;
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	(void)cb;
	(void)user_data;
	return 200;
}

static int slot_mock_chat_with_tools(struct model *self, struct arena *arena,
				     const char *system_prompt,
				     struct chat_message *messages,
				     int msg_count,
				     struct tool_desc *tools, int tool_count,
				     struct chat_response *response,
				     sse_callback thought_cb, void *thought_ud)
{
	struct slot_mock_data *d = (struct slot_mock_data *)self->handle;

	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)tools;
	(void)tool_count;
	memset(response, 0, sizeof(*response));
	d->last_msg_count = msg_count;
	d->call_count++;
	if (d->call_count == 1) {
		if (thought_cb)
			thought_cb("run two tools", thought_ud);
		response->content = strdup("run two tools");
		response->tool_call_count = 2;
		response->tool_calls = (struct tool_call *)calloc(
			2, sizeof(*response->tool_calls));
		if (!response->tool_calls)
			return -ENOMEM;
		snprintf(response->tool_calls[0].id,
			 sizeof(response->tool_calls[0].id), "slot_call_0");
		strncpy(response->tool_calls[0].name, "slot_a",
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = strdup("{\"n\":1}");
		snprintf(response->tool_calls[1].id,
			 sizeof(response->tool_calls[1].id), "slot_call_1");
		strncpy(response->tool_calls[1].name, "slot_b",
			sizeof(response->tool_calls[1].name) - 1);
		response->tool_calls[1].arguments = strdup("{\"n\":2}");
		return 200;
	}
	response->content = strdup("done with both tools");
	if (thought_cb)
		thought_cb(response->content, thought_ud);
	return 200;
}

static void slot_mock_destroy(struct model *self)
{
	if (!self)
		return;
	free(self->handle);
	free(self);
}

static struct model *create_slot_mock_llm(struct slot_mock_data **out)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	struct slot_mock_data *d;

	if (!m)
		return NULL;
	d = (struct slot_mock_data *)calloc(1, sizeof(*d));
	if (!d) {
		free(m);
		return NULL;
	}
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "slot-mock", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = slot_mock_chat;
	m->chat_with_tools = slot_mock_chat_with_tools;
	m->destroy = slot_mock_destroy;
	m->handle = d;
	if (out)
		*out = d;
	return m;
}

/* ---- basic fixture ---- */

class ReactTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	struct arena *ar;
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
		ar = arena_create(0);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
		arena_destroy(ar);
		tool_registry_cleanup(&tools);
	}
};

/* ---- mock LLM fixture ---- */

class MockLlmTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	struct model *llm;
	struct mock_llm_data *llm_data;
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
		llm = NULL;
		llm_data = NULL;
	}
	void TearDown() override {
		if (llm) model_destroy(llm);
		tokenizer_destroy(tok);
		tool_registry_cleanup(&tools);
	}
	void setup_llm_with_response(const char *response) {
		llm = create_mock_llm(response);
		llm_data = (struct mock_llm_data *)llm->handle;
	}
	void setup_streaming_llm_with_response(const char *response) {
		llm = create_mock_streaming_llm(response);
		llm_data = (struct mock_llm_data *)llm->handle;
	}
};

/* ============================================= */
/* Basic React tests                             */
/* ============================================= */

TEST_F(ReactTest, CreateDestroy) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_NONE);
	EXPECT_EQ(ctx->last_error_code, 0);
	EXPECT_EQ(ctx->max_iterations, 10);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, DefaultTimeoutAndRetries) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->step_timeout_seconds, 330);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, Reset) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->state = REACT_STATE_THINKING;
	react_reset(ctx);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_NONE);
	EXPECT_EQ(ctx->last_error_code, 0);
	EXPECT_EQ(ctx->steps, nullptr);
	EXPECT_EQ(ctx->step_count, 0);
	EXPECT_EQ(ctx->cancelled, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelAndAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelFn) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->cancelled, 0);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_cancel(nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CreateStep) {
	struct arena *arena = arena_create(1024);
	struct react_step *s = react_step_create(arena, REACT_STEP_THOUGHT, "thinking", nullptr, nullptr, nullptr);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_THOUGHT);
	EXPECT_STREQ(s->content, "thinking");
	arena_destroy(arena);
}

TEST_F(ReactTest, CreateStepWithTool) {
	struct arena *arena = arena_create(1024);
	struct react_step *s = react_step_create(arena, REACT_STEP_ACTION, "calling tool",
						 "text_gen", "{\"prompt\":\"hi\"}", nullptr);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_ACTION);
	EXPECT_STREQ(s->tool_name, "text_gen");
	EXPECT_STREQ(s->tool_args, "{\"prompt\":\"hi\"}");
	arena_destroy(arena);
}

TEST_F(ReactTest, StepNames) {
	EXPECT_STREQ(react_step_type_name(REACT_STEP_THOUGHT), "Thought");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_ACTION), "Action");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_OBSERVATION), "Observation");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_FINAL), "Final");
}

TEST_F(ReactTest, StateNames) {
	EXPECT_STREQ(react_state_name(REACT_STATE_INIT), "INIT");
	EXPECT_STREQ(react_state_name(REACT_STATE_THINKING), "THINKING");
	EXPECT_STREQ(react_state_name(REACT_STATE_DONE), "DONE");
	EXPECT_STREQ(react_state_name(REACT_STATE_ABORT), "ABORT");
	EXPECT_STREQ(react_state_name(REACT_STATE_TOOL_FAIL), "TOOL_FAIL");
	EXPECT_STREQ(react_state_name(REACT_STATE_ACTING), "ACTING");
	EXPECT_STREQ(react_state_name(REACT_STATE_OBSERVING), "OBSERVING");
}

TEST_F(ReactTest, OutcomeNames) {
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_NONE), "none");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_SUCCESS), "success");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_CANCELLED), "cancelled");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_TIMEOUT), "timeout");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_MAX_ITERATIONS),
		     "max_iterations");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_LLM_ERROR),
		     "llm_error");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_TOOL_ERROR),
		     "tool_error");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_GUARDRAIL_DENIED),
		     "guardrail_denied");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_INTERNAL_ERROR),
		     "internal_error");
}

TEST_F(ReactTest, DestroyNull) {
	EXPECT_NO_FATAL_FAILURE(react_context_destroy(nullptr));
}

TEST_F(ReactTest, ResetNull) {
	EXPECT_NO_FATAL_FAILURE(react_reset(nullptr));
}

TEST_F(ReactTest, StepDestroyNull) {
	EXPECT_NO_FATAL_FAILURE(react_step_destroy(nullptr));
}

TEST_F(ReactTest, ToolRegistryIntegration) {
	tool_register(&tools, "text_gen", "Generate text",
		      "{\"type\":\"object\"}", test_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tools->count, 1);
	struct tool_entry *e = tool_lookup(ctx->tools, "text_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "text_gen");
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunBasic) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, "hello world", nullptr, nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunNullInput) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, nullptr, nullptr, nullptr);
	EXPECT_NE(rc, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, NullContext) {
	EXPECT_NE(react_context_create(nullptr, tok, &cfg, nullptr), nullptr);
}

TEST_F(ReactTest, RunWithCallback) {
	static int callback_count = 0;
	callback_count = 0;
	auto cb = [](enum react_step_type type, const char *content, void *ud) -> int {
		callback_count++;
		return 0;
	};
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	react_run(ctx, "test input", cb, nullptr);
	react_context_destroy(ctx);
	EXPECT_GT(callback_count, 0);
}

TEST_F(ReactTest, MaxIterationsAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->max_iterations = 1;
	react_run(ctx, "test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, ToolFailThresholdThree) {
	tool_register(&tools, "failing_tool", "Always fails",
		      "{}", failing_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Mock LLM tests: full ReAct cycle              */
/* ============================================= */

TEST_F(MockLlmTest, LlmFinalDirectly) {
	setup_llm_with_response("Final: Hello, I can help with that.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "Hello"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmThoughtAndFinal) {
	setup_llm_with_response("Thought: Let me think.\nFinal: Here is my answer.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "what is 2+2?", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "Here is my answer"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmActionToolCall) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: Using test tool.\nAction: test_tool({\"prompt\":\"hi\"})\n\nFinal: Done.");
	struct react_context *ctx2 = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx2, nullptr);
	ctx2->llm_model = llm;
	ctx2->max_iterations = 5;
	int cb_count = 0;
	auto cb = [](enum react_step_type type, const char *content, void *ud) -> int {
		int *n = (int *)ud;
		(*n)++;
		return 0;
	};
	react_run(ctx2, "use the test tool", cb, &cb_count);
	EXPECT_TRUE(ctx2->state == REACT_STATE_DONE ||
		    ctx2->state == REACT_STATE_ABORT);
	react_context_destroy(ctx2);
}

TEST_F(MockLlmTest, LlmStreamingFinal) {
	setup_streaming_llm_with_response("Final: streamed answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "stream test", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmToolFailRetries) {
	int call_count = 0;
	tool_register(&tools, "fail_tool", "Fails every time", "{}",
		      failing_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: try fail_tool.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "call failing tool", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_DONE ||
		    ctx->state == REACT_STATE_ABORT);
	EXPECT_GE(ctx->step_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmToolFailMaxRetries) {
	tool_register(&tools, "fail_tool", "Fails every time", "{}",
		      failing_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: try again.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 10;
	ctx->tool_max_retries = 3;
	react_run(ctx, "keep calling failing tool", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "failed"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmCallCount) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple question", nullptr, nullptr);
	EXPECT_EQ(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmMultiStepCallCount) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	setup_llm_with_response(
		"Thought: Step 1.\nAction: test_tool({\"prompt\":\"hi\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "multi-step question", nullptr, nullptr);
	EXPECT_GE(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

/* ---- Multi-response termination tests ---- */

TEST_F(MockLlmTest, ActionThenFinal) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Using tool.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Tool done.\nFinal: The answer is here."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "answer is here") != nullptr);
	react_context_destroy(ctx);
}

static bool event_recorder_has_name(struct morph_event_recorder *rec,
				    const char *name)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

static int event_recorder_count_name(struct morph_event_recorder *rec,
				     const char *name)
{
	int count = 0;

	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		if (cJSON_IsString(name_item) &&
		    strcmp(name_item->valuestring, name) == 0)
			count++;
		cJSON_Delete(root);
	}
	return count;
}

static bool event_recorder_has_outcome(struct morph_event_recorder *rec,
				       const char *name,
				       const char *outcome)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *outcome_item = data ?
			cJSON_GetObjectItem(data, "outcome") : nullptr;
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0 &&
			cJSON_IsString(outcome_item) &&
			strcmp(outcome_item->valuestring, outcome) == 0;
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

TEST_F(MockLlmTest, EmitsStructuredToolEvents) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Using tool.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Tool done.\nFinal: The answer is here."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.turn.begin"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.thought.delta"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.action"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.call"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.running"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.result"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.turn.end"));
	EXPECT_TRUE(event_recorder_has_outcome(&rec, "react.turn.end",
					       "success"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsStructuredArtifactEvents) {
	tool_register(&tools, "artifact_tool", "Returns artifact", "{}",
		      artifact_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Creating artifact.\nAction: artifact_tool({})\n",
		"Thought: Done.\nFinal: Artifact is ready."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "make an artifact", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_has_name(&rec, "artifact.ready"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultipleToolCallsUseSlotArray) {
	int slot_a_count = 0;
	int slot_b_count = 0;
	struct slot_mock_data *slot_data = nullptr;

	tool_register(&tools, "slot_a", "Slot A", "{}",
		      call_count_tool_fn, &slot_a_count, nullptr);
	tool_register(&tools, "slot_b", "Slot B", "{}",
		      call_count_tool_fn, &slot_b_count, nullptr);
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run both", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_EQ(slot_a_count, 1);
	EXPECT_EQ(slot_b_count, 1);
	ASSERT_NE(slot_data, nullptr);
	EXPECT_EQ(slot_data->call_count, 2);
	EXPECT_EQ(event_recorder_count_name(&rec, "tool.result"), 2);
	EXPECT_EQ(event_recorder_count_name(&rec, "tool.running"), 2);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "both tools"), nullptr);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MessageArrayGrowsBeyondInitialCapacity) {
	struct slot_mock_data *slot_data = nullptr;
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 1;

	for (int i = 0; i < 70; i++) {
		char content[64];
		snprintf(content, sizeof(content), "history %d", i);
		msg_list_append(&ctx->messages,
				msg_list_create(ctx->session_arena,
						i % 2 == 0 ? "user" :
						"assistant",
						content, 1));
	}

	int rc = react_run(ctx, "final prompt", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(slot_data, nullptr);
	EXPECT_EQ(slot_data->last_msg_count, 71);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionActionThenFinal) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Step 1.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Step 2.\nAction: test_tool({\"p\":\"2\"})\n",
		"Thought: Done.\nFinal: Final result after two steps."
	};
	llm = create_multi_mock_llm(responses, 3);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "multi step", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "after two steps") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionNeverFinal) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Step 1.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Step 2.\nAction: test_tool({\"p\":\"2\"})\n",
		"Thought: Step 3.\nAction: test_tool({\"p\":\"3\"})\n",
		"Thought: Step 4.\nAction: test_tool({\"p\":\"4\"})\n",
		"Thought: Step 5.\nAction: test_tool({\"p\":\"5\"})\n",
	};
	llm = create_multi_mock_llm(responses, 5);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 3;
	int rc = react_run(ctx, "never final", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_REACT_MAX_ITERATIONS);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_MAX_ITERATIONS);
	EXPECT_EQ(ctx->last_error_code, MORPH_ERR_REACT_MAX_ITERATIONS);
	EXPECT_STREQ(ctx->outcome_reason, "max_iterations");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolFailThenFinal) {
	tool_register(&tools, "fail_tool", "Fails always", "{}",
		      failing_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Try failing tool.\nAction: fail_tool({\"q\":\"x\"})\n",
		"Thought: It failed, giving up.\nFinal: Sorry, tool failed."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "call tool", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "failed") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmFailureReturnsAbort) {
	llm = create_mock_llm("should not matter");
	llm_data = (struct mock_llm_data *)llm->handle;
	llm_data->should_fail = 1;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "trigger LLM failure", nullptr, nullptr);
	EXPECT_EQ(rc, -EIO);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_LLM_ERROR);
	EXPECT_EQ(ctx->last_error_code, -EIO);
	EXPECT_STREQ(ctx->outcome_reason, "llm_error");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StepTimeoutReturnsDistinctOutcome) {
	setup_llm_with_response("Final: too slow");
	ASSERT_NE(llm_data, nullptr);
	llm_data->sleep_ms = 1100;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->step_timeout_seconds = 1;
	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "slow response", nullptr, nullptr);
	EXPECT_EQ(rc, -ETIMEDOUT);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_TIMEOUT);
	EXPECT_EQ(ctx->last_error_code, -ETIMEDOUT);
	EXPECT_STREQ(ctx->outcome_reason, "step_timeout");
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.timed_out"));
	EXPECT_TRUE(event_recorder_has_outcome(&rec, "react.turn.end",
					       "timeout"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StepCountAfterFinal) {
	setup_llm_with_response("Final: quick answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple", nullptr, nullptr);
	EXPECT_GE(ctx->step_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CallbackReceivesFinalStep) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple question", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "direct answer"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CancelDuringRun) {
	setup_llm_with_response("Thought: thinking...\nFinal: answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_cancel(ctx);
	react_run(ctx, "cancelled test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

static int drain_cancel_once(void *user_data, struct react_action *out,
			     int block)
{
	int *count = (int *)user_data;

	(void)block;
	if (*count > 0)
		return 0;
	(*count)++;
	out->type = "cancel";
	out->payload_json = NULL;
	return 1;
}

TEST_F(MockLlmTest, ActionDrainCancelBeforeLlmCall) {
	setup_llm_with_response("Final: should not be called");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int drain_count = 0;
	ASSERT_EQ(react_set_action_drain(ctx, drain_cancel_once,
					 &drain_count), 0);

	int rc = react_run(ctx, "cancel through drain", nullptr, nullptr);
	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_CANCELLED);
	EXPECT_EQ(llm_data->call_count, 0);
	EXPECT_EQ(drain_count, 1);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ModelTimeoutField) {
	llm = create_mock_llm("Final: test");
	ASSERT_NE(llm, nullptr);
	EXPECT_EQ(llm->timeout_seconds, 60);
	llm->timeout_seconds = 30;
	EXPECT_EQ(llm->timeout_seconds, 30);
	model_destroy(llm);
	llm = nullptr;
}

TEST_F(MockLlmTest, ConfigurableMaxIterations) {
	setup_llm_with_response("Thought: looping.\nAction: test_tool({})");
	tool_register(&tools, "test_tool", "Test", "{}", test_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 3;
	react_run(ctx, "loop test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE ||
		    ctx->state == REACT_STATE_DONE);
	EXPECT_LE(ctx->step_count, 3 * 4 + 2);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolCallCount) {
	int call_count = 0;
	tool_register(&tools, "counter", "Counts calls", "{}",
		      call_count_tool_fn, &call_count, nullptr);
	setup_llm_with_response("Thought: call counter.\nAction: counter({})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "count calls", nullptr, nullptr);
	EXPECT_GE(call_count, 1);
	react_context_destroy(ctx);
}

/* ============================================= */
/* HTTP timeout tests                             */
/* ============================================= */

class HttpTimeoutTest : public ::testing::Test {
protected:
	void SetUp() override { http_init(); }
	void TearDown() override { http_cleanup(); }
};

TEST_F(HttpTimeoutTest, TimeoutFnNullUrl) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout(nullptr, "", 0,
					  "application/json", nullptr, 0, 10,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

TEST_F(HttpTimeoutTest, TimeoutFnNullCallback) {
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test", "", 0,
					  "application/json", nullptr, 0, 10,
					  nullptr, nullptr);
	EXPECT_NE(rc, 0);
}

TEST_F(HttpTimeoutTest, TimeoutFnConnectRefused) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test",
					  "{}", 2,
					  "application/json", nullptr, 0, 2,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

static int global_global_sse_write_adapter(const char *data, size_t len, void *user_data) {
	struct sse_parser *parser = (struct sse_parser *)user_data;
	sse_parser_feed(parser, data, len);
	return 0;
}

TEST_F(HttpTimeoutTest, ZeroTimeoutUsesDefault) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test",
					  "{}", 2,
					  "application/json", nullptr, 0, 0,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

/* ============================================= */
/* Mock HTTP server for integration tests         */
/* ============================================= */

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

struct mock_server {
	int port;
	int server_fd;
	pthread_t thread;
	int thread_started;
	volatile int running;
	const char *response_body;
	int response_status;
	int request_count;
	char request_method[16];
	char last_request[8192];
};

static void *mock_http_server_thread(void *arg)
{
	struct mock_server *srv = (struct mock_server *)arg;
	while (srv->running) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(srv->server_fd,
				       (struct sockaddr *)&client_addr,
				       &client_len);
		if (client_fd < 0)
			continue;
		char buf[8192];
		ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
		if (n <= 0) {
			close(client_fd);
			continue;
		}
		buf[n] = '\0';
		srv->request_count++;
		snprintf(srv->last_request, sizeof(srv->last_request), "%s", buf);
		sscanf(buf, "%15s", srv->request_method);
		int is_sse = (strstr(buf, "Accept: text/event-stream") != NULL ||
			      strstr(buf, "text/event-stream") != NULL);
		if (srv->response_body && is_sse) {
			const char *data = srv->response_body;
			char header[512];
			int hlen = snprintf(header, sizeof(header),
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/event-stream\r\n"
				"Cache-Control: no-cache\r\n"
				"Connection: close\r\n"
				"\r\n");
			send(client_fd, header, hlen, 0);
			char event_buf[2048];
			int elen = snprintf(event_buf, sizeof(event_buf),
				"data: %s\n\n", data);
			send(client_fd, event_buf, elen, 0);
			const char *done = "data: [DONE]\n\n";
			send(client_fd, done, strlen(done), 0);
		} else if (srv->response_body) {
			int body_len = strlen(srv->response_body);
			char header[512];
			int hlen = snprintf(header, sizeof(header),
				"HTTP/1.1 %d OK\r\n"
				"Content-Type: application/json\r\n"
				"Content-Length: %d\r\n"
				"Connection: close\r\n"
				"\r\n",
				srv->response_status > 0 ? srv->response_status : 200,
				body_len);
			send(client_fd, header, hlen, 0);
			send(client_fd, srv->response_body, body_len, 0);
		} else {
			const char *resp_404 = "HTTP/1.1 404 Not Found\r\n"
					       "Content-Length: 0\r\n"
					       "Connection: close\r\n"
					       "\r\n";
			send(client_fd, resp_404, strlen(resp_404), 0);
		}
		close(client_fd);
	}
	return nullptr;
}

static int mock_server_start(struct mock_server *srv, int suggested_port)
{
	const char *saved_body = srv->response_body;
	int saved_status = srv->response_status;
	memset(srv, 0, sizeof(*srv));
	srv->response_body = saved_body;
	srv->response_status = saved_status;
	srv->server_fd = -1;
	srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->server_fd < 0)
		return -1;
	int opt = 1;
	setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(suggested_port);
	if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	if (listen(srv->server_fd, 5) < 0) {
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	socklen_t addr_len = sizeof(addr);
	getsockname(srv->server_fd, (struct sockaddr *)&addr, &addr_len);
	srv->port = ntohs(addr.sin_port);
	srv->running = 1;
	if (pthread_create(&srv->thread, nullptr, mock_http_server_thread, srv) != 0) {
		srv->running = 0;
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	srv->thread_started = 1;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return 0;
}

static void mock_server_stop(struct mock_server *srv)
{
	srv->running = 0;
	if (srv->server_fd >= 0) {
		shutdown(srv->server_fd, SHUT_RDWR);
		close(srv->server_fd);
		srv->server_fd = -1;
	}
	if (srv->thread_started) {
		pthread_join(srv->thread, nullptr);
		srv->thread_started = 0;
	}
}

/* ---- Integration tests with mock server ---- */

class MockServerTest : public ::testing::Test {
protected:
	struct mock_server srv;
	void SetUp() override {
		memset(&srv, 0, sizeof(srv));
		srv.server_fd = -1;
		http_init();
	}
	void TearDown() override {
		if (srv.running || srv.thread_started || srv.server_fd >= 0)
			mock_server_stop(&srv);
		http_cleanup();
	}
};

#define START_MOCK_OR_SKIP(srv)						\
	do {								\
		if (mock_server_start((srv), 0) != 0)			\
			GTEST_SKIP() << "local HTTP mock server unavailable";	\
	} while (0)

TEST_F(MockServerTest, HttpGetSuccess) {
	srv.response_body = "{\"status\":\"ok\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp3 = {0};
	char url4[256];
	snprintf(url4, sizeof(url4), "http://127.0.0.1:%d/test", srv.port);
	int rc4 = http_get(url4, &resp3);
	EXPECT_EQ(rc4, 0);
	EXPECT_EQ(resp3.status_code, 200);
	EXPECT_NE(resp3.body.data, nullptr);
	EXPECT_TRUE(resp3.body.data && strstr(resp3.body.data, "ok"));
	http_response_free(&resp3);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostSuccess) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/test", srv.port);
	int rc = http_post(url, "{\"data\":1}", 10,
			   "application/json", &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	EXPECT_NE(resp.body.data, nullptr);
	EXPECT_TRUE(resp.body.data && strstr(resp.body.data, "posted"));
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostEmptyBodyUsesPost) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/empty", srv.port);
	int rc = http_post(url, "", 0, "application/json", &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostExAddsExtraHeadersAndClearsResponse) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	resp.status_code = 999;
	resp.body.len = 123;
	resp.headers.len = 456;
	const char *headers[] = { "X-Morph-Test: yes" };
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/headers", srv.port);
	int rc = http_post_ex(url, "{}", 2, "application/json",
			      headers, 1, &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	EXPECT_NE(strstr(srv.last_request, "X-Morph-Test: yes"), nullptr);
	EXPECT_GT(resp.body.len, 0u);
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEStreamingResponse) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct sse_test_info info = {0, ""};
	auto sse_cb = [](const char *event, const char *data, void *ud) -> int {
		auto *i = (struct sse_test_info *)ud;
		i->count++;
		if (data) i->last_data = data;
		return 0;
	};
	struct sse_parser parser;
	sse_parser_init(&parser, sse_cb, &info);
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc = http_post_sse_ex(url, "{}", 2,
				   "application/json", nullptr, 0,
				   global_sse_write_adapter, &parser);
	EXPECT_EQ(rc, 200);
	EXPECT_GT(info.count, 0);
	EXPECT_STREQ(srv.request_method, "POST");
	sse_parser_free(&parser);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEWithTimeout) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"test\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct sse_parser parser2;
	sse_parser_init(&parser2, nullptr, nullptr);
	char url3[256];
	snprintf(url3, sizeof(url3), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc3 = http_post_sse_ex_timeout(url3, "{}", 2,
					   "application/json", nullptr, 0, 30,
					   global_sse_write_adapter, &parser2);
	EXPECT_EQ(rc3, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	sse_parser_free(&parser2);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmStreamsReasoningWithoutAccumulatingIt) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);

	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);

	struct chat_message msg = {
		(char *)"user",
		(char *)"hello",
		NULL,
		NULL,
		0,
	};
	struct chat_response response;
	std::string streamed;

	int rc = model->chat_with_tools_stream(
		model, arena, NULL, &msg, 1, NULL, 0, &response,
		[](enum llm_stream_kind kind, const char *token, void *ud) -> int {
			auto *out = static_cast<std::string *>(ud);
			if (kind == LLM_STREAM_REASONING)
				out->append("R:");
			else
				out->append("C:");
			if (token)
				out->append(token);
			return 0;
		},
		&streamed);
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(streamed, "R:thinkC:answer");
	ASSERT_NE(response.content, nullptr);
	EXPECT_STREQ(response.content, "answer");

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSECallbackErrorPropagates) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"test\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	auto failing_cb = [](const char *data, size_t len, void *ud) -> int {
		(void)data;
		(void)len;
		(void)ud;
		return -EIO;
	};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc = http_post_sse_ex(url, "{}", 2,
				  "application/json", nullptr, 0,
				  failing_cb, nullptr);
	EXPECT_EQ(rc, -EIO);
	mock_server_stop(&srv);
}

/* ============================================= */
/* System prompt tests                           */
/* ============================================= */

TEST_F(ReactTest, SystemPromptCreateDestroy) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->system_prompt = strdup("Be creative and concise.");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptNoCrash) {
	setup_llm_with_response("Final: done");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->system_prompt = strdup("Always rhyme.");
	react_run(ctx, "say something", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	react_context_destroy(ctx);
}

struct capt_prompt_data {
	char *prompt;
	char *system_prompt;
	const char *resp;
};

static int capt_prompt_chat(struct model *self, struct arena *arena,
			    const char *system_prompt,
			    const char **messages, int n,
			    sse_callback cb, void *user_data)
{
	(void)arena;
	(void)user_data;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->prompt);
	d->prompt = (n > 0 && messages[0]) ? strdup(messages[0]) : nullptr;
	free(d->system_prompt);
	d->system_prompt = system_prompt ? strdup(system_prompt) : nullptr;
	if (cb && d->resp)
		cb(d->resp, user_data);
	return 200;
}

static int capt_prompt_chat_with_tools(struct model *self, struct arena *arena,
				       const char *system_prompt,
				       struct chat_message *messages, int msg_count,
				       struct tool_desc *tools, int tool_count,
				       struct chat_response *response,
				       sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->system_prompt);
	d->system_prompt = system_prompt ? strdup(system_prompt) : nullptr;

	memset(response, 0, sizeof(*response));
	const char *content = d->resp ? d->resp : "";
	char *final_pos = strcasestr_local((char *)content, "Final:");
	if (final_pos) {
		final_pos += 6;
		while (*final_pos == ' ') final_pos++;
		content = final_pos;
	}
	response->content = strdup(content);
	if (thought_cb && response->content)
		thought_cb(response->content, thought_ud);
	return 200;
}

static void capt_prompt_destroy(struct model *self)
{
	if (!self) return;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->prompt);
	free(d->system_prompt);
	free(d);
	free(self);
}

TEST_F(MockLlmTest, SystemPromptAppearsInPrompt) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->system_prompt = strdup("Custom instruction here.");
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	const char *found = nullptr;
	if (cd->system_prompt)
		found = strstr(cd->system_prompt, "Custom instruction here.");
	EXPECT_NE(found, nullptr) << "system_prompt should appear in the LLM prompt";
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptRequiresMarkdownLinksForUrls) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Format web URLs as Markdown links"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "do not leave bare http(s) URLs in final answers"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptIncludesMarkdownOutputRules) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "MARKDOWN OUTPUT"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Do not wrap the entire response in a code block"),
		  nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Use GitHub Flavored Markdown tables"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Chinese prose may use normal Chinese punctuation"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MemoryContextAppearsInPrompt) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ASSERT_EQ(react_set_memory_context(ctx,
			"Persistent memory for this session.\n- Preferred language: Chinese\n"),
		  0);
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "Preferred language: Chinese"), nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Multi-turn conversation (message accumulation)*/
/* ============================================= */

TEST_F(MockLlmTest, MultiTurnMessageAccumulation) {
	setup_llm_with_response("Final: ok");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "turn 1", nullptr, nullptr);
	int after_first = msg_list_count(ctx->messages);
	EXPECT_EQ(after_first, 2);

	react_run(ctx, "turn 2", nullptr, nullptr);
	int after_second = msg_list_count(ctx->messages);
	EXPECT_EQ(after_second, 4);

	react_run(ctx, "turn 3", nullptr, nullptr);
	int after_third = msg_list_count(ctx->messages);
	EXPECT_EQ(after_third, 6);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultiTurnFinalAnswerUpdated) {
	setup_llm_with_response("Final: first answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "first", nullptr, nullptr);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "first answer") != nullptr);

	llm_data->response = "Final: second answer";
	react_run(ctx, "second", nullptr, nullptr);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "second answer") != nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultiTurnMessageRolesAlternate) {
	setup_llm_with_response("Final: answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "hello", nullptr, nullptr);
	struct message_list *cur = ctx->messages;
	EXPECT_STREQ(cur->role, "user");
	EXPECT_STREQ(cur->content, "hello");
	cur = cur->next;
	ASSERT_NE(cur, nullptr);
	EXPECT_STREQ(cur->role, "assistant");
	EXPECT_TRUE(strstr(cur->content, "answer") != nullptr);

	react_context_destroy(ctx);
}

/* ============================================= */
/* Compression integration within react_run      */
/* ============================================= */

static int test_compress_cb(const char *text, void *user_data, char **out)
{
	(void)text;
	(void)user_data;
	*out = strdup("[COMPRESSED SUMMARY]");
	return *out ? 0 : -ENOMEM;
}

TEST_F(MockLlmTest, CompressTriggerInReact) {
	cfg.max_context_tokens = 6;
	cfg.summarize_threshold_ratio = 0.5;

	setup_llm_with_response("Final: after compress");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->compress.summarize = test_compress_cb;
	ctx->compress.summarize_user_data = nullptr;
	ctx->compress.max_history_rounds = 1;

	react_run(ctx, "first", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	react_run(ctx, "second", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CompressPreservesAfterFlow) {
	cfg.max_context_tokens = 6;
	cfg.summarize_threshold_ratio = 0.5;

	setup_llm_with_response("Final: ok");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->compress.summarize = test_compress_cb;
	ctx->compress.summarize_user_data = nullptr;
	ctx->compress.max_history_rounds = 1;

	for (int i = 0; i < 4; i++) {
		char input[32];
		snprintf(input, sizeof(input), "run %d", i);
		react_run(ctx, input, nullptr, nullptr);
		EXPECT_EQ(ctx->state, REACT_STATE_DONE);
		EXPECT_NE(ctx->final_answer, nullptr);
	}
	react_context_destroy(ctx);
}

/* ============================================= */
/* Tool execution edge cases                     */
/* ============================================= */

TEST_F(MockLlmTest, ActionToolNotFound) {
	tool_register(&tools, "good_tool", "Works", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: try bad tool.\nAction: bad_tool({\"x\":1})\n",
		"Thought: it failed.\nFinal: tool not available"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "not available") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionInvalidFormat) {
	tool_register(&tools, "test_tool", "Test", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: bad format.\nAction: test_tool({\"x\")\n",
		"Thought: adjusting.\nFinal: recovered"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "recovered") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionNoToolsRegistered) {
	setup_llm_with_response("Action: some_tool({})\n");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

static int null_result_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	tool_result_clear(result);
	return 0;
}

TEST_F(MockLlmTest, ToolNullResult) {
	tool_register(&tools, "null_tool", "Returns null result", "{}",
		      null_result_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: calling null tool.\nAction: null_tool({})\n",
		"Thought: got null.\nFinal: handled null result"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "handled null result") != nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* LLM response edge cases                       */
/* ============================================= */

TEST_F(MockLlmTest, EmptyLlmResponse) {
	setup_llm_with_response("");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ResponseOnlyFinalPrefix) {
	setup_llm_with_response("Final:");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_STREQ(ctx->final_answer, "");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ThoughtOnly) {
	setup_llm_with_response("Thought: just thinking\n");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Cancellation during tool execution            */
/* ============================================= */

static int self_cancel_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	struct react_context *ctx = (struct react_context *)user_data;
	ctx->cancelled = 1;
	(void)tool_result_take_text(result, strdup("{\"cancelled\":true}"));
	return 0;
}

TEST_F(MockLlmTest, CancelDuringToolExecution) {
	const char *responses[] = {
		"Thought: run self-cancel tool.\nAction: self_cancel({})\n",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	tool_register(&tools, "self_cancel", "Cancels context", "{}",
		      self_cancel_tool_fn, ctx, nullptr);
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "cancel me", nullptr, nullptr);
	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_CANCELLED);
	EXPECT_EQ(ctx->last_error_code, -ECANCELED);
	EXPECT_STREQ(ctx->outcome_reason, "user_cancelled");
	react_context_destroy(ctx);
}

/* ============================================= */
/* Step list traversal                           */
/* ============================================= */

TEST_F(MockLlmTest, StepLinkedListTraversal) {
	const char *responses[] = {
		"Thought: step 1.\nAction: counter({})\n",
		"Thought: step 2.\nFinal: done after two steps"
	};
	int call_count = 0;
	tool_register(&tools, "counter", "Counts", "{}",
		      call_count_tool_fn, &call_count, nullptr);
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "multi-step flow", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_GE(call_count, 1);
	ASSERT_GE(ctx->step_count, 3);
	struct react_step *s = ctx->steps;
	int type_order[] = {REACT_STEP_THOUGHT, REACT_STEP_ACTION,
			    REACT_STEP_OBSERVATION, REACT_STEP_THOUGHT,
			    REACT_STEP_FINAL};
	int idx = 0;
	while (s && idx < 5) {
		EXPECT_EQ(s->type, type_order[idx]);
		s = s->next;
		idx++;
	}
	react_context_destroy(ctx);
}

/* ============================================= */
/* Context reuse after complex interaction       */
/* ============================================= */

TEST_F(MockLlmTest, ReuseContextAfterDone) {
	setup_llm_with_response("Final: first");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "first input", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	llm_data->response = "Final: second";
	react_run(ctx, "second input", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "second") != nullptr);

	react_context_destroy(ctx);
}

/* ============================================= */
/* Message list with file paths                  */
/* ============================================= */

TEST_F(ReactTest, MsgListWithFilePaths) {
	struct message_list *m = msg_list_create(ar, "user", "check file", 2);
	ASSERT_NE(m, nullptr);
	m->file_paths = (char **)calloc(2, sizeof(char *));
	m->file_paths[0] = strdup("/tmp/test.txt");
	m->file_count = 1;
	EXPECT_STREQ(m->file_paths[0], "/tmp/test.txt");
	EXPECT_EQ(m->file_count, 1);
}

TEST_F(ReactTest, MsgListCreateNullContent) {
	struct message_list *m = msg_list_create(ar, "user", nullptr, 1);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->content, "");
}

TEST_F(ReactTest, MsgListAppendToNullHead) {
	struct message_list *head = nullptr;
	msg_list_append(nullptr, nullptr);
	EXPECT_NO_FATAL_FAILURE(msg_list_append(&head, nullptr));
	EXPECT_EQ(msg_list_count(head), 0);
}

TEST_F(ReactTest, ContextNeedsCompressExactThreshold) {
	struct compress_config test_cfg = {
		.max_context_tokens = 10,
		.max_history_rounds = 2,
		.summarize_threshold_ratio = 0.5,
	};
	struct message_list *head = nullptr;
	for (int i = 0; i < 5; i++)
		msg_list_append(&head, msg_list_create(ar, "user", "msg", 1));
	int needs = context_needs_compress(head, nullptr, &test_cfg);
	EXPECT_EQ(needs, 1);
}

TEST_F(ReactTest, ContextNeedsCompressBelowThreshold) {
	struct compress_config test_cfg = {
		.max_context_tokens = 100,
		.max_history_rounds = 2,
		.summarize_threshold_ratio = 0.5,
	};
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "hi", 1));
	int needs = context_needs_compress(head, nullptr, &test_cfg);
	EXPECT_EQ(needs, 0);
}

/* ============================================= */
/* Guardrail tests                                */
/* ============================================= */

TEST_F(ReactTest, GuardrailStepTypeName) {
	EXPECT_STREQ(react_step_type_name(REACT_STEP_REFLECTION), "Reflection");
}

TEST_F(ReactTest, GuardrailStateName) {
	EXPECT_STREQ(react_state_name(REACT_STATE_GUARDRAIL), "GUARDRAIL");
}

TEST_F(ReactTest, GuardrailDefaultDisabled) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->guardrail.enabled, 0);
	EXPECT_EQ(ctx->guardrail_retry_count, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, GuardrailEnabledField) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 2;
	ctx->guardrail.max_empty_rounds = 2;
	EXPECT_EQ(ctx->guardrail.enabled, 1);
	EXPECT_EQ(ctx->guardrail.max_retries, 2);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailDisabledNoGuardrailStep) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 0;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	bool has_guardrail = false;
	struct react_step *s = ctx->steps;
	while (s) {
		if (s->type == REACT_STEP_REFLECTION)
			has_guardrail = true;
		s = s->next;
	}
	EXPECT_FALSE(has_guardrail);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailEnabledPassesOnGoodAnswer) {
	const char *responses[] = {
		"Final: the answer with saved: output.png",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailCancelDuringRetry) {
	const char *responses[] = {
		"Final: answer",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 3;
	react_cancel(ctx);
	react_run(ctx, "test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

static enum guardrail_verdict reject_blocked_input(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->user_input && strstr(ctx->user_input, "blocked")) {
		snprintf(reason, cap, "blocked input");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict reject_bad_answer(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->proposed_answer && strstr(ctx->proposed_answer, "bad")) {
		snprintf(reason, cap, "bad answer");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict reject_tool_output(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->tool_result && strstr(ctx->tool_result, "test")) {
		snprintf(reason, cap, "tool output rejected");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

TEST_F(MockLlmTest, InputGuardrailRejectsBeforeLlmCall) {
	setup_llm_with_response("Final: should not be called");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_blocked",
		GUARDRAIL_HOOK_INPUT, GUARDRAIL_RULE_C, reject_blocked_input,
		NULL, NULL, "Use a different request."), 0);

	int rc = react_run(ctx, "this is blocked", nullptr, nullptr);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_GUARDRAIL_DENIED);
	EXPECT_EQ(llm_data->call_count, 0);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "blocked input"), nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, OutputGuardrailRetriesAndFinalizes) {
	const char *responses[] = {
		"Final: bad answer",
		"Final: good answer"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_bad",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C, reject_bad_answer,
		NULL, NULL, "Try again."), 0);

	int rc = react_run(ctx, "answer carefully", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(data->call_count, 2);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "good answer"), nullptr);
	bool saw_reflection = false;
	for (struct react_step *s = ctx->steps; s; s = s->next) {
		if (s->type == REACT_STEP_REFLECTION &&
		    s->content && strstr(s->content, "bad answer"))
			saw_reflection = true;
	}
	EXPECT_TRUE(saw_reflection);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolOutputGuardrailRewritesObservation) {
	tool_register(&tools, "test_tool", "A test tool", "{}",
		      test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: use tool.\nAction: test_tool({})\n",
		"Final: done after guarded observation"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_tool",
		GUARDRAIL_HOOK_TOOL_OUTPUT, GUARDRAIL_RULE_C,
		reject_tool_output, NULL, NULL,
		"Inspect the tool output."), 0);

	int rc = react_run(ctx, "guard tool output", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	bool saw_guarded_observation = false;
	bool saw_reflection = false;
	for (struct react_step *s = ctx->steps; s; s = s->next) {
		if (s->type == REACT_STEP_OBSERVATION &&
		    s->content && strstr(s->content,
					"guardrail: tool output rejected"))
			saw_guarded_observation = true;
		if (s->type == REACT_STEP_REFLECTION &&
		    s->content && strstr(s->content, "tool output rejected"))
			saw_reflection = true;
	}
	EXPECT_TRUE(saw_guarded_observation);
	EXPECT_TRUE(saw_reflection);

	react_context_destroy(ctx);
}

/* ---- HITL (Human-in-the-Loop) tests ---- */

static int hitl_deny_count = 0;

static enum hitl_verdict hitl_deny_callback(const char *tool_name,
					     const char *tool_args,
					     void *user_data)
{
	(void)tool_args;
	(void)user_data;
	hitl_deny_count++;
	if (strcmp(tool_name, "dangerous_tool") == 0)
		return HITL_DENY;
	return HITL_APPROVE;
}

static enum hitl_verdict hitl_always_callback(const char *tool_name,
					      const char *tool_args,
					      void *user_data)
{
	(void)tool_args;
	(void)user_data;
	if (strcmp(tool_name, "dangerous_tool") == 0)
		return HITL_ALWAYS;
	return HITL_APPROVE;
}

static enum hitl_verdict hitl_approve_all_callback(const char *tool_name,
						   const char *tool_args,
						   void *user_data)
{
	(void)tool_name;
	(void)tool_args;
	(void)user_data;
	return HITL_APPROVE;
}

TEST(HitlTest, NeedsApprovalDisabled) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "test_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "test_tool"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalEnabledAllTools) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "test_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "test_tool"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalSpecificTool) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(&reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.tools_count = 1;
	strncpy(ctx->hitl.tools[0], "bash_exec", HITL_TOOL_NAME_MAX - 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalInternalApprovalTool) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(&reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL,
		      NULL);
	struct tool_entry *e = tool_lookup(&reg, "bash_exec");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.tools_count = 2;
	strncpy(ctx->hitl.tools[0], "bash_exec", HITL_TOOL_NAME_MAX - 1);
	strncpy(ctx->hitl.tools[1], "dangerous_tool", HITL_TOOL_NAME_MAX - 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 0);
	EXPECT_EQ(hitl_needs_approval(ctx, "dangerous_tool"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalAutoApproved) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	hitl_add_auto_approved(&ctx->hitl, "bash_exec");
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalReadonlyAutoApprove) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 1;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalReadonlyNoAutoApprove) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, AddAutoApprovedIdempotent) {
	struct hitl_config h;
	memset(&h, 0, sizeof(h));
	h.auto_approved_count = 0;
	hitl_add_auto_approved(&h, "tool_a");
	EXPECT_EQ(h.auto_approved_count, 1);
	hitl_add_auto_approved(&h, "tool_a");
	EXPECT_EQ(h.auto_approved_count, 1);
	hitl_add_auto_approved(&h, "tool_b");
	EXPECT_EQ(h.auto_approved_count, 2);
}

TEST(HitlTest, ConfigDefaultsDisabled) {
	struct config cfg;
	config_set_defaults(&cfg);
	EXPECT_EQ(cfg.react.hitl_enabled, 0);
	EXPECT_EQ(cfg.react.hitl_tools_count, 0);
	EXPECT_EQ(cfg.react.hitl_auto_approve_readonly, 1);
}

TEST(HitlTest, ToolIsReadonly) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	EXPECT_EQ(tool_is_readonly(&reg, "file_read"), 0);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	EXPECT_EQ(tool_is_readonly(&reg, "file_read"), 1);
	EXPECT_EQ(tool_is_readonly(&reg, "nonexistent"), 0);
}

TEST(HitlTest, DenyCallbackPreventsExecution) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(&reg, "safe_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_deny_callback;
	ctx->hitl.approval_user_data = NULL;
	hitl_deny_count = 0;
	enum hitl_verdict v1 = ctx->hitl.approval_cb("dangerous_tool", "{}", NULL);
	EXPECT_EQ(v1, HITL_DENY);
	enum hitl_verdict v2 = ctx->hitl.approval_cb("safe_tool", "{}", NULL);
	EXPECT_EQ(v2, HITL_APPROVE);
	EXPECT_EQ(hitl_deny_count, 2);
	react_context_destroy(ctx);
}

TEST(HitlTest, DenyDuringReactRunSkipsToolExecution) {
	struct tool_registry reg;
	struct tokenizer *tok = tokenizer_create("gpt-4o", 128000);
	struct compress_config ccfg = {0};
	int dangerous_count = 0;
	const char *responses[] = {
		"Thought: try dangerous.\nAction: dangerous_tool({})\n",
		"Final: denied and done"
	};
	struct model *llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;

	tool_registry_init(&reg);
	tool_register(&reg, "dangerous_tool", "desc", "{}",
		      call_count_tool_fn, &dangerous_count, NULL);
	ccfg.max_context_tokens = 128000;
	ccfg.max_history_rounds = 6;
	ccfg.summarize_threshold_ratio = 0.8;
	ccfg.compress_target_ratio = 0.5;
	struct react_context *ctx = react_context_create(&reg, tok, &ccfg,
							NULL);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_deny_callback;
	hitl_deny_count = 0;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run dangerous", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_EQ(dangerous_count, 0);
	EXPECT_EQ(hitl_deny_count, 1);
	EXPECT_EQ(data->call_count, 2);
	EXPECT_TRUE(event_recorder_has_name(&rec, "hitl.request"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "hitl.denied"));
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "denied and done"), nullptr);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
	model_destroy(llm);
	tokenizer_destroy(tok);
	tool_registry_cleanup(&reg);
}

TEST(HitlTest, AlwaysCallbackAutoApproves) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(&reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_always_callback;
	ctx->hitl.approval_user_data = NULL;
	enum hitl_verdict v = ctx->hitl.approval_cb("dangerous_tool", "{}", NULL);
	EXPECT_EQ(v, HITL_ALWAYS);
	hitl_add_auto_approved(&ctx->hitl, "dangerous_tool");
	EXPECT_EQ(hitl_needs_approval(ctx, "dangerous_tool"), 0);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Extensible Guardrail tests                    */
/* ============================================= */

TEST(Guardrail, RegisterBuiltinRules) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	EXPECT_EQ(cfg.rule_count, 5);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "empty_answer"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "consecutive_empty"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "tools_all_failed"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "creative_no_media"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "creative_file_missing"), nullptr);
}

TEST(Guardrail, RuleEnableDisable) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	EXPECT_EQ(guardrail_rule_disable(&cfg, "empty_answer"), 0);
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "empty_answer")->enabled, 0);
	EXPECT_EQ(guardrail_rule_enable(&cfg, "empty_answer"), 0);
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "empty_answer")->enabled, 1);
	EXPECT_EQ(guardrail_rule_disable(&cfg, "nonexistent"), -ENOENT);
}

TEST(Guardrail, EmptyAnswerFail) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "empty_answer");
	arena_destroy(a);
}

TEST(Guardrail, EmptyAnswerPass) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "Hello world";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, CustomCRule) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	auto my_check = [](const struct guardrail_eval_ctx *ctx,
			   char *reason, size_t cap) -> enum guardrail_verdict {
		if (ctx->user_input && strstr(ctx->user_input, "forbidden"))
			{ snprintf(reason, cap, "Forbidden word"); return GUARDRAIL_FAIL; }
		return GUARDRAIL_PASS;
	};
	guardrail_rule_register(&cfg, "no_forbidden", GUARDRAIL_HOOK_INPUT,
		GUARDRAIL_RULE_C, my_check, NULL, NULL, "Do not use forbidden words.");
	EXPECT_EQ(cfg.rule_count, 6);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.user_input = "this is forbidden text";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "no_forbidden");
	arena_destroy(a);
}

TEST(Guardrail, DisabledConfigPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 0;
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, DuplicateNameRejected) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	int rc = guardrail_rule_register(&cfg, "empty_answer",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C, NULL, NULL, NULL, NULL);
	EXPECT_EQ(rc, -EEXIST);
}

TEST(Guardrail, ConsecutiveEmptyFail) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	guardrail_rule_disable(&cfg, "empty_answer");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.empty_round_count = 3;
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "consecutive_empty");
	arena_destroy(a);
}

TEST(Guardrail, RuleLookupNull) {
	EXPECT_EQ(guardrail_rule_lookup(NULL, "test"), nullptr);
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "test"), nullptr);
}

TEST(Guardrail, HookIsolation) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, LlmRuleNoModelPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.llm = NULL;
	guardrail_rule_register(&cfg, "llm_check", GUARDRAIL_HOOK_INPUT,
		GUARDRAIL_RULE_LLM, NULL, "Check for bad content", NULL, "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.user_input = "bad stuff";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, ExtRuleNoEntryPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_rule_register(&cfg, "ext_check", GUARDRAIL_HOOK_OUTPUT,
		GUARDRAIL_RULE_EXT, NULL, NULL, "", "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "answer";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, SetLlm) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_set_llm(&cfg, NULL);
	EXPECT_EQ(cfg.llm, nullptr);
	struct model m;
	memset(&m, 0, sizeof(m));
	guardrail_set_llm(&cfg, &m);
	EXPECT_EQ(cfg.llm, &m);
}

TEST(Guardrail, BuiltinRulesAutoRegistered) {
	struct guardrail_config gcfg;
	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.enabled = 1;
	struct compress_config ccfg = {0};
	struct react_context *ctx = react_context_create(nullptr, nullptr, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->guardrail.rule_count, 5);
	EXPECT_NE(guardrail_rule_lookup(&ctx->guardrail, "empty_answer"), nullptr);
	react_context_destroy(ctx);
}

static std::string build_agent_ui_tags_so()
{
	std::string src = std::string(MORPH_TEST_SOURCE_DIR) +
		"/exts/guardrail-agent-ui-tags/agent_ui_tags.c";
	std::string out = "/tmp/morph_agent_ui_tags_" +
		std::to_string((long long)getpid()) + ".so";
	std::string cmd = "cc -shared -fPIC -o " + out + " " + src;
	int status = system(cmd.c_str());
	if (status != 0)
		return "";
	return out;
}

static void register_agent_ui_tags_ext(struct guardrail_config *cfg,
				       const std::string &so_path)
{
	guardrail_rule_register(cfg, "guardrail-agent-ui-tags",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_EXT, NULL,
		"Validate supported Agent UI tags.", so_path.c_str(),
		"Regenerate using only supported Agent UI tags.");
	struct guardrail_rule *rule = guardrail_rule_lookup(cfg, "guardrail-agent-ui-tags");
	ASSERT_NE(rule, nullptr);
	rule->ext_type = GUARDRAIL_EXT_SO;
	ASSERT_EQ(guardrail_ext_so_load(rule), 0);
}

TEST(Guardrail, AgentUiTagsExtSupportedTagsPass) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"<m:vocab word=\"access\" lang=\"en-US\">获取</m:vocab>\n"
		"<m:sentence lang=\"en-US\">I can access it.</m:sentence>\n"
		"<m:button label=\"继续\" action=\"practice.next\" />";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

TEST(Guardrail, AgentUiTagsExtUnsupportedTagFails) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"<m:ask_user question=\"认识 access 吗?\" />\n"
		"<m:vocab word=\"access\">获取</m:vocab>";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "guardrail-agent-ui-tags");
	EXPECT_NE(strstr(r.reason, "ask_user"), nullptr);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

TEST(Guardrail, AgentUiTagsExtUnknownTagFails) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "Choose: <m:quiz id=\"q1\">...</m:quiz>";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "guardrail-agent-ui-tags");
	EXPECT_NE(strstr(r.reason, "quiz"), nullptr);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

static enum guardrail_verdict mock_so_check_fn(
	const struct guardrail_eval_ctx *ctx,
	char *reason_out, size_t reason_cap)
{
	if (ctx->proposed_answer && strstr(ctx->proposed_answer, "BLOCK"))
	{
		snprintf(reason_out, reason_cap, "Blocked by mock .so rule.");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

TEST(Guardrail, ExtSoRuleWithCheckFn) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_rule_register(&cfg, "so_check", GUARDRAIL_HOOK_OUTPUT,
		GUARDRAIL_RULE_C, mock_so_check_fn, NULL, NULL, "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "This is BLOCK content";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "so_check");
	arena_destroy(a);
}

TEST(Guardrail, ExtSoLoadBadPathFails) {
	struct guardrail_rule rule;
	memset(&rule, 0, sizeof(rule));
	strncpy(rule.name, "bad_so", sizeof(rule.name) - 1);
	strncpy(rule.ext_entry, "/nonexistent/path.so", sizeof(rule.ext_entry) - 1);
	int rc = guardrail_ext_so_load(&rule);
	EXPECT_NE(rc, 0);
	EXPECT_EQ(rule.dl_handle, nullptr);
	EXPECT_EQ(rule.ext_check, nullptr);
}

TEST(Guardrail, ExtSoUnloadNullSafe) {
	guardrail_ext_so_unload(nullptr);
	struct guardrail_rule rule;
	memset(&rule, 0, sizeof(rule));
	guardrail_ext_so_unload(&rule);
}
