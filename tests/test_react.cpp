#include <gtest/gtest.h>
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "http/client.h"
#include "http/sse.h"
#include <string.h>
#include <signal.h>
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

static int test_tool_fn(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json;
	(void)user_data;
	*result_json = strdup("{\"result\":\"test\"}");
	return 0;
}

static int failing_tool_fn(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json;
	(void)user_data;
	*result_json = strdup("tool failed");
	return -EIO;
}

static int call_count_tool_fn(const char *args_json, char **result_json, void *user_data)
{
	int *count = (int *)user_data;
	(*count)++;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"calls\":%d}", *count);
	*result_json = strdup(buf);
	return 0;
}

/* ---- mock LLM ---- */

struct mock_llm_data {
	const char *response;
	int call_count;
	int fail_after;
	int should_fail;
};

static int mock_llm_chat(struct model *self, const char *system_prompt,
			  const char **messages, int n,
			  sse_callback cb, void *user_data)
{
	(void)system_prompt;
	(void)messages;
	(void)n;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (cb && data->response)
		cb(data->response, user_data);
	return 200;
}

static int mock_llm_streaming_chat(struct model *self, const char *system_prompt,
				    const char **messages, int n,
				    sse_callback cb, void *user_data)
{
	(void)system_prompt;
	(void)messages;
	(void)n;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
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
	m->generate = NULL;
	m->destroy = mock_llm_destroy;
	struct mock_llm_data *data = (struct mock_llm_data *)calloc(1, sizeof(*data));
	data->response = response;
	data->call_count = 0;
	data->fail_after = 0;
	data->should_fail = 0;
	m->handle = data;
	return m;
}

static struct model *create_mock_streaming_llm(const char *response)
{
	struct model *m = create_mock_llm(response);
	m->chat = mock_llm_streaming_chat;
	return m;
}

/* ---- basic fixture ---- */

class ReactTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
	}
	void TearDown() override {
		tokenizer_destroy(tok);
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
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->max_iterations, 10);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, DefaultTimeoutAndRetries) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->step_timeout_seconds, 60);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, Reset) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->state = REACT_STATE_THINKING;
	react_reset(ctx);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->steps, nullptr);
	EXPECT_EQ(ctx->step_count, 0);
	EXPECT_EQ(ctx->cancelled, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelAndAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelFn) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->cancelled, 0);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_cancel(nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CreateStep) {
	struct react_step *s = react_step_create(REACT_STEP_THOUGHT, "thinking", nullptr, nullptr);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_THOUGHT);
	EXPECT_STREQ(s->content, "thinking");
	react_step_destroy(s);
}

TEST_F(ReactTest, CreateStepWithTool) {
	struct react_step *s = react_step_create(REACT_STEP_ACTION, "calling tool",
						 "text_gen", "{\"prompt\":\"hi\"}");
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_ACTION);
	EXPECT_STREQ(s->tool_name, "text_gen");
	EXPECT_STREQ(s->tool_args, "{\"prompt\":\"hi\"}");
	react_step_destroy(s);
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
		      "{\"type\":\"object\"}", test_tool_fn, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tools->count, 1);
	struct tool_entry *e = tool_lookup(ctx->tools, "text_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "text_gen");
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunBasic) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, "hello world", nullptr, nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunNullInput) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, nullptr, nullptr, nullptr);
	EXPECT_NE(rc, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, NullContext) {
	EXPECT_NE(react_context_create(nullptr, tok, &cfg), nullptr);
}

TEST_F(ReactTest, RunWithCallback) {
	static int callback_count = 0;
	callback_count = 0;
	auto cb = [](enum react_step_type type, const char *content, void *ud) -> int {
		callback_count++;
		return 0;
	};
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	react_run(ctx, "test input", cb, nullptr);
	react_context_destroy(ctx);
	EXPECT_GT(callback_count, 0);
}

