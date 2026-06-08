#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tools/text_gen.h"
#include "agent/tools/text_qa.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

class TextGenTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	void SetUp() override {
		tool_registry_init(&reg);
	}
	void TearDown() override {
		tool_registry_cleanup(&reg);
	}
};

TEST_F(TextGenTest, RegisterTool) {
	int rc = text_gen_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "text_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "text_gen");
}

TEST_F(TextGenTest, RegisterQaTool) {
	int rc = text_qa_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "text_qa");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "text_qa");
}

TEST_F(TextGenTest, BothToolsRegistered) {
	EXPECT_EQ(text_gen_init(&reg, NULL), 0);
	EXPECT_EQ(text_qa_init(&reg, NULL), 0);
	EXPECT_EQ(reg.count, 2);
	EXPECT_NE(tool_lookup(&reg, "text_gen"), nullptr);
	EXPECT_NE(tool_lookup(&reg, "text_qa"), nullptr);
}

TEST_F(TextGenTest, ExecMissingPrompt) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecNullArgs) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", NULL, &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecEmptyArgs) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", "", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecMalformedJson) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", "not json at all", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	/* Should still detect missing prompt even from malformed JSON */
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecMissingPrompt) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecNullArgs) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa", NULL, &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecEmptyArgs) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa", "", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecWithPromptOnlyNoLLM) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", "{\"prompt\":\"hello\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecWithPromptOnlyNoLLM) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa", "{\"prompt\":\"test\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecVeryLongPromptArg) {
	text_gen_init(&reg, NULL);
	char long_json[16384];
	size_t pos = 0;
	pos += snprintf(long_json + pos, sizeof(long_json) - pos, "{\"prompt\":\"");
	for (int i = 0; i < 1000; i++)
		pos += snprintf(long_json + pos, sizeof(long_json) - pos, "test%d ", i);
	snprintf(long_json + pos, sizeof(long_json) - pos, "\"}");

	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen", long_json, &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecPromptWithSpecialChars) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen",
		"{\"prompt\":\"hello\\nworld\\ttab\\\"quote\\u0041\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ExecPromptWithUnicode) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_gen",
		"{\"prompt\":\"你好世界こんにちは\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecWithContext) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa",
		"{\"prompt\":\"summarize\",\"context\":\"long text here\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, QaExecWithEmptyContext) {
	text_qa_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "text_qa",
		"{\"prompt\":\"test\",\"context\":\"\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "no LLM") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(TextGenTest, ToolNotFound) {
	text_gen_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "nonexistent_tool", "{}", &result);
	EXPECT_NE(rc, 0);
	EXPECT_EQ(result.text.data, nullptr);
}

TEST_F(TextGenTest, NullResultPtr) {
	text_gen_init(&reg, NULL);
	int rc = tool_exec(&reg, "text_gen", "{}", NULL);
	EXPECT_NE(rc, 0);
}

TEST(TokenizerEdgeCases, NullText) {
	EXPECT_EQ(tokenizer_estimate_tokens(NULL), 0);
}

TEST(TokenizerEdgeCases, EmptyText) {
	EXPECT_EQ(tokenizer_estimate_tokens(""), 0);
}

TEST(TokenizerEdgeCases, SingleChar) {
	EXPECT_GT(tokenizer_estimate_tokens("a"), 0);
}

TEST(TokenizerEdgeCases, VeryLongText) {
	char big[65536];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	int tokens = tokenizer_estimate_tokens(big);
	EXPECT_GT(tokens, 1000);
}

TEST(TokenizerEdgeCases, MixedUnicode) {
	const char *mixed = "Hello 世界 🌍 こんにちは";
	int tokens = tokenizer_estimate_tokens(mixed);
	EXPECT_GT(tokens, 0);
}

TEST(TokenizerEdgeCases, OnlyNewlines) {
	int tokens = tokenizer_estimate_tokens("\n\n\n\n\n");
	EXPECT_GT(tokens, 0);
}

TEST(TokenizerEdgeCases, JsonLike) {
	const char *json = "{\"key\": \"value with \\\"escaped\\\" quotes\"}";
	int tokens = tokenizer_estimate_tokens(json);
	EXPECT_GT(tokens, 0);
}

TEST(TokenizerEdgeCases, ConsecutiveSpaces) {
	const char *spaces = "a     b";
	int tokens = tokenizer_estimate_tokens(spaces);
	EXPECT_GT(tokens, 0);
}

TEST(TokenizerEdgeCases, TabAndSpaces) {
	const char *text = "\t\tindented\n\t\ttext";
	int tokens = tokenizer_estimate_tokens(text);
	EXPECT_GT(tokens, 0);
}
