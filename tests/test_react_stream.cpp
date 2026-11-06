#include <gtest/gtest.h>

extern "C" {
#include "agent/react_stream.h"
#include "util/signals.h"
#include "util/utf8.h"
}

#include <string>
#include <utility>
#include <vector>

class ReactStreamTest : public ::testing::Test
{
      protected:
	struct react_stream stream{};
	volatile sig_atomic_t cancelled = 0;
	int suppressed = 0;
	int emit_error = 0;
	int calls = 0;
	int reentrant_result = 0;
	bool reenter = false;
	std::vector<std::pair<react_step_type, std::string>> deltas;

	static int capture(enum react_step_type type, const char *text, void *opaque)
	{
		auto *self = static_cast<ReactStreamTest *>(opaque);

		self->calls++;
		EXPECT_EQ(utf8valid(text), nullptr);
		self->deltas.emplace_back(type, text);
		if (self->reenter)
			self->reentrant_result = react_stream_cb("nested", &self->stream);
		return self->emit_error;
	}

	void SetUp() override
	{
		struct react_stream_options options{};

		options.emit = capture;
		options.user_data = this;
		options.cancelled = &cancelled;
		options.suppressed = &suppressed;
		ASSERT_EQ(react_stream_init(&stream, &options), 0);
	}

	void TearDown() override
	{
		react_stream_cleanup(&stream);
	}

	std::string text(enum react_step_type type)
	{
		std::string result;

		for (const auto &delta : deltas) {
			if (delta.first == type)
				result += delta.second;
		}
		return result;
	}
};

TEST_F(ReactStreamTest, LegacyMarkersAndUtf8SurviveEveryByteBoundary)
{
	for (const char *marker : {"Final:", "Final answer:", "Final Answer:"}) {
		std::string thought = "Thought: 检查结果\n";
		std::string answer = "完成，结果正确。";
		std::string input = thought + "  " + marker + " \t" + answer;

		for (char byte : input) {
			char chunk[] = {byte, '\0'};

			ASSERT_EQ(react_stream_cb(chunk, &stream), 0);
		}
		ASSERT_EQ(react_stream_finish(&stream), 0);
		EXPECT_EQ(text(REACT_STEP_THOUGHT), thought);
		EXPECT_EQ(text(REACT_STEP_FINAL), answer);
		EXPECT_EQ(morph_buf_cstr(&stream.text), input);
		struct react_stream_options options = stream.options;

		react_stream_cleanup(&stream);
		deltas.clear();
		ASSERT_EQ(react_stream_init(&stream, &options), 0);
	}
}

TEST_F(ReactStreamTest, NativeContentKeepsLiteralMarkersAndSeparateReasoning)
{
	std::string reasoning = "检查中……";
	std::string content = "Final: 这是正文\nFinal Answer: 仍是正文";

	for (char byte : reasoning) {
		char chunk[] = {byte, '\0'};

		ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_REASONING, chunk, &stream), 0);
	}
	for (char byte : content) {
		char chunk[] = {byte, '\0'};

		ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, chunk, &stream), 0);
	}
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_REASONING), reasoning);
	EXPECT_EQ(text(REACT_STEP_CONTENT), content);
	EXPECT_TRUE(text(REACT_STEP_FINAL).empty());
	EXPECT_EQ(morph_buf_cstr(&stream.text), content);
}

TEST_F(ReactStreamTest, InlineAndIncompleteMarkersRemainOrdinaryText)
{
	std::string input = "Print Final: literally.\nFinal answ";

	ASSERT_EQ(react_stream_cb(input.c_str(), &stream), 0);
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_THOUGHT), input);
	EXPECT_TRUE(text(REACT_STEP_FINAL).empty());
}

TEST_F(ReactStreamTest, LongIndentationAndCarriageReturnsKeepMarkerBoundaries)
{
	std::string indentation(32768, ' ');
	std::string input = "checked\r" + indentation + "Final Answer:\tcomplete";

	for (size_t offset = 0; offset < input.size(); offset += 11) {
		std::string chunk = input.substr(offset, 11);

		ASSERT_EQ(react_stream_cb(chunk.c_str(), &stream), 0);
	}
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_THOUGHT), "checked\r");
	EXPECT_EQ(text(REACT_STEP_FINAL), "complete");
}

TEST_F(ReactStreamTest, CancellationAndSuppressionPreventCallbacks)
{
	morph_signal_flag_set(&cancelled, 1);
	EXPECT_EQ(react_stream_cb("cancelled", &stream), -EINTR);
	EXPECT_EQ(react_stream_finish(&stream), -EINTR);
	EXPECT_EQ(stream.text.len, 0u);
	EXPECT_EQ(calls, 0);
	morph_signal_flag_set(&cancelled, 0);
	suppressed = 1;
	ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_REASONING, "reasoning", &stream), 0);
	ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, "buffered", &stream), 0);
	EXPECT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(calls, 0);
	suppressed = 0;
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_REASONING), "reasoning");
	EXPECT_EQ(text(REACT_STEP_CONTENT), "buffered");
}

TEST_F(ReactStreamTest, SinkFailureIsStickyAndNeverReplaysAcceptedBytes)
{
	emit_error = -EIO;
	EXPECT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, "one", &stream), -EIO);
	EXPECT_EQ(react_stream_cb("two", &stream), -EIO);
	EXPECT_EQ(react_stream_finish(&stream), -EIO);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(morph_buf_cstr(&stream.text), std::string("one"));
}

TEST_F(ReactStreamTest, IncompleteUtf8FailsInsteadOfSilentlyDroppingBytes)
{
	ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, "ok\xe4\xb8", &stream), 0);
	EXPECT_EQ(react_stream_finish(&stream), MORPH_ERR_PROTOCOL);
	EXPECT_EQ(text(REACT_STEP_CONTENT), "ok");
	EXPECT_EQ(react_stream_cb("more", &stream), MORPH_ERR_PROTOCOL);
}

TEST_F(ReactStreamTest, ReentrantInputCannotInvalidateTheBorrowedDelta)
{
	reenter = true;
	ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, "original", &stream), 0);
	EXPECT_EQ(reentrant_result, -EBUSY);
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_CONTENT), "original");
	EXPECT_EQ(morph_buf_cstr(&stream.text), std::string("original"));
}

TEST_F(ReactStreamTest, LargeResponsesPreserveAllBytesAcrossBufferGrowth)
{
	std::string input(1024 * 1024, 'x');

	for (size_t offset = 0; offset < input.size(); offset += 113) {
		std::string chunk = input.substr(offset, 113);

		ASSERT_EQ(react_typed_stream_cb(LLM_STREAM_CONTENT, chunk.c_str(), &stream), 0);
	}
	ASSERT_EQ(react_stream_finish(&stream), 0);
	EXPECT_EQ(text(REACT_STEP_CONTENT), input);
	EXPECT_EQ(morph_buf_cstr(&stream.text), input);
}
