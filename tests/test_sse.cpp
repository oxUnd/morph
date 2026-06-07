#include <gtest/gtest.h>
#include "http/sse.h"
#include <string.h>

class SseTest : public ::testing::Test {
protected:
	struct sse_parser parser;
	int event_count;
	std::string last_event;
	std::string last_data;

	static int sse_cb(const char *event, const char *data, void *user_data) {
		SseTest *self = static_cast<SseTest *>(user_data);
		self->event_count++;
		self->last_event = event;
		self->last_data = data;
		return 0;
	}

	void SetUp() override {
		event_count = 0;
		last_event.clear();
		last_data.clear();
		sse_parser_init(&parser, sse_cb, this);
	}
	void TearDown() override {
		sse_parser_free(&parser);
	}
};

TEST_F(SseTest, Init) {
	EXPECT_NE(parser.buf.data, nullptr);
	EXPECT_EQ(parser.buf.len, 0);
}

TEST_F(SseTest, SingleDataEvent) {
	const char *data = "data: hello\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "hello");
}

TEST_F(SseTest, MultipleDataEvents) {
	const char *data = "data: hello\n\ndata: world\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_data, "world");
}

TEST_F(SseTest, EventField) {
	const char *data = "event: message\ndata: payload\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, FieldValuesMayOmitSpaceAfterColon) {
	const char *data = "event:message\ndata:payload\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, IgnoresIdRetryAndCommentFields) {
	const char *data = ": keepalive\nid: 42\nretry:1000\ndata: payload\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
	EXPECT_STREQ(parser.id.data, "42");
	EXPECT_EQ(parser.retry_ms, 1000);
}

TEST_F(SseTest, CombinesMultilineData) {
	const char *data = "data: hello\ndata: world\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "hello\nworld");
}

TEST_F(SseTest, DispatchesEachCompleteEvent) {
	const char *data = "data: a\n\ndata: b\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "b");
}

TEST_F(SseTest, EventNameResetsAfterDispatch) {
	const char *data = "event: custom\ndata: first\n\ndata: second\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "second");
}

TEST_F(SseTest, EmptyDataEventDispatches) {
	const char *data = "data:\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "");
}

TEST_F(SseTest, InvalidRetryIsIgnored) {
	const char *data = "retry: nope\nretry: -1\nretry: +100\nretry: 250\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 0);
	EXPECT_EQ(parser.retry_ms, 250);
}

TEST_F(SseTest, HandlesCrLfLineEndings) {
	const char *data = "data: payload\r\n\r\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, FragmentedInput) {
	sse_parser_feed(&parser, "data: ", 6);
	EXPECT_EQ(event_count, 0);
	sse_parser_feed(&parser, "hello\n\n", 7);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_data, "hello");
}

TEST_F(SseTest, EmptyInput) {
	sse_parser_feed(&parser, "", 0);
	EXPECT_EQ(event_count, 0);
}

TEST_F(SseTest, NullInput) {
	sse_parser_feed(&parser, nullptr, 10);
	EXPECT_EQ(event_count, 0);
}

TEST_F(SseTest, FreeNull) {
	EXPECT_NO_FATAL_FAILURE(sse_parser_free(nullptr));
}

TEST_F(SseTest, InitNull) {
	EXPECT_NO_FATAL_FAILURE(sse_parser_init(nullptr, nullptr, nullptr));
}
