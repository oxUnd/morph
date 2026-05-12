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
	EXPECT_EQ(parser.buf, nullptr);
	EXPECT_EQ(parser.buf_len, 0);
}

TEST_F(SseTest, SingleDataEvent) {
	const char *data = "data: hello\n\n";
	sse_parser_feed(&parser, data, strlen(data));
	EXPECT_EQ(event_count, 1);
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
	EXPECT_EQ(event_count, 2);
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