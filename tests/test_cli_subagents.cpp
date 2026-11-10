#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/application.h"
#include "sapi/cli/models/subagents.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/subagents.h"
#include "sapi/cli/widgets/transcript.h"
#include "util/cleanup.h"
#include "util/json.h"
}

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <sstream>
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
	ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &document, 80, 0), 0);
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
		ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &document, columns, 0), 0);
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

class SubagentsColumns
{
      public:
	SubagentsColumns()
	{
		const char *value = std::getenv("COLUMNS");
		had_value = value != nullptr;
		previous = value ? value : "";
		setenv("COLUMNS", "80", 1);
	}
	~SubagentsColumns()
	{
		if (had_value)
			setenv("COLUMNS", previous.c_str(), 1);
		else
			unsetenv("COLUMNS");
	}

      private:
	bool had_value = false;
	std::string previous;
};

static int subagents_text_event(struct cli_subagents_thread *thread, const char *name,
				const char *text)
{
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!data || !cJSON_AddStringToObject(data, "agent_id", thread->id) ||
	    !cJSON_AddStringToObject(data, "text", text))
		return -ENOMEM;
	struct morph_event event{};
	event.name = name;
	event.type = MORPH_EVENT_REACT;
	event.data = data;
	return cli_subagents_thread_event(thread, &event);
}

static std::string subagents_plain(const morph_buf_t &document)
{
	char *plain MORPH_CLEANUP(morph_charp_cleanup) =
		utf8_strip_ansi_dup(morph_buf_cstr_borrow(&document), document.len, nullptr);
	return plain ? plain : "";
}

static void subagents_expect_sgr_rows(const morph_buf_t &document, int columns,
				      bool full_background = false)
{
	std::string text = morph_buf_cstr_borrow(&document);
	EXPECT_EQ(utf8valid(text.c_str()), nullptr);
	for (size_t offset = 0; offset < text.size(); offset++) {
		if (text[offset] != '\033')
			continue;
		ASSERT_LT(offset + 1, text.size());
		ASSERT_EQ(text[++offset], '[');
		while (++offset < text.size() && ((text[offset] >= '0' && text[offset] <= '9') ||
						  text[offset] == ';' || text[offset] == ':')) {}
		ASSERT_LT(offset, text.size());
		ASSERT_EQ(text[offset], 'm');
	}
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		EXPECT_LE(utf8_display_width_ansi(line.c_str()),
			  static_cast<size_t>(full_background ? columns : columns - 1));
		if (!line.empty()) {
			EXPECT_EQ(line.find("\033[0m"), 0u);
			ASSERT_GE(line.size(), 4u);
			EXPECT_EQ(line.substr(line.size() - 4), "\033[0m");
		}
	}
}

TEST(CliSubagentsTest, UserBlocksMatchThePrimaryFeedWithoutCaptureOrCursorControls)
{
	SubagentsColumns columns;
	auto thread = make_thread("/root/worker");
	const char *text = "Review 中文🙂\nsecond line\033[31m RAW_COLOR\033[0m\033[2J";
	ASSERT_EQ(subagents_text_event(thread.get(), "react.turn.begin", text), 0);
	struct cli_application ctx{};
	ctx.composer.ultra = 1;
	testing::internal::CaptureStdout();
	cli_presentation_user_input(&ctx, text);
	std::string primary = testing::internal::GetCapturedStdout();
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&document, 256), 0);
	ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &document, 80, 0), 0);
	EXPECT_EQ(primary, morph_buf_cstr_borrow(&document));
	EXPECT_NE(primary.find("\033[48;5;234m"), std::string::npos);
	EXPECT_NE(primary.find("›› "), std::string::npos);
	EXPECT_EQ(primary.find("\033[31m"), std::string::npos);
	EXPECT_EQ(primary.find("\033[K"), std::string::npos);
	EXPECT_EQ(primary.find("\033[2J"), std::string::npos);
	subagents_expect_sgr_rows(document, 80, true);
}

