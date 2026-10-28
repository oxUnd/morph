#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/command_job.h"
#include "sapi/cli/terminal.h"
int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
int cli_presentation_event(struct cli_context *ctx, const struct morph_event *event);
}

#include <fcntl.h>
#include <poll.h>
#include <regex>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

class CliTerminalTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void TearDown() override
	{
		cli_terminal_cleanup(&ctx);
		cli_set_color_enabled(&ctx, 1);
	}
};

TEST_F(CliTerminalTest, NonTerminalStatusPrintsOnlyWhenChanged)
{
	cli_set_color_enabled(&ctx, 0);
	ASSERT_EQ(cli_terminal_init(&ctx, stdout, -1), 0);

	testing::internal::CaptureStdout();
	cli_terminal_live_set(&ctx, "Thinking…");
	cli_terminal_live_set(&ctx, "Thinking…");
	cli_terminal_live_set(&ctx, "Running tool…");
	cli_terminal_live_set(&ctx, "unsafe\nstatus");
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("Thinking…"), output.rfind("Thinking…"));
	EXPECT_NE(output.find("Running tool…"), std::string::npos);
	EXPECT_NE(output.find("unsafe status"), std::string::npos);
	EXPECT_EQ(output.find("unsafe\nstatus"), std::string::npos);
	EXPECT_EQ(cli_terminal_live_active(&ctx), 1);
	EXPECT_EQ(cli_terminal_next_frame_ms(&ctx), -1);
	cli_terminal_live_clear(&ctx);
	EXPECT_EQ(cli_terminal_live_active(&ctx), 0);
}

