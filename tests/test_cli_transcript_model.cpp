#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/models/transcript.h"
#include "session_history.h"
#include "cJSON.h"
#include "util/error.h"
}

#include <cstring>
#include <string>

class CliTranscriptModelTest : public ::testing::Test
{
      protected:
	cli_transcript_model *model{};
	cli_transcript_change change{};

	void SetUp() override
	{
		ASSERT_EQ(cli_transcript_model_create(&model), 0);
	}
	void TearDown() override
	{
		cli_transcript_model_destroy(model);
	}

	int Apply(enum morph_event_type type, const char *name, const char *json, double now)
	{
		cJSON *data = cJSON_Parse(json);
		if (!data)
			return -EINVAL;
		morph_event event{};
		event.type = type;
		event.name = name;
		event.data = data;
		int rc = cli_transcript_model_reduce(model, &event, now, &change);
		cJSON_Delete(data);
		return rc;
	}

	const cli_transcript_tool *Tool(size_t index) const
	{
		return static_cast<const cli_transcript_tool *>(
			morph_array_borrow(&model->tools, index));
	}
};

TEST_F(CliTranscriptModelTest, CreatePreservesExistingModelOwner)
{
	auto *saved = model;
	EXPECT_EQ(cli_transcript_model_create(&model), -EEXIST);
	EXPECT_EQ(model, saved);
}

TEST_F(CliTranscriptModelTest, InterleavedCallsKeepTheirIdsAndReasoningPositions)
{
	ASSERT_EQ(cli_transcript_model_reasoning_append(model, "before", 6, 1), 0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"file_read\",\"tool_call_id\":\"a\",\"args\":{\"path\":"
			"\"first\"}}",
			10),
		  0);
	EXPECT_EQ(change.kind, CLI_TRANSCRIPT_TOOL_STARTED);
	EXPECT_EQ(change.tool_index, 0u);
	ASSERT_EQ(cli_transcript_model_reasoning_append(model, "middle", 6, 1), 0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"file_read\",\"tool_call_id\":\"b\",\"args\":{\"path\":"
			"\"second\"}}",
			11),
		  0);
	ASSERT_EQ(cli_transcript_model_reasoning_append(model, " after", 6, 0), 0);
	ASSERT_EQ(model->reasoning.nelts, 2u);
	const auto *first = static_cast<const cli_transcript_reasoning *>(
		morph_array_borrow(&model->reasoning, 0));
	const auto *second = static_cast<const cli_transcript_reasoning *>(
		morph_array_borrow(&model->reasoning, 1));
	EXPECT_EQ(first->after_tools, 0u);
	EXPECT_EQ(second->after_tools, 1u);
	EXPECT_STREQ(morph_buf_cstr_borrow(&second->text), "middle after");
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool\":\"file_read\",\"tool_call_id\":\"a\","
			"\"result\":\"{\\\"data\\\":{\\\"returned_lines\\\":3}}\"}",
			12),
		  0);
	EXPECT_EQ(change.kind, CLI_TRANSCRIPT_TOOL_FINISHED);
	EXPECT_EQ(change.tool_index, 0u);
	EXPECT_STREQ(Tool(0)->subject, "first");
	EXPECT_STREQ(Tool(1)->subject, "second");
	EXPECT_STREQ(Tool(0)->summary, "3 lines");
	EXPECT_DOUBLE_EQ(Tool(0)->started, 10);
	EXPECT_DOUBLE_EQ(Tool(0)->ended, 12);
	EXPECT_EQ(cli_transcript_model_active_borrow(model), Tool(1));
}

