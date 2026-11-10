#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/application.h"
#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/subagents.h"
#include "util/cleanup.h"
#include "util/json.h"
}

#include <cstring>
#include <memory>
#include <string>

using SubagentThread =
	std::unique_ptr<cli_subagents_thread, decltype(&cli_subagents_thread_destroy)>;

static SubagentThread make_thread(const char *id)
{
	cli_subagents_thread *thread = nullptr;
	EXPECT_EQ(cli_subagents_thread_create(id, &thread), 0);
	return SubagentThread(thread, cli_subagents_thread_destroy);
}

TEST(CliSubagentsTest, ThreadReplayKeepsContentToolAndFollowupChronology)
{
	auto thread = make_thread("/root/worker");
	ASSERT_NE(thread, nullptr);
	const char *events[] = {
		R"({
  "type": "react",
  "name": "react.turn.begin",
  "data": {
    "agent_id": "/root/worker",
    "text": "FIRST_USER",
    "agent_event_sequence": 1
  }
})",
		R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/worker",
    "text": "BEFORE_TOOL",
    "agent_event_sequence": 2
  }
})",
		R"({
  "type": "tool",
  "name": "tool.call",
  "data": {
    "agent_id": "/root/worker",
    "tool": "file_read",
    "tool_call_id": "same",
    "args": {
      "file_path": "FIRST_FILE"
    },
    "agent_event_sequence": 3
  }
})",
		R"({
  "type": "tool",
  "name": "tool.result",
  "data": {
    "agent_id": "/root/worker",
    "tool": "file_read",
    "tool_call_id": "same",
    "result": "first result",
    "agent_event_sequence": 4
  }
})",
		R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/worker",
    "text": "AFTER_TOOL",
    "agent_event_sequence": 5
  }
})",
		R"({
  "type": "react",
  "name": "react.turn.begin",
  "data": {
    "agent_id": "/root/worker",
    "text": "FOLLOWUP_USER",
    "agent_event_sequence": 6
  }
})",
		R"({
  "type": "tool",
  "name": "tool.call",
  "data": {
    "agent_id": "/root/worker",
    "tool": "file_read",
    "tool_call_id": "same",
    "args": {
      "file_path": "SECOND_FILE"
    },
    "agent_event_sequence": 7
  }
})",
	};
	for (const char *event : events)
		ASSERT_EQ(cli_subagents_thread_replay(thread.get(), event), 0);
	struct cli_application ctx{};
	ctx.output.color_disabled = 1;
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&document, 1024), 0);
	ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &document, 80), 0);
	std::string text = morph_buf_cstr_borrow(&document);
	EXPECT_LT(text.find("FIRST_USER"), text.find("BEFORE_TOOL"));
	EXPECT_LT(text.find("BEFORE_TOOL"), text.find("FIRST_FILE"));
	EXPECT_LT(text.find("FIRST_FILE"), text.find("AFTER_TOOL"));
	EXPECT_LT(text.find("AFTER_TOOL"), text.find("FOLLOWUP_USER"));
	EXPECT_LT(text.find("FOLLOWUP_USER"), text.find("SECOND_FILE"));
	EXPECT_EQ(thread->transcript->tools.nelts, 2u);
}

TEST(CliSubagentsTest, ReplayFrontierDropsQueuedDuplicatesAndOtherThreads)
{
	auto thread = make_thread("/root/a");
	ASSERT_NE(thread, nullptr);
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(),
					      R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/a",
    "text": "甲乙",
    "agent_event_sequence": 20
  }
})"),
		  0);
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(),
					      R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/a",
    "text": "duplicate",
    "agent_event_sequence": 20
  }
})"),
		  0);
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(),
					      R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/b",
    "text": "foreign",
    "agent_event_sequence": 21
  }
})"),
		  0);
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(),
					      R"({
  "type": "react",
  "name": "react.content.delta",
  "data": {
    "agent_id": "/root/a",
    "text": "丙",
    "agent_event_sequence": 21
  }
})"),
		  0);
	ASSERT_EQ(thread->segments.nelts, 1u);
	auto *segment =
		static_cast<cli_subagents_segment *>(morph_array_borrow(&thread->segments, 0));
	EXPECT_STREQ(morph_buf_cstr_borrow(&segment->text), "甲乙丙");
	EXPECT_EQ(thread->sequence, 21);
}

TEST(CliSubagentsTest, ClosedViewerCannotPollutePrimaryTranscriptWithScopedCallbacks)
{
	struct cli_application ctx{};
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.presentation_ready = 1;
	ASSERT_EQ(cli_transcript_model_create(&ctx.transcript), 0);
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(R"({
		"agent_id":"/root/child","agent_event_sequence":1,"tool":"file_read",
		"tool_call_id":"same","args":{"file_path":"foreign"}})");
	struct morph_event event{};
	event.type = MORPH_EVENT_TOOL;
	event.name = "tool.call";
	event.data = data;
	testing::internal::CaptureStdout();
	EXPECT_EQ(cli_presentation_event(&ctx, &event), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_TRUE(output.empty());
	EXPECT_EQ(ctx.transcript->tools.nelts, 0u);
	cli_transcript_model_destroy(ctx.transcript);
}

TEST(CliSubagentsTest, LargeMessagesRetainCompleteUtf8Tail)
{
	auto thread = make_thread("/root/worker");
	std::string text(20000, 'x');
	text += "\n尾部_SENTINEL";
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON_AddStringToObject(data, "agent_id", "/root/worker");
	cJSON_AddNumberToObject(data, "agent_event_sequence", 1);
	cJSON_AddStringToObject(data, "text", text.c_str());
	struct morph_event event{};
	event.type = MORPH_EVENT_REACT;
	event.name = "react.content.delta";
	event.data = data;
	ASSERT_EQ(cli_subagents_thread_event(thread.get(), &event), 0);
	struct cli_application ctx{};
	ctx.output.color_disabled = 1;
	for (int columns : {120, 80, 60}) {
		morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {};
		ASSERT_EQ(morph_buf_init(&document, 1024), 0);
		ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &document, columns), 0);
		EXPECT_NE(strstr(morph_buf_cstr_borrow(&document), "尾部_SENTINEL"), nullptr);
	}
}

TEST(CliSubagentsTest, NonstreamedCommentsAndFinalOnlyResponsesRemainVisible)
{
	auto thread = make_thread("/root/worker");
	const char *events[] = {
		R"({"type":"react","name":"react.content.end","data":{
		"agent_id":"/root/worker","text":"Nonstreamed comment"}})",
		R"({"type":"tool","name":"tool.call","data":{
		"agent_id":"/root/worker","tool":"file_read","args":{"file_path":"test"}}})",
		R"({"type":"react","name":"react.final","data":{
		"agent_id":"/root/worker","text":"Final-only response"}})",
	};
	for (const char *event : events)
		ASSERT_EQ(cli_subagents_thread_replay(thread.get(), event), 0);
	ASSERT_EQ(thread->segments.nelts, 3u);
	auto *first =
		static_cast<cli_subagents_segment *>(morph_array_borrow(&thread->segments, 0));
	auto *last = static_cast<cli_subagents_segment *>(morph_array_borrow(&thread->segments, 2));
	EXPECT_STREQ(morph_buf_cstr_borrow(&first->text), "Nonstreamed comment");
	EXPECT_STREQ(morph_buf_cstr_borrow(&last->text), "Final-only response");
}
