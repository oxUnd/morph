#include <gtest/gtest.h>
#include "agent/context.h"
#include "agent/tokenizer.h"
#include "util/arena.h"
#include <string.h>

class ContextTest : public ::testing::Test {
protected:
	struct tokenizer *tok;
	struct arena *ar;
	void SetUp() override {
		tok = tokenizer_create("gpt-4o", 128000);
		ar = arena_create(0);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
		arena_destroy(ar);
	}
};

TEST_F(ContextTest, CreateMessage) {
	struct message_list *m = msg_list_create(ar, "user", "hello world", 3);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->role, "user");
	EXPECT_STREQ(m->content, "hello world");
	EXPECT_EQ(m->token_count, 3);
}

TEST_F(ContextTest, CreateNullRole) {
	struct message_list *m = msg_list_create(ar, nullptr, "content", 1);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->role, "");
}

TEST_F(ContextTest, AppendMessages) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "hi", 1));
	msg_list_append(&head, msg_list_create(ar, "assistant", "hello", 1));
	msg_list_append(&head, msg_list_create(ar, "user", "how are you", 3));
	EXPECT_EQ(msg_list_count(head), 3);
}

TEST_F(ContextTest, AppendNull) {
	struct message_list *head = nullptr;
	msg_list_append(&head, nullptr);
	EXPECT_EQ(msg_list_count(head), 0);
}

TEST_F(ContextTest, DestroyNull) {
	EXPECT_NO_FATAL_FAILURE(msg_list_destroy(nullptr));
}

TEST_F(ContextTest, TokenCountWithTokenizer) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "hello world", 0));
	msg_list_append(&head, msg_list_create(ar, "assistant", "hi there", 0));
	int count = context_token_count(head, tok);
	EXPECT_GT(count, 0);
}

TEST_F(ContextTest, TokenCountWithoutTokenizer) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "hello", 5));
	msg_list_append(&head, msg_list_create(ar, "assistant", "hi", 2));
	int count = context_token_count(head, nullptr);
	EXPECT_EQ(count, 7);
}

TEST_F(ContextTest, EmptyList) {
	EXPECT_EQ(msg_list_count(nullptr), 0);
	EXPECT_EQ(context_token_count(nullptr, tok), 0);
}

TEST_F(ContextTest, NeedsCompress) {
	struct compress_config cfg = {
		.max_context_tokens = 100,
		.max_history_rounds = 6,
		.summarize_threshold_ratio = 0.8,
		.compress_target_ratio = 0.5,
	};
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "short message", 0));
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 0);

	char long_text[600];
	int pos = 0;
	for (int i = 0; i < 100 && pos < 598; i++) {
		long_text[pos++] = (char)0xE4;
		long_text[pos++] = (char)0xB8;
		long_text[pos++] = (char)0xAD;
	}
	long_text[pos] = '\0';
	msg_list_append(&head, msg_list_create(ar, "user", long_text, 0));
	int count = context_token_count(head, tok);
	EXPECT_GE(count, 80) << "Expected >= 80 tokens for 100 CJK chars, got " << count;
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 1);
}

TEST_F(ContextTest, NeedsCompressNullCfg) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "test", 1));
	EXPECT_EQ(context_needs_compress(head, tok, nullptr), 0);
}
