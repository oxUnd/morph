#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/presentation/presenter_state.h"

#include "sapi/cli/services/turn.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/adapters/input/readline.h"
#include "sapi/cli/adapters/media/image_loader.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/history.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/widgets/banner.h"
#include "sapi/cli/pages/approval.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/services/transcript.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/application.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/widgets/shell_style.h"
#include "util/utf8.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/file.h"
#include "agent/react.h"
#include "agent/tool_context.h"
#include "event/event.h"
#include "http/client.h"
#include "stb_image_write.h"
#include "stb_image.h"

struct cli_cancel_monitor;
struct cli_cancel_monitor *cli_cancel_monitor_start(struct cli_application *ctx, int fd);
}

#include <string>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <webp/encode.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <vector>

TEST(CliMarkdownImageTest, BlockPreviewsFitBoundsAndShowOriginalPathAndAlt)
{
	struct cli_application ctx{};
	char source[] = "/tmp/morph-cli-preview-XXXXXX";
	int fd = mkstemp(source);
	ASSERT_GE(fd, 0);
	close(fd);
	const int sizes[][2] = {{1600, 300}, {1600, 900}, {900, 1600}, {12, 36}};
	for (const auto &size : sizes) {
		SCOPED_TRACE(std::to_string(size[0]) + "x" + std::to_string(size[1]));
		std::vector<unsigned char> pixels(
			static_cast<size_t>(size[0]) * static_cast<size_t>(size[1]) * 3u, 180);
		ASSERT_NE(stbi_write_png(source, size[0], size[1], 3, pixels.data(), size[0] * 3),
			  0);
		std::string markdown =
			"![First **photo** `alt`](file://" + std::string(source) + ")\n\n";
		testing::internal::CaptureStdout();
		cli_markdown_render_ansi(&ctx, markdown.c_str());
		std::string output = testing::internal::GetCapturedStdout();
		auto image = output.find("\033_Ga=T,f=100,");
		ASSERT_NE(image, std::string::npos);
		unsigned int columns = 0, rows = 0;
		ASSERT_EQ(sscanf(output.c_str() + image, "\033_Ga=T,f=100,i=%*u,U=1,q=2,c=%u,r=%u",
				 &columns, &rows),
			  2);
		EXPECT_GT(columns, 0u);
		EXPECT_LE(columns, 48u);
		EXPECT_GT(rows, 0u);
		EXPECT_LE(rows, 12u);
		/* Terminal cells are 9x18 pixels without a terminal size query.
		 * Allow one cell of rounding when preserving the source aspect ratio. */
		double expected_columns = static_cast<double>(rows) * 2.0 * size[0] / size[1];
		EXPECT_LE(static_cast<double>(columns), expected_columns + 1.0);
		EXPECT_GE(static_cast<double>(columns),
			  static_cast<double>(rows - 1u) * 2.0 * size[0] / size[1] - 1.0);
		if (size[0] == 12) {
			EXPECT_EQ(columns, 2u);
			EXPECT_EQ(rows, 2u);
		}
		std::string caption = "(\033]8;;file://" + std::string(source) + "\033\\" + source +
				      "\033]8;;\033\\ · First photo alt)";
		EXPECT_NE(output.find(caption), std::string::npos);
		EXPECT_LT(image, output.find(caption));
		EXPECT_EQ(output.find("morph-markdown-image-"), std::string::npos);
	}
	EXPECT_EQ(unlink(source), 0);
}

TEST(CliShellStyleTest, ColorsCommandsStringsAndOperatorsWithoutChangingText)
{
	morph_buf_t styled;
	const char *source = "MODE=1 rg '中文 | text' src/ && head -20 > out";
	ASSERT_EQ(morph_buf_init(&styled, 128), 0);
	ASSERT_EQ(cli_shell_style(&styled, source, 0), 0);
	std::string output = morph_buf_cstr_borrow(&styled);
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

TEST(CliMarkdownImageTest, SupportedFormatsRenderInsideTableAndReleaseFiles)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	char directory[] = "/tmp/morph-cli-images-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	unsigned char pixels[12 * 36 * 3];
	memset(pixels, 180, sizeof(pixels));
	for (const char *extension : {"jpg", "bmp", "png", "webp"}) {
		std::string source = std::string(directory) + "/test." + extension;
		int written = strcmp(extension, "jpg") == 0
				      ? stbi_write_jpg(source.c_str(), 12, 36, 3, pixels, 90)
			      : strcmp(extension, "bmp") == 0
				      ? stbi_write_bmp(source.c_str(), 12, 36, 3, pixels)
				      : stbi_write_png(source.c_str(), 12, 36, 3, pixels, 36);
		if (strcmp(extension, "webp") == 0) {
			uint8_t *encoded = nullptr;
			size_t length = WebPEncodeLosslessRGB(pixels, 12, 36, 36, &encoded);
			ASSERT_GT(length, 0u);
			written = file_write_all(source.c_str(),
						 reinterpret_cast<const char *>(encoded),
						 length) == 0;
			WebPFree(encoded);
		}
		ASSERT_NE(written, 0);
		char *resolved = nullptr;
		ASSERT_EQ(cli_image_loader_load(source.c_str(), &resolved, nullptr), 0);
		ASSERT_NE(resolved, nullptr);
		std::string temporary = resolved;
		int w, h, channels;
		EXPECT_EQ(stbi_info(resolved, &w, &h, &channels), 1);
		EXPECT_EQ(w, 12);
		EXPECT_EQ(h, 36);
		cli_image_loader_release(resolved, nullptr);
		EXPECT_FALSE(std::filesystem::exists(temporary));
		std::string markdown = "before\n\n| image | text |\n|---|---|\n| ![](" + source +
				       ") | beside |\n\nafter\n";
		testing::internal::CaptureStdout();
		cli_markdown_render_ansi(_cli_output_context_ptr, markdown.c_str());
		std::string output = testing::internal::GetCapturedStdout();
		auto image = output.find("\033_Ga=T,f=100,");
		ASSERT_NE(image, std::string::npos);
		EXPECT_LT(output.find("┌"), image);
		EXPECT_LT(image, output.find("└"));
		EXPECT_NE(output.find("beside"), std::string::npos);
		EXPECT_EQ(output.find("[image"), std::string::npos);
		EXPECT_EQ(unlink(source.c_str()), 0);
	}
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(CliMarkdownImageTest, MissingImageStaysInsideTable)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	testing::internal::CaptureStdout();
	cli_markdown_render_ansi(_cli_output_context_ptr,
				 "| image |\n|---|\n| ![](/missing.jpg) |\n");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_LT(output.find("┌"), output.find("[image unavailable:"));
	EXPECT_LT(output.find("[image unavailable:"), output.find("└"));
	EXPECT_EQ(output.find("\033_G"), std::string::npos);
}

TEST(CliMarkdownImageTest, RemoteGifRendersInsideTable)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	static const unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a', 1,   0,    1, 0, 0x80, 0,
					    0,	 0,   0,   0,	255, 255, 255, 0x2c, 0, 0, 0,	 0,
					    1,	 0,   1,   0,	0,   2,	  2,   0x44, 1, 0, 0x3b};
	int server = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(server, 0);
	struct sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
	ASSERT_EQ(listen(server, 1), 0);
	socklen_t size = sizeof(address);
	ASSERT_EQ(getsockname(server, reinterpret_cast<sockaddr *>(&address), &size), 0);
	std::thread responder([&] {
		int client = accept(server, nullptr, nullptr);
		if (client < 0)
			return;
		char request[BUFSIZ];
		(void)recv(client, request, sizeof(request), 0);
		std::string response =
			"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sizeof(gif)) +
			"\r\nConnection: close\r\n\r\n";
		response.append(reinterpret_cast<const char *>(gif), sizeof(gif));
		(void)send(client, response.data(), response.size(), 0);
		close(client);
	});
	std::string markdown = "| remote |\n|---|\n| ![](http://127.0.0.1:" +
			       std::to_string(ntohs(address.sin_port)) + "/image.gif) |\n";
	testing::internal::CaptureStdout();
	cli_markdown_render_ansi(_cli_output_context_ptr, markdown.c_str());
	std::string output = testing::internal::GetCapturedStdout();
	shutdown(server, SHUT_RDWR);
	close(server);
	responder.join();
	auto image = output.find("\033_Ga=T,f=100,");
	ASSERT_NE(image, std::string::npos);
	EXPECT_LT(output.find("┌"), image);
	EXPECT_LT(image, output.find("└"));
}

TEST(CliShellStyleTest, WrapsUtf8AndDropsTerminalControls)
{
	morph_buf_t styled;
	ASSERT_EQ(morph_buf_init(&styled, 128), 0);
	ASSERT_EQ(cli_shell_style(&styled, "printf '中文🙂abcdef'\033]0;unsafe\a | cat", 12), 0);
	std::string output = morph_buf_cstr_borrow(&styled);
	EXPECT_EQ(output.find("unsafe"), std::string::npos);
	EXPECT_EQ(utf8valid(output.c_str()), nullptr);
	EXPECT_NE(output.find("\n    "), std::string::npos);
	EXPECT_NE(output.find("\033[36mcat"), std::string::npos);
	morph_buf_cleanup(&styled);
}

TEST(CliShellStyleTest, SummaryHidesOnlyLiteralCurrentDirectoryPrefix)
{
	EXPECT_STREQ(cli_shell_summary_borrow("cd /work && ls -la", "/work"), "ls -la");
	EXPECT_STREQ(cli_shell_summary_borrow("cd '/work dir'; git status", "/work dir"),
		     "git status");
	EXPECT_STREQ(cli_shell_summary_borrow("cd -- \"/work dir\" && pwd", "/work dir"), "pwd");
	EXPECT_STREQ(cli_shell_summary_borrow("cd . && ls", "/work"), "ls");
	for (const char *source : {"cd /other && ls", "cd $PWD && ls", "cd /work || ls",
				   "cd /work & ls", "cd /work", "cd /work && ", "cd '/work && ls",
				   "cd /work;; ls", "cd /work extra && ls", "echo cd /work && ls"})
		EXPECT_STREQ(cli_shell_summary_borrow(source, "/work"), source);
}

class CliPresentationTest : public ::testing::Test
{
      protected:
	struct cli_application ctx{};
	char dir[PATH_MAX]{};
	FILE *details_input = nullptr;
	int details_master = -1;

	void SetUp() override
	{
		cli_output_set_color_enabled(&ctx, 0);
		ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
		ctx.presentation_ready = 1;
		ctx.turn_active = 1;
		std::strcpy(dir, "/tmp/morph-cli-presentation-XXXXXX");
		ASSERT_NE(mkdtemp(dir), nullptr);
		std::strcpy(ctx.workdir, dir);
		ASSERT_EQ(cli_feed_init(&ctx, stdout, STDOUT_FILENO), 0);
		ASSERT_EQ(cli_input_init(&ctx), 0);
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
	}

	void TearDown() override
	{
		std::error_code error;

		if (ctx.base.state)
			morph_tui_application_deinit(&ctx.base);
		if (details_input)
			fclose(details_input);
		if (details_master >= 0)
			close(details_master);
		cli_presentation_cleanup(&ctx);
		cli_input_cleanup(&ctx);
		cli_feed_cleanup(&ctx);
		std::filesystem::remove_all(dir, error);
		cli_output_set_color_enabled(&ctx, 1);
	}

	void ToggleDetails()
	{
		if (!ctx.base.state) {
			details_master = posix_openpt(O_RDWR | O_NOCTTY);
			ASSERT_GE(details_master, 0);
			ASSERT_EQ(grantpt(details_master), 0);
			ASSERT_EQ(unlockpt(details_master), 0);
			details_input = fopen(ptsname(details_master), "r+");
			ASSERT_NE(details_input, nullptr);
			struct morph_tui_application_options options{};

			options.input = details_input;
			options.output = stdout;
			options.external_signals = 1;
			ASSERT_EQ(morph_tui_application_init(&ctx.base, &options), 0);
			static const struct morph_tui_controller_ops root_ops{};
			struct morph_tui_controller root = {&root_ops, nullptr,
							    MORPH_TUI_INPUT_EXTERNAL, 0, 0};

			ASSERT_EQ(morph_tui_application_present_controller_take(&ctx.base, &root),
				  0);
			ASSERT_EQ(morph_tui_application_step(&ctx.base, 0), 0);
		}
		cli_turn_details_toggle(&ctx);
		ASSERT_EQ(morph_tui_application_step(&ctx.base, 0), 0);
	}

	void Emit(enum morph_event_type type, const char *name, const char *phase, cJSON *data)
	{
		struct morph_event event{type, name, phase, nullptr, data, "turn_test"};
		ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	}

	static cJSON *TextData(const char *text)
	{
		cJSON *data = cJSON_CreateObject();
		cJSON_AddStringToObject(data, "text", text);
		return data;
	}
};

