#include <gtest/gtest.h>
#include "agent/compress.h"
#include "agent/context.h"
#include <string.h>

class CompressTest : public ::testing::Test {
protected:
	void TearDown() override {}
};

static struct message_list *make_msg(const char *role, const char *content,
				      int compressed)
{
	struct message_list *m = msg_list_create(role, content, 1);
	if (compressed) m->compressed = 1;
	return m;
}

// ── compress_sliding_window ─────────────────────────────────

TEST_F(CompressTest, SlidingWindowBasic) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 10; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "message %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	int original_count = msg_list_count(head);
	struct compress_result result = {0};
	int rc = compress_sliding_window(&head, 2, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_GT(result.messages_removed, 0);
	EXPECT_EQ(result.original_tokens, 10);
	EXPECT_LT(msg_list_count(head), original_count);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SlidingWindowKeepAll) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "msg1", 0));
	msg_list_append(&head, make_msg("assistant", "msg2", 0));
	struct compress_result result = {0};
	int rc = compress_sliding_window(&head, 10, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(result.messages_removed, 0);
	EXPECT_EQ(msg_list_count(head), 2);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SlidingWindowNull) {
	struct compress_result result = {0};
	int rc = compress_sliding_window(nullptr, 5, &result);
	EXPECT_NE(rc, 0);
	rc = compress_sliding_window(nullptr, 5, nullptr);
	EXPECT_NE(rc, 0);
}

TEST_F(CompressTest, SlidingWindowPreservesSystem) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("system", "SYSTEM", 0));
	for (int i = 0; i < 8; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "msg %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	struct compress_result result = {0};
	compress_sliding_window(&head, 1, &result);
	EXPECT_EQ(result.messages_removed, 7);
	EXPECT_EQ(msg_list_count(head), 2);
	EXPECT_STREQ(head->role, "system");
	msg_list_destroy(head);
}

TEST_F(CompressTest, SlidingWindowZeroKeep) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("system", "SYS", 0));
	msg_list_append(&head, make_msg("user", "a", 0));
	msg_list_append(&head, make_msg("assistant", "b", 0));
	struct compress_result result = {0};
	int rc = compress_sliding_window(&head, 0, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(result.messages_removed, 2);
	EXPECT_EQ(msg_list_count(head), 1);
	EXPECT_STREQ(head->role, "system");
	msg_list_destroy(head);
}

TEST_F(CompressTest, SlidingWindowExactFit) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 4; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "msg %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	struct compress_result result = {0};
	int rc = compress_sliding_window(&head, 2, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(result.messages_removed, 0);
	EXPECT_EQ(msg_list_count(head), 4);
	msg_list_destroy(head);
}

// ── compress_react_trace ────────────────────────────────────

TEST_F(CompressTest, ReactTrace) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "hello", 0));
	msg_list_append(&head, make_msg("assistant", "processed", 1));
	msg_list_append(&head, make_msg("user", "next", 0));
	struct compress_result result = {0};
	int rc = compress_react_trace(&head, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(result.messages_removed, 1);
	EXPECT_EQ(msg_list_count(head), 2);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ReactTraceAllCompressed) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "a", 1));
	msg_list_append(&head, make_msg("assistant", "b", 1));
	struct compress_result result = {0};
	compress_react_trace(&head, &result);
	EXPECT_EQ(result.messages_removed, 2);
	EXPECT_EQ(head, nullptr);
}

TEST_F(CompressTest, ReactTraceNoneCompressed) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "a", 0));
	msg_list_append(&head, make_msg("assistant", "b", 0));
	struct compress_result result = {0};
	compress_react_trace(&head, &result);
	EXPECT_EQ(result.messages_removed, 0);
	EXPECT_EQ(msg_list_count(head), 2);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ReactTraceEmpty) {
	struct compress_result result = {0};
	int rc = compress_react_trace(nullptr, &result);
	EXPECT_NE(rc, 0);
}

TEST_F(CompressTest, ReactTraceAllRemoved) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "x", 1));
	struct compress_result result = {0};
	compress_react_trace(&head, &result);
	EXPECT_EQ(result.messages_removed, 1);
	EXPECT_EQ(head, nullptr);
}

// ── compress_detect_react_cycles ────────────────────────────

