#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/terminal.h"
#include "sapi/cli/shell_style.h"
#include "util/utf8.h"
#include "agent/react.h"
#include "event/event.h"
#include "http/client.h"

int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_reset(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
void cli_presentation_prepare_prompt(struct cli_context *ctx);
int cli_presentation_event(struct cli_context *ctx,
			   const struct morph_event *ev);
void cli_transcript_toggle(struct cli_context *ctx);
void cli_transcript_finish(struct cli_context *ctx);
void cli_presentation_finish(struct cli_context *ctx);
const char *cli_input_prompt(void);
struct cli_cancel_monitor;
struct cli_cancel_monitor *cli_cancel_monitor_start(int fd);
void cli_cancel_monitor_stop(struct cli_cancel_monitor *monitor);
extern volatile sig_atomic_t cli_sigint_received;
}

#include <string>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

TEST(CliShellStyleTest, ColorsCommandsStringsAndOperatorsWithoutChangingText)
{
	morph_buf_t styled;
	const char *source = "MODE=1 rg '中文 | text' src/ && head -20 > out";
	ASSERT_EQ(morph_buf_init(&styled, 128), 0);
	ASSERT_EQ(cli_shell_style(&styled, source, 0), 0);
	std::string output = morph_buf_cstr(&styled);
	EXPECT_NE(output.find("\033[36mrg\033[0m"), std::string::npos);
	EXPECT_NE(output.find("\033[32m'中文 | text'\033[0m"), std::string::npos);
	EXPECT_NE(output.find("\033[33m&&\033[0m"), std::string::npos);
	EXPECT_NE(output.find("\033[36mhead\033[0m"), std::string::npos);
	EXPECT_EQ(output.find("\033[36mout"), std::string::npos);
	char *plain = utf8_terminal_sanitize_dup(output.c_str(), output.size(),
		UTF8_TERMINAL_TEXT_MULTILINE, nullptr);
	ASSERT_NE(plain, nullptr);
	EXPECT_STREQ(plain, source);
	free(plain);
	morph_buf_cleanup(&styled);
}

TEST(CliShellStyleTest, WrapsUtf8AndDropsTerminalControls)
{
	morph_buf_t styled;
	ASSERT_EQ(morph_buf_init(&styled, 128), 0);
	ASSERT_EQ(cli_shell_style(&styled,
		"printf '中文🙂abcdef'\033]0;unsafe\a | cat", 12), 0);
	std::string output = morph_buf_cstr(&styled);
	EXPECT_EQ(output.find("unsafe"), std::string::npos);
	EXPECT_EQ(utf8valid(output.c_str()), nullptr);
	EXPECT_NE(output.find("\n    "), std::string::npos);
	EXPECT_NE(output.find("\033[36mcat"), std::string::npos);
	morph_buf_cleanup(&styled);
}

TEST(CliShellStyleTest, SummaryHidesOnlyLiteralCurrentDirectoryPrefix)
{
	EXPECT_STREQ(cli_shell_summary("cd /work && ls -la", "/work"), "ls -la");
	EXPECT_STREQ(cli_shell_summary("cd '/work dir'; git status", "/work dir"),
		"git status");
	EXPECT_STREQ(cli_shell_summary("cd -- \"/work dir\" && pwd", "/work dir"), "pwd");
	EXPECT_STREQ(cli_shell_summary("cd . && ls", "/work"), "ls");
	for (const char *source : {"cd /other && ls", "cd $PWD && ls", "cd /work || ls",
		"cd /work & ls", "cd /work", "cd /work && ", "cd '/work && ls",
		"cd /work;; ls", "cd /work extra && ls", "echo cd /work && ls"})
		EXPECT_STREQ(cli_shell_summary(source, "/work"), source);
}

class CliPresentationTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void SetUp() override
	{
		cli_set_color_enabled(0);
		ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
		ctx.presentation_ready = 1;
		ctx.turn_active = 1;
		ASSERT_EQ(cli_terminal_init(&ctx, stdout, STDOUT_FILENO), 0);
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
	}

	void TearDown() override
	{
		cli_presentation_cleanup(&ctx);
		cli_terminal_cleanup(&ctx);
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

TEST(CliInputPromptTest, AnsiSequencesAreMarkedAsInvisible)
{
	cli_set_color_enabled(1);
	const unsigned char *prompt = reinterpret_cast<const unsigned char *>(
		cli_input_prompt());
	int ignored = 0;
	int escapes = 0;
	if (!std::strchr(reinterpret_cast<const char *>(prompt), 0x01))
		GTEST_SKIP() << "readline is not available";

	for (; *prompt; prompt++) {
		if (*prompt == 0x01) {
			EXPECT_EQ(ignored, 0);
			ignored = 1;
		} else if (*prompt == 0x02) {
			EXPECT_EQ(ignored, 1);
			ignored = 0;
		} else if (*prompt == 0x1b) {
			EXPECT_EQ(ignored, 1);
			escapes++;
		}
	}
	EXPECT_EQ(ignored, 0);
	EXPECT_GT(escapes, 0);
}

TEST(CliCancelMonitorTest, StandaloneEscapeRequestsCancellation)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	struct cli_cancel_monitor *monitor = cli_cancel_monitor_start(slave);
	ASSERT_NE(monitor, nullptr);

	react_sigint_flag = 0;
	cli_sigint_received = 0;
	ASSERT_EQ(write(master, "\033", 1), 1);
	for (int i = 0; i < 50 && !react_sigint_flag; i++)
		usleep(10000);
	cli_cancel_monitor_stop(monitor);

	EXPECT_EQ(react_sigint_flag, 1);
	EXPECT_EQ(cli_sigint_received, 1);
	react_sigint_flag = 0;
	http_clear_signal_cancel();
	close(slave);
	close(master);
}

TEST(CliCancelMonitorTest, EscapeSequenceDoesNotCancel)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	struct cli_cancel_monitor *monitor = cli_cancel_monitor_start(slave);
	ASSERT_NE(monitor, nullptr);

	react_sigint_flag = 0;
	cli_sigint_received = 0;
	ASSERT_EQ(write(master, "\033[A", 3), 3);
	usleep(100000);
	cli_cancel_monitor_stop(monitor);

	EXPECT_EQ(react_sigint_flag, 0);
	EXPECT_EQ(cli_sigint_received, 0);
	http_clear_signal_cancel();
	close(slave);
	close(master);
}

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

TEST_F(CliPresentationTest, OncePrintsCompactionProgressAndResult)
{
	cJSON *begin = cJSON_CreateObject();
	cJSON *completed = cJSON_CreateObject();

	cJSON_AddNumberToObject(begin, "iteration", 22);
	cJSON_AddNumberToObject(begin, "before_tokens", 91676);
	cJSON_AddNumberToObject(begin, "after_tokens", 91676);
	cJSON_AddNumberToObject(begin, "compaction_count", 1);
	cJSON_AddNumberToObject(completed, "iteration", 22);
	cJSON_AddNumberToObject(completed, "before_tokens", 91676);
	cJSON_AddNumberToObject(completed, "after_tokens", 15672);
	cJSON_AddNumberToObject(completed, "compaction_count", 1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.compaction.begin", "begin", begin);
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end",
	     completed);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "status: Compressing context…\n"
		"context: compacted 91676 -> 15672 tokens "
		"(iteration 22, pass 1)\n");
	cJSON_Delete(begin);
	cJSON_Delete(completed);
}

TEST_F(CliPresentationTest, InteractivePrintsPersistentCompactionResult)
{
	cJSON *completed = cJSON_CreateObject();

	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON_AddNumberToObject(completed, "iteration", 22);
	cJSON_AddNumberToObject(completed, "before_tokens", 91676);
	cJSON_AddNumberToObject(completed, "after_tokens", 15672);
	cJSON_AddNumberToObject(completed, "compaction_count", 1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end",
	     completed);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Context  compacted 91676 → 15672 tokens "
		"(iteration 22, pass 1)"), std::string::npos);
	cJSON_Delete(completed);
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