TEST_F(CliTranscriptModelTest, StreamingAndResultSemanticsWorkWithoutAnApplication)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"exec\",\"tool_call_id\":\"exec\",\"args\":{\"command\":"
			"\"run\"}}",
			100),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.stream.delta",
			"{\"tool_call_id\":\"exec\",\"text\":\"hello \\u001b[3\"}", 101),
		  0);
	EXPECT_EQ(change.stream_offset, 0u);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.stream.delta",
			"{\"tool_call_id\":\"exec\",\"text\":\"1mworld\\u001b[0m\"}", 102),
		  0);
	EXPECT_EQ(change.kind, CLI_TRANSCRIPT_TOOL_STREAMED);
	EXPECT_EQ(change.stream_offset, 6u);
	EXPECT_STREQ(morph_buf_cstr_borrow(&Tool(0)->stream), "hello world");
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool_call_id\":\"exec\",\"result\":\"{\\\"ok\\\":false,"
			"\\\"data\\\":{\\\"exit_code\\\":2,\\\"stderr\\\":\\\"failure\\\","
			"\\\"duration_ms\\\":500,\\\"timed_out\\\":true}}\"}",
			105),
		  0);
	EXPECT_EQ(Tool(0)->state, -1);
	EXPECT_TRUE(Tool(0)->timed_out);
	EXPECT_STREQ(Tool(0)->error, "failure");
	EXPECT_STREQ(Tool(0)->summary, "exit 2");
	EXPECT_DOUBLE_EQ(Tool(0)->started, 104.5);
	EXPECT_EQ(cli_transcript_model_active_borrow(model), nullptr);
	ASSERT_EQ(Apply(MORPH_EVENT_REACT, "react.observation",
			"{\"tool_call_id\":\"exec\",\"text\":\"do not replace the result\"}", 106),
		  0);
	EXPECT_EQ(change.kind, CLI_TRANSCRIPT_OBSERVATION);
	EXPECT_FALSE(change.observation_added);
	EXPECT_NE(strstr(Tool(0)->result, "exit_code"), nullptr);
}

TEST_F(CliTranscriptModelTest, ProcessPollingLinksTheOriginalCommandAndTracksRunningStatus)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"exec\",\"tool_call_id\":\"exec\",\"args\":{\"command\":"
			"\"sleep 10\"}}",
			1),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool_call_id\":\"exec\",\"result\":\"{\\\"data\\\":{"
			"\\\"session_id\\\":\\\"job-42\\\",\\\"status\\\":\\\"running\\\"}}\"}",
			2),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"process\",\"tool_call_id\":\"poll\","
			"\"args\":{\"action\":\"poll\",\"session_id\":\"job-42\"}}",
			3),
		  0);
	EXPECT_STREQ(Tool(1)->subject, "sleep 10");
	EXPECT_TRUE(Tool(1)->process_poll);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool_call_id\":\"poll\",\"result\":\"{\\\"data\\\":{"
			"\\\"status\\\":\\\"running\\\"}}\"}",
			4),
		  0);
	EXPECT_TRUE(Tool(1)->process_running);
	EXPECT_TRUE(cli_transcript_tool_is_running(Tool(1)));
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool_call_id\":\"poll\",\"result\":\"{\\\"data\\\":{"
			"\\\"status\\\":\\\"completed\\\"}}\"}",
			5),
		  0);
	EXPECT_FALSE(Tool(1)->process_running);
	EXPECT_FALSE(cli_transcript_tool_is_running(Tool(1)));
}

TEST_F(CliTranscriptModelTest, ObservationIsOwnedAndAddedOnlyOnce)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"file_read\",\"tool_call_id\":\"call\",\"args\":{}}", 1),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_REACT, "react.observation",
			"{\"tool_call_id\":\"call\",\"text\":\"original\"}", 2),
		  0);
	EXPECT_TRUE(change.observation_added);
	EXPECT_STREQ(Tool(0)->result, "original");
	ASSERT_EQ(Apply(MORPH_EVENT_REACT, "react.observation",
			"{\"tool_call_id\":\"call\",\"text\":\"duplicate\"}", 3),
		  0);
	EXPECT_FALSE(change.observation_added);
	EXPECT_STREQ(Tool(0)->result, "original");
	ASSERT_EQ(Apply(MORPH_EVENT_REACT, "react.observation",
			"{\"tool\":\"plan\",\"tool_call_id\":\"call\",\"text\":\"plan\"}", 4),
		  0);
	EXPECT_EQ(change.kind, CLI_TRANSCRIPT_UNCHANGED);
}

