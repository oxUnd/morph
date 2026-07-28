#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "event/event.h"

int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_reset(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
int cli_presentation_event(struct cli_context *ctx,
			   const struct morph_event *ev);
}

#include <string>

class CliPresentationTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void SetUp() override
	{
		cli_set_color_enabled(0);
		ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
		ctx.presentation_ready = 1;
		ctx.turn_active = 1;
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
	}

	void TearDown() override
	{
		cli_presentation_cleanup(&ctx);
		cli_set_color_enabled(1);
	}

	void Emit(enum morph_event_type type, const char *name,
		  const char *phase, cJSON *data)
	{
		struct morph_event event{
			type, name, phase, nullptr, data, "turn_test"
		};
		ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	}

	static cJSON *TextData(const char *text)
	{
		cJSON *data = cJSON_CreateObject();
		cJSON_AddStringToObject(data, "text", text);
		return data;
	}
};

TEST_F(CliPresentationTest, OncePrintsOrderedPlainProgress)
{
	cJSON *thought_delta = TextData("Inspecting the CLI");
	cJSON *thought_end = TextData("Inspecting the CLI");
	cJSON *tool = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *observation = TextData("run.c\noutput.c");
	cJSON *final = TextData("Result with **raw markdown**.");

	cJSON_AddStringToObject(tool, "tool", "file_list");
	cJSON_AddItemToObject(tool, "args", args);
	cJSON_AddStringToObject(args, "path", "src/sapi/cli");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta",
	     thought_delta);
	Emit(MORPH_EVENT_REACT, "react.thought.end", "end", thought_end);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", tool);
	Emit(MORPH_EVENT_REACT, "react.observation", "end", observation);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("\033"), std::string::npos);
	EXPECT_NE(output.find("thought: Inspecting the CLI"),
		  std::string::npos);
	EXPECT_EQ(output.find("Inspecting the CLI",
			      output.find("Inspecting the CLI") + 1),
		  std::string::npos);
	EXPECT_NE(output.find("tool: file_list {\"path\":\"src/sapi/cli\"}"),
		  std::string::npos);
	EXPECT_NE(output.find("observation:\nrun.c\noutput.c"),
		  std::string::npos);
	EXPECT_NE(output.find("final:\nResult with **raw markdown**."),
		  std::string::npos);

	cJSON_Delete(thought_delta);
	cJSON_Delete(thought_end);
	cJSON_Delete(tool);
	cJSON_Delete(observation);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, OnceDiscardsProvisionalFinalThought)
{
	cJSON *delta = TextData("The final answer");
	cJSON *final = TextData("The final answer");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", delta);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "final:\nThe final answer\n");

	cJSON_Delete(delta);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, OncePrintsThinkingStatusAsPlainText)
{
	cJSON *thinking = TextData("");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thinking", "begin", thinking);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "status: Thinking…\n");
	EXPECT_EQ(output.find("\033"), std::string::npos);
	cJSON_Delete(thinking);
}

TEST_F(CliPresentationTest, OncePrintsReasoningAsLabeledPlainText)
{
	cJSON *reasoning = TextData("Checking constraints");
	cJSON *final = TextData("Done");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta",
	     reasoning);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "reasoning: Checking constraints\nfinal:\nDone\n");
	EXPECT_EQ(output.find("\033"), std::string::npos);
	cJSON_Delete(reasoning);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, OnceReportsArtifactWithoutRendering)
{
	cJSON *artifact = cJSON_CreateObject();
	cJSON_AddStringToObject(artifact, "kind", "image");
	cJSON_AddStringToObject(artifact, "path", "/tmp/generated.png");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", artifact);
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", artifact);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "artifact: image /tmp/generated.png\n");
	cJSON_Delete(artifact);
}

TEST_F(CliPresentationTest, JsonModeEmitsOneNdjsonObject)
{
	cJSON *data = TextData("hello");
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	struct morph_event event{
		MORPH_EVENT_REACT, "react.final", "end", "final answer",
		data, "turn_test"
	};

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	std::string output = testing::internal::GetCapturedStdout();
	ASSERT_FALSE(output.empty());
	ASSERT_EQ(output.back(), '\n');

	cJSON *parsed = cJSON_Parse(output.c_str());
	ASSERT_NE(parsed, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(parsed, "name")->valuestring,
		     "react.final");
	cJSON_Delete(parsed);
	cJSON_Delete(data);
}