TEST_F(CliPresentationTest, RestoresInactiveHistoryAndToolsWithoutInternalRecords)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.turn_active = 0;
	struct model_history_item items[7]{};
	std::string contents[] = {
		"ORIGINAL_QUESTION",   "CHECKING_FILES",      R"({"ok":true,"data":{
		"stdout":"HISTORICAL_TOOL_OUTPUT","exit_code":0,"duration_ms":2400}})",
		"**ORIGINAL_ANSWER**", "INTERNAL_COMPACTION", "LATEST_QUESTION",
		"LATEST_ANSWER"};
	const char *kinds[] = {"user_message",	    "assistant_tool_calls", "tool_result",
			       "assistant_message", "compaction_summary",   "user_message",
			       "assistant_message"};
	char calls[] = R"({"calls":[{"name":"exec","tool_call_id":"history_tool",
		"arguments":"{\"command\":\"printf HISTORICAL_COMMAND\"}"}],
		"reasoning_content":"HISTORICAL_REASONING"})";
	char id[] = "history_tool";
	for (size_t i = 0; i < 7; i++) {
		strcpy(items[i].kind, kinds[i]);
		items[i].content = contents[i].data();
		items[i].next = i < 6 ? &items[i + 1] : nullptr;
	}
	items[1].payload_json = calls;
	items[2].tool_call_id = id;
	testing::internal::CaptureStdout();
	int rc = cli_history_render_session(&ctx, items, nullptr);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_LT(output.find("ORIGINAL_QUESTION"), output.find("CHECKING_FILES"));
	EXPECT_LT(output.find("CHECKING_FILES"), output.find("HISTORICAL_COMMAND"));
	EXPECT_LT(output.find("HISTORICAL_COMMAND"), output.find("ORIGINAL_ANSWER"));
	EXPECT_LT(output.find("ORIGINAL_ANSWER"), output.find("LATEST_QUESTION"));
	EXPECT_LT(output.find("LATEST_QUESTION"), output.find("LATEST_ANSWER"));
	EXPECT_NE(output.find("HISTORICAL_TOOL_OUTPUT"), std::string::npos);
	EXPECT_NE(output.find("2.4s"), std::string::npos);
	EXPECT_EQ(output.find("INTERNAL_COMPACTION"), std::string::npos);
	EXPECT_EQ(output.find("Worked for"), std::string::npos);
	EXPECT_EQ(ctx.turn_active, 0);
}

TEST_F(CliPresentationTest, RestoredPendingToolHasNoLiveTimerOrInventedFailure)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse(R"({"name":"exec","tool_call_id":"pending",
		"arguments":"{\"command\":\"printf PENDING_COMMAND\"}"})");
	ASSERT_NE(call, nullptr);
	testing::internal::CaptureStdout();
	int rc = cli_transcript_restore_tool(&ctx, call, nullptr);
	cli_transcript_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	cJSON_Delete(call);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(output.find("PENDING_COMMAND"), std::string::npos);
	EXPECT_NE(output.find("no recorded result"), std::string::npos);
	EXPECT_EQ(output.find("FAILED"), std::string::npos);
	EXPECT_EQ(output.find("0.0s"), std::string::npos);
	EXPECT_EQ(output.find("Interrupted before"), std::string::npos);
}

TEST_F(CliPresentationTest, RestoresOriginalToolOutputAndFailureMetadata)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.tool_details = 1;
	cJSON *call = cJSON_Parse(R"({"name":"exec","tool_call_id":"failed",
		"arguments":"{\"command\":\"printf ORIGINAL_COMMAND\"}"})");
	ASSERT_NE(call, nullptr);
	char stored[] = "MODEL_HISTORY_TRUNCATED";
	char metadata[] = R"({"original_content":"ORIGINAL_TOOL_OUTPUT", "error_code":-13})";
	struct model_history_item result{};
	result.content = stored;
	result.payload_json = metadata;
	testing::internal::CaptureStdout();
	int rc = cli_transcript_restore_tool(&ctx, call, &result);
	std::string output = testing::internal::GetCapturedStdout();
	cJSON_Delete(call);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(output.find("ORIGINAL_TOOL_OUTPUT"), std::string::npos);
	EXPECT_NE(output.find("ORIGINAL_COMMAND"), std::string::npos);
	EXPECT_NE(output.find("FAILED"), std::string::npos);
	EXPECT_EQ(output.find("MODEL_HISTORY_TRUNCATED"), std::string::npos);
	EXPECT_EQ(output.find("0.0s"), std::string::npos);
}

TEST_F(CliPresentationTest, RestoresLegacyMessagesWithTerminalControlsSanitized)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	struct message question{}, answer{}, system{};
	strcpy(question.role, "user");
	strcpy(answer.role, "assistant");
	strcpy(system.role, "system");
	char input[] = "LEGACY_QUESTION\033[2J";
	char response[] = "**LEGACY_ANSWER**";
	char internal[] = "INTERNAL_SYSTEM_MESSAGE";
	question.content = input;
	answer.content = response;
	system.content = internal;
	question.next = &answer;
	answer.next = &system;
	testing::internal::CaptureStdout();
	int rc = cli_history_render_session(&ctx, nullptr, &question);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_NE(output.find("LEGACY_QUESTION"), std::string::npos);
	EXPECT_NE(output.find("LEGACY_ANSWER"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	EXPECT_EQ(output.find("INTERNAL_SYSTEM_MESSAGE"), std::string::npos);
}

TEST(CliInputPromptTest, AnsiSequencesAreMarkedAsInvisible)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	cli_output_set_color_enabled(_cli_output_context_ptr, 1);
	const unsigned char *prompt = reinterpret_cast<const unsigned char *>(
		cli_readline_input_prompt_borrow(_cli_output_context_ptr));
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
	struct cli_application ctx{};
	struct cli_cancel_monitor *monitor = cli_cancel_monitor_start(&ctx, slave);
	ASSERT_NE(monitor, nullptr);

	ctx.interrupted = 0;
	ASSERT_EQ(write(master, "\033", 1), 1);
	for (int i = 0; i < 50 && !ctx.interrupted; i++)
		usleep(10000);
	cli_cancel_monitor_stop(monitor);

	EXPECT_EQ(ctx.interrupted, 1);
	EXPECT_EQ(ctx.interrupted, 1);
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
	struct cli_application ctx{};
	struct cli_cancel_monitor *monitor = cli_cancel_monitor_start(&ctx, slave);
	ASSERT_NE(monitor, nullptr);

	ctx.interrupted = 0;
	ASSERT_EQ(write(master, "\033[A", 3), 3);
	usleep(100000);
	cli_cancel_monitor_stop(monitor);

	EXPECT_EQ(ctx.interrupted, 0);
	EXPECT_EQ(ctx.interrupted, 0);
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
	Emit(MORPH_EVENT_REACT, "react.thought.delta", "delta", thought_delta);
	Emit(MORPH_EVENT_REACT, "react.thought.end", "end", thought_end);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", tool);
	Emit(MORPH_EVENT_REACT, "react.observation", "end", observation);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("\033"), std::string::npos);
	EXPECT_NE(output.find("thought: Inspecting the CLI"), std::string::npos);
	EXPECT_EQ(output.find("Inspecting the CLI", output.find("Inspecting the CLI") + 1),
		  std::string::npos);
	EXPECT_NE(output.find("tool: file_list {\"path\":\"src/sapi/cli\"}"), std::string::npos);
	EXPECT_NE(output.find("observation:\nrun.c\noutput.c"), std::string::npos);
	EXPECT_NE(output.find("final:\nResult with **raw markdown**."), std::string::npos);

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

TEST_F(CliPresentationTest, HookMessagesPersistAndQuietSuccessLeavesNoOutput)
{
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_BACKGROUND, "hook.started", "begin", nullptr);
	Emit(MORPH_EVENT_BACKGROUND, "hook.completed", "end", nullptr);
	std::string quiet = testing::internal::GetCapturedStdout();
	EXPECT_TRUE(quiet.empty());

	struct morph_event message{MORPH_EVENT_BACKGROUND,
				   "hook.message",
				   "end",
				   "Review failed\033[2J; run tests",
				   nullptr,
				   "turn_test"};
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &message), 0);
	std::string warning = testing::internal::GetCapturedStdout();
	EXPECT_NE(warning.find("Hook"), std::string::npos);
	EXPECT_NE(warning.find("Review failed"), std::string::npos);
	EXPECT_EQ(warning.find("\033[2J"), std::string::npos);
	EXPECT_EQ(warning.find("\033"), std::string::npos);
}

TEST_F(CliPresentationTest, BackgroundWarningsAreVisibleAndSanitizedInAllModes)
{
	for (auto mode :
	     {CLI_PRESENT_ONCE_PLAIN, CLI_PRESENT_INTERACTIVE, CLI_PRESENT_EVENTS_JSON}) {
		ctx.presentation_mode = mode;
		struct morph_event warning{MORPH_EVENT_BACKGROUND,
					   "history.tool_output.warning",
					   "warning",
					   "3 of 4 tool results were truncated.\033[2J "
					   "Review context.tool_result_max_tokens.",
					   nullptr,
					   "turn_test"};

		testing::internal::CaptureStdout();
		ASSERT_EQ(cli_presentation_event(&ctx, &warning), 0);
		std::string output = testing::internal::GetCapturedStdout();
		EXPECT_NE(output.find("3 of 4 tool results were truncated"), std::string::npos);
		EXPECT_NE(output.find("context.tool_result_max_tokens"), std::string::npos);
		EXPECT_EQ(output.find("\033[2J"), std::string::npos);
		if (mode == CLI_PRESENT_EVENTS_JSON) {
			cJSON *event = cJSON_Parse(output.c_str());
			ASSERT_NE(event, nullptr);
			EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(event, "name")),
				     "history.tool_output.warning");
			cJSON_Delete(event);
		} else {
			EXPECT_NE(output.find("Warning"), std::string::npos);
		}
	}
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
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end", completed);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "status: Compacting context…\n"
			  "context: compacted 91676 -> 15672 tokens\n");
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
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end", completed);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("◇ Context compacted  92k → 16k"), std::string::npos);
	EXPECT_EQ(output.find("iteration"), std::string::npos);
	EXPECT_EQ(output.find("pass"), std::string::npos);
	EXPECT_EQ(output.find("Thinking…"), std::string::npos);
	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
	cJSON_Delete(completed);
}

TEST_F(CliPresentationTest, CompactionReplacesLiveStateWithOneIndependentHistoryRow)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *data = cJSON_CreateObject();
	cJSON_AddNumberToObject(data, "before_tokens", 103869);
	cJSON_AddNumberToObject(data, "after_tokens", 68804);
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.compaction.begin", "begin", data);
	EXPECT_GE(cli_feed_activity_elapsed_ms(&ctx), 0);
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end", data);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\n◇ Context compacted  104k → 69k · 0s\n\n"), std::string::npos);
	EXPECT_EQ(output.find("Context compacted"), output.rfind("Context compacted"));
	EXPECT_EQ(output.find("└"), std::string::npos);
	EXPECT_EQ(cli_feed_activity_elapsed_ms(&ctx), -1);
	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
	cJSON_Delete(data);
}

TEST_F(CliPresentationTest, CompactionCompletionFitsNarrowTerminal)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	const char *previous = getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	setenv("COLUMNS", "24", 1);
	cJSON *data = cJSON_CreateObject();
	cJSON_AddNumberToObject(data, "before_tokens", 103869);
	cJSON_AddNumberToObject(data, "after_tokens", 68804);
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.compaction.completed", "end", data);
	std::string output = testing::internal::GetCapturedStdout();
	if (previous)
		setenv("COLUMNS", saved.c_str(), 1);
	else
		unsetenv("COLUMNS");
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
		EXPECT_LE(utf8_display_width_ansi(line.c_str()), 23U);
	EXPECT_NE(output.find("◇ Context compacted"), std::string::npos);
	cJSON_Delete(data);
}

TEST_F(CliPresentationTest, CompactionFailureIsIndependentAndSanitizesError)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *data = cJSON_CreateObject();
	cJSON_AddStringToObject(data, "error", "parse error\n\033[2Jhidden");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.compaction.begin", "begin", data);
	Emit(MORPH_EVENT_REACT, "react.compaction.failed", "failed", data);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\n◇ Context compression failed · parse error"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	EXPECT_EQ(output.find("iteration"), std::string::npos);
	EXPECT_EQ(cli_feed_activity_elapsed_ms(&ctx), -1);
	cJSON_Delete(data);
}