TEST_F(CompressTest, DetectBasicCycle) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("assistant", "Thought: need to search", 0));
	msg_list_append(&head, make_msg("assistant", "file_read({\"path\":\"/tmp\"})", 0));
	msg_list_append(&head, make_msg("user", "Observation: file found", 0));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 3);
	struct message_list *cur = head;
	while (cur) { EXPECT_EQ(cur->compressed, 1); cur = cur->next; }
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectToolErrorCycle) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("assistant", "Thought: try", 0));
	msg_list_append(&head, make_msg("assistant", "img_gen({\"prompt\":\"cat\"})", 0));
	msg_list_append(&head, make_msg("user", "tool error: failed (code -5)", 0));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 3);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectNoMatchWrongRoles) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "Thought: hello", 0));
	msg_list_append(&head, make_msg("assistant", "action call", 0));
	msg_list_append(&head, make_msg("user", "Observation: ok", 0));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 0);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectNoMatchIncompleteTriplet) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("assistant", "Thought: thinking", 0));
	msg_list_append(&head, make_msg("assistant", "action call", 0));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 0);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectAlreadyCompressed) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("assistant", "Thought: old", 1));
	msg_list_append(&head, make_msg("assistant", "old_action", 1));
	msg_list_append(&head, make_msg("user", "Observation: old", 1));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 0);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectMultipleCycles) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 3; i++) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Thought: iter %d", i);
		msg_list_append(&head, make_msg("assistant", buf, 0));
		snprintf(buf, sizeof(buf), "file_read({\"n\":%d})", i);
		msg_list_append(&head, make_msg("assistant", buf, 0));
		snprintf(buf, sizeof(buf), "Observation: result %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 9);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectMixedCompressedAndFresh) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("assistant", "Thought: old", 1));
	msg_list_append(&head, make_msg("assistant", "old_action", 1));
	msg_list_append(&head, make_msg("user", "Observation: old", 1));
	msg_list_append(&head, make_msg("assistant", "Thought: new", 0));
	msg_list_append(&head, make_msg("assistant", "new_action", 0));
	msg_list_append(&head, make_msg("user", "Observation: new", 0));
	int marked = compress_detect_react_cycles(head);
	EXPECT_EQ(marked, 3);
	msg_list_destroy(head);
}

TEST_F(CompressTest, DetectEmptyList) {
	int marked = compress_detect_react_cycles(nullptr);
	EXPECT_EQ(marked, 0);
}

// ── compress_summarize ──────────────────────────────────────

static int test_summarize_cb(const char *text, void *user_data, char **out)
{
	(void)user_data;
	*out = strdup("[SUMMARY]");
	return *out ? 0 : -ENOMEM;
}

static int test_summarize_fail_cb(const char *text, void *user_data, char **out)
{
	(void)text;
	(void)user_data;
	return -1;
}

TEST_F(CompressTest, SummarizeBasic) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 6; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "message %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	struct compress_result result = {0};
	int rc = compress_summarize(&head, 1, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(rc, 1);
	EXPECT_EQ(result.messages_summarized, 4);
	EXPECT_STREQ(head->role, "system");
	EXPECT_TRUE(strstr(head->content, "SUMMARY") != nullptr);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizeBelowThreshold) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "hi", 0));
	msg_list_append(&head, make_msg("assistant", "hello", 0));
	struct compress_result result = {0};
	int rc = compress_summarize(&head, 2, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(result.messages_summarized, 0);
	EXPECT_EQ(msg_list_count(head), 2);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizePreservesRecent) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 8; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "old msg %d", i);
		msg_list_append(&head, make_msg("user", buf, 0));
	}
	msg_list_append(&head, make_msg("user", "recent 1", 0));
	msg_list_append(&head, make_msg("assistant", "recent 2", 0));
	struct compress_result result = {0};
	compress_summarize(&head, 1, test_summarize_cb, nullptr, &result);
	int count = msg_list_count(head);
	EXPECT_EQ(count, 3);
	EXPECT_STREQ(head->role, "system");
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizePreservesSystem) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("system", "important system msg", 0));
	msg_list_append(&head, make_msg("user", "user1", 0));
	msg_list_append(&head, make_msg("user", "user2", 0));
	msg_list_append(&head, make_msg("user", "user3", 0));
	struct compress_result result = {0};
	compress_summarize(&head, 0, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(result.messages_summarized, 3);
	EXPECT_EQ(msg_list_count(head), 2);
	struct message_list *cur = head;
	EXPECT_STREQ(cur->role, "system");
	EXPECT_TRUE(strstr(cur->content, "SUMMARY") != nullptr);
	cur = cur->next;
	EXPECT_STREQ(cur->role, "system");
	EXPECT_STREQ(cur->content, "important system msg");
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizePreservesCompressed) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "compress me", 0));
	msg_list_append(&head, make_msg("user", "already compressed", 1));
	msg_list_append(&head, make_msg("user", "keep me", 0));
	msg_list_append(&head, make_msg("assistant", "recent", 0));
	struct compress_result result = {0};
	compress_summarize(&head, 1, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(msg_list_count(head), 3);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizeCallbackFallback) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "only one old", 0));
	msg_list_append(&head, make_msg("user", "keep this", 0));
	struct compress_result result = {0};
	int rc = compress_summarize(&head, 0, test_summarize_fail_cb, nullptr, &result);
	EXPECT_LT(rc, 0);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizeNullParams) {
	struct compress_result result = {0};
	int rc = compress_summarize(nullptr, 1, test_summarize_cb, nullptr, &result);
	EXPECT_NE(rc, 0);
	rc = compress_summarize(nullptr, 1, test_summarize_cb, nullptr, nullptr);
	EXPECT_NE(rc, 0);
}