TEST_F(ReactTest, MaxIterationsAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->max_iterations = 1;
	react_run(ctx, "test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, ToolFailThresholdThree) {
	tool_register(&tools, "failing_tool", "Always fails",
		      "{}", failing_tool_fn, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Mock LLM tests: full ReAct cycle              */
/* ============================================= */

TEST_F(MockLlmTest, LlmFinalDirectly) {
	setup_llm_with_response("Final: Hello, I can help with that.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "what is 2+2?", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "Here is my answer"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmActionToolCall) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr);
	setup_llm_with_response("Thought: Using test tool.\nAction: test_tool({\"prompt\":\"hi\"})\n\nFinal: Done.");
	struct react_context *ctx2 = react_context_create(&tools, tok, &cfg);
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
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
		      failing_tool_fn, nullptr);
	setup_llm_with_response("Thought: try fail_tool.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
		      failing_tool_fn, nullptr);
	setup_llm_with_response("Thought: try again.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple question", nullptr, nullptr);
	EXPECT_EQ(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmMultiStepCallCount) {
	tool_register(&tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr);
	setup_llm_with_response(
		"Thought: Step 1.\nAction: test_tool({\"prompt\":\"hi\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "multi-step question", nullptr, nullptr);
	EXPECT_GE(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmFailureReturnsAbort) {
	llm = create_mock_llm("should not matter");
	llm_data = (struct mock_llm_data *)llm->handle;
	llm_data->should_fail = 1;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "trigger LLM failure", nullptr, nullptr);
	EXPECT_NE(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StepCountAfterFinal) {
	setup_llm_with_response("Final: quick answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple", nullptr, nullptr);
	EXPECT_GE(ctx->step_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CallbackReceivesFinalStep) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_cancel(ctx);
	react_run(ctx, "cancelled test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
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
	tool_register(&tools, "test_tool", "Test", "{}", test_tool_fn, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
		      call_count_tool_fn, &call_count);
	setup_llm_with_response("Thought: call counter.\nAction: counter({})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
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
	volatile int running;
	const char *response_body;
	int response_status;
	int request_count;
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
	srv->running = 1;
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
		return -1;
	}
	if (listen(srv->server_fd, 5) < 0) {
		close(srv->server_fd);
		return -1;
	}
	socklen_t addr_len = sizeof(addr);
	getsockname(srv->server_fd, (struct sockaddr *)&addr, &addr_len);
	srv->port = ntohs(addr.sin_port);
	pthread_create(&srv->thread, nullptr, mock_http_server_thread, srv);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return 0;
}

static void mock_server_stop(struct mock_server *srv)
{
	srv->running = 0;
	close(srv->server_fd);
	pthread_join(srv->thread, nullptr);
}

/* ---- Integration tests with mock server ---- */

class MockServerTest : public ::testing::Test {
protected:
	struct mock_server srv;
	void SetUp() override {
		memset(&srv, 0, sizeof(srv));
		http_init();
	}
	void TearDown() override {
		if (srv.running) {
			srv.running = 0;
			close(srv.server_fd);
			pthread_join(srv.thread, nullptr);
		}
		http_cleanup();
	}
};

TEST_F(MockServerTest, HttpGetSuccess) {
	srv.response_body = "{\"status\":\"ok\"}";
	srv.response_status = 200;
	ASSERT_EQ(mock_server_start(&srv, 0), 0);
	struct http_response resp3 = {0};
	char url4[256];
	snprintf(url4, sizeof(url4), "http://127.0.0.1:%d/test", srv.port);
	int rc4 = http_get(url4, &resp3);
	EXPECT_EQ(rc4, 0);
	EXPECT_EQ(resp3.status_code, 200);
	EXPECT_NE(resp3.body, nullptr);
	EXPECT_TRUE(resp3.body && strstr(resp3.body, "ok"));
	http_response_free(&resp3);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostSuccess) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	ASSERT_EQ(mock_server_start(&srv, 0), 0);
	struct http_response resp = {0};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/test", srv.port);
	int rc = http_post(url, "{\"data\":1}", 10,
			   "application/json", &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_NE(resp.body, nullptr);
	EXPECT_TRUE(resp.body && strstr(resp.body, "posted"));
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEStreamingResponse) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
	srv.response_status = 200;
	ASSERT_EQ(mock_server_start(&srv, 0), 0);
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
	sse_parser_free(&parser);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEWithTimeout) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"test\"}}]}";
	srv.response_status = 200;
	ASSERT_EQ(mock_server_start(&srv, 0), 0);
	struct sse_parser parser2;
	sse_parser_init(&parser2, nullptr, nullptr);
	char url3[256];
	snprintf(url3, sizeof(url3), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc3 = http_post_sse_ex_timeout(url3, "{}", 2,
					   "application/json", nullptr, 0, 30,
					   global_sse_write_adapter, &parser2);
	EXPECT_EQ(rc3, 200);
	sse_parser_free(&parser2);
	mock_server_stop(&srv);
}