TEST_F(CliPresentationTest, OncePrintsReasoningAsLabeledPlainText)
{
	cJSON *reasoning = TextData("Checking constraints");
	cJSON *final = TextData("Done");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", reasoning);
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
	EXPECT_FALSE(
		morph_strmap_contains(&ctx.presenter->rendered_artifacts, "/tmp/generated.png"));
	EXPECT_FALSE(
		morph_strmap_contains(&ctx.presenter->rendered_artifacts, "/tmp/generated.mp4"));
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("  └ image: /tmp/generated.png\n"
			      "  └ video: /tmp/generated.mp4\n"),
		  0u);
	EXPECT_NE(output.find("[image unavailable:"), std::string::npos);
	EXPECT_FALSE(
		morph_strmap_contains(&ctx.presenter->rendered_artifacts, "/tmp/generated.png"));
	EXPECT_FALSE(
		morph_strmap_contains(&ctx.presenter->rendered_artifacts, "/tmp/generated.mp4"));
	cJSON_Delete(image);
	cJSON_Delete(video);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, JsonModeEmitsOneNdjsonObject)
{
	cJSON *data = TextData("hello");
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	struct morph_event event{MORPH_EVENT_REACT, "react.final", "end",
				 "final answer",    data,	   "turn_test"};

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	std::string output = testing::internal::GetCapturedStdout();
	ASSERT_FALSE(output.empty());
	ASSERT_EQ(output.back(), '\n');

	cJSON *parsed = cJSON_Parse(output.c_str());
	ASSERT_NE(parsed, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(parsed, "name")->valuestring, "react.final");
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
	EXPECT_EQ(output.find("Compact", output.find("Compact") + 1), std::string::npos);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveSeparatesToolBlocksFromFinalAnswer)
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

struct CliTestTextStyle {
	int foreground = -1;
	bool bold = false;
};

static CliTestTextStyle cli_test_style_at(const std::string &output, const char *text,
					 size_t start = 0)
{
	CliTestTextStyle style;
	size_t position = output.find(text, start);
	EXPECT_NE(position, std::string::npos);
	if (position == std::string::npos)
		return style;
	for (size_t offset = 0; offset < position; offset++) {
		if (output[offset] != '\033' || offset + 1 >= output.size() || output[offset + 1] != '[')
			continue;
		size_t end = offset + 2;
		while (end < position && (static_cast<unsigned char>(output[end]) < 0x40u ||
			static_cast<unsigned char>(output[end]) > 0x7eu))
			end++;
		if (end >= position || output[end] != 'm')
			continue;
		std::istringstream parameters(output.substr(offset + 2, end - offset - 2));
		std::vector<int> codes;
		std::string parameter;
		while (std::getline(parameters, parameter, ';'))
			codes.push_back(std::atoi(parameter.c_str()));
		if (codes.empty())
			codes.push_back(0);
		for (size_t index = 0; index < codes.size(); index++) {
			int code = codes[index];
			if (code == 0)
				style = CliTestTextStyle{};
			else if (code == 1)
				style.bold = true;
			else if (code == 22)
				style.bold = false;
			else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97))
				style.foreground = code;
			else if (code == 39)
				style.foreground = -1;
			else if ((code == 38 || code == 48) && index + 1 < codes.size()) {
				int mode = codes[++index];
				index += mode == 5 ? 1u : mode == 2 ? 3u : 0u;
				if (code == 38)
					style.foreground = -2;
			}
		}
		offset = end;
	}
	return style;
}

static std::string cli_test_plain_text(const std::string &output)
{
	char *plain MORPH_CLEANUP(morph_charp_cleanup) =
		utf8_strip_ansi_dup(output.c_str(), output.size(), nullptr);
	return plain ? plain : "";
}

TEST_F(CliPresentationTest, InteractiveColorsToolAndFinalMarkersDifferently)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *final = TextData("Done");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);

	cJSON_AddStringToObject(call, "tool", "file_list");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(cli_test_style_at(output, "◉").foreground, 36);
	EXPECT_TRUE(cli_test_style_at(output, "file_list").bold);
	EXPECT_EQ(cli_test_style_at(output, "●").foreground, -1);
	EXPECT_TRUE(cli_test_style_at(output, "●").bold);

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
	EXPECT_EQ(output.find("\033[?2026h"), std::string::npos);
	EXPECT_EQ(output.find("\033[?2026l"), std::string::npos);
	EXPECT_NE(output.find("Stream"), std::string::npos);
	EXPECT_NE(output.find("• item"), std::string::npos);
	EXPECT_EQ(output.find("fallback final payload"), std::string::npos);
	EXPECT_EQ(ctx.presenter->markdown_stream, nullptr);
	EXPECT_EQ(ctx.presenter->final_rendered, 1);

	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, StreamingImageCaptionAppearsOnceAndAllowsEmptyAlt)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	std::string source = std::string(dir) + "/photo.png";
	unsigned char pixels[12 * 36 * 3];
	memset(pixels, 180, sizeof(pixels));
	ASSERT_NE(stbi_write_png(source.c_str(), 12, 36, 3, pixels, 36), 0);
	std::string markdown = "![](" + source + ")\n\n";
	std::string caption = "(\033]8;;file://" + source + "\033\\" + source + "\033]8;;\033\\)";
	cJSON *body = TextData(markdown.c_str());

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	std::string partial = testing::internal::GetCapturedStdout();
	EXPECT_NE(partial.find("\033_Ga=T,f=100,"), std::string::npos);
	EXPECT_NE(partial.find(caption), std::string::npos);
	EXPECT_EQ(partial.find(caption), partial.rfind(caption));
	EXPECT_EQ(partial.find(" · "), std::string::npos);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final", "end", body);
	std::string final_output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(final_output.find(caption), std::string::npos);
	EXPECT_EQ(final_output.find("\033_Ga=T,f=100,"), std::string::npos);
	cJSON_Delete(body);
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
	EXPECT_EQ(ctx.presenter->markdown_stream, nullptr);
	EXPECT_EQ(ctx.presenter->final_rendered, 1);

	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, PlainNativeAnswerIsFinalWithoutThoughtOrDuplicateContent)
{
	cJSON *body = TextData("Native answer");
	cJSON *reasoning = TextData("Private reasoning");
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", reasoning);
	Emit(MORPH_EVENT_REACT, "react.final", "end", body);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("reasoning:"), std::string::npos);
	EXPECT_NE(output.find("final:"), std::string::npos);
	EXPECT_EQ(output.find("thought:"), std::string::npos);
	EXPECT_EQ(output.find("content:"), std::string::npos);
	EXPECT_EQ(output.find("Native answer"), output.rfind("Native answer"));
	cJSON_Delete(body);
	cJSON_Delete(reasoning);
}

TEST_F(CliPresentationTest, PlainNativeToolPreambleKeepsContentClassification)
{
	cJSON *preamble = TextData("Searching first");
	cJSON *answer = TextData("Answer after search");
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", preamble);
	Emit(MORPH_EVENT_REACT, "react.content.end", "end", preamble);
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", answer);
	Emit(MORPH_EVENT_REACT, "react.final", "end", answer);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("content: Searching first"), std::string::npos);
	EXPECT_NE(output.find("final:\nAnswer after search"), std::string::npos);
	EXPECT_EQ(output.find("thought:"), std::string::npos);
	EXPECT_EQ(output.find("Searching first"), output.rfind("Searching first"));
	cJSON_Delete(preamble);
	cJSON_Delete(answer);
}

TEST_F(CliPresentationTest, InteractiveNativeContentSurvivesInterleavedReasoning)
{
	cJSON *body = TextData("# Visible answer\n");
	cJSON *reasoning = TextData("Separate reasoning");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", reasoning);
	Emit(MORPH_EVENT_REACT, "react.final", "end", body);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Visible answer"), std::string::npos);
	EXPECT_EQ(output.find("Visible answer"), output.rfind("Visible answer"));
	EXPECT_NE(output.find("Separate reasoning"), std::string::npos);
	cJSON_Delete(body);
	cJSON_Delete(reasoning);
}

TEST_F(CliPresentationTest, PlainNativePreambleIsNotTruncatedToStatusLength)
{
	std::string long_text(4096, 'x');
	long_text += " end of content";
	cJSON *body = TextData(long_text.c_str());
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_REACT, "react.content.end", "end", body);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find(long_text), std::string::npos);
	cJSON_Delete(body);
}

TEST_F(CliPresentationTest, CancelledNativeAnswerPreservesSanitizedPartialContent)
{
	cJSON *body = TextData("Partial\033[31m answer");
	cJSON *outcome = cJSON_CreateObject();
	cJSON_AddStringToObject(outcome, "outcome", "cancelled");
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_REACT, "react.turn.end", "cancelled", outcome);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("content: Partial answer"), std::string::npos);
	EXPECT_EQ(output.find("\033[31m"), std::string::npos);
	cJSON_Delete(body);
	cJSON_Delete(outcome);
}

TEST_F(CliPresentationTest, CancelledInteractiveAnswerCommitsPartialContentOnce)
{
	cJSON *body = TextData("Partial interactive answer");
	cJSON *outcome = cJSON_CreateObject();
	cJSON_AddStringToObject(outcome, "outcome", "cancelled");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_REACT, "react.turn.end", "cancelled", outcome);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Partial interactive answer"), std::string::npos);
	EXPECT_EQ(output.find("Partial interactive answer"),
		  output.rfind("Partial interactive answer"));
	EXPECT_EQ(ctx.presenter->markdown_stream, nullptr);
	cJSON_Delete(body);
	cJSON_Delete(outcome);
}

TEST_F(CliPresentationTest, CancelledTurnUsesNeutralStatus)
{
	for (auto mode : {CLI_PRESENT_ONCE_PLAIN, CLI_PRESENT_INTERACTIVE}) {
		cli_presentation_reset(&ctx);
		ctx.presentation_mode = mode;
		cli_output_set_color_enabled(&ctx, 1);
		cJSON *data = cJSON_CreateObject();
		cJSON_AddStringToObject(data, "outcome", "cancelled");
		cJSON_AddNumberToObject(data, "error_code", -ECANCELED);
		cJSON_AddStringToObject(data, "error", "Operation canceled");
		cJSON_AddStringToObject(data, "detail", "user_cancelled");
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_REACT, "react.turn.end", "cancelled", data);
		std::string output = testing::internal::GetCapturedStdout();
		EXPECT_NE(output.find(mode == CLI_PRESENT_ONCE_PLAIN ? "cancelled\n"
								     : "\033[2m◇ Cancelled\033[0m"),
			  std::string::npos);
		EXPECT_EQ(output.find("Error"), std::string::npos);
		EXPECT_EQ(output.find("Operation canceled"), std::string::npos);
		EXPECT_EQ(output.find("user_cancelled"), std::string::npos);
		EXPECT_EQ(output.find("\033[31m"), std::string::npos);
		cJSON_Delete(data);
	}
}

TEST_F(CliPresentationTest, FailedTurnStillUsesErrorStatus)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);
	cJSON *data = cJSON_CreateObject();
	cJSON_AddStringToObject(data, "outcome", "internal_error");
	cJSON_AddNumberToObject(data, "error_code", -ECANCELED);
	cJSON_AddStringToObject(data, "error", "Operation canceled");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.turn.end", "failed", data);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\033[31m⊗ Error"), std::string::npos);
	EXPECT_NE(output.find("Operation canceled"), std::string::npos);
	cJSON_Delete(data);
}

TEST_F(CliPresentationTest, NativeContentEndWithoutDeltasRendersPreamble)
{
	cJSON *body = TextData("Preamble without deltas");
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.end", "end", body);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("content: Preamble without deltas"), std::string::npos);
	EXPECT_EQ(output.find("thought:"), std::string::npos);
	cJSON_Delete(body);
}

TEST_F(CliPresentationTest, InteractiveCollapsesReasoningIntoDetails)
{
	cJSON *first = TextData("Inspect");
	cJSON *second = TextData("ing the state\nA hidden detail");
	cJSON *answer = TextData("Done");
	cJSON *final = TextData("fallback final payload");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", answer);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[2m\033[3m"), std::string::npos);
	EXPECT_NE(output.find("Inspecting the state…"), std::string::npos);
	EXPECT_EQ(output.find("A hidden detail"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_turn_details_render(&ctx, 0);
	std::string details = testing::internal::GetCapturedStdout();
	EXPECT_NE(details.find("Inspecting the state"), std::string::npos);
	EXPECT_NE(details.find("A hidden detail"), std::string::npos);
	EXPECT_NE(output.find("\n\033[1m●\033[0m "), std::string::npos);
	EXPECT_EQ(output.find("Reasoning", output.find("Reasoning") + 1), std::string::npos);
	EXPECT_EQ(output.find("fallback final payload"), std::string::npos);
	EXPECT_EQ(ctx.presenter->event_stream_visible, 0);
	EXPECT_EQ(ctx.presenter->final_rendered, 1);

	cli_output_set_color_enabled(&ctx, 0);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(answer);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, AnswerSharesSubmittedInputMarginWithAndWithoutStreaming)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	for (int color : {0, 1}) {
		cli_output_set_color_enabled(&ctx, color);
		for (bool streaming : {false, true}) {
			cli_presentation_reset(&ctx);
			ctx.output.trailing_newlines = 0;
			cJSON *answer = TextData("Done");
			testing::internal::CaptureStdout();
			cli_presentation_user_input(&ctx, "Review");
			if (streaming)
				Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", answer);
			Emit(MORPH_EVENT_REACT, "react.final", "end", answer);
			std::string output = testing::internal::GetCapturedStdout();
			char *plain =
				utf8_terminal_sanitize_dup(output.c_str(), output.size(),
							   UTF8_TERMINAL_TEXT_MULTILINE, nullptr);
			ASSERT_NE(plain, nullptr);
			std::istringstream rows(plain);
			std::string normalized;
			std::string row;
			while (std::getline(rows, row)) {
				size_t visible_end = row.find_last_not_of(" \t");
				row.resize(visible_end == std::string::npos ? 0 : visible_end + 1);
				normalized += row + '\n';
			}
			std::string expected = color ? "› Review\n\n\n● " : "> Review\n\n● ";
			EXPECT_NE(normalized.find(expected), std::string::npos) << normalized;
			free(plain);
			cJSON_Delete(answer);
		}
	}
}

