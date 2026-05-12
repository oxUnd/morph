#include <gtest/gtest.h>
#include "agent/compress.h"
#include "agent/context.h"
#include <string.h>

class CompressTest : public ::testing::Test {
protected:
	void TearDown() override {}
};

TEST_F(CompressTest, SlidingWindowBasic) {
	struct message_list *head = nullptr;
	for (int i = 0; i < 10; i++) {
		char buf[64];
		snprintf(buf, sizeof(buf), "message %d", i);
		msg_list_append(&head, msg_list_create("user", buf, 1));
	}
	int original_count = msg_list_count(head);
	EXPECT_EQ(original_count, 10);
	struct compress_result result = {0};
	int rc = compress_sliding_window(&head, 2, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_GT(result.messages_removed, 0);
	EXPECT_LT(msg_list_count(head), original_count);
	msg_list_destroy(head);
}

TEST_F(CompressTest, SlidingWindowKeepAll) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create("user", "msg1", 1));
	msg_list_append(&head, msg_list_create("assistant", "msg2", 1));
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
}

TEST_F(CompressTest, ReactTrace) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create("user", "hello", 1));
	struct message_list *m2 = msg_list_create("assistant", "processed", 1);
	m2->compressed = 1;
	msg_list_append(&head, m2);
	msg_list_append(&head, msg_list_create("user", "next", 1));
	int original_count = msg_list_count(head);
	struct compress_result result = {0};
	int rc = compress_react_trace(&head, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_LT(msg_list_count(head), original_count);
	msg_list_destroy(head);
}

TEST_F(CompressTest, ExtractKeyInfo) {
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create("user",
		"saved file_path: /tmp/output.png", 5));
	msg_list_append(&head, msg_list_create("assistant",
		"your output: result image", 5));
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