TEST_F(CliTerminalTest, TtyViewportCoalescesFramesAndRestoresComposer)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 24;
	size.ws_row = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_terminal_init(&ctx, output, slave), 0);

	cli_terminal_live_set(&ctx, "first viewport frame");
	char bytes[BUFSIZ];
	ssize_t count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string first(bytes);
	EXPECT_NE(first.find("first viewport"), std::string::npos);
	EXPECT_NE(first.find("\033[2K"), std::string::npos);

	cli_terminal_live_set(&ctx, "coalesced update");
	errno = 0;
	count = read(master, bytes, sizeof(bytes));
	EXPECT_EQ(count, -1);
	EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
	EXPECT_GE(cli_terminal_next_frame_ms(&ctx), 0);

	cli_terminal_render_frame(&ctx, 1);
	EXPECT_GT(cli_terminal_next_frame_ms(&ctx), 800);
	cli_terminal_history_begin(&ctx);
	fprintf(output, "committed history\n");
	fflush(output);
	cli_terminal_history_end(&ctx);
	cli_terminal_composer_resume(&ctx);
	cli_terminal_composer_suspend(&ctx);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string restored(bytes);
	EXPECT_NE(restored.find("coalesced update"), std::string::npos);
	EXPECT_NE(restored.find("committed history"), std::string::npos);
	EXPECT_NE(restored.find("\033[1A"), std::string::npos);

	cli_terminal_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, TtyViewportRendersAndConsumesPromptQueue)
{
	struct cli_command_job job{};
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 48;
	size.ws_row = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ASSERT_EQ(cli_command_job_init(&job), 0);
	ctx.input_job = &job;
	ASSERT_EQ(cli_command_job_prompt(&job, "first adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "second adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "third adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "fourth adjustment", nullptr), 0);
	cli_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_terminal_init(&ctx, output, slave), 0);

	char bytes[BUFSIZ];
	cli_terminal_queue_changed(&ctx);
	ssize_t count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string queue_only(bytes);
	EXPECT_NE(queue_only.find("first adjustment"), std::string::npos);
	EXPECT_EQ(queue_only.find("Thinking…"), std::string::npos);

	cli_terminal_live_set(&ctx, "Thinking…");
	cli_terminal_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string queued(bytes);
	size_t first = queued.find("first adjustment");
	size_t second = queued.find("second adjustment");
	size_t status = queued.find("Thinking…");
	ASSERT_NE(first, std::string::npos);
	ASSERT_NE(second, std::string::npos);
	ASSERT_NE(status, std::string::npos);
	EXPECT_LT(first, second);
	EXPECT_LT(status, queued.find("Messages queued"));
	EXPECT_LT(queued.find("Messages queued"), first);
	EXPECT_NE(queued.find("↳ first adjustment"), std::string::npos);
	EXPECT_NE(queued.find("+1 more queued"), std::string::npos);
	EXPECT_LT(second, queued.find("third adjustment"));
	EXPECT_EQ(queued.find("fourth adjustment"), std::string::npos);

	char *consumed = cli_command_job_take_prompt(&job, nullptr);
	ASSERT_NE(consumed, nullptr);
	EXPECT_STREQ(consumed, "first adjustment");
	free(consumed);
	cli_terminal_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string redrawn(bytes);
	EXPECT_EQ(redrawn.find("first adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("second adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("third adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("fourth adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("\033[1A"), std::string::npos);

	ctx.input_job = nullptr;
	cli_terminal_cleanup(&ctx);
	cli_command_job_cleanup(&job);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, IndependentActivityOverridesToolAndKeepsTimerAfterResize)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.presentation_ready = 1;
	ctx.turn_active = 1;
	ASSERT_EQ(cli_terminal_init(&ctx, output, slave), 0);
	ASSERT_EQ(cli_presentation_init(&ctx), 0);
	cJSON *data = cJSON_Parse(
		"{\"tool\":\"exec\",\"tool_call_id\":\"active\",\"args\":{\"command\":\"sleep 30\"}}");
	struct morph_event event{MORPH_EVENT_TOOL, "tool.call", "begin", nullptr, data, "turn"};
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	(void)testing::internal::GetCapturedStdout();
	char bytes[BUFSIZ];
	while (read(master, bytes, sizeof(bytes)) > 0) {}
	cli_terminal_activity_set(&ctx, "Compacting context…");
	cli_terminal_render_frame(&ctx, 1);
	ssize_t count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("Compacting context…  0s"), std::string::npos);
	EXPECT_EQ(std::string(bytes).find("exec"), std::string::npos);
	usleep(1100000);
	size.ws_col = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_terminal_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("1s"), std::string::npos);
	EXPECT_GE(cli_terminal_activity_elapsed_ms(&ctx), 1000);
	size.ws_col = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_terminal_resize(&ctx);
	EXPECT_EQ(cli_terminal_columns(&ctx), 12);
	cli_terminal_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("Widen term…"), std::string::npos);
	EXPECT_EQ(std::string(bytes).find('\n'), std::string::npos);
	cli_terminal_live_clear(&ctx);
	EXPECT_EQ(cli_terminal_activity_elapsed_ms(&ctx), -1);
	cli_presentation_cleanup(&ctx);
	cJSON_Delete(data);
	cli_terminal_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, WorkAnimationPreservesComposerAndQueuedContent)
{
	struct cli_command_job job{};
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ASSERT_EQ(cli_command_job_init(&job), 0);
	ctx.input_job = &job;
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.input_status.enabled = 1;
	ASSERT_EQ(cli_command_job_prompt(&job, "queued adjustment", nullptr), 0);
	ASSERT_EQ(cli_terminal_init(&ctx, output, slave), 0);
	cli_terminal_turn_begin(&ctx);
	cli_terminal_live_set(&ctx, "stable tool output");
	cli_terminal_composer_resume(&ctx);
	char bytes[BUFSIZ];
	while (read(master, bytes, sizeof(bytes)) > 0) {}
	usleep(850000);
	ASSERT_EQ(cli_terminal_render_tick(&ctx, 3), 1);
	ssize_t count = read(master, bytes, sizeof(bytes));
	ASSERT_GT(count, 0);
	std::string first(bytes, static_cast<size_t>(count));
	EXPECT_NE(first.find("\033[7A"), std::string::npos);
	EXPECT_NE(first.find("\033[38;2;"), std::string::npos);
	EXPECT_NE(first.find("\0337"), std::string::npos);
	EXPECT_NE(first.find("\0338"), std::string::npos);
	EXPECT_EQ(first.find("stable tool output"), std::string::npos);
	EXPECT_EQ(first.find("queued adjustment"), std::string::npos);
	EXPECT_EQ(first.find("\033[2K"), std::string::npos);
	EXPECT_EQ(first.find('\n'), std::string::npos);
	EXPECT_LE(cli_terminal_next_frame_ms(&ctx), 32);
	usleep(70000);
	ASSERT_EQ(cli_terminal_render_tick(&ctx, 3), 1);
	count = read(master, bytes, sizeof(bytes));
	ASSERT_GT(count, 0);
	EXPECT_NE(first, std::string(bytes, static_cast<size_t>(count)));
	cli_terminal_turn_end(&ctx, 0);
	EXPECT_EQ(cli_terminal_render_tick(&ctx, 3), 0);
	ctx.input_job = nullptr;
	cli_terminal_cleanup(&ctx);
	cli_command_job_cleanup(&job);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, FinalOutputKeepsComposerRowWhileWorkerFinishes)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.input_status.enabled = 1;
	ASSERT_EQ(cli_terminal_init(&ctx, output, slave), 0);
	cli_terminal_turn_begin(&ctx);
	/* Final is visible while the worker still owns persistence/cleanup. */
	ctx.final_rendered = 1;
	cli_terminal_composer_resume(&ctx);
	fflush(output);
	char bytes[BUFSIZ];
	ssize_t count = read(master, bytes, sizeof(bytes));
	ASSERT_GT(count, 0);
	EXPECT_NE(std::string(bytes, static_cast<size_t>(count)).find("esc to interrupt"),
		std::string::npos);
	cli_terminal_composer_suspend(&ctx);
	cli_terminal_turn_end(&ctx, 0);
	cli_terminal_composer_resume(&ctx);
	fflush(output);
	count = read(master, bytes, sizeof(bytes));
	ASSERT_GT(count, 0);
	std::string transition(bytes, static_cast<size_t>(count));
	EXPECT_NE(transition.find("Worked for"), std::string::npos);
	/* No text wraps at this width. Count vertical cursor controls and LF
	 * to verify the composer ends at the exact row it started on. */
	std::regex vertical("\x1b\\[([0-9]*)([AB])|(\n)");
	int displacement = 0;
	for (auto match = std::sregex_iterator(transition.begin(), transition.end(), vertical);
	     match != std::sregex_iterator(); ++match) {
		if ((*match)[3].matched) {
			displacement++;
		} else {
			int rows = (*match)[1].str().empty() ? 1 : std::stoi((*match)[1]);
			displacement += (*match)[2] == "A" ? -rows : rows;
		}
	}
	EXPECT_EQ(displacement, 0);
	cli_terminal_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}