TEST_F(CliPresentationTest, InteractiveDefersMediaRenderingUntilFinal)
{
	cJSON *image = cJSON_CreateObject();
	cJSON *video = cJSON_CreateObject();
	cJSON *final = TextData("![generated](/tmp/generated.png)");
	cJSON_AddStringToObject(image, "kind", "image");
	cJSON_AddStringToObject(image, "path", "/tmp/generated.png");
	cJSON_AddStringToObject(video, "kind", "video");
	cJSON_AddStringToObject(video, "path", "/tmp/generated.mp4");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", image);
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", image);
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", video);
	Emit(MORPH_EVENT_ARTIFACT, "artifact.ready", "ready", video);
	EXPECT_FALSE(morph_strmap_contains(&ctx.rendered_artifacts,
					   "/tmp/generated.png"));
	EXPECT_FALSE(morph_strmap_contains(&ctx.rendered_artifacts,
					   "/tmp/generated.mp4"));
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("  └ image: /tmp/generated.png\n"
			      "  └ video: /tmp/generated.mp4\n"), 0u);
	EXPECT_TRUE(morph_strmap_contains(&ctx.rendered_artifacts,
					  "/tmp/generated.png"));
	EXPECT_FALSE(morph_strmap_contains(&ctx.rendered_artifacts,
					   "/tmp/generated.mp4"));
	cJSON_Delete(image);
	cJSON_Delete(video);
	cJSON_Delete(final);
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
	EXPECT_EQ(output.find("\n● "), 0u);
	EXPECT_EQ(output.find("  Compact"), std::string::npos);
	EXPECT_NE(output.find("Compact"), std::string::npos);
	EXPECT_NE(output.find("\n  • first item"), std::string::npos);
	EXPECT_NE(output.find("\n  • second item"), std::string::npos);
	EXPECT_EQ(output.find("Compact", output.find("Compact") + 1),
		  std::string::npos);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveDistinguishesToolAndFinalMarkers)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *final = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(call, "tool", "file_list");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("◉ file_list"), std::string::npos);
	EXPECT_NE(output.find("\n● "), std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);

	cJSON_Delete(call);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveColorsToolAndFinalMarkersDifferently)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *final = TextData("Done");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_set_color_enabled(1);

	cJSON_AddStringToObject(call, "tool", "file_list");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[33m◉\033[0m "),
		  std::string::npos);
	EXPECT_NE(output.find("\n\033[1m●\033[0m "),
		  std::string::npos);

	cJSON_Delete(call);
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

	EXPECT_EQ(output.find("\n● "), 0u);
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

TEST_F(CliPresentationTest, StreamPrefixWaitsForVisibleMarkdown)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *partial = TextData("A buffered paragraph");
	cJSON *final = TextData("A buffered paragraph");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", partial);
	std::string pending = testing::internal::GetCapturedStdout();
	EXPECT_EQ(pending.find("●"), std::string::npos);
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string rendered = testing::internal::GetCapturedStdout();
	EXPECT_NE(rendered.find("● "), std::string::npos);
	EXPECT_NE(rendered.find("A buffered paragraph"), std::string::npos);
	EXPECT_EQ(rendered.find("●"), rendered.rfind("●"));
	cJSON_Delete(partial);
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

	EXPECT_EQ(output.find("\n● "), 0u);
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
	EXPECT_NE(output.find("\n\033[1m●\033[0m "),
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

TEST_F(CliPresentationTest, OnceNeutralizesObservationControlSequences)
{
	cJSON *observation = TextData(
		"before\033[2J\033]52;c;secret\aafter\rhidden");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", observation);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("beforeafter\\rhidden"), std::string::npos);
	EXPECT_EQ(output.find("secret"), std::string::npos);
	EXPECT_EQ(output.find('\033'), std::string::npos);
	cJSON_Delete(observation);
}

TEST_F(CliPresentationTest, InteractiveStripsSplitFinalEscapeSequence)
{
	cJSON *first = TextData("before\033[");
	cJSON *second = TextData("2Jafter");
	cJSON *final = TextData("fallback");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_set_color_enabled(1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("beforeafter"), std::string::npos);
	EXPECT_EQ(output.find("2Jafter"), std::string::npos);
	EXPECT_EQ(output.find("fallback"), std::string::npos);
	cli_set_color_enabled(0);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveStripsSplitReasoningOscSequence)
{
	cJSON *first = TextData("before\033]52;c;sec");
	cJSON *second = TextData("ret\aafter");
	cJSON *final = TextData("done");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_set_color_enabled(1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("before"), std::string::npos);
	EXPECT_NE(output.find("after"), std::string::npos);
	EXPECT_EQ(output.find("secret"), std::string::npos);
	EXPECT_EQ(output.find("52;c"), std::string::npos);
	cli_set_color_enabled(0);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST(CliOutputSafetyTest, PrintfBlocksDangerousSequencesButKeepsTrustedSgr)
{
	cli_set_color_enabled(1);
	testing::internal::CaptureStdout();
	cli_printf("\033[31mtrusted\033[0m %s\n",
		"bad\033[2J\033]52;c;secret\aend");
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[31mtrusted\033[0m"),
		std::string::npos);
	EXPECT_NE(output.find("badend"), std::string::npos);
	EXPECT_EQ(output.find("secret"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	cli_set_color_enabled(0);
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

	EXPECT_NE(output.find("Thinking…"), std::string::npos);
	cJSON_Delete(thinking);
}

TEST_F(CliPresentationTest, PreparingPromptClearsLiveStatus)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	cli_terminal_live_set(&ctx, "Running tool…");
	ASSERT_EQ(cli_terminal_live_active(&ctx), 1);
	cli_presentation_prepare_prompt(&ctx);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(cli_terminal_live_active(&ctx), 0);
	EXPECT_NE(output.find("Running tool…"), std::string::npos);
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

TEST_F(CliPresentationTest, InteractiveRendersMcpFailureAsOneTree)
{
	cJSON *connecting = cJSON_CreateObject();
	cJSON *failed = cJSON_CreateObject();
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(connecting, "server", "meego");
	cJSON_AddStringToObject(connecting, "transport", "http");
	cJSON_AddStringToObject(failed, "server", "meego");
	cJSON_AddStringToObject(failed, "transport", "http");
	cJSON_AddNumberToObject(failed, "error_code",
				MORPH_ERR_NOT_CONFIGURED);
	cJSON_AddStringToObject(
		failed, "error",
		"Missing MCP token: environment variable 'MEEGO_TOKEN' is not set");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_MCP, "mcp.connecting", "begin", connecting);
	Emit(MORPH_EVENT_MCP, "mcp.failed", "failed", failed);
	std::string output = testing::internal::GetCapturedStdout();
	size_t root = output.find("MCP meego");

	ASSERT_NE(root, std::string::npos);
	EXPECT_EQ(output.find("MCP meego", root + 1), std::string::npos);
	EXPECT_NE(output.find("├ Connecting"), std::string::npos);
	EXPECT_NE(output.find(
		"└ ✗ Failed · Missing MCP token: environment variable "
		"'MEEGO_TOKEN' is not set"), std::string::npos);
	EXPECT_EQ(ctx.mcp_tree_active, 0);

	cJSON_Delete(connecting);
	cJSON_Delete(failed);
}

TEST_F(CliPresentationTest, InteractiveRendersMcpSuccessAsOneTree)
{
	cJSON *connecting = cJSON_CreateObject();
	cJSON *connected = cJSON_CreateObject();
	cJSON *discovering = cJSON_CreateObject();
	cJSON *ready = cJSON_CreateObject();
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(connecting, "server", "meego");
	cJSON_AddStringToObject(connected, "server", "meego");
	cJSON_AddStringToObject(discovering, "server", "meego");
	cJSON_AddStringToObject(ready, "server", "meego");
	cJSON_AddNumberToObject(ready, "tools", 12);
	cJSON_AddNumberToObject(ready, "resources", 3);
	cJSON_AddNumberToObject(ready, "prompts", 2);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_MCP, "mcp.connecting", "begin", connecting);
	Emit(MORPH_EVENT_MCP, "mcp.connected", "end", connected);
	Emit(MORPH_EVENT_MCP, "mcp.discovering", "begin", discovering);
	Emit(MORPH_EVENT_MCP, "mcp.ready", "ready", ready);
	std::string output = testing::internal::GetCapturedStdout();
	size_t root = output.find("MCP meego");

	ASSERT_NE(root, std::string::npos);
	EXPECT_EQ(output.find("MCP meego", root + 1), std::string::npos);
	EXPECT_NE(output.find("├ Connecting"), std::string::npos);
	EXPECT_NE(output.find("├ Connected"), std::string::npos);
	EXPECT_NE(output.find("├ Discovering capabilities"),
		  std::string::npos);
	EXPECT_NE(output.find("└ ✓ Ready · 12 tools, 3 resources, 2 prompts"),
		  std::string::npos);
	EXPECT_EQ(ctx.mcp_tree_active, 0);

	cJSON_Delete(connecting);
	cJSON_Delete(connected);
	cJSON_Delete(discovering);
	cJSON_Delete(ready);
}

TEST_F(CliPresentationTest, InteractiveExpandsCompleteToolTranscript)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *options = cJSON_CreateObject();
	cJSON *result = cJSON_CreateObject();
	cJSON *result_args = cJSON_CreateObject();
	cJSON *observation = TextData("{\"ok\":true,\"count\":15}");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.tool_details = 1;

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

	EXPECT_NE(output.find("query:\n    today's news"), std::string::npos);
	EXPECT_NE(output.find("options:"), std::string::npos);
	EXPECT_NE(output.find("limit:\n    15"), std::string::npos);
	EXPECT_NE(output.find("safe:\n    true"), std::string::npos);
	EXPECT_NE(output.find("◉ web_search"), std::string::npos);
	EXPECT_NE(output.find("ok:\n    true"), std::string::npos);
	EXPECT_NE(output.find("count:\n    15"), std::string::npos);
	EXPECT_EQ(output.find("{\"query\""), std::string::npos);
	EXPECT_EQ(output.find("{\"ok\""), std::string::npos);

	cJSON_Delete(call);
	cJSON_Delete(result);
	cJSON_Delete(observation);
}

TEST_F(CliPresentationTest, InteractiveRendersApplyPatchAsDiff)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	const char *patch =
		"*** Begin Patch\n"
		"*** Update File: src/example.c\n"
		"@@\n"
		"-old value\n"
		"+new value\n"
		"*** End Patch";
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.tool_details = 1;

	cJSON_AddStringToObject(call, "tool", "apply_patch");
	cJSON_AddStringToObject(call, "toolTitle", "Apply patch");
	cJSON_AddStringToObject(args, "input", patch);
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("apply_patch src/example.c"), std::string::npos);
	EXPECT_NE(output.find("*** Update File: src/example.c"),
		  std::string::npos);
	EXPECT_NE(output.find("-old value"), std::string::npos);
	EXPECT_NE(output.find("+new value"), std::string::npos);
	EXPECT_NE(output.find("*** End Patch"), std::string::npos);
	EXPECT_EQ(output.find("input:"), std::string::npos);
	EXPECT_EQ(output.find("patch display truncated"), std::string::npos);

	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, InteractiveExpandsEmbeddedJsonOneLevel)
{
	cJSON *observation = TextData("structured result");
	cJSON *result = cJSON_CreateObject();
	const char *embedded =
		"{\"ok\":true,\"identity\":\"user\","
		"\"data\":{\"chats\":[1,2,3]},"
		"\"messages\":[{\"id\":\"one\"},{\"id\":\"two\"}]}";
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(result, "payload", embedded);
	cJSON_AddItemToObject(observation, "data", result);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", observation);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("├ ok: true"), std::string::npos);
	EXPECT_NE(output.find("├ identity: user"), std::string::npos);
	EXPECT_NE(output.find("├ data: {...} 1 item"), std::string::npos);
	EXPECT_NE(output.find("└ messages: [...] 2 items"),
		  std::string::npos);
	EXPECT_EQ(output.find("\"chats\""), std::string::npos);
	EXPECT_EQ(output.find("[0]"), std::string::npos);

	cJSON_Delete(observation);
}

TEST_F(CliPresentationTest, InteractiveWrapsLongTranscriptStrings)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	const char *old_columns = getenv("COLUMNS");
	std::string saved_columns = old_columns ? old_columns : "";
	int had_columns = old_columns != nullptr;
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.tool_details = 1;

	ASSERT_EQ(setenv("COLUMNS", "52", 1), 0);
	cJSON_AddStringToObject(call, "tool", "bash_exec");
	cJSON_AddStringToObject(
		args, "command",
		"NOTICE=1 lark-cli im +chat-messages-list "
		"--chat-id oc_1a6eff8d491c1dd23b76789f01d625b "
		"--page-size 100");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("    $ NOTICE=1 lark-cli im"),
		  std::string::npos);
	EXPECT_NE(output.find("+chat-messages-list"), std::string::npos);
	EXPECT_NE(output.find("--chat-id"), std::string::npos);
	EXPECT_NE(output.find("--page-size 100"), std::string::npos);
	EXPECT_EQ(output.find("(truncated)"), std::string::npos);

	if (had_columns)
		ASSERT_EQ(setenv("COLUMNS", saved_columns.c_str(), 1), 0);
	else
		ASSERT_EQ(unsetenv("COLUMNS"), 0);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, ToolDetailsTogglePreservesOrderAndCompletedAnswer)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *first = cJSON_Parse(
		"{\"tool\":\"file_read\",\"tool_call_id\":\"first\","
		"\"args\":{\"file_path\":\"first.txt\",\"private_arg\":\"hidden argument\"}}");
	cJSON *second = cJSON_Parse(
		"{\"tool\":\"file_read\",\"tool_call_id\":\"second\","
		"\"args\":{\"file_path\":\"second.txt\"}}");
	cJSON *first_result = cJSON_Parse(
		"{\"tool\":\"file_read\",\"tool_call_id\":\"first\","
		"\"result\":\"FIRST OUTPUT\"}");
	cJSON *second_result = cJSON_Parse(
		"{\"tool\":\"file_read\",\"tool_call_id\":\"second\","
		"\"result\":\"SECOND OUTPUT\"}");
	cJSON *thought = TextData("Checking files");
	cJSON *final = TextData("Files checked");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.end", "end", thought);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", first);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", second);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", second_result);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", first_result);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	cli_presentation_finish(&ctx);
	ctx.turn_active = 0;
	std::string compact = testing::internal::GetCapturedStdout();
	EXPECT_EQ(compact.find("hidden argument"), std::string::npos);
	EXPECT_EQ(compact.find("FIRST OUTPUT"), std::string::npos);
	EXPECT_EQ(compact.find("succeeded"), std::string::npos);
	EXPECT_NE(compact.find("first.txt"), std::string::npos);
	EXPECT_NE(compact.find("Files checked"), std::string::npos);
	EXPECT_EQ(compact.find("\033[2J"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_transcript_finish(&ctx);
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());

	testing::internal::CaptureStdout();
	cli_transcript_toggle(&ctx);
	std::string expanded = testing::internal::GetCapturedStdout();
	EXPECT_NE(expanded.find("hidden argument"), std::string::npos);
	EXPECT_LT(expanded.find("first.txt"), expanded.find("FIRST OUTPUT"));
	EXPECT_LT(expanded.find("FIRST OUTPUT"), expanded.find("second.txt"));
	EXPECT_LT(expanded.find("second.txt"), expanded.find("SECOND OUTPUT"));
	EXPECT_NE(expanded.find("Tool details"), std::string::npos);
	EXPECT_NE(expanded.find("\033[?1049h"), std::string::npos);
	EXPECT_EQ(expanded.find("Files checked"), std::string::npos);
	EXPECT_EQ(ctx.turn_active, 0);

	testing::internal::CaptureStdout();
	cli_transcript_toggle(&ctx);
	std::string collapsed = testing::internal::GetCapturedStdout();
	EXPECT_EQ(collapsed.find("FIRST OUTPUT"), std::string::npos);
	EXPECT_EQ(collapsed.find("Read  first.txt"), std::string::npos);
	EXPECT_EQ(collapsed.find("succeeded"), std::string::npos);
	EXPECT_NE(collapsed.find("\033[?1049l"), std::string::npos);
	EXPECT_EQ(ctx.details_open, 0);
	EXPECT_EQ(ctx.final_rendered, 1);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(first_result);
	cJSON_Delete(second_result);
	cJSON_Delete(thought);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, CompactShellFailureShowsReasonWithoutFullOutput)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse(
		"{\"tool\":\"bash_exec\",\"tool_call_id\":\"shell\","
		"\"args\":{\"command\":\"cmake --build build\"}}");
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "tool_call_id", "shell");
	cJSON_AddStringToObject(result, "tool", "bash_exec");
	cJSON_AddStringToObject(result, "result",
		"{\"data\":{\"exit_code\":2,\"stdout\":\"VERBOSE OUTPUT\","
		"\"stderr\":\"undefined reference to react_init\"}}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	cli_presentation_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("⊗ bash_exec"), std::string::npos);
	EXPECT_NE(output.find("undefined reference to react_init"), std::string::npos);
	EXPECT_EQ(output.find("failed"), std::string::npos);
	EXPECT_EQ(output.find("VERBOSE OUTPUT"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, ShellColorsSurviveFullScreenCapture)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_set_color_enabled(1);
	cJSON *call = cJSON_Parse(
		"{\"tool\":\"bash_exec\",\"args\":{\"command\":\"rg 'hello' src/ | head\"}}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string compact = testing::internal::GetCapturedStdout();
	EXPECT_NE(compact.find("\033[36mrg\033[0m"), std::string::npos);
	EXPECT_NE(compact.find("\033[32m'hello'\033[0m"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_transcript_toggle(&ctx);
	std::string full = testing::internal::GetCapturedStdout();
	EXPECT_NE(full.find("    $ \033[36mrg\033[0m"), std::string::npos);
	EXPECT_NE(full.find("\033[33m|\033[0m"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_transcript_toggle(&ctx);
	(void)testing::internal::GetCapturedStdout();
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, CompactKeepsLongToolNameAndShortensRepositoryPath)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	char cwd[PATH_MAX];
	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_AddObjectToObject(call, "args");
	cJSON_AddStringToObject(call, "tool", "remote_file_inspection");
	std::string path = std::string(cwd) + "/README.md";
	cJSON_AddStringToObject(args, "path", path.c_str());
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("remote_file_inspection README.md"), std::string::npos);
	EXPECT_EQ(output.find(cwd), std::string::npos);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, ToolRowsUseTerminalWidthWithRightGutter)
{
	const char *previous = getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	bool had_columns = previous != nullptr;
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_AddObjectToObject(call, "args");
	cJSON_AddStringToObject(call, "tool", "bash_exec");
	std::string command = "echo " + std::string(240, 'x');
	cJSON_AddStringToObject(args, "command", command.c_str());
	for (int columns : {52, 120, 180}) {
		std::string width = std::to_string(columns);
		EXPECT_EQ(setenv("COLUMNS", width.c_str(), 1), 0);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		std::string output = testing::internal::GetCapturedStdout();
		size_t start = output.find("◉");
		EXPECT_NE(start, std::string::npos);
		if (start != std::string::npos) {
			std::string row = output.substr(start, output.find('\n', start) - start);
			EXPECT_EQ(utf8_display_width(row.c_str()), (size_t)(columns - 2));
		}
	}
	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, TranscriptGrowsPastInitialEventAndToolCapacity)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	for (int i = 0; i < 40; i++) {
		cJSON *delta = TextData("Progress. ");
		Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", delta);
		cJSON_Delete(delta);
		cJSON *call = cJSON_CreateObject();
		std::string id = "call-" + std::to_string(i);
		cJSON_AddStringToObject(call, "tool", "example");
		cJSON_AddStringToObject(call, "tool_call_id", id.c_str());
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		cJSON_AddStringToObject(call, "result", "done");
		Emit(MORPH_EVENT_TOOL, "tool.result", "end", call);
		cJSON_Delete(call);
	}
	cJSON *final = TextData("All calls completed");
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	cli_presentation_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(output.find("succeeded"), std::string::npos);
	EXPECT_NE(output.find("All calls completed"), std::string::npos);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, ExpandedStreamSanitizesSplitControlsAndUtf8)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.tool_details = 1;
	cJSON *call = cJSON_Parse(
		"{\"tool\":\"example\",\"tool_call_id\":\"stream\",\"args\":{}}");
	cJSON *stream = cJSON_CreateObject();
	cJSON_AddStringToObject(stream, "tool_call_id", "stream");
	cJSON_AddStringToObject(stream, "text", "safe\033]52;c;SECRET");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.stream.delta", "delta", stream);
	cJSON_ReplaceItemInObject(stream, "text", cJSON_CreateString("\a\xe4\xb8"));
	Emit(MORPH_EVENT_TOOL, "tool.stream.delta", "delta", stream);
	cJSON_ReplaceItemInObject(stream, "text", cJSON_CreateString("\xad\nend"));
	Emit(MORPH_EVENT_TOOL, "tool.stream.delta", "delta", stream);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(output.find("SECRET"), std::string::npos);
	EXPECT_EQ(output.find('\033'), std::string::npos);
	EXPECT_NE(output.find("中"), std::string::npos);
	EXPECT_NE(output.find("end"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_transcript_toggle(&ctx);
	cli_transcript_toggle(&ctx);
	std::string replay = testing::internal::GetCapturedStdout();
	EXPECT_EQ(replay.find("SECRET"), std::string::npos);
	EXPECT_NE(replay.find("safe中"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(stream);
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