TEST_F(CliPresentationTest, InteractiveReasoningRespectsDisabledColor)
{
	cJSON *reasoning = TextData("Checking constraints");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", reasoning);
	cli_presentation_flush_stream(&ctx);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Checking constraints"), std::string::npos);
	EXPECT_EQ(output.find("…"), std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);
	cJSON_Delete(reasoning);
}

TEST_F(CliPresentationTest, OnceNeutralizesObservationControlSequences)
{
	cJSON *observation = TextData("before\033[2J\033]52;c;secret\aafter\rhidden");

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
	cli_output_set_color_enabled(&ctx, 1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("beforeafter"), std::string::npos);
	EXPECT_EQ(output.find("2Jafter"), std::string::npos);
	EXPECT_EQ(output.find("fallback"), std::string::npos);
	cli_output_set_color_enabled(&ctx, 0);
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
	cli_output_set_color_enabled(&ctx, 1);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", first);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("beforeafter"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_turn_details_render(&ctx, 0);
	output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("beforeafter"), std::string::npos);
	EXPECT_EQ(output.find("secret"), std::string::npos);
	EXPECT_EQ(output.find("52;c"), std::string::npos);
	cli_output_set_color_enabled(&ctx, 0);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(final);
}

TEST(CliOutputSafetyTest, PrintfBlocksDangerousSequencesButKeepsTrustedSgr)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	cli_output_set_color_enabled(_cli_output_context_ptr, 1);
	testing::internal::CaptureStdout();
	cli_output_printf(_cli_output_context_ptr, "\033[31mtrusted\033[0m %s\n",
			  "bad\033[2J\033]52;c;secret\aend");
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[31mtrusted\033[0m"), std::string::npos);
	EXPECT_NE(output.find("badend"), std::string::npos);
	EXPECT_EQ(output.find("secret"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	cli_output_set_color_enabled(_cli_output_context_ptr, 0);
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
	EXPECT_EQ(ctx.presenter->final_rendered, 1);

	cJSON_Delete(thought);
	cJSON_Delete(call);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, InteractiveDoesNotDuplicateWorkingStatus)
{
	cJSON *thinking = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thinking", "begin", thinking);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("Thinking…"), std::string::npos);
	cJSON_Delete(thinking);
}

TEST_F(CliPresentationTest, TurnBeginsWithoutRedundantStartingStatus)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.turn_active = 0;

	testing::internal::CaptureStdout();
	cli_turn_begin(&ctx);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("Starting…"), std::string::npos);
	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
}

TEST_F(CliPresentationTest, SteeringDoesNotRestoreThinkingStatus)
{
	cJSON *steer = TextData("updated requirement");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.user.steer", "end", steer);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("updated requirement"), std::string::npos);
	EXPECT_EQ(output.find("Requirement applied"), std::string::npos);
	EXPECT_EQ(output.find("Thinking…"), std::string::npos);
	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
	cJSON_Delete(steer);
}

TEST_F(CliPresentationTest, SteeringCommitsOriginalInputInsteadOfAttachmentPayload)
{
	cJSON *steer = TextData("Inspect [Image #1] [Image: /tmp/attachment.png]");
	cJSON_AddStringToObject(steer, "display_text", "Inspect [Image #1]");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.user.steer", "end", steer);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Inspect [Image #1]"), std::string::npos);
	EXPECT_EQ(output.find("Inspect [Image #1]"), output.rfind("Inspect [Image #1]"));
	EXPECT_EQ(output.find("/tmp/attachment.png"), std::string::npos);
	EXPECT_EQ(output.find("Requirement applied"), std::string::npos);
	cJSON_Delete(steer);
}

TEST_F(CliPresentationTest, PreparingPromptClearsLiveStatus)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	cli_feed_live_set(&ctx, "Running tool…");
	ASSERT_EQ(cli_feed_live_active(&ctx), 1);
	cli_presentation_prepare_prompt(&ctx);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
	EXPECT_NE(output.find("Running tool…"), std::string::npos);
}

TEST_F(CliPresentationTest, InteractiveCoalescesBackgroundLifecycle)
{
	cJSON *started = TextData("");
	cJSON *ready = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.turn_active = 0;
	struct morph_event begin{MORPH_EVENT_BACKGROUND,
				 "background.started",
				 "begin",
				 "memory consolidation queued",
				 started,
				 "turn_test"};
	struct morph_event end{MORPH_EVENT_BACKGROUND,
			       "background.ready",
			       "ready",
			       "memory consolidation queued",
			       ready,
			       "turn_test"};

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &begin), 0);
	ASSERT_EQ(cli_presentation_event(&ctx, &end), 0);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("memory consolidation queued"), std::string::npos);
	EXPECT_EQ(output.find("memory consolidation queued",
			      output.find("memory consolidation queued") + 1),
		  std::string::npos);
	EXPECT_EQ(output.find("Background"), std::string::npos);

	cJSON_Delete(started);
	cJSON_Delete(ready);
}

TEST_F(CliPresentationTest, BackgroundLifecyclePreservesForegroundStatus)
{
	cJSON *started = TextData("");
	cJSON *ready = TextData("");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	struct morph_event begin{MORPH_EVENT_BACKGROUND,
				 "background.started",
				 "begin",
				 "memory consolidation queued",
				 started,
				 "turn_test"};
	struct morph_event end{MORPH_EVENT_BACKGROUND,
			       "background.ready",
			       "ready",
			       "memory consolidation queued",
			       ready,
			       "turn_test"};

	testing::internal::CaptureStdout();
	cli_feed_live_set(&ctx, "Thinking…");
	ASSERT_EQ(cli_presentation_event(&ctx, &begin), 0);
	ASSERT_EQ(cli_presentation_event(&ctx, &end), 0);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Thinking…"), std::string::npos);
	EXPECT_EQ(output.find("memory consolidation queued"), std::string::npos);
	EXPECT_EQ(cli_feed_live_active(&ctx), 1);

	cJSON_Delete(started);
	cJSON_Delete(ready);
}

TEST_F(CliPresentationTest, SessionTitleEventsOnlyUpdateFooterAndPreserveActivity)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_feed_live_set(&ctx, "Thinking...");
	struct morph_event event{MORPH_EVENT_BACKGROUND, "session.title", "begin",
				 "Naming session...",	 nullptr,	  nullptr};

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	EXPECT_EQ(ctx.input_status.session_titles_pending, 2);
	event.phase = "end";
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	EXPECT_EQ(ctx.input_status.session_titles_pending, 1);
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(ctx.input_status.session_titles_pending, 0);
	EXPECT_TRUE(output.empty());
	EXPECT_EQ(cli_feed_live_active(&ctx), 1);
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
	cJSON_AddNumberToObject(failed, "error_code", MORPH_ERR_NOT_CONFIGURED);
	cJSON_AddStringToObject(failed, "error",
				"Missing MCP token: environment variable 'MEEGO_TOKEN' is not set");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_MCP, "mcp.connecting", "begin", connecting);
	Emit(MORPH_EVENT_MCP, "mcp.failed", "failed", failed);
	std::string output = testing::internal::GetCapturedStdout();
	size_t root = output.find("MCP meego");

	ASSERT_NE(root, std::string::npos);
	EXPECT_EQ(output.find("MCP meego", root + 1), std::string::npos);
	EXPECT_NE(output.find("├ Connecting"), std::string::npos);
	EXPECT_NE(output.find("└ ⊗ Failed · Missing MCP token: environment variable "
			      "'MEEGO_TOKEN' is not set"),
		  std::string::npos);
	EXPECT_EQ(ctx.presenter->mcp_tree_active, 0);

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
	cli_feed_live_set(&ctx, "Thinking…");
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
	EXPECT_NE(output.find("├ Discovering capabilities"), std::string::npos);
	EXPECT_NE(output.find("└ ● Ready · 12 tools, 3 resources, 2 prompts"), std::string::npos);
	EXPECT_EQ(ctx.presenter->mcp_tree_active, 0);
	EXPECT_EQ(cli_feed_live_active(&ctx), 1);

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
	EXPECT_NE(output.find("web_search"), std::string::npos);
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
	const char *patch = "*** Begin Patch\n"
			    "*** Update File: src/example.c\n"
			    "@@\n"
			    "-old value\n"
			    "+new value\n"
			    "*** End Patch";
	const char *content = "foo\nold value\nbar\n";

	std::filesystem::create_directories(std::string(dir) + "/src");
	ASSERT_EQ(file_write_all((std::string(dir) + "/src/example.c").c_str(), content,
				 std::strlen(content)),
		  0);
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
	EXPECT_NE(output.find("*** Update File: src/example.c"), std::string::npos);
	EXPECT_NE(output.find("@@ -1,3 +1,3 @@"), std::string::npos);
	EXPECT_NE(output.find("-old value"), std::string::npos);
	EXPECT_NE(output.find("+new value"), std::string::npos);
	EXPECT_EQ(output.find("*** End Patch"), std::string::npos);
	EXPECT_NE(output.find("│     2 -old value"), std::string::npos);
	EXPECT_NE(output.find("│     2 +new value"), std::string::npos);
	EXPECT_EQ(output.find("input:"), std::string::npos);
	EXPECT_EQ(output.find("patch display truncated"), std::string::npos);

	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, InteractiveCompactFeedShowsApplyPatchDiff)
{
	cJSON *call = cJSON_CreateObject();
	cJSON *args = cJSON_CreateObject();
	cJSON *result = cJSON_CreateObject();
	const char *patch = "*** Begin Patch\n"
			    "*** Update File: src/example.c\n"
			    "@@\n"
			    "-old value\n"
			    "+new value\n"
			    "*** End Patch";
	const char *content = "foo\nold value\nbar\n";

	std::filesystem::create_directories(std::string(dir) + "/src");
	ASSERT_EQ(file_write_all((std::string(dir) + "/src/example.c").c_str(), content,
				 std::strlen(content)),
		  0);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	cJSON_AddStringToObject(call, "tool", "apply_patch");
	cJSON_AddStringToObject(call, "tool_call_id", "patch-1");
	cJSON_AddStringToObject(args, "input", patch);
	cJSON_AddItemToObject(call, "args", args);
	cJSON_AddStringToObject(result, "tool", "apply_patch");
	cJSON_AddStringToObject(result, "tool_call_id", "patch-1");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	ASSERT_EQ(file_write_all((std::string(dir) + "/src/example.c").c_str(),
				 "foo\nnew value\nbar\n", 18),
		  0);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("apply_patch src/example.c"), std::string::npos);
	EXPECT_NE(output.find("@@ -1,3 +1,3 @@"), std::string::npos);
	EXPECT_NE(output.find("ctrl+o to expand"), std::string::npos);
	EXPECT_EQ(output.find("+new value"), std::string::npos);
	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string details = testing::internal::GetCapturedStdout();
	EXPECT_NE(details.find("│     2 -old value"), std::string::npos);
	EXPECT_NE(details.find("│     2 +new value"), std::string::npos);

	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, InteractiveExpandsEmbeddedJsonOneLevel)
{
	cJSON *observation = TextData("structured result");
	cJSON *result = cJSON_CreateObject();
	const char *embedded = "{\"ok\":true,\"identity\":\"user\","
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
	EXPECT_NE(output.find("└ messages: [...] 2 items"), std::string::npos);
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
	cJSON_AddStringToObject(call, "tool", "exec");
	cJSON_AddStringToObject(args, "command",
				"NOTICE=1 lark-cli im +chat-messages-list "
				"--chat-id oc_1a6eff8d491c1dd23b76789f01d625b "
				"--page-size 100");
	cJSON_AddItemToObject(call, "args", args);

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("    $ NOTICE=1 lark-cli im"), std::string::npos);
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
	cJSON *second = cJSON_Parse("{\"tool\":\"file_read\",\"tool_call_id\":\"second\","
				    "\"args\":{\"file_path\":\"second.txt\"}}");
	cJSON *first_result = cJSON_Parse("{\"tool\":\"file_read\",\"tool_call_id\":\"first\","
					  "\"result\":\"FIRST OUTPUT\"}");
	cJSON *second_result = cJSON_Parse("{\"tool\":\"file_read\",\"tool_call_id\":\"second\","
					   "\"result\":\"SECOND OUTPUT\"}");
	cJSON *thought = TextData("Checking files");
	cJSON *reasoning = TextData("Compare the two files");
	cJSON *final = TextData("Files checked");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.thought.end", "end", thought);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", first);
	Emit(MORPH_EVENT_REACT, "react.reasoning.delta", "delta", reasoning);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", second);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", second_result);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", first_result);
	Emit(MORPH_EVENT_REACT, "react.final", "end", final);
	cli_presentation_finish(&ctx);
	ctx.turn_active = 0;
	std::string compact = testing::internal::GetCapturedStdout();
	EXPECT_EQ(compact.find("hidden argument"), std::string::npos);
	EXPECT_NE(compact.find("FIRST OUTPUT"), std::string::npos);
	EXPECT_EQ(compact.find("succeeded"), std::string::npos);
	EXPECT_NE(compact.find("first.txt"), std::string::npos);
	EXPECT_NE(compact.find("Files checked"), std::string::npos);
	EXPECT_EQ(compact.find("\033[2J"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_transcript_finish(&ctx);
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());

	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string expanded = testing::internal::GetCapturedStdout();
	EXPECT_NE(expanded.find("hidden argument"), std::string::npos);
	EXPECT_LT(expanded.find("first.txt"), expanded.find("FIRST OUTPUT"));
	EXPECT_LT(expanded.find("FIRST OUTPUT"), expanded.find("Compare the two files"));
	EXPECT_LT(expanded.find("Compare the two files"), expanded.find("second.txt"));
	EXPECT_LT(expanded.find("second.txt"), expanded.find("SECOND OUTPUT"));
	EXPECT_NE(expanded.find("Turn details"), std::string::npos);
	EXPECT_NE(expanded.find("\033[?1049h"), std::string::npos);
	EXPECT_EQ(expanded.find("Files checked"), std::string::npos);
	EXPECT_EQ(ctx.turn_active, 0);

	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string collapsed = testing::internal::GetCapturedStdout();
	EXPECT_EQ(collapsed.find("FIRST OUTPUT"), std::string::npos);
	EXPECT_EQ(collapsed.find("Read  first.txt"), std::string::npos);
	EXPECT_EQ(collapsed.find("succeeded"), std::string::npos);
	EXPECT_NE(collapsed.find("\033[?1049l"), std::string::npos);
	EXPECT_EQ(cli_turn_details_is_open(&ctx), 0);
	EXPECT_EQ(ctx.presenter->final_rendered, 1);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(first_result);
	cJSON_Delete(second_result);
	cJSON_Delete(thought);
	cJSON_Delete(reasoning);
	cJSON_Delete(final);
}

