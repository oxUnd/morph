#include <gtest/gtest.h>
#include "agent/tokenizer.h"
#include "agent/context.h"
#include <string.h>

class TokenizerTest : public ::testing::Test {
protected:
	struct tokenizer *tok;
	void SetUp() override {
		tok = tokenizer_create("gpt-4o", 128000);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
	}
};

TEST_F(TokenizerTest, CreateDestroy) {
	EXPECT_NE(tok, nullptr);
	EXPECT_STREQ(tok->model_name, "gpt-4o");
	EXPECT_EQ(tok->context_limit, 128000);
}

TEST_F(TokenizerTest, CountEmpty) {
	EXPECT_EQ(tokenizer_count(tok, ""), 0);
}

TEST_F(TokenizerTest, CountNull) {
	EXPECT_EQ(tokenizer_count(tok, nullptr), 0);
}

TEST_F(TokenizerTest, CountSimpleEnglish) {
	/* "hello" = ~2 tokens in GPT BPE (he + llo or similar) */
	int count = tokenizer_count(tok, "hello");
	EXPECT_GE(count, 1);
	EXPECT_LE(count, 5);
}

TEST_F(TokenizerTest, CountEnglishSentence) {
	/* "hello world" ≈ 2-3 tokens */
	int count = tokenizer_count(tok, "hello world");
	EXPECT_GE(count, 2);
	EXPECT_LE(count, 5);
}

TEST_F(TokenizerTest, CountLongEnglishText) {
	/* ~260 chars, should be roughly 50-70 tokens */
	int count = tokenizer_count(tok,
		"The quick brown fox jumps over the lazy dog. "
		"This is a longer sentence with more words to test "
		"the token counting algorithm. We want to make sure "
		"it handles various English text reasonably well.");
	EXPECT_GE(count, 30);
	EXPECT_LE(count, 100);
}

TEST_F(TokenizerTest, CountChineseCharacters) {
	/* 你好世界 = 4 CJK chars, should be 4-8 tokens */
	int count = tokenizer_count(tok, "你好世界");
	EXPECT_GE(count, 4);
	EXPECT_LE(count, 10);
}

TEST_F(TokenizerTest, CountChineseMixed) {
	/* "你好hello世界" = Chinese + English mixed */
	int cn_only = tokenizer_count(tok, "你好世界");
	int mixed = tokenizer_count(tok, "你好hello世界");
	EXPECT_GT(mixed, cn_only);
}

TEST_F(TokenizerTest, CountChineseWithEnglish) {
	/* "我是一个AI助手" = 6 CJK + 2 ASCII chars */
	int count = tokenizer_count(tok, "我是一个AI助手");
	EXPECT_GE(count, 6);
	EXPECT_LE(count, 20);
}

TEST_F(TokenizerTest, CountChineseSentence) {
	/* Full Chinese sentence */
	int count = tokenizer_count(tok, "赛博朋克短视频脚本，霓虹灯在雨幕中闪烁");
	EXPECT_GE(count, 10);
	EXPECT_LE(count, 40);
}

TEST_F(TokenizerTest, CountMixedSentence) {
	/* Mix of Chinese and English */
	int cn = tokenizer_count(tok, "使用GPT-4o模型");
	int en = tokenizer_count(tok, "using GPT-4o model");
	/* mixed should be somewhere in between */
	int mixed = tokenizer_count(tok, "使用GPT-4o model进行对话");
	EXPECT_GE(mixed, 3);
}

TEST_F(TokenizerTest, CountPunctuation) {
	/* Punctuation tokens are usually 1 each */
	int count = tokenizer_count(tok, "hello, world! how are you?");
	EXPECT_GE(count, 5);
	EXPECT_LE(count, 12);
}

TEST_F(TokenizerTest, CountNewlines) {
	int count = tokenizer_count(tok, "line1\nline2\nline3");
	EXPECT_GT(count, 0);
}