TEST_F(CompressTest, SummarizeLargeText) {
	struct message_list *head = nullptr;
	char big[2048];
	memset(big, 'A', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	for (int i = 0; i < 4; i++) {
		msg_list_append(&head, make_msg("user", big, 0));
	}
	struct compress_result result = {0};
	int rc = compress_summarize(&head, 0, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(rc, 1);
	EXPECT_GT(result.original_tokens, 0);
	EXPECT_GT(result.compressed_tokens, 0);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SummarizeExactTrim) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "a", 0));
	msg_list_append(&head, make_msg("assistant", "b", 0));
	struct compress_result result = {0};
	int rc = compress_summarize(&head, 1, test_summarize_cb, nullptr, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(msg_list_count(head), 2);
	msg_list_destroy(head);
}

// ── extract_key_info ────────────────────────────────────────

TEST_F(CompressTest, ExtractKeyInfo) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user",
		"saved file_path: /tmp/output.png", 0));
	msg_list_append(&head, make_msg("assistant",
		"your output: result image", 0));
	struct key_info *info = extract_key_info(head);
	EXPECT_NE(info, nullptr);
	key_info_free(info);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ExtractKeyInfoEmpty) {
	struct key_info *info = extract_key_info(nullptr);
	EXPECT_EQ(info, nullptr);
}

TEST_F(CompressTest, KeyInfoFree) {
	EXPECT_NO_FATAL_FAILURE(key_info_free(nullptr));
}

TEST_F(CompressTest, ExtractKeyInfoMultiplePatterns) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user",
		"downloaded file from url", 0));
	msg_list_append(&head, make_msg("assistant",
		"generated image: /tmp/img.png", 0));
	msg_list_append(&head, make_msg("assistant",
		"error: something went wrong", 0));
	struct key_info *info = extract_key_info(head);
	int count = 0;
	struct key_info *cur = info;
	while (cur) { count++; cur = cur->next; }
	EXPECT_EQ(count, 3);
	key_info_free(info);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ExtractKeyInfoDedup) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user",
		"saved output to /tmp/x", 0));
	msg_list_append(&head, make_msg("user",
		"saved output to /tmp/y", 0));
	struct key_info *info = extract_key_info(head);
	int count = 0;
	struct key_info *cur = info;
	while (cur) { count++; cur = cur->next; }
	EXPECT_EQ(count, 2);
	key_info_free(info);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ExtractKeyInfoNoMatch) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user", "hello world", 0));
	msg_list_append(&head, make_msg("assistant", "how are you", 0));
	struct key_info *info = extract_key_info(head);
	EXPECT_EQ(info, nullptr);
	key_info_free(info);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ExtractKeyInfoLineContext) {
	struct message_list *head = nullptr;
	msg_list_append(&head, make_msg("user",
		"first line\ngenerated: /tmp/x\nthird line", 0));
	struct key_info *info = extract_key_info(head);
	EXPECT_NE(info, nullptr);
	EXPECT_STREQ(info->key, "generated");
	EXPECT_TRUE(strstr(info->value, "generated: /tmp/x") != nullptr);
	key_info_free(info);
	msg_list_destroy(head);
}