TEST_F(CliPresentationTest, CompactShellFailureShowsReasonWithoutFullOutput)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"shell\","
				  "\"args\":{\"command\":\"cmake --build build\"}}");
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "tool_call_id", "shell");
	cJSON_AddStringToObject(result, "tool", "exec");
	cJSON_AddStringToObject(result, "result",
				"{\"data\":{\"exit_code\":2,\"stdout\":\"VERBOSE OUTPUT\","
				"\"stderr\":\"undefined reference to react_init\"}}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	cli_presentation_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\n⊗ Exec"), std::string::npos);
	EXPECT_NE(output.find("FAILED · "), std::string::npos);
	EXPECT_NE(output.find("undefined reference to react_init"), std::string::npos);
	EXPECT_EQ(output.find("failed"), std::string::npos);
	EXPECT_EQ(output.find("VERBOSE OUTPUT"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, CompactFeedUsesActualToolNameAndCommandOnOneLine)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);
	cJSON *call = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"probe\",\"args\":{"
				  "\"description\":\"Resolve endpoint DNS\","
				  "\"command\":\"dig +short mcp.exa.ai\"}}");
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "tool_call_id", "probe");
	cJSON_AddStringToObject(
		result, "result",
		"{\"ok\":true,\"data\":{\"exit_code\":0,\"duration_ms\":300,"
		"\"stdout\":\"104.18.6.160\\nSECOND_LINE\\nTHIRD_LINE\\nFOURTH_DETAIL_LINE\"}}");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	(void)testing::internal::GetCapturedStdout();
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(cli_test_style_at(output, "●").foreground, 32);
	EXPECT_TRUE(cli_test_style_at(output, "Exec").bold);
	EXPECT_NE(cli_test_plain_text(output).find("\n● Exec dig +short mcp.exa.ai"), std::string::npos);
	EXPECT_EQ(output.find("Resolve endpoint DNS"), std::string::npos);
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_EQ(output.find("    $ "), std::string::npos);
	EXPECT_NE(output.find("0.3s"), std::string::npos);
	EXPECT_EQ(cli_test_style_at(output, "dig").foreground, 36);
	EXPECT_NE(output.find("  └ 104.18.6.160"), std::string::npos);
	EXPECT_EQ(output.find("FOURTH_DETAIL_LINE"), std::string::npos);
	EXPECT_NE(output.find("    SECOND_LINE"), std::string::npos);
	EXPECT_NE(output.find("    THIRD_LINE"), std::string::npos);
	EXPECT_NE(output.find("+ 1 line (ctrl+o to expand)"), std::string::npos);

	testing::internal::CaptureStdout();
	ToggleDetails();
	EXPECT_NE(testing::internal::GetCapturedStdout().find("FOURTH_DETAIL_LINE"),
		  std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, AppendFeedTimeoutIsAmberAndRetainsFailureState)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);
	cJSON *call = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"timeout\",\"args\":{"
				  "\"description\":\"Probe HTTPS connection\",\"command\":\"curl "
				  "-I https://exa.ai\"}}");
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "tool_call_id", "timeout");
	cJSON_AddStringToObject(
		result, "result",
		"{\"ok\":true,\"data\":{\"status\":\"timed_out\",\"duration_ms\":10000,"
		"\"stderr\":\"TCP connection timed out\"}}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	(void)testing::internal::GetCapturedStdout();
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(cli_test_style_at(output, "●").foreground, 33);
	EXPECT_TRUE(cli_test_style_at(output, "Exec").bold);
	EXPECT_NE(output.find("TIMEOUT · 10.0s"), std::string::npos);
	EXPECT_EQ(cli_test_style_at(output, "TCP connection timed out").foreground, 33);
	EXPECT_EQ(output.find("\033[32m●"), std::string::npos);
	EXPECT_EQ(output.find("\033[31m"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, CompactMcpPreviewShowsBoundedTextAndKeepsOriginalToolName)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse("{\"tool\":\"mcp_demo__search\",\"tool_call_id\":\"search\","
				  "\"args\":{\"query\":\"pattern\"}}");
	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "tool_call_id", "search");
	cJSON_AddStringToObject(result, "result",
				"{\"content\":[{\"type\":\"text\",\"text\":\"first\\nsecond\"},"
				"{\"type\":\"text\",\"text\":\"third\\nfourth\\nfifth\\n\"}]}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	(void)testing::internal::GetCapturedStdout();
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("mcp_demo__search pattern"), std::string::npos);
	EXPECT_NE(output.find("  └ first\n    second\n    third\n"), std::string::npos);
	EXPECT_NE(output.find("+ 2 lines (ctrl+o to expand)"), std::string::npos);
	EXPECT_EQ(output.find("fourth"), std::string::npos);
	EXPECT_EQ(output.find("Ran "), std::string::npos);
	EXPECT_NE(output.find("●"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(result);
}

TEST_F(CliPresentationTest, CompactProcessPollShowsCommandOnlyWhenFinished)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *exec_call = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"exec-1\","
				       "\"args\":{\"command\":\"cmake --build build\"}}");
	cJSON *exec_result =
		cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"exec-1\","
			    "\"result\":\"{\\\"ok\\\":true,\\\"data\\\":{"
			    "\\\"session_id\\\":\\\"proc-1\\\","
			    "\\\"status\\\":\\\"running\\\",\\\"duration_ms\\\":1000}}\"}");
	cJSON *poll_call = cJSON_Parse("{\"tool\":\"process\",\"tool_call_id\":\"poll-1\","
				       "\"args\":{\"session_id\":\"proc-1\",\"action\":\"poll\"}}");
	cJSON *poll_running =
		cJSON_Parse("{\"tool\":\"process\",\"tool_call_id\":\"poll-1\","
			    "\"result\":\"{\\\"ok\\\":true,\\\"data\\\":{"
			    "\\\"session_id\\\":\\\"proc-1\\\","
			    "\\\"status\\\":\\\"running\\\",\\\"duration_ms\\\":1800}}\"}");
	cJSON *final_call =
		cJSON_Parse("{\"tool\":\"process\",\"tool_call_id\":\"poll-2\","
			    "\"args\":{\"session_id\":\"proc-1\",\"action\":\"poll\"}}");
	cJSON *final_result = cJSON_Parse("{\"tool\":\"process\",\"tool_call_id\":\"poll-2\","
					  "\"result\":\"{\\\"ok\\\":true,\\\"data\\\":{"
					  "\\\"session_id\\\":\\\"proc-1\\\","
					  "\\\"status\\\":\\\"exited\\\",\\\"exit_code\\\":0,"
					  "\\\"duration_ms\\\":2500}}\"}");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", exec_call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", exec_result);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", poll_call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", poll_running);
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", final_call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", final_result);
	std::string output = testing::internal::GetCapturedStdout();

	size_t wait = output.find("\n● process cmake --build build");
	EXPECT_NE(wait, std::string::npos);
	EXPECT_EQ(output.find("\n● process cmake --build build", wait + 1), std::string::npos)
		<< output;
	EXPECT_NE(output.find("process "), std::string::npos);
	EXPECT_EQ(output.find("proc-1"), std::string::npos);
	EXPECT_NE(output.find("2.5s"), std::string::npos);
	EXPECT_NE(output.find("◉ Exec cmake --build build  RUNNING · 1.0s"), std::string::npos);

	cJSON_Delete(exec_call);
	cJSON_Delete(exec_result);
	cJSON_Delete(poll_call);
	cJSON_Delete(poll_running);
	cJSON_Delete(final_call);
	cJSON_Delete(final_result);
}