TEST_F(CliPresentationTest, InteractiveUsesCompactFinalWithoutLabel)
{
	cJSON *final = TextData("# Compact\n\n- first item\n- second item");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("final:"), std::string::npos);
	EXPECT_EQ(output.find("\n• "), 0u);
	EXPECT_NE(output.find("Compact"), std::string::npos);
	EXPECT_NE(output.find("\n  • first item"), std::string::npos);
	EXPECT_NE(output.find("\n  • second item"), std::string::npos);
	EXPECT_EQ(output.find("Compact", output.find("Compact") + 1),
		  std::string::npos);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveStreamsFinalMarkdownDeltas)
{
	cJSON *first = TextData("# Stream");
	cJSON *second = TextData("ed\n\n- item");
	cJSON *final = TextData("fallback final payload");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("\n• "), 0u);
	EXPECT_NE(output.find("\033[?2026h"), std::string::npos);
	EXPECT_NE(output.find("\033[?2026l"), std::string::npos);
	EXPECT_NE(output.find("Stream"), std::string::npos);
	EXPECT_NE(output.find("• item"), std::string::npos);
	EXPECT_EQ(output.find("fallback final payload"), std::string::npos);
	EXPECT_EQ(ctx.markdown_stream, nullptr);
	EXPECT_EQ(ctx.final_rendered, 1);

	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractivePromotesProvisionalContentDeltas)
{
	cJSON *first = TextData("# Native");
	cJSON *second = TextData(" stream");
	cJSON *final = TextData("fallback final payload");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("\n• "), 0u);
	EXPECT_NE(output.find("Native stream"), std::string::npos);
	EXPECT_EQ(output.find("fallback final payload"), std::string::npos);
	EXPECT_EQ(ctx.markdown_stream, nullptr);
	EXPECT_EQ(ctx.final_rendered, 1);

	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveStreamsReasoningAsDimText)
{
	cJSON *first = TextData("Inspect");
	cJSON *second = TextData("ing the state");
	cJSON *answer = TextData("Done");
	cJSON *final = TextData("fallback final payload");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_set_color_enabled(1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", answer);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[2m• Reasoning  \033[0m"),
		  std::string::npos);
	EXPECT_NE(output.find("\033[2mInspect\033[0m"),
		  std::string::npos);
	EXPECT_NE(output.find("\033[2ming the state\033[0m"),
		  std::string::npos);
	EXPECT_NE(output.find("\n\033[1m\033[36m•\033[0m "),
		  std::string::npos);
	EXPECT_EQ(output.find("Reasoning", output.find("Reasoning") + 1),
		  std::string::npos);
	EXPECT_EQ(output.find("fallback final payload"), std::string::npos);
	EXPECT_EQ(ctx.event_stream_visible, 0);
	EXPECT_EQ(ctx.final_rendered, 1);

	cli_set_color_enabled(0);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(answer);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveReasoningRespectsDisabledColor)
{
	cJSON *reasoning = TextData("Checking constraints");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta",
	     reasoning);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("• Reasoning  Checking constraints"),
		  std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);
	cJSON_Delete(reasoning);
}

TEST_F(CliPresentationTest, InteractiveToolThoughtDoesNotHideLaterFinal)
{
	cJSON *thought = TextData("Calling a tool");
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *final = TextData("Authoritative final");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(call, "tool", "file_list");
	cJSON_AddStringToObject(args, "path", ".");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", thought);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Calling a tool"), std::string::npos);
	EXPECT_NE(output.find("Authoritative final"), std::string::npos);
	EXPECT_EQ(ctx.final_rendered, 1);

	cJSON_Delete(thought);
	cJSON_Delete(call);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveShowsThinkingStatus)
{
	cJSON *thinking = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thinking", "begin", thinking);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Reasoning…"), std::string::npos);
	cJSON_Delete(thinking);
}

