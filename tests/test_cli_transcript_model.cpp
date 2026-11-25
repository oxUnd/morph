#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/presentation/transcript_details.h"
#include <nb/cleanup.h>
#include <nothing/tui/terminal/text.h>
#include "persistence/orm/session_history.h"
#include "cJSON.h"
#include "errors.h"
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
			return NB_ERR_INVAL;

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
		return static_cast<const cli_transcript_tool *>(nb_vec_at(&model->tools, index));
	}

	int Result(const char *id, const char *text)
	{
		cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!data || !cJSON_AddStringToObject(data, "tool_call_id", id) ||
		    !cJSON_AddStringToObject(data, "result", text))
			return NB_ERR_NOMEM;

		struct morph_event event{};
		event.type = MORPH_EVENT_TOOL;
		event.name = "tool.result";
		event.data = data;
		return cli_transcript_model_reduce(model, &event, 2, &change);
	}
};

TEST_F(CliTranscriptModelTest, DetailsSeparateOutputsAndKeepCompleteEnvelopeInRawMode)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
		R"({"tool":"exec","tool_call_id":"shell","args":{"command":"printf test",)"
		R"("options":{"enabled":true,"values":[1,null,"text"]}}})",
		1), 0);
	ASSERT_EQ(Result("shell", R"({"ok":true,"data":{"stdout":"OUTPUT_SENTINEL\n",)"
		R"("stderr":"ERROR_SENTINEL\n","exit_code":0},"ui":{"label":"RAW_UI_ONLY"},)"
		R"("meta":{"version":"RAW_META_ONLY"}})"), 0);
	for (int columns : {120, 80, 60}) {
		nb_buf formatted NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_EQ(cli_transcript_details_append(&formatted, model, columns, 0, 0), 0);
		const std::string text = nb_buf_cstr(&formatted);
		EXPECT_LT(text.find("Arguments"), text.find("Standard output"));
		EXPECT_LT(text.find("Standard output"), text.find("OUTPUT_SENTINEL"));
		EXPECT_LT(text.find("Standard error"), text.find("ERROR_SENTINEL"));
		EXPECT_NE(text.find("Exit 0"), std::string::npos);
		EXPECT_NE(text.find("\"enabled\":"), std::string::npos);
		EXPECT_NE(text.find("null"), std::string::npos);
		EXPECT_EQ(text.find("RAW_UI_ONLY"), std::string::npos);
		EXPECT_EQ(text.find("RAW_META_ONLY"), std::string::npos);
		for (size_t offset = 0; offset < text.size();) {
			size_t end = text.find('\n', offset);
			if (end == std::string::npos)
				end = text.size();

			const std::string row = text.substr(offset, end - offset);
			EXPECT_LE(nt_terminal_display_width_ansi(row.c_str()),
				static_cast<size_t>(columns));
			offset = end + 1;
		}

		nb_buf raw NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_EQ(cli_transcript_details_append(&raw, model, columns, 0, 1), 0);
		const std::string original = nb_buf_cstr(&raw);
		EXPECT_NE(original.find("Arguments JSON"), std::string::npos);
		EXPECT_NE(original.find("Result JSON"), std::string::npos);
		EXPECT_NE(original.find("RAW_UI_ONLY"), std::string::npos);
		EXPECT_NE(original.find("RAW_META_ONLY"), std::string::npos);
	}
}

TEST_F(CliTranscriptModelTest, DetailsShowPatchOnceWithLineNumbersAndPreserveRawInput)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
		R"({"tool":"apply_patch","tool_call_id":"patch","args":{"input":)"
		R"("*** Begin Patch\n*** Update File: file.c\n@@\n-old\n)"
		R"(+NEW_SENTINEL\n*** End Patch"}})",
		1), 0);
	ASSERT_EQ(Result("patch", R"({"ok":true,"data":{"changed":true,"checkpoint":{"id":7}},)"
		R"("ui":{"patch_diff":"*** Update File: file.c\n@@ -42,1 +73,1 @@\n-old\n)"
		R"(+NEW_SENTINEL\n"}})"), 0);
	nb_buf formatted NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(cli_transcript_details_append(&formatted, model, 80, 0, 0), 0);
	const std::string text = nb_buf_cstr(&formatted);
	const size_t change = text.find("NEW_SENTINEL");
	ASSERT_NE(change, std::string::npos);
	EXPECT_EQ(text.find("NEW_SENTINEL", change + 1), std::string::npos);
	EXPECT_NE(text.find("   42       -old"), std::string::npos);
	EXPECT_NE(text.find("         73 +NEW_SENTINEL"), std::string::npos);
	EXPECT_EQ(text.find("patch_diff"), std::string::npos);
	EXPECT_EQ(text.find("checkpoint"), std::string::npos);
	nb_buf raw NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(cli_transcript_details_append(&raw, model, 80, 0, 1), 0);
	EXPECT_NE(std::string(nb_buf_cstr(&raw)).find("*** Begin Patch"), std::string::npos);
	EXPECT_NE(std::string(nb_buf_cstr(&raw)).find("checkpoint"), std::string::npos);
	EXPECT_NE(std::string(nb_buf_cstr(&raw)).find("patch_diff"), std::string::npos);
}