TEST_F(CliPresentationTest, ShellColorsSurviveFullScreenCapture)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);
	cJSON *call = cJSON_Parse(
		"{\"tool\":\"exec\",\"args\":{\"command\":\"rg 'hello' src/ | head\"}}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string compact = testing::internal::GetCapturedStdout();
	EXPECT_TRUE(cli_test_style_at(compact, "Exec").bold);
	EXPECT_EQ(cli_test_style_at(compact, "rg").foreground, 36);
	EXPECT_EQ(compact.find("    $ "), std::string::npos);
	EXPECT_EQ(cli_test_style_at(compact, "'hello'").foreground, 32);
	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string full = testing::internal::GetCapturedStdout();
	EXPECT_NE(cli_test_plain_text(full).find("    $ rg"), std::string::npos);
	EXPECT_EQ(cli_test_style_at(full, "rg", full.find("    $ ")).foreground, 36);
	EXPECT_EQ(cli_test_style_at(full, "|").foreground, 33);
	testing::internal::CaptureStdout();
	ToggleDetails();
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
	cJSON_AddStringToObject(call, "tool", "exec");
	std::string command = "echo " + std::string(240, 'x');
	cJSON_AddStringToObject(args, "command", command.c_str());
	for (int columns : {52, 120, 180}) {
		std::string width = std::to_string(columns);
		EXPECT_EQ(setenv("COLUMNS", width.c_str(), 1), 0);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		std::string output = testing::internal::GetCapturedStdout();
		size_t start = output.find("◉ Exec ");
		EXPECT_NE(start, std::string::npos);
		if (start != std::string::npos) {
			std::string row = output.substr(start, output.find('\n', start) - start);
			EXPECT_LE(utf8_display_width(row.c_str()), (size_t)(columns - 2));
			EXPECT_GE(utf8_display_width(row.c_str()), (size_t)(columns - 3));
		}
	}
	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, OversizedRemoteToolNameDoesNotWrapFeedHeader)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_CreateObject();
	std::string name(512, 'x');
	cJSON_AddStringToObject(call, "tool", name.c_str());
	cJSON_AddObjectToObject(call, "args");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string output = testing::internal::GetCapturedStdout();
	size_t start = output.find("xxx");
	ASSERT_NE(start, std::string::npos);
	std::string row = output.substr(start, output.find('\n', start) - start);
	EXPECT_LE(utf8_display_width(row.c_str()), static_cast<size_t>(cli_list_columns() - 2));
	EXPECT_NE(row.find("…"), std::string::npos);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, LiveToolPreviewFollowsTailAndRetainsFullStream)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"live\","
				  "\"args\":{\"command\":\"make\"}}");
	cJSON *delta = cJSON_Parse(
		"{\"tool_call_id\":\"live\","
		"\"text\":\"FIRST_DETAIL_LINE\\nsecond\\nthird\\n中文\\tprogress\\n\"}");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.stream.delta", "update", delta);
	(void)testing::internal::GetCapturedStdout();
	morph_buf_t body;
	ASSERT_EQ(morph_buf_init(&body, 128), 0);
	EXPECT_EQ(cli_transcript_live_body(ctx.transcript, &body, 100, 0), 3);
	EXPECT_STREQ(morph_buf_cstr_borrow(&body), "  └ second\n    third\n    中文 progress");
	EXPECT_EQ(utf8valid(morph_buf_cstr_borrow(&body)), nullptr);
	morph_buf_cleanup(&body);
	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string details = testing::internal::GetCapturedStdout();
	EXPECT_NE(details.find("FIRST_DETAIL_LINE"), std::string::npos);
	cJSON_Delete(delta);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, NarrowTimeoutRowsFitActualWidth)
{
	const char *previous = getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	bool had_columns = previous != nullptr;
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse("{\"tool\":\"exec\",\"args\":{\"command\":\"sleep 30\"}}");
	cJSON *result =
		cJSON_Parse("{\"tool\":\"exec\",\"result\":"
			    "\"{\\\"data\\\":{\\\"status\\\":\\\"timed_out\\\","
			    "\\\"duration_ms\\\":10000,\\\"stderr\\\":\\\"中文 timeout\\\"}}\"}");
	for (int columns : {8, 12, 20, 24, 40}) {
		EXPECT_EQ(setenv("COLUMNS", std::to_string(columns).c_str(), 1), 0);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		(void)testing::internal::GetCapturedStdout();
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
		std::string output = testing::internal::GetCapturedStdout();
		std::istringstream lines(output);
		std::string line;
		while (std::getline(lines, line)) {
			EXPECT_EQ(utf8valid(line.c_str()), nullptr);
			EXPECT_LE(utf8_display_width_ansi(line.c_str()),
				  static_cast<size_t>(columns - 2))
				<< line;
		}
		if (columns >= 12)
			EXPECT_NE(output.find("TIMEOUT"), std::string::npos);
	}
	if (had_columns)
		setenv("COLUMNS", saved.c_str(), 1);
	else
		unsetenv("COLUMNS");
	cJSON_Delete(result);
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
	cJSON *call = cJSON_Parse("{\"tool\":\"example\",\"tool_call_id\":\"stream\",\"args\":{}}");
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
	ToggleDetails();
	ToggleDetails();
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
	cJSON_AddItemToArray(
		steps, cJSON_Parse("{\"description\":\"Review changes\",\"status\":\"pending\"}"));
	cJSON_AddItemToArray(
		steps, cJSON_Parse("{\"description\":\"Failed check\",\"status\":\"failed\"}"));
	cJSON_AddItemToArray(
		steps, cJSON_Parse("{\"description\":\"Skipped check\",\"status\":\"skipped\"}"));
	cJSON_AddItemToObject(plan, "steps", steps);
	cJSON_AddItemToArray(plans, plan);
	cJSON_AddItemToObject(result, "plans", plans);
	cJSON_AddItemToObject(event_data, "data", result);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Plan · 1/5 completed"), std::string::npos);
	EXPECT_NE(output.find("● Inspect CLI"), std::string::npos);
	EXPECT_NE(output.find("◉ Implement presenter"), std::string::npos);
	EXPECT_NE(output.find("◯ Review changes"), std::string::npos);
	EXPECT_NE(output.find("⊗ Failed check"), std::string::npos);
	EXPECT_NE(output.find("– Skipped check"), std::string::npos);
	EXPECT_EQ(output.find("{\"plans\""), std::string::npos);
	cJSON_Delete(event_data);
}

TEST_F(CliPresentationTest, InteractivePlanOperationsRenderOnlySelectedId)
{
	cJSON *event_data = cJSON_Parse(R"({"tool":"plan","text":"plan state","data":{
		"selected_plan_id":"current","selected_plan":"same","plans":[
		{"id":"previous","name":"same","steps":[
			{"description":"Previous completed work","status":"completed"}]},
		{"id":"current","name":"same","steps":[
			{"description":"Current work","status":"in_progress","active":true}]},
		{"id":"other","name":"other","steps":[
			{"description":"Other pending work","status":"pending"}]}
	]}})");
	ASSERT_NE(event_data, nullptr);
	cJSON *data = cJSON_GetObjectItemCaseSensitive(event_data, "data");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	for (const char *command : {"create", "update", "checkpoint", "verify", "get"}) {
		SCOPED_TRACE(command);
		cJSON_DeleteItemFromObjectCaseSensitive(data, "command");
		cJSON_AddStringToObject(data, "command", command);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
		std::string output = testing::internal::GetCapturedStdout();

		EXPECT_EQ(output, "  Plan · 0/1 completed\n  ◉ Current work\n");
	}

	/* A selected completed plan must still be visible when queried directly. */
	cJSON_ReplaceItemInObject(data, "selected_plan_id", cJSON_CreateString("previous"));
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(output, "  Plan · 1/1 completed\n  ● Previous completed work\n");
	cJSON_Delete(event_data);
}

TEST_F(CliPresentationTest, InteractivePlanGetByNameRendersFirstMatch)
{
	cJSON *event_data = cJSON_Parse(R"({"tool":"plan","text":"plan state","data":{
		"command":"get","selected_plan":"same","plans":[
		{"id":"other","name":"other","steps":[
			{"description":"Other work","status":"completed"}]},
		{"id":"first","name":"same","steps":[
			{"description":"Selected work","status":"completed"}]},
		{"id":"second","name":"same","steps":[
			{"description":"Duplicate name work","status":"pending"}]}
	]}})");
	ASSERT_NE(event_data, nullptr);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output, "  Plan · 1/1 completed\n  ● Selected work\n");
	cJSON_Delete(event_data);
}

TEST_F(CliPresentationTest, InteractivePlanQueriesWithoutSelectionRenderAllPlans)
{
	cJSON *event_data = cJSON_Parse(R"({"tool":"plan","text":"plan state","data":{
		"plans":[
		{"id":"previous","name":"first","steps":[
			{"description":"Previous work","status":"completed"}]},
		{"id":"current","name":"second","steps":[
			{"description":"Current work","status":"pending"}]}
	]}})");
	ASSERT_NE(event_data, nullptr);
	cJSON *data = cJSON_GetObjectItemCaseSensitive(event_data, "data");
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;

	for (const char *command : {"list", "get"}) {
		SCOPED_TRACE(command);
		cJSON_DeleteItemFromObjectCaseSensitive(data, "command");
		cJSON_AddStringToObject(data, "command", command);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_REACT, "react.observation", "end", event_data);
		std::string output = testing::internal::GetCapturedStdout();

		EXPECT_EQ(output, "  Plan · 1/1 completed\n  ● Previous work\n"
				  "  Plan · 0/1 completed\n  ◯ Current work\n");
	}
	cJSON_Delete(event_data);
}

TEST(CliApprovalCardTest, WrapsEveryFieldWithinMatchingBorders)
{
	const char *configured = getenv("COLUMNS");
	std::string saved = configured ? configured : "";
	bool had_columns = configured != nullptr;
	struct cli_application ctx = {};
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	std::string long_value;
	for (int i = 0; i < 30; i++)
		long_value += "中文/path-without-spaces/";
	const char *programs[] = {long_value.c_str(), "python3"};
	struct tool_operation op = {};
	op.kind = TOOL_OP_COMMAND;
	op.tool_name = long_value.c_str();
	op.action = long_value.c_str();
	op.reason = long_value.c_str();
	op.scope = long_value.c_str();
	op.principal = long_value.c_str();
	op.target = long_value.c_str();
	op.programs = programs;
	op.programs_count = 2;
	for (int columns : {8, 12, 18, 19, 24, 40, 60, 80, 120}) {
		EXPECT_EQ(setenv("COLUMNS", std::to_string(columns).c_str(), 1), 0);
		for (int kind = 0; kind < 4; kind++) {
			testing::internal::CaptureStdout();
			if (kind == 0) {
				cli_approval_render_tool(&ctx, long_value.c_str(),
							 long_value.c_str());
			} else {
				op.kind = kind == 1 ? TOOL_OP_COMMAND : TOOL_OP_PATH_WRITE;
				op.path_access = kind == 3 ? TOOL_PATH_ACCESS_ALL : 0;
				cli_approval_render_operation(&ctx, &op);
			}
			std::string output = testing::internal::GetCapturedStdout();
			char *safe =
				utf8_terminal_sanitize_dup(output.c_str(), output.size(),
							   UTF8_TERMINAL_TEXT_MULTILINE, nullptr);
			EXPECT_NE(safe, nullptr);
			if (!safe)
				continue;
			std::istringstream lines(safe);
			free(safe);
			std::string line;
			std::string body;
			size_t width = static_cast<size_t>(std::min(columns - 2, 80));
			int borders = 0;
			while (std::getline(lines, line)) {
				if (line.empty())
					continue;
				EXPECT_EQ(utf8valid(line.c_str()), nullptr);
				EXPECT_EQ(utf8_display_width(line.c_str()), width) << line;
				if (line.rfind("╭", 0) == 0 || line.rfind("╰", 0) == 0) {
					const std::string corner =
						line.rfind("╭", 0) == 0 ? "╮" : "╯";
					EXPECT_EQ(line.substr(line.size() - corner.size()), corner);
					borders++;
				} else {
					EXPECT_EQ(line.rfind("│ ", 0), 0u) << line;
					const std::string edge = " │";
					EXPECT_EQ(line.substr(line.size() - edge.size()), edge);
					size_t prefix =
						std::string("│ ").size() + (width >= 17 ? 11u : 0u);
					std::string content = line.substr(
						prefix, line.size() - prefix - edge.size());
					content.erase(content.find_last_not_of(' ') + 1);
					body += content;
				}
			}
			EXPECT_EQ(borders, 2);
			/* Long values must survive wrapping, including their tail. */
			size_t offset = 0;
			int values = 0;
			while ((offset = body.find(long_value, offset)) != std::string::npos) {
				values++;
				offset += long_value.size();
			}
			EXPECT_EQ(values, kind == 0 ? 2 : 5);
		}
	}
	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
}

TEST(CliUserInputTest, SeparatesMultilineInputAndSanitizesTerminalControls)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	cli_output_set_color_enabled(_cli_output_context_ptr, 0);
	testing::internal::CaptureStdout();
	cli_presentation_user_input(_cli_output_context_ptr, "Fix 中文🙂\nsecond\tline\033[2J");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\n> Fix 中文🙂"), std::string::npos);
	EXPECT_NE(output.find("\n  second line"), std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);
	EXPECT_EQ(output.find("Requirement applied"), std::string::npos);
	EXPECT_EQ(output.substr(0, 1), "\n");
	EXPECT_EQ(output.back(), '\n');
	EXPECT_EQ(output.substr(output.size() - 2), "\n\n");
	EXPECT_EQ(output.substr(1, output.size() - 3).find("\n\n"), std::string::npos);
	cli_output_set_color_enabled(_cli_output_context_ptr, 1);
}

TEST(CliUserInputTest, WrapsWithoutTruncatingWideText)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	cli_output_set_color_enabled(_cli_output_context_ptr, 0);
	std::string input;
	for (int i = 0; i < 160; i++)
		input += "中文🙂";
	testing::internal::CaptureStdout();
	cli_presentation_user_input(_cli_output_context_ptr, input.c_str());
	std::string output = testing::internal::GetCapturedStdout();
	size_t count = 0;
	for (size_t pos = 0; (pos = output.find("🙂", pos)) != std::string::npos;
	     pos += strlen("🙂"))
		count++;
	EXPECT_EQ(count, 160u);
	EXPECT_EQ(utf8valid(output.c_str()), nullptr);
	EXPECT_EQ(output.find("truncated"), std::string::npos);
	cli_output_set_color_enabled(_cli_output_context_ptr, 1);
}

TEST(CliUserInputTest, ShortMessageHasOuterMarginsWithoutInternalPaddingRows)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	cli_output_set_color_enabled(_cli_output_context_ptr, 0);
	testing::internal::CaptureStdout();
	cli_presentation_user_input(_cli_output_context_ptr, "Review");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(output, "\n> Review\n\n");
	cli_output_set_color_enabled(_cli_output_context_ptr, 1);
}