TEST(CliSubagentsTest, AssistantMarkdownUsesFeedStylesAndKeepsRootStreamsUntouched)
{
	auto thread = make_thread("/root/worker");
	ASSERT_EQ(subagents_text_event(
			  thread.get(), "react.content.delta",
			  "# Review heading\n\n**Strong answer**\n\n- First item\n- Second item\n\n"
			  "```c\nint answer = 42;\nreturn answer;\n```\n\n"
			  "[link](https://example.test)\033]52;c;PAYLOAD\a\033[2J"),
		  0);
	struct cli_application ctx{};
	struct cli_presenter_state presenter{};
	ctx.presenter = &presenter;
	ctx.output.trailing_newlines = 1;
	ctx.output.command_result_pending = 1;
	ctx.output.capture_styled = 1;
	ctx.output.capture_history = 1;
	presenter.markdown_stream_kind = 123;
	presenter.markdown_stream_visible = 1;
	presenter.reasoning_stream_offset = 7;
	ASSERT_EQ(morph_buf_init(&presenter.markdown_stream_text, 32), 0);
	ASSERT_EQ(morph_buf_puts(&presenter.markdown_stream_text, "ROOT_STREAM"), 0);
	morph_buf_t captured MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&captured, 32), 0);
	ASSERT_EQ(morph_buf_puts(&captured, "ROOT_CAPTURE"), 0);
	ctx.output.capture = &captured;
	morph_buf_t document MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&document, 256), 0);
	testing::internal::CaptureStdout();
	int rc = cli_subagents_transcript(&ctx, thread.get(), &document, 60, 0);
	std::string leaked = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_TRUE(leaked.empty());
	std::string plain = subagents_plain(document);
	EXPECT_NE(plain.find("● "), std::string::npos);
	EXPECT_NE(plain.find("Review heading"), std::string::npos);
	EXPECT_NE(plain.find("Strong answer"), std::string::npos);
	EXPECT_NE(plain.find("First item"), std::string::npos);
	EXPECT_NE(plain.find("int answer = 42;"), std::string::npos);
	EXPECT_EQ(plain.find("**"), std::string::npos);
	EXPECT_EQ(plain.find("```"), std::string::npos);
	EXPECT_EQ(plain.find("PAYLOAD"), std::string::npos);
	EXPECT_EQ(plain.find("Assistant\n"), std::string::npos);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&document), "\033[1m"), nullptr);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&document), "\033[38;"), nullptr);
	subagents_expect_sgr_rows(document, 60);
	EXPECT_EQ(ctx.output.capture, &captured);
	EXPECT_EQ(ctx.output.capture_styled, 1);
	EXPECT_EQ(ctx.output.capture_history, 1);
	EXPECT_EQ(ctx.output.trailing_newlines, 1);
	EXPECT_EQ(ctx.output.command_result_pending, 1);
	EXPECT_STREQ(morph_buf_cstr_borrow(&captured), "ROOT_CAPTURE");
	EXPECT_EQ(presenter.markdown_stream_kind, 123);
	EXPECT_EQ(presenter.markdown_stream_visible, 1);
	EXPECT_EQ(presenter.reasoning_stream_offset, 7u);
	EXPECT_STREQ(morph_buf_cstr_borrow(&presenter.markdown_stream_text), "ROOT_STREAM");
	morph_buf_cleanup(&presenter.markdown_stream_text);
}