TEST_F(CliTranscriptModelTest, HistoricalReplayDoesNotSynthesizePatchPreviewsOrInterruptedCalls)
{
	cJSON *call = cJSON_Parse("{\"name\":\"apply_patch\",\"tool_call_id\":\"historical\","
				  "\"input\":\"*** Begin Patch\\n*** Update File: "
				  "absent.c\\n-old\\n+new\\n*** End Patch\"}");
	ASSERT_NE(call, nullptr);
	size_t index;
	ASSERT_EQ(cli_transcript_model_restore(model, call, nullptr, &index), 0);
	cJSON_Delete(call);
	EXPECT_EQ(index, 0u);
	EXPECT_TRUE(Tool(0)->historical);
	EXPECT_EQ(Tool(0)->state, 0);
	EXPECT_DOUBLE_EQ(Tool(0)->started, 0);
	EXPECT_STREQ(Tool(0)->patch_preview, "");
	EXPECT_STREQ(Tool(0)->subject, "absent.c");
	EXPECT_STREQ(Tool(0)->summary, "+1 -1");
	cli_transcript_model_finish(model, 20);
	EXPECT_EQ(Tool(0)->state, 0);
	EXPECT_FALSE(Tool(0)->interrupted);
	EXPECT_DOUBLE_EQ(Tool(0)->ended, 0);
}

TEST_F(CliTranscriptModelTest, HistoricalFailuresKeepOriginalResultAndErrorMetadata)
{
	cJSON *call = cJSON_Parse("{\"name\":\"exec\",\"tool_call_id\":\"history\","
				  "\"arguments\":\"{\\\"command\\\":\\\"sleep 10\\\"}\"}");
	ASSERT_NE(call, nullptr);
	model_history_item result{};
	result.content = const_cast<char *>("stored abbreviated result");
	std::string metadata = "{\"original_content\":\"full result\",\"error_code\":" +
			       std::to_string(-ETIMEDOUT) + "}";
	result.payload_json = metadata.data();
	size_t index;
	ASSERT_EQ(cli_transcript_model_restore(model, call, &result, &index), 0);
	cJSON_Delete(call);
	EXPECT_STREQ(Tool(index)->result, "full result");
	EXPECT_EQ(Tool(index)->state, -1);
	EXPECT_STREQ(Tool(index)->error, morph_strerror(-ETIMEDOUT));
	EXPECT_TRUE(Tool(index)->timed_out);
	EXPECT_TRUE(Tool(index)->historical);
}

TEST_F(CliTranscriptModelTest, FinishOnlyInterruptsUnresolvedLiveCallsAndIsIdempotent)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"exec\",\"tool_call_id\":\"pending\",\"args\":{}}", 10),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
			"{\"tool\":\"file_read\",\"tool_call_id\":\"done\",\"args\":{}}", 11),
		  0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.result",
			"{\"tool_call_id\":\"done\",\"result\":\"complete\"}", 12),
		  0);
	cli_transcript_model_finish(model, 15);
	EXPECT_TRUE(model->finished);
	EXPECT_TRUE(Tool(0)->interrupted);
	EXPECT_EQ(Tool(0)->state, -1);
	EXPECT_DOUBLE_EQ(Tool(0)->ended, 15);
	EXPECT_NE(strstr(Tool(0)->error, "Interrupted"), nullptr);
	EXPECT_FALSE(Tool(1)->interrupted);
	EXPECT_EQ(Tool(1)->state, 1);
	cli_transcript_model_finish(model, 99);
	EXPECT_DOUBLE_EQ(Tool(0)->ended, 15);
	EXPECT_DOUBLE_EQ(Tool(1)->ended, 12);
}