TEST(CliWelcomeTest, FitsBoundedLayoutAndWrapsHintsAtNarrowWidths)
{
	struct cli_application context = {};
	const char *columns = getenv("COLUMNS");
	std::string saved = columns ? columns : "";
	bool had_columns = columns != nullptr;
	std::string wide_output;

	cli_output_set_color_enabled(&context, 0);
	for (int width : {1, 12, 20, 40, 60, 80, 120}) {
		SCOPED_TRACE(width);
		ASSERT_EQ(setenv("COLUMNS", std::to_string(width).c_str(), 1), 0);
		testing::internal::CaptureStdout();
		cli_banner_print(&context, "deepseek-flash", "~/Work/macos/cat");
		std::string output = testing::internal::GetCapturedStdout();

		EXPECT_EQ(output.find("\033"), std::string::npos);
		EXPECT_EQ(utf8valid(output.c_str()), nullptr);
		std::istringstream lines(output);
		std::string line;
		while (std::getline(lines, line)) {
			EXPECT_LE(utf8_display_width(line.c_str()),
				  static_cast<size_t>(std::min(width, 80)));
		}
		if (width >= 40) {
			EXPECT_NE(output.find("morph"), std::string::npos);
			EXPECT_NE(output.find("Workspace"), std::string::npos);
			EXPECT_NE(output.find("~/Work/macos/cat"), std::string::npos);
			EXPECT_NE(output.find("Model"), std::string::npos);
			EXPECT_NE(output.find("deepseek-flash"), std::string::npos);
			EXPECT_NE(output.find("/help"), std::string::npos);
			EXPECT_NE(output.find("Commands"), std::string::npos);
			EXPECT_NE(output.find("/switch"), std::string::npos);
			EXPECT_NE(output.find("Sessions"), std::string::npos);
			EXPECT_NE(output.find("./image.png"), std::string::npos);
			EXPECT_NE(output.find("Attach image"), std::string::npos);
			EXPECT_NE(output.find("Enter Send"), std::string::npos);
			EXPECT_NE(output.find("Ctrl+J New line"), std::string::npos);
			EXPECT_NE(output.find("Esc Cancel"), std::string::npos);
			EXPECT_NE(output.find("Ctrl+Cmd+V Paste image"), std::string::npos);
		}
		if (width == 80)
			wide_output = output;
		if (width == 120)
			EXPECT_EQ(output, wide_output);
	}
	if (had_columns)
		setenv("COLUMNS", saved.c_str(), 1);
	else
		unsetenv("COLUMNS");
}

TEST(CliWelcomeTest, SanitizesMetadataAndKeepsLongPathTail)
{
	struct cli_application context = {};
	const char *columns = getenv("COLUMNS");
	std::string saved = columns ? columns : "";
	bool had_columns = columns != nullptr;

	cli_output_set_color_enabled(&context, 0);
	for (int width : {1, 12, 20, 40, 60, 80, 120}) {
		SCOPED_TRACE(width);
		ASSERT_EQ(setenv("COLUMNS", std::to_string(width).c_str(), 1), 0);
		testing::internal::CaptureStdout();
		cli_banner_print(&context, "model\033[2J\r\n",
				 "/very/long/中文/workspace/path/ending\033]0;spoof\a");
		std::string output = testing::internal::GetCapturedStdout();

		EXPECT_EQ(output.find("\033"), std::string::npos);
		EXPECT_EQ(output.find('\r'), std::string::npos);
		EXPECT_EQ(output.find('\a'), std::string::npos);
		EXPECT_EQ(utf8valid(output.c_str()), nullptr);
		std::istringstream lines(output);
		std::string line;
		while (std::getline(lines, line)) {
			EXPECT_LE(utf8_display_width(line.c_str()),
				  static_cast<size_t>(std::min(width, 80)));
		}
		if (width >= 40) {
			EXPECT_NE(output.find("Workspace"), std::string::npos);
			EXPECT_NE(output.find("Model"), std::string::npos);
			EXPECT_NE(output.find("/ending"), std::string::npos);
		}
	}
	if (had_columns)
		setenv("COLUMNS", saved.c_str(), 1);
	else
		unsetenv("COLUMNS");
}

TEST(CliWelcomeTest, UsesSharedPurpleAccentWithoutCyan)
{
	struct cli_application context = {};
	const char *columns = getenv("COLUMNS");
	std::string saved = columns ? columns : "";
	bool had_columns = columns != nullptr;

	ASSERT_EQ(setenv("COLUMNS", "80", 1), 0);
	cli_output_set_color_enabled(&context, 1);
	testing::internal::CaptureStdout();
	cli_banner_print(&context, "deepseek-flash", "~/Work/macos/cat");
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("\033[38;5;141m"), std::string::npos);
	EXPECT_EQ(output.find("\033[38;5;51m"), std::string::npos);
	EXPECT_EQ(output.find("\033[36m"), std::string::npos);
	if (had_columns)
		setenv("COLUMNS", saved.c_str(), 1);
	else
		unsetenv("COLUMNS");
}

TEST(CliApprovalCardTest, DirectoryBatchDisplaysAllRequestedAccessAndLifetime)
{
	struct cli_application context = {};
	context.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	struct tool_operation operation = {};
	operation.kind = TOOL_OP_PATH_DELETE;
	operation.tool_name = "request_permissions";
	operation.principal = "*";
	operation.target = "/workspace/project";
	operation.scope = "session";
	operation.reason = "Update generated project files";
	operation.path_access = TOOL_PATH_ACCESS_ALL;
	testing::internal::CaptureStdout();
	cli_approval_render_operation(&context, &operation);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Directory Access Approval"), std::string::npos);
	EXPECT_NE(output.find("read/write/delete"), std::string::npos);
	EXPECT_NE(output.find("Lifetime"), std::string::npos);
	EXPECT_NE(output.find("session"), std::string::npos);
	EXPECT_NE(output.find("All tools and commands"), std::string::npos);
	EXPECT_NE(output.find("Update generated project files"), std::string::npos);
}

TEST_F(CliPresentationTest, SlashCommandHasAPlainSanitizedHeading)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cli_output_set_color_enabled(&ctx, 1);
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&ctx, "/rename 中文\033[2J\nname");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("\033[38;5;141m/rename 中文"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	EXPECT_EQ(output.find("\033[48;"), std::string::npos);
	EXPECT_EQ(output.find("›"), std::string::npos);
	EXPECT_EQ(output.back(), '\n');
	EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 1);
}

TEST_F(CliPresentationTest, SlashHeadingDoesNotChangePlainOrJsonOutput)
{
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&ctx, "/share");
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	cli_presentation_command_input(&ctx, "/share");
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&ctx, "/share");
	EXPECT_EQ(testing::internal::GetCapturedStdout(), "/share\n");
}

TEST_F(CliPresentationTest, SlashResultGapOnlyChangesInteractiveOutput)
{
	testing::internal::CaptureStdout();
	cli_presentation_command_end(&ctx);
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	cli_presentation_command_end(&ctx);
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	cli_presentation_command_end(&ctx);
	EXPECT_EQ(testing::internal::GetCapturedStdout(), "\n");
	cli_output_set_color_enabled(&ctx, 1);
	testing::internal::CaptureStdout();
	cli_presentation_command_end(&ctx);
	EXPECT_EQ(testing::internal::GetCapturedStdout(), "\033[0m\n");
}

TEST_F(CliPresentationTest, SlashOutputGapIsDeferredAndNormalizesHeadingNewlines)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&ctx, "/hi");
	cli_presentation_command_end(&ctx);
	EXPECT_EQ(testing::internal::GetCapturedStdout(), "/hi\n\n");
	for (const char *prefix : {"", "\n", "\n\n"}) {
		testing::internal::CaptureStdout();
		cli_presentation_command_input(&ctx, "/share");
		cli_output_printf(&ctx, "%sSaved to ~/Downloads/share.html\n", prefix);
		cli_presentation_command_end(&ctx);
		EXPECT_EQ(testing::internal::GetCapturedStdout(),
			  "/share\n\nSaved to ~/Downloads/share.html\n\n");
	}
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&ctx, "/model");
	cli_output_putchar(&ctx, '\n');
	cli_output_fputs(&ctx, "\nCurrent model\n", stdout);
	cli_presentation_command_end(&ctx);
	EXPECT_EQ(testing::internal::GetCapturedStdout(), "/model\n\nCurrent model\n\n");
}

TEST_F(CliPresentationTest, FileReadDetailsPrioritizeContentAndContinuation)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse("{\"tool\":\"file_read\",\"tool_call_id\":\"read\","
				  "\"args\":{\"file_path\":\"sample.txt\"}}");
	cJSON *response = cJSON_CreateObject();
	cJSON_AddStringToObject(response, "tool", "file_read");
	cJSON_AddStringToObject(response, "tool_call_id", "read");
	cJSON_AddStringToObject(response, "result",
				"{\"ok\":true,\"data\":{\"file_path\":\"sample.txt\","
				"\"version\":\"INTERNAL_VERSION_MARKER\",\"encoding\":\"utf-8\","
				"\"returned_lines\":2,\"truncated\":true,\"next_byte_offset\":123,"
				"\"content\":\"FIRST_FILE_LINE\\nLAST_FILE_LINE\\n\"}}");

	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", response);
	(void)testing::internal::GetCapturedStdout();
	testing::internal::CaptureStdout();
	ToggleDetails();
	std::string expanded = testing::internal::GetCapturedStdout();
	EXPECT_NE(expanded.find("FIRST_FILE_LINE"), std::string::npos);
	EXPECT_NE(expanded.find("LAST_FILE_LINE"), std::string::npos);
	EXPECT_NE(expanded.find("2 returned lines"), std::string::npos);
	EXPECT_NE(expanded.find("More content at byte offset 123"), std::string::npos);
	EXPECT_EQ(expanded.find("INTERNAL_VERSION_MARKER"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(response);
}

TEST_F(CliPresentationTest, CollaborationUsesSemanticTitlesAndNoGenericRunningBody)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse(R"({"tool":"spawn_agent","tool_call_id":"spawn",
		"args":{"task_name":"parser","message":"Inspect the parser independently."}})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	std::string pending = testing::internal::GetCapturedStdout();
	EXPECT_NE(pending.find("Spawning"), std::string::npos);
	EXPECT_EQ(pending.find("spawn_agent"), std::string::npos);
	morph_buf_t line{};
	morph_buf_t body{};
	ASSERT_EQ(morph_buf_init(&line, 128), 0);
	ASSERT_EQ(morph_buf_init(&body, 128), 0);
	EXPECT_EQ(cli_transcript_live_text(ctx.transcript, &line, 60, 0), 1);
	EXPECT_STREQ(morph_buf_cstr_borrow(&line), "• Spawning /root/parser");
	EXPECT_EQ(cli_transcript_live_body(ctx.transcript, &body, 60, 0), 0);
	EXPECT_EQ(body.len, 0u);
	cJSON *result = cJSON_Parse(R"({"tool":"spawn_agent","tool_call_id":"spawn",
		"result":"{\"agent_id\":\"/root/parser\"}"})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
	std::string completed = testing::internal::GetCapturedStdout();
	EXPECT_NE(completed.find("Spawned"), std::string::npos);
	EXPECT_NE(completed.find("/root/parser"), std::string::npos);
	EXPECT_NE(completed.find("Inspect the parser independently."), std::string::npos);
	EXPECT_EQ(completed.find("agent_id"), std::string::npos);
	EXPECT_EQ(completed.find("Running…"), std::string::npos);
	morph_buf_cleanup(&line);
	morph_buf_cleanup(&body);
	cJSON_Delete(result);
	cJSON_Delete(call);
}

TEST_F(CliPresentationTest, AgentWaitHasAConciseTitleAndPreservesFailureReason)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *call = cJSON_Parse(R"({"tool":"wait_agent","tool_call_id":"wait","args":{}})");
	cJSON *failure = cJSON_Parse(R"({"tool":"wait_agent","tool_call_id":"wait",
		"error":"Permission denied","error_code":-13})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
	Emit(MORPH_EVENT_TOOL, "tool.failed", "end", failure);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Waiting for agents"), std::string::npos);
	EXPECT_NE(output.find("Permission denied"), std::string::npos);
	EXPECT_EQ(output.find("wait_agent"), std::string::npos);
	cJSON_Delete(call);
	cJSON_Delete(failure);
}