TEST_F(CliTranscriptModelTest, DetailsKeepReasoningOrderAndNeutralizeTerminalControls)
{
	ASSERT_EQ(cli_transcript_model_reasoning_append(model, "FIRST_REASON", 12, 1), 0);
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
		R"({"tool":"file_read","tool_call_id":"read","args":{"file_path":"file.txt"}})",
		1), 0);
	ASSERT_EQ(Result("read",
		R"({"ok":true,"data":{"content":"FILE_BODY\n","returned_lines":1}})"), 0);
	const char *reason = "LAST_REASON\033[2J";
	ASSERT_EQ(cli_transcript_model_reasoning_append(model, reason, strlen(reason), 1), 0);
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(cli_transcript_details_append(&text, model, 60, 0, 0), 0);
	const std::string document = nb_buf_cstr(&text);
	EXPECT_LT(document.find("FIRST_REASON"), document.find("FILE_BODY"));
	EXPECT_LT(document.find("FILE_BODY"), document.find("LAST_REASON"));
	EXPECT_EQ(document.find("\033[2J"), std::string::npos);
	EXPECT_EQ(document.find("\033["), std::string::npos);
}

TEST_F(CliTranscriptModelTest, CreatePreservesExistingModelOwner)
{
	auto *saved = model;
	EXPECT_EQ(cli_transcript_model_create(&model), NB_ERR_EXISTS);
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
	ASSERT_EQ(model->reasoning.len, 2u);
	const auto *first =
		static_cast<const cli_transcript_reasoning *>(nb_vec_at(&model->reasoning, 0));
	const auto *second =
		static_cast<const cli_transcript_reasoning *>(nb_vec_at(&model->reasoning, 1));
	EXPECT_EQ(first->after_tools, 0u);
	EXPECT_EQ(second->after_tools, 1u);
	EXPECT_STREQ(nb_buf_cstr(&second->text), "middle after");
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
	EXPECT_STREQ(nb_buf_cstr(&Tool(0)->stream), "hello world");
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
			"{\"tool\":\"background\",\"tool_call_id\":\"poll\","
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
			       std::to_string(NB_ERR_TIMEOUT) + "}";
	result.payload_json = metadata.data();
	size_t index;
	ASSERT_EQ(cli_transcript_model_restore(model, call, &result, &index), 0);
	cJSON_Delete(call);
	EXPECT_STREQ(Tool(index)->result, "full result");
	EXPECT_EQ(Tool(index)->state, -1);
	EXPECT_STREQ(Tool(index)->error,
		     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_TIMEOUT));
	EXPECT_TRUE(Tool(index)->timed_out);
	EXPECT_TRUE(Tool(index)->historical);
}

TEST_F(CliTranscriptModelTest, PatchExecutionDiffReplacesPreviewAndSurvivesHistoryReplay)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
		"{\"tool\":\"apply_patch\",\"tool_call_id\":\"patch\",\"args\":{}}", 1), 0);
	ASSERT_EQ(cli_transcript_model_set_preview(model, 0, "outdated preview"), 0);
	const char *result_json = "{\"ok\":true,\"data\":{\"changed\":true},\"ui\":{"
		"\"patch_diff\":\"*** Update File: file.c\\n@@ -40,1 +70,1 @@\\n-old\\n+new\\n\"}}";
	cJSON *event NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	ASSERT_NE(event, nullptr);
	cJSON_AddStringToObject(event, "tool_call_id", "patch");
	cJSON_AddStringToObject(event, "result", result_json);
	struct morph_event finished{};
	finished.name = "tool.result";
	finished.type = MORPH_EVENT_TOOL;
	finished.data = event;
	ASSERT_EQ(cli_transcript_model_reduce(model, &finished, 2, &change), 0);
	EXPECT_NE(std::string(Tool(0)->patch_preview).find("@@ -40,1 +70,1 @@"),
		std::string::npos);
	cJSON *call NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		"{\"name\":\"apply_patch\",\"tool_call_id\":\"replay\",\"arguments\":\"{}\"}");
	ASSERT_NE(call, nullptr);
	struct model_history_item result{};
	result.content = const_cast<char *>(result_json);
	size_t index = SIZE_MAX;
	ASSERT_EQ(cli_transcript_model_restore(model, call, &result, &index), 0);
	EXPECT_TRUE(Tool(index)->historical);
	EXPECT_STREQ(Tool(index)->patch_preview, Tool(0)->patch_preview);
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

TEST_F(CliTranscriptModelTest, LegacyProcessNameIsExplicitlyDeprecatedInDisplay)
{
	ASSERT_EQ(Apply(MORPH_EVENT_TOOL, "tool.call",
		R"({"tool":"process","tool_call_id":"legacy","args":{"action":"wait",
		"session_id":"proc_old"}})", 1), 0);
	EXPECT_TRUE(Tool(0)->shell);
	EXPECT_STREQ(Tool(0)->name, "process");
	nb_buf row{};
	ASSERT_EQ((nb_buf_init(&row), nb_buf_reserve(&row, 128) ? 0 : NB_ERR_NOMEM), 0);
	for (int columns : {120, 80, 60, 40}) {
		nb_buf_clear(&row);
		EXPECT_EQ(cli_transcript_live_text(model, &row, columns, 0), 1);
		EXPECT_NE(std::string(nb_buf_cstr(&row)).find("process [deprecated]"),
			  std::string::npos);
	}

	nb_buf_cleanup(&row);
}