TEST(CliSubagentsTest, CompactToolsReuseTheFeedAndDetailsRetainFullArgumentsAndResults)
{
	SubagentsColumns columns;
	auto thread = make_thread("/root/worker");
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(), R"({"type":"tool","name":"tool.call",
		"data":{"agent_id":"/root/worker","tool":"file_read","tool_call_id":"read",
		"args":{"file_path":"src/example.c","hidden":"RAW_ARGUMENT_SENTINEL"}}})"),
		  0);
	ASSERT_EQ(cli_subagents_thread_replay(thread.get(), R"({"type":"tool","name":"tool.result",
		"data":{"agent_id":"/root/worker","tool":"file_read","tool_call_id":"read",
		"result":"{\"ok\":true,\"data\":{\"content\":\"FIRST_LINE\\nSECOND_LINE\\nTHIRD_LINE\\nRESULT_TAIL\"}}"}})"),
		  0);
	struct cli_application ctx{};
	morph_buf_t compact MORPH_CLEANUP(morph_buf_cleanup) = {};
	morph_buf_t details MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&compact, 256), 0);
	ASSERT_EQ(morph_buf_init(&details, 256), 0);
	ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &compact, 80, 0), 0);
	auto *tool = static_cast<const cli_transcript_tool *>(
		morph_array_borrow(&thread->transcript->tools, 0));
	ASSERT_NE(tool, nullptr);
	testing::internal::CaptureStdout();
	cli_transcript_render_tool(&ctx, tool, 1);
	std::string primary = testing::internal::GetCapturedStdout();
	EXPECT_EQ(primary, morph_buf_cstr_borrow(&compact));
	std::string plain = subagents_plain(compact);
	EXPECT_NE(plain.find("src/example.c"), std::string::npos);
	EXPECT_NE(plain.find("FIRST_LINE"), std::string::npos);
	EXPECT_EQ(plain.find("RAW_ARGUMENT_SENTINEL"), std::string::npos);
	EXPECT_EQ(plain.find("RESULT_TAIL"), std::string::npos);
	EXPECT_EQ(plain.find("\"content\""), std::string::npos);
	ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &details, 80, 1), 0);
	plain = subagents_plain(details);
	EXPECT_NE(plain.find("RAW_ARGUMENT_SENTINEL"), std::string::npos);
	EXPECT_NE(plain.find("RESULT_TAIL"), std::string::npos);
	subagents_expect_sgr_rows(details, 80);
}

TEST(CliSubagentsTest, ReasoningIsQuietByDefaultAndFullyScrollableInDetailsWithoutColor)
{
	auto thread = make_thread("/root/worker");
	std::string reasoning = "Check dependencies first.\n";
	for (int i = 0; i < 4000; i++)
		reasoning += "中文🙂";
	reasoning += "\nREASONING_TAIL";
	ASSERT_EQ(subagents_text_event(thread.get(), "react.reasoning.delta", reasoning.c_str()),
		  0);
	struct cli_application ctx{};
	ctx.output.color_disabled = 1;
	for (int width : {120, 80, 60}) {
		morph_buf_t compact MORPH_CLEANUP(morph_buf_cleanup) = {};
		morph_buf_t details MORPH_CLEANUP(morph_buf_cleanup) = {};
		ASSERT_EQ(morph_buf_init(&compact, 256), 0);
		ASSERT_EQ(morph_buf_init(&details, 1024), 0);
		ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &compact, width, 0), 0);
		EXPECT_NE(strstr(compact.data, "Check dependencies first."), nullptr);
		EXPECT_EQ(strstr(compact.data, "REASONING_TAIL"), nullptr);
		EXPECT_EQ(strstr(compact.data, "Reasoning\n"), nullptr);
		ASSERT_EQ(cli_subagents_transcript(&ctx, thread.get(), &details, width, 1), 0);
		EXPECT_NE(strstr(details.data, "REASONING_TAIL"), nullptr);
		EXPECT_EQ(strchr(details.data, '\033'), nullptr);
		EXPECT_EQ(utf8valid(details.data), nullptr);
		size_t emoji_count = 0;
		std::string full = details.data;
		for (size_t offset = 0; (offset = full.find("🙂", offset)) != std::string::npos;
		     offset += strlen("🙂"))
			emoji_count++;
		EXPECT_EQ(emoji_count, 4000u);
	}
}