TEST(CliAgentsPresentationTest, CompactActivityBoundsRowsAndOmitsTaskPrompts)
{
	struct agent_info agents[4]{};
	std::string instruction(2000, 'x');
	for (int i = 0; i < 4; i++) {
		std::snprintf(agents[i].id, sizeof(agents[i].id), "/root/worker_%d", i);
		agents[i].status = AGENT_RUNNING;
		agents[i].description = instruction.data();
	}
	for (int columns : {120, 80, 60}) {
		morph_buf_t out{};
		ASSERT_EQ(morph_buf_init(&out, 128), 0);
		EXPECT_EQ(cli_agents_rows(&out, agents, 4, columns, 3, 0), 4);
		std::string rendered = morph_buf_cstr_borrow(&out);
		EXPECT_NE(rendered.find("/root/worker_0"), std::string::npos);
		EXPECT_EQ(rendered.find(instruction), std::string::npos);
		EXPECT_NE(rendered.find("└ +1 more · /agents"), std::string::npos);
		std::istringstream lines(rendered);
		std::string line;
		while (std::getline(lines, line))
			EXPECT_LT(utf8_display_width(line.c_str()), static_cast<size_t>(columns));
		morph_buf_cleanup(&out);
	}
}

TEST_F(CliPresentationTest, NestedAgentToolRowsKeepTheParentPath)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	for (const char *operation : {"spawn_agent", "send_message", "followup_task",
				      "interrupt_agent"}) {
		SCOPED_TRACE(operation);
		cJSON *call MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		cJSON_AddStringToObject(call, "tool", operation);
		cJSON_AddStringToObject(call, "tool_call_id", operation);
		cJSON_AddStringToObject(call, "agent_id", "/root/b");
		cJSON *args = cJSON_AddObjectToObject(call, "args");
		cJSON_AddStringToObject(args, "task_name", "aa");
		cJSON_AddStringToObject(args, "target", "aa");
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		(void)testing::internal::GetCapturedStdout();
		const auto *tool = cli_transcript_model_active_borrow(ctx.transcript);
		ASSERT_NE(tool, nullptr);
		morph_buf_t row MORPH_CLEANUP(morph_buf_cleanup) = {};
		ASSERT_EQ(morph_buf_init(&row, 128), 0);
		EXPECT_EQ(cli_agent_tools_row(tool, &row, 80, 0), 1);
		EXPECT_NE(strstr(morph_buf_cstr_borrow(&row), "/root/b/aa"), nullptr);
	}
}

TEST(CliAgentsPresentationTest, NestedActivityDistinguishesSameNamesAcrossBranches)
{
	struct agent_info agents[2]{};
	std::strcpy(agents[0].id, "/root/a/aa");
	std::strcpy(agents[1].id, "/root/b/aa");
	for (auto &agent : agents)
		agent.status = AGENT_RUNNING;
	for (int columns : {120, 80, 60}) {
		morph_buf_t out MORPH_CLEANUP(morph_buf_cleanup) = {};
		ASSERT_EQ(morph_buf_init(&out, 128), 0);
		EXPECT_EQ(cli_agents_rows(&out, agents, 2, columns, 3, 0), 2);
		EXPECT_NE(strstr(morph_buf_cstr_borrow(&out), "/root/a/aa"), nullptr);
		EXPECT_NE(strstr(morph_buf_cstr_borrow(&out), "/root/b/aa"), nullptr);
	}
}

TEST_F(CliPresentationTest, MultiAgentV2AnalyticsCallsStayInDetailsWithoutDefaultNoise)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	for (const char *name : {"send_message", "followup_task", "interrupt_agent", "list_agents"}) {
		SCOPED_TRACE(name);
		cJSON *call = cJSON_CreateObject();
		cJSON_AddStringToObject(call, "tool", name);
		cJSON_AddStringToObject(call, "tool_call_id", name);
		cJSON *args = cJSON_AddObjectToObject(call, "args");
		cJSON_AddStringToObject(args, "target", "/root/reader");
		cJSON_AddStringToObject(args, "message", "Preserved steering evidence");
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.call", "begin", call);
		std::string pending = testing::internal::GetCapturedStdout();
		EXPECT_TRUE(pending.empty());
		morph_buf_t live{};
		ASSERT_EQ(morph_buf_init(&live, 128), 0);
		EXPECT_EQ(cli_transcript_live_text(ctx.transcript, &live, 80, 0), 0);
		EXPECT_EQ(cli_transcript_live_body(ctx.transcript, &live, 80, 0), 0);
		EXPECT_EQ(live.len, 0u);
		morph_buf_cleanup(&live);
		cJSON *result = cJSON_CreateObject();
		cJSON_AddStringToObject(result, "tool", name);
		cJSON_AddStringToObject(result, "tool_call_id", name);
		cJSON_AddStringToObject(result, "result",
			R"({"ok":true,"data":{"text":"Preserved operation result"}})");
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_TOOL, "tool.result", "end", result);
		std::string completed = testing::internal::GetCapturedStdout();
		EXPECT_TRUE(completed.empty());
		cJSON_Delete(result);
		cJSON_Delete(call);
	}
	morph_buf_t details{};
	ASSERT_EQ(morph_buf_init(&details, 1024), 0);
	EXPECT_EQ(cli_transcript_render_details(&ctx, ctx.transcript, &details), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&details), "Preserved steering evidence"), nullptr);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&details), "Preserved operation result"), nullptr);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&details), "/root/reader"), nullptr);
	morph_buf_cleanup(&details);
}

TEST(CliAgentsPresentationTest, AgentDetailsPreserveLargeMultilineResultsAndStripControls)
{
	std::string result(10000, 'x');
	result += "\n\n```c\nint answer = 42;\n```\n\033[2JTAIL_AFTER_LONG_RESULT";
	for (int columns : {120, 80, 60}) {
		morph_buf_t details{};
		ASSERT_EQ(morph_buf_init(&details, 128), 0);
		ASSERT_EQ(cli_agents_detail(&details, "Result", result.c_str(), columns, 0), 0);
		std::string rendered = morph_buf_cstr_borrow(&details);
		EXPECT_EQ(static_cast<size_t>(std::count(rendered.begin(), rendered.end(), 'x')), 10000u);
		EXPECT_NE(rendered.find("\n    \n    ```c\n    int answer = 42;\n"), std::string::npos);
		EXPECT_NE(rendered.find("TAIL_AFTER_LONG_RESULT"), std::string::npos);
		EXPECT_EQ(rendered.find('\033'), std::string::npos);
		EXPECT_EQ(utf8valid(rendered.c_str()), nullptr);
		std::istringstream lines(rendered);
		std::string line;
		while (std::getline(lines, line))
			EXPECT_LT(utf8_display_width(line.c_str()), static_cast<size_t>(columns));
		morph_buf_cleanup(&details);
	}
}

TEST_F(CliPresentationTest, AgentCompletionWaitsUntilParentMarkdownAnswerIsComplete)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *first = TextData("PARENT_START **unfinished");
	cJSON *second = TextData(" emphasis** PARENT_END");
	cJSON *answer = TextData("PARENT_START **unfinished emphasis** PARENT_END");
	cJSON *completed = cJSON_Parse(R"({"parent_session_id":0,
		"agent_id":"/root/reader","status":"completed"})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", first);
	Emit(MORPH_EVENT_BACKGROUND, "agent.completed", "end", completed);
	std::string pending = testing::internal::GetCapturedStdout();
	EXPECT_EQ(pending.find("Finished"), std::string::npos);
	EXPECT_GT(ctx.presenter->agent_receipts.len, 0u);
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", second);
	Emit(MORPH_EVENT_REACT, "react.final", "end", answer);
	cli_presentation_finish(&ctx);
	cli_presentation_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	ASSERT_NE(output.find("PARENT_END"), std::string::npos);
	ASSERT_NE(output.find("Finished"), std::string::npos);
	EXPECT_LT(output.find("PARENT_END"), output.find("Finished"));
	EXPECT_EQ(output.find("Finished"), output.rfind("Finished"));
	EXPECT_EQ(ctx.presenter->agent_receipts.len, 0u);
	cJSON_Delete(first);
	cJSON_Delete(second);
	cJSON_Delete(answer);
	cJSON_Delete(completed);
}

TEST_F(CliPresentationTest, AgentReceiptsFlushAfterFailedOrCancelledParentWithoutDuplicates)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	for (const char *phase : {"failed", "cancelled"}) {
		SCOPED_TRACE(phase);
		cli_presentation_reset(&ctx);
		cJSON *body = TextData("PARENT_PARTIAL_BEFORE_ABORT");
		cJSON *completed = cJSON_Parse(R"({"parent_session_id":0,
			"agent_id":"/root/reader","status":"failed"})");
		cJSON *outcome = cJSON_CreateObject();
		cJSON_AddStringToObject(outcome, "outcome", phase);
		cJSON_AddStringToObject(outcome, "error", "Parent stopped");
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", body);
		Emit(MORPH_EVENT_BACKGROUND, "agent.completed", "end", completed);
		std::string pending = testing::internal::GetCapturedStdout();
		EXPECT_EQ(pending.find("Failed"), std::string::npos);
		testing::internal::CaptureStdout();
		Emit(MORPH_EVENT_REACT, "react.turn.end", phase, outcome);
		cli_presentation_finish(&ctx);
		cli_presentation_finish(&ctx);
		std::string output = testing::internal::GetCapturedStdout();
		ASSERT_NE(output.find("PARENT_PARTIAL_BEFORE_ABORT"), std::string::npos);
		ASSERT_NE(output.find("Failed"), std::string::npos);
		EXPECT_LT(output.find("PARENT_PARTIAL_BEFORE_ABORT"), output.find("Failed"));
		EXPECT_EQ(output.find("Failed"), output.rfind("Failed"));
		EXPECT_EQ(ctx.presenter->agent_receipts.len, 0u);
		cJSON_Delete(body);
		cJSON_Delete(completed);
		cJSON_Delete(outcome);
	}
}

TEST_F(CliPresentationTest, PresentationFinishCommitsParentBeforeDeferredAgentReceipt)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *body = TextData("PARENT_FALLBACK_BOUNDARY");
	cJSON *completed = cJSON_Parse(R"({"parent_session_id":0,
		"agent_id":"/root/reader","status":"interrupted"})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.final.delta", "delta", body);
	Emit(MORPH_EVENT_BACKGROUND, "agent.completed", "end", completed);
	std::string pending = testing::internal::GetCapturedStdout();
	EXPECT_EQ(pending.find("Interrupted"), std::string::npos);
	testing::internal::CaptureStdout();
	cli_presentation_finish(&ctx);
	cli_presentation_finish(&ctx);
	std::string output = testing::internal::GetCapturedStdout();
	ASSERT_NE(output.find("PARENT_FALLBACK_BOUNDARY"), std::string::npos);
	ASSERT_NE(output.find("Interrupted"), std::string::npos);
	EXPECT_LT(output.find("PARENT_FALLBACK_BOUNDARY"), output.find("Interrupted"));
	EXPECT_EQ(output.find("Interrupted"), output.rfind("Interrupted"));
	EXPECT_EQ(ctx.presenter->agent_receipts.len, 0u);
	cJSON_Delete(body);
	cJSON_Delete(completed);
}

TEST_F(CliPresentationTest, AgentReceiptFlushesAtAuthoritativeCommentaryMessageBoundary)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *body = TextData("PARENT_COMMENTARY_BEFORE_CHILD_RECEIPT");
	cJSON *completed = cJSON_Parse(R"({"parent_session_id":0,
		"agent_id":"/root/reader","status":"completed"})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.delta", "delta", body);
	Emit(MORPH_EVENT_BACKGROUND, "agent.completed", "end", completed);
	std::string pending = testing::internal::GetCapturedStdout();
	EXPECT_EQ(pending.find("Finished"), std::string::npos);
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_REACT, "react.content.end", "end", body);
	std::string output = testing::internal::GetCapturedStdout();
	ASSERT_NE(output.find("PARENT_COMMENTARY_BEFORE_CHILD_RECEIPT"), std::string::npos);
	ASSERT_NE(output.find("Finished"), std::string::npos);
	EXPECT_LT(output.find("PARENT_COMMENTARY_BEFORE_CHILD_RECEIPT"), output.find("Finished"));
	EXPECT_EQ(ctx.presenter->agent_receipts.len, 0u);
	cJSON_Delete(body);
	cJSON_Delete(completed);
}

TEST_F(CliPresentationTest, PlanUpdatedEventUsesExistingChecklistPresentation)
{
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	cJSON *data = cJSON_Parse(R"({"kind":"plan_state","selected_plan_id":"task",
		"plan":[{"step":"Inspect parser","status":"completed"},
		{"step":"Verify parser","status":"in_progress"}],
		"plans":[{"id":"task","name":"Task plan","steps":[
		{"description":"Inspect parser","status":"completed","active":false},
		{"description":"Verify parser","status":"in_progress","active":true}]}]})");
	testing::internal::CaptureStdout();
	Emit(MORPH_EVENT_BACKGROUND, "plan.updated", "end", data);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Plan · 1/2 completed"), std::string::npos);
	EXPECT_NE(output.find("Inspect parser"), std::string::npos);
	EXPECT_NE(output.find("Verify parser"), std::string::npos);
	EXPECT_EQ(output.find("plan_state"), std::string::npos);
	cJSON_Delete(data);
}
