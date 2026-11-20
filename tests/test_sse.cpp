#include <gtest/gtest.h>
#include <nb/sse.h>
#include <string.h>

class SseTest : public ::testing::Test {
protected:
  nb_sse *parser = nullptr;
  int event_count;
  std::string last_event;
  std::string last_data;

  static nb_status sse_cb(nb_sse *, const nb_sse_event *event, void *user_data, nb_err *)
  {
	  SseTest *self = static_cast<SseTest *>(user_data);
	  self->event_count++;
	  self->last_event.assign(event->value.message.event.ptr, event->value.message.event.len);
	  self->last_data.assign(event->value.message.data.ptr, event->value.message.data.len);
	  return NB_OK;
  }

	void SetUp() override {
		event_count = 0;
		last_event.clear();
		last_data.clear();
		ASSERT_EQ(nb_sse_new(nullptr, &parser, nullptr), NB_OK);
		ASSERT_EQ(nb_sse_on(parser, NB_SSE_MESSAGE, sse_cb, this, nullptr, nullptr), NB_OK);
	}
	void TearDown() override {
		nb_sse_free(parser);
	}
};

TEST_F(SseTest, Init) {
	EXPECT_NE(parser, nullptr);
	EXPECT_EQ(nb_sse_last_event_id(parser).len, 0u);
}

TEST_F(SseTest, SingleDataEvent) {
	const char *data = "data: hello\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "hello");
}

TEST_F(SseTest, MultipleDataEvents) {
	const char *data = "data: hello\n\ndata: world\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_data, "world");
}

TEST_F(SseTest, EventField) {
	const char *data = "event: message\ndata: payload\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, FieldValuesMayOmitSpaceAfterColon) {
	const char *data = "event:message\ndata:payload\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, IgnoresIdRetryAndCommentFields) {
	const char *data = ": keepalive\nid: 42\nretry:1000\ndata: payload\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "payload");
	EXPECT_TRUE(nb_str_eq_cstr(nb_sse_last_event_id(parser), "42"));
	uint64_t retry = 0;
	ASSERT_TRUE(nb_sse_retry_ms(parser, &retry));
	EXPECT_EQ(retry, 1000u);
}

TEST_F(SseTest, CombinesMultilineData) {
	const char *data = "data: hello\ndata: world\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "hello\nworld");
}

TEST_F(SseTest, DispatchesEachCompleteEvent) {
	const char *data = "data: a\n\ndata: b\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "b");
}

TEST_F(SseTest, EventNameResetsAfterDispatch) {
	const char *data = "event: custom\ndata: first\n\ndata: second\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 2);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "second");
}

TEST_F(SseTest, EmptyDataEventDispatches) {
	const char *data = "data:\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_event, "message");
	EXPECT_EQ(last_data, "");
}

TEST_F(SseTest, InvalidRetryIsIgnored) {
	const char *data = "retry: nope\nretry: -1\nretry: +100\nretry: 250\n\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 0);
	uint64_t retry = 0;
	ASSERT_TRUE(nb_sse_retry_ms(parser, &retry));
	EXPECT_EQ(retry, 250u);
}

TEST_F(SseTest, HandlesCrLfLineEndings) {
	const char *data = "data: payload\r\n\r\n";
	EXPECT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_data, "payload");
}

TEST_F(SseTest, FragmentedInput) {
	EXPECT_EQ(nb_sse_feed(parser, "data: ", 6, nullptr), NB_OK);
	EXPECT_EQ(event_count, 0);
	EXPECT_EQ(nb_sse_feed(parser, "hello\n\n", 7, nullptr), NB_OK);
	EXPECT_EQ(event_count, 1);
	EXPECT_EQ(last_data, "hello");
}

TEST_F(SseTest, EmptyInput) {
	EXPECT_EQ(nb_sse_feed(parser, "", 0, nullptr), NB_OK);
	EXPECT_EQ(event_count, 0);
}

TEST_F(SseTest, NullInput) {
	EXPECT_EQ(nb_sse_feed(parser, nullptr, 10, nullptr), NB_ERR_INVAL);
	EXPECT_EQ(event_count, 0);
}

TEST_F(SseTest, FreeNull) {
	EXPECT_NO_FATAL_FAILURE(nb_sse_free(nullptr));
}

TEST_F(SseTest, InitNull) {
	EXPECT_EQ(nb_sse_new(nullptr, nullptr, nullptr), NB_ERR_INVAL);
}

TEST_F(SseTest, EofDoesNotInventTerminalEvent)
{
	const char *data = "data: [DONE]\n";
	ASSERT_EQ(nb_sse_feed(parser, data, strlen(data), nullptr), NB_OK);
	ASSERT_EQ(nb_sse_finish(parser, nullptr), NB_OK);
	EXPECT_EQ(event_count, 0);
}
