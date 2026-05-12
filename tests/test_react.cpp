#include <gtest/gtest.h>
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/tokenizer.h"
#include <string.h>

static int test_tool_fn(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json;
	(void)user_data;
	*result_json = strdup("{\"result\":\"test\"}");
	return 0;
}

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

TEST_F(ReactTest, CreateDestroy) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->max_iterations, 10);
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