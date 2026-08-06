#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/terminal.h"
}

#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

class CliTerminalTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void TearDown() override
	{
		cli_terminal_cleanup(&ctx);
		cli_set_color_enabled(1);
	}
};

TEST_F(CliTerminalTest, NonTerminalStatusPrintsOnlyWhenChanged)
{
	cli_set_color_enabled(0);
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
	cli_set_color_enabled(1);
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