TEST_F(CliPresentationTest, InteractiveCoalescesBackgroundLifecycle)
{
	cJSON *started = TextData("");
	cJSON *ready = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	struct morph_event begin{
		MORPH_EVENT_BACKGROUND, "background.started", "begin",
		"memory consolidation queued", started, "turn_test"
	};
	struct morph_event end{
		MORPH_EVENT_BACKGROUND, "background.ready", "ready",
		"memory consolidation queued", ready, "turn_test"
	};

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &begin), 0);
	ASSERT_EQ(cli_presentation_event(&ctx, &end), 0);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("memory consolidation queued"),
		  std::string::npos);
	EXPECT_EQ(output.find("memory consolidation queued",
			      output.find("memory consolidation queued") + 1),
		  std::string::npos);
	EXPECT_EQ(output.find("Background"), std::string::npos);

	cJSON_Delete(started);
	cJSON_Delete(ready);
}

TEST_F(CliPresentationTest, InteractiveRendersToolArgsAndResultAsTree)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *options = cJSON_CreateObject();
	cJSON *result = cJSON_CreateObject();
	cJSON *result_args = cJSON_CreateObject();
	cJSON *observation = TextData("{\"ok\":true,\"count\":15}");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(call, "tool", "web_search");
	cJSON_AddStringToObject(args, "query", "today's news");
	cJSON_AddNumberToObject(options, "limit", 15);
	cJSON_AddBoolToObject(options, "safe", 1);
	cJSON_AddItemToObject(args, "options", options);
	cJSON_AddItemToObject(call, "args", args);
	cJSON_AddStringToObject(result, "tool", "web_search");
	cJSON_AddItemToObject(result, "args", result_args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	Emit(MORPH_EVENT_REACT, "react.observation", "end", observation);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("├ query: today's news"), std::string::npos);
	EXPECT_NE(output.find("└ options:"), std::string::npos);
	EXPECT_NE(output.find("├ limit: 15"), std::string::npos);
	EXPECT_NE(output.find("└ safe: true"), std::string::npos);
	EXPECT_NE(output.find("✓ web_search completed"), std::string::npos);
	EXPECT_NE(output.find("├ ok: true"), std::string::npos);
	EXPECT_NE(output.find("└ count: 15"), std::string::npos);
	EXPECT_EQ(output.find("{\"query\""), std::string::npos);
	EXPECT_EQ(output.find("{\"ok\""), std::string::npos);

	cJSON_Delete(call);
	cJSON_Delete(result);
	cJSON_Delete(observation);
}

TEST_F(CliPresentationTest, InteractiveRendersStructuredPlan)
{
	cJSON *event_data = TextData("{\"plans\":[]}");
	cJSON *result = cJSON_CreateObject();
	cJSON *plans = cJSON_CreateArray();
	cJSON *plan = cJSON_CreateObject();
	cJSON *steps = cJSON_CreateArray();
	cJSON *done = cJSON_CreateObject();
	cJSON *active = cJSON_CreateObject();

	cJSON_AddStringToObject(event_data, "tool", "plan");
	cJSON_AddStringToObject(done, "description", "Inspect CLI");
	cJSON_AddStringToObject(done, "status", "completed");
	cJSON_AddItemToArray(steps, done);
	cJSON_AddStringToObject(active, "description", "Implement presenter");
	cJSON_AddStringToObject(active, "status", "in_progress");
	cJSON_AddBoolToObject(active, "active", 1);
	cJSON_AddItemToArray(steps, active);
	cJSON_AddItemToObject(plan, "steps", steps);
	cJSON_AddItemToArray(plans, plan);
	cJSON_AddItemToObject(result, "plans", plans);
	cJSON_AddItemToObject(event_data, "data", result);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Updated plan"), std::string::npos);
	EXPECT_NE(output.find("Inspect CLI"), std::string::npos);
	EXPECT_NE(output.find("Implement presenter"), std::string::npos);
	EXPECT_EQ(output.find("{\"plans\""), std::string::npos);
	cJSON_Delete(event_data);
}