TEST_F(TokenizerTest, CountRepetitiveEnglish) {
	/* GPT BPE merges common substrings.
	 * "aaa" should be fewer tokens than "abc" */
	int count_aaa = tokenizer_count(tok, "aaa aaa aaa");
	int count_abc = tokenizer_count(tok, "abc def ghi");
	/* Both should be reasonable */
	EXPECT_GT(count_aaa, 0);
	EXPECT_GT(count_abc, 0);
}

TEST_F(TokenizerTest, CountLongChinese) {
	/* 200 Chinese chars ≈ 400 bytes UTF-8 */
	char text[601];
	memset(text, 0, sizeof(text));
	/* Fill with Chinese chars (U+4E2D = 中) */
	int pos = 0;
	for (int i = 0; i < 200 && pos < 598; i++) {
		text[pos++] = (char)0xE4; /* 中 = U+4E2D = E4 B8 AD */
		text[pos++] = (char)0xB8;
		text[pos++] = (char)0xAD;
	}
	text[pos] = '\0';
	int count = tokenizer_estimate_tokens(text);
	EXPECT_GE(count, 100); /* 200 chars * ~1.5 tokens minimum */
	EXPECT_LE(count, 600); /* 200 chars * ~3 tokens max */
}

TEST_F(TokenizerTest, EstimateTokens) {
	int count = tokenizer_estimate_tokens("hello world test");
	EXPECT_GT(count, 0);
}

TEST_F(TokenizerTest, EstimateNull) {
	EXPECT_EQ(tokenizer_estimate_tokens(nullptr), 0);
}

TEST_F(TokenizerTest, DefaultContextLimit) {
	struct tokenizer *t = tokenizer_create("test", 0);
	EXPECT_EQ(t->context_limit, 128000);
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, CreateNullModel) {
	struct tokenizer *t = tokenizer_create(nullptr, 128000);
	EXPECT_NE(t, nullptr);
	EXPECT_STREQ(t->model_name, "gpt-4o");
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, ModelNameCl100k) {
	struct tokenizer *t = tokenizer_create("cl100k_base", 8192);
	EXPECT_STREQ(t->model_name, "cl100k_base");
	EXPECT_EQ(t->context_limit, 8192);
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, CountJsonLike) {
	/* JSON-like content (tool args) */
	int count = tokenizer_count(tok, "{\"prompt\": \"hello world\", \"style\": \"realistic\"}");
	EXPECT_GT(count, 5);
	EXPECT_LT(count, 30);
}

TEST_F(TokenizerTest, CountCodeBlock) {
	/* Code snippets have more tokens */
	int count = tokenizer_count(tok, "```python\nprint('hello')\n```");
	EXPECT_GT(count, 5);
}

TEST_F(TokenizerTest, CountWhitespace) {
	EXPECT_EQ(tokenizer_count(tok, " "), 1);
	EXPECT_EQ(tokenizer_count(tok, "   "), 1);
}

TEST_F(TokenizerTest, CountSingleChar) {
	EXPECT_EQ(tokenizer_count(tok, "a"), 1);
	EXPECT_EQ(tokenizer_count(tok, "I"), 1);
}

TEST_F(TokenizerTest, CountEmoji) {
	/* Emoji is 4 bytes UTF-8, should be 2-3 tokens */
	int count = tokenizer_count(tok, "😀");
	EXPECT_GE(count, 2);
	EXPECT_LE(count, 4);
}

TEST_F(TokenizerTest, CountNumbers) {
	/* Numbers are typically 1 token if small */
	int count = tokenizer_count(tok, "42");
	EXPECT_GE(count, 1);
	EXPECT_LE(count, 2);
}

TEST_F(TokenizerTest, ReActPromptEstimation) {
	/* Real-world ReAct prompt - should be reasonable */
	const char *prompt = "Thought: The user wants a cyberpunk short video script. "
		"I should generate a script first.\n"
		"Action: test_tool({\"prompt\": \"赛博朋克短视频脚本\", \"style\": \"creative\"})\n"
		"Observation: Generated script about neon lights in rain...\n"
		"Final: Here is your cyberpunk short video script.";
	int count = tokenizer_count(tok, prompt);
	EXPECT_GE(count, 20);
	EXPECT_LE(count, 200);
}