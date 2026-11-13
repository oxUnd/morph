#include <gtest/gtest.h>
#include "util/terminal_size.h"
#include <morph/tui/components.h>
#include <morph/tui/widgets/tree.h>
#include "util/utf8.h"
#include "util/cleanup.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <sys/ioctl.h>
#include <thread>
#include <string>
#include <unistd.h>

class TuiTest : public ::testing::Test
{
      protected:
	FILE *output = nullptr;
	void SetUp() override
	{
		ASSERT_NE(output = tmpfile(), nullptr);
	}
	void TearDown() override
	{
		if (output)
			fclose(output);
	}
	std::string take()
	{
		fflush(output);
		long length = ftell(output);
		std::string text(static_cast<size_t>(length), '\0');
		rewind(output);
		(void)fread(text.data(), 1, text.size(), output);
		(void)ftruncate(fileno(output), 0);
		rewind(output);
		return text;
	}
};

TEST_F(TuiTest, DecodesNavigationPasteAndUtf8Bytes)
{
	struct morph_tui_input input_state{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	const char input[] = "a\033[A\033OB\033[C\033[D\033[H\033[4~"
			     "\033[5~\033[6~\033[200~\033[201~\xe4\xb8\xad\033[99~z";
	ASSERT_EQ(write(fds[1], input, sizeof(input) - 1), static_cast<ssize_t>(sizeof(input) - 1));
	const int expected[] = {'a',
				MORPH_TUI_KEY_UP,
				MORPH_TUI_KEY_DOWN,
				MORPH_TUI_KEY_RIGHT,
				MORPH_TUI_KEY_LEFT,
				MORPH_TUI_KEY_HOME,
				MORPH_TUI_KEY_END,
				MORPH_TUI_KEY_PAGE_UP,
				MORPH_TUI_KEY_PAGE_DOWN,
				MORPH_TUI_KEY_PASTE_BEGIN,
				MORPH_TUI_KEY_PASTE_END,
				0xe4,
				0xb8,
				0xad,
				0,
				'z'};
	for (int key : expected)
		EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), key);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), 0);
	ASSERT_EQ(write(fds[1], "\033", 1), 1);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 100), 27);
	close(fds[1]);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), -ECANCELED);
	close(fds[0]);
	EXPECT_EQ(morph_tui_input_read(&input_state, -1, 0), -EBADF);
}

TEST_F(TuiTest, DecodesWheelAndFocusWithoutLeakingEscapeBytes)
{
	struct morph_tui_input input{};
	struct {
		const char *sequence;
		int expected;
	} cases[] = {
		{"\033[<64;10;20M", MORPH_TUI_KEY_WHEEL_UP},
		{"\033[<65;10;20M", MORPH_TUI_KEY_WHEEL_DOWN},
		{"\033[<64;10;20m", 0},
		{"\033[<0;10;20M", MORPH_TUI_KEY_MOUSE},
		{"\033[I", MORPH_TUI_KEY_FOCUS_IN},
		{"\033[O", MORPH_TUI_KEY_FOCUS_OUT},
	};

	for (const auto &item : cases) {
		SCOPED_TRACE(item.sequence);
		int result = 0;

		for (const unsigned char *byte =
			     reinterpret_cast<const unsigned char *>(item.sequence);
		     *byte; byte++) {
			EXPECT_EQ(result, 0);
			result = morph_tui_input_feed(&input, *byte);
		}
		EXPECT_EQ(result, item.expected);
		EXPECT_EQ(morph_tui_input_feed(&input, 'x'), 'x');
	}
}

TEST_F(TuiTest, DecodesMouseDragReleaseModifiersAndRejectsInvalidReports)
{
	struct morph_tui_input input{};
	auto feed = [&input](const char *sequence) {
		int key = 0;
		for (const unsigned char *p = reinterpret_cast<const unsigned char *>(sequence);
		     *p; p++)
			key = morph_tui_input_feed(&input, *p);
		return key;
	};
	EXPECT_EQ(feed("\033[<0;10;20M"), MORPH_TUI_KEY_MOUSE);
	EXPECT_EQ(input.mouse.button, MORPH_TUI_MOUSE_LEFT);
	EXPECT_EQ(input.mouse.action, MORPH_TUI_MOUSE_PRESS);
	EXPECT_EQ(input.mouse.x, 9);
	EXPECT_EQ(input.mouse.y, 19);
	EXPECT_EQ(input.mouse.modifiers, 0U);
	EXPECT_EQ(feed("\033[<60;80;24M"), MORPH_TUI_KEY_MOUSE);
	EXPECT_EQ(input.mouse.action, MORPH_TUI_MOUSE_MOVE);
	EXPECT_EQ(input.mouse.modifiers,
		  static_cast<unsigned>(MORPH_TUI_MOUSE_SHIFT | MORPH_TUI_MOUSE_ALT |
					MORPH_TUI_MOUSE_CTRL));
	EXPECT_EQ(feed("\033[<0;80;24m"), MORPH_TUI_KEY_MOUSE);
	EXPECT_EQ(input.mouse.action, MORPH_TUI_MOUSE_RELEASE);
	EXPECT_EQ(feed("\033[<2;1;1M"), MORPH_TUI_KEY_MOUSE);
	EXPECT_EQ(input.mouse.button, MORPH_TUI_MOUSE_RIGHT);
	EXPECT_EQ(input.mouse.x, 0);
	EXPECT_EQ(input.mouse.y, 0);
	for (const char *invalid : {"\033[<0;0;1M", "\033[<0;1;0M", "\033[<3;1;1M",
				    "\033[<128;1;1M", "\033[<0;2147483648;1M",
				    "\033[<0;1;1X", "\033[<66;1;1M"}) {
		EXPECT_EQ(feed(invalid), 0) << invalid;
		EXPECT_EQ(morph_tui_input_feed(&input, 'x'), 'x');
	}
}

TEST_F(TuiTest, TerminalColumnsValidateEnvironmentAndPreserveCallerFallback)
{
	const char *configured = getenv("COLUMNS");
	const bool had_columns = configured != nullptr;
	const std::string saved = configured ? configured : "";
	struct {
		const char *value;
		int expected;
	} cases[] = {{"60", 60},
		     {"0", 100},
		     {"-1", 100},
		     {"80x", 100},
		     {"999999999999999999999999", 100},
		     {"", 100}};

	for (const auto &item : cases) {
		ASSERT_EQ(setenv("COLUMNS", item.value, 1), 0);
		EXPECT_EQ(morph_terminal_columns(fileno(output), 100), item.expected);
	}
	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
}

TEST_F(TuiTest, PartialSequenceSurvivesPollingBoundaries)
{
	struct morph_tui_input input_state{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	ASSERT_EQ(write(fds[1], "\033[", 2), 2);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), 0);
	ASSERT_EQ(write(fds[1], "A", 1), 1);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), MORPH_TUI_KEY_UP);
	ASSERT_EQ(write(fds[1], "\033[", 2), 2);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 100), 0);
	ASSERT_EQ(write(fds[1], "x", 1), 1);
	EXPECT_EQ(morph_tui_input_read(&input_state, fds[0], 0), 'x');
	close(fds[0]);
	close(fds[1]);
}

TEST_F(TuiTest, CellSanitizesControlsAndClipsCjkToViewport)
{
	ASSERT_EQ(morph_tui_draw_cell(output, 8, 2, 1, 6, 20, "中文abc\033[2J", ""), 0);
	std::string text = take();
	EXPECT_EQ(text.find("\033[1;6H"), 0U);
	EXPECT_EQ(text.find("\033[2J"), std::string::npos);
	EXPECT_NE(text.find("中"), std::string::npos);
	EXPECT_EQ(text.find("文"), std::string::npos);
	ASSERT_EQ(morph_tui_draw_cell(output, 8, 2, 2, 8, 1, "x", ""), 0);
	EXPECT_EQ(take(), "\033[2;8H\033[0mx");
	ASSERT_EQ(morph_tui_draw_cell(output, 8, 2, 0, 1, 2, "x", ""), 0);
	EXPECT_TRUE(take().empty());
}

TEST_F(TuiTest, FramesOnlyRedrawChangedRowsAndInvalidateOnResize)
{
	struct morph_tui_frame frame{};
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 3, "one\ntwo\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 3, "one\ntwo\n"), 0);
	EXPECT_TRUE(take().empty());
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 3, "one\nshort\n"), 0);
	EXPECT_EQ(take(), "\033[?2026h\033[2;1H\033[0m\033[2Kshort\033[0m\033[?2026l");
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 3, "one\n"), 0);
	EXPECT_NE(take().find("\033[2;1H\033[0m\033[2K"), std::string::npos);
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 40, 2, "one\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	morph_tui_frame_cleanup(&frame);
	morph_tui_frame_cleanup(&frame);
}

TEST_F(TuiTest, SessionRestoresTermiosAndScreenModes)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	FILE *input = fopen(ptsname(master), "r+");
	ASSERT_NE(input, nullptr);
	struct termios before{}, during{}, after{};
	ASSERT_EQ(tcgetattr(fileno(input), &before), 0);
	struct morph_tui_terminal_session session{};
	ASSERT_EQ(morph_tui_terminal_session_enter(&session, input, output,
						   MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
							   MORPH_TUI_TERMINAL_SESSION_PASTE |
							   MORPH_TUI_TERMINAL_SESSION_MOUSE),
		  0);
	ASSERT_EQ(tcgetattr(fileno(input), &during), 0);
	EXPECT_EQ(during.c_lflag & (ICANON | ECHO | ISIG | IEXTEN), 0U);
	EXPECT_EQ(morph_tui_terminal_session_enter(&session, input, output, 0), -EBUSY);
	EXPECT_EQ(take(), "\033[?1049h\033[?25l\033[?2004h\033[?1000h\033[?1002h\033[?1006h");
	ASSERT_EQ(morph_tui_terminal_session_leave(&session), 0);
	ASSERT_EQ(tcgetattr(fileno(input), &after), 0);
	/* BSD sets PENDIN when changing input processing on a PTY. */
#ifdef PENDIN
	EXPECT_EQ(after.c_lflag & ~static_cast<tcflag_t>(PENDIN),
		  before.c_lflag & ~static_cast<tcflag_t>(PENDIN));
#else
	EXPECT_EQ(after.c_lflag, before.c_lflag);
#endif
	EXPECT_EQ(after.c_oflag, before.c_oflag);
	EXPECT_EQ(after.c_cflag, before.c_cflag);
	EXPECT_EQ(after.c_iflag, before.c_iflag);
	EXPECT_EQ(after.c_cc[VMIN], before.c_cc[VMIN]);
	EXPECT_EQ(after.c_cc[VTIME], before.c_cc[VTIME]);
	EXPECT_EQ(take(), "\033[?1006l\033[?1002l\033[?1000l\033[?2004l\033[0m\033[?25h\033[?1049l");
	EXPECT_EQ(morph_tui_terminal_session_leave(&session), 0);
	EXPECT_TRUE(take().empty());
	/* Inline sessions must preserve NO_COLOR's absence of SGR output. */
	ASSERT_EQ(morph_tui_terminal_session_enter(&session, input, output,
						   MORPH_TUI_TERMINAL_SESSION_PASTE),
		  0);
	EXPECT_EQ(take(), "\033[?25l\033[?2004h");
	ASSERT_EQ(morph_tui_terminal_session_leave(&session), 0);
	EXPECT_EQ(take(), "\033[?2004l\033[?25h");
	FILE *unwritable = fopen("/dev/null", "r");
	ASSERT_NE(unwritable, nullptr);
	EXPECT_LT(morph_tui_terminal_session_enter(&session, input, unwritable, 0), 0);
	EXPECT_FALSE(session.active);
	ASSERT_EQ(tcgetattr(fileno(input), &after), 0);
	EXPECT_EQ(after.c_lflag & (ICANON | ECHO | ISIG | IEXTEN),
		  before.c_lflag & (ICANON | ECHO | ISIG | IEXTEN));
	fclose(unwritable);
	fclose(input);
	close(master);
}

TEST_F(TuiTest, FailedSessionDoesNotEmitTerminalControls)
{
	FILE *input = tmpfile();
	ASSERT_NE(input, nullptr);
	struct morph_tui_terminal_session session{};
	EXPECT_EQ(morph_tui_terminal_session_enter(&session, input, output, 0), -ENOTTY);
	EXPECT_EQ(morph_tui_terminal_session_leave(&session), 0);
	EXPECT_TRUE(take().empty());
	int columns = 0, rows = 0;
	morph_tui_terminal_dimensions(-1, &columns, &rows);
	EXPECT_EQ(columns, 80);
	EXPECT_EQ(rows, 24);
	EXPECT_FALSE(morph_tui_terminal_available(input, output));
	fclose(input);
}

TEST_F(TuiTest, PureDecoderDiscardsOverlongSequencesAndResynchronizes)
{
	struct morph_tui_input input{};
	EXPECT_EQ(morph_tui_input_feed(&input, 27), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, '['), 0);
	for (int i = 0; i < MORPH_TUI_INPUT_SEQUENCE_MAX + 20; i++)
		EXPECT_EQ(morph_tui_input_feed(&input, '1'), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, '~'), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 'q'), 'q');
	EXPECT_EQ(morph_tui_input_feed(&input, 27), 0);
	EXPECT_EQ(morph_tui_input_expire(&input), 27);
	EXPECT_EQ(morph_tui_input_expire(&input), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 27), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, '['), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 27), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 'O'), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 'F'), MORPH_TUI_KEY_END);
}

TEST_F(TuiTest, InterruptedReadPreservesPendingSequence)
{
	struct morph_tui_input input{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	struct sigaction handler{}, previous{};
	handler.sa_handler = +[](int) {};
	sigemptyset(&handler.sa_mask);
	ASSERT_EQ(sigaction(SIGUSR1, &handler, &previous), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, 27), 0);
	EXPECT_EQ(morph_tui_input_feed(&input, '['), 0);
	pthread_t reader = pthread_self();
	std::thread interrupt([reader] {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		(void)pthread_kill(reader, SIGUSR1);
	});
	int rc = morph_tui_input_read(&input, fds[0], 1000);
	interrupt.join();
	(void)sigaction(SIGUSR1, &previous, nullptr);
	EXPECT_EQ(rc, -EINTR);
	EXPECT_EQ(write(fds[1], "B", 1), 1);
	EXPECT_EQ(morph_tui_input_read(&input, fds[0], 0), MORPH_TUI_KEY_DOWN);
	close(fds[0]);
	close(fds[1]);
}

TEST_F(TuiTest, CellCompositionHasNoIoAndPropagatesErrorsBeforeOutput)
{
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	EXPECT_EQ(morph_tui_draw_cell_append(&commands, 10, 2, 1, 1, 4, "abc", ""), 0);
	EXPECT_TRUE(take().empty());
	EXPECT_EQ(morph_tui_draw_write(output, &commands), 0);
	EXPECT_EQ(take(), "\033[1;1H\033[0mabc ");
	/* A later drawing error must reject the entire composed screen. */
	EXPECT_EQ(morph_tui_draw_cell_append(&commands, 10, 2, 1, 1, 4, nullptr, ""), -EINVAL);
	EXPECT_EQ(morph_tui_draw_write(output, &commands), -EINVAL);
	EXPECT_TRUE(take().empty());
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, CellsResetPreviousBackgroundAndSelectionBeforeApplyingTheirOwnStyle)
{
	morph_buf_t commands MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	ASSERT_EQ(morph_tui_draw_cell_append(&commands, 8, 3, 1, 1, 8, "selected",
					     "\033[7;48;2;16;18;20m"), 0);
	ASSERT_EQ(morph_tui_draw_cell_append(&commands, 8, 3, 2, 1, 8, "normal", ""), 0);
	ASSERT_EQ(morph_tui_draw_cell_append_aligned(&commands, 8, 3, 3, 1, 8, "42",
						 "\033[36m", 1), 0);
	const std::string text = morph_buf_cstr_borrow(&commands);
	EXPECT_NE(text.find("\033[1;1H\033[0m\033[7;48;2;16;18;20mselected"),
		  std::string::npos);
	EXPECT_NE(text.find("\033[2;1H\033[0mnormal  "), std::string::npos);
	EXPECT_NE(text.find("\033[3;1H\033[0m\033[36m      42"), std::string::npos);
}

TEST_F(TuiTest, FrameInvalidationAndOutputFailureForceFullRepaint)
{
	struct morph_tui_frame frame{};
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 2, "one\n"), 0);
	(void)take();
	morph_tui_frame_invalidate(&frame);
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 2, "one\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	FILE *unwritable = fopen("/dev/null", "r");
	ASSERT_NE(unwritable, nullptr);
	EXPECT_LT(morph_tui_frame_present(&frame, unwritable, 80, 2, "two\n"), 0);
	fclose(unwritable);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&frame.previous)), "one\n");
	ASSERT_EQ(morph_tui_frame_present(&frame, output, 80, 2, "two\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	FILE *other = tmpfile();
	ASSERT_NE(other, nullptr);
	ASSERT_EQ(morph_tui_frame_present(&frame, other, 80, 2, "two\n"), 0);
	EXPECT_GT(ftell(other), 0);
	fclose(other);
	morph_tui_frame_cleanup(&frame);
}

TEST_F(TuiTest, LayoutRegionsClampAndClipAtTheirOwnBoundary)
{
	struct morph_tui_terminal_rect area{1, 1, 20, 10};
	struct morph_tui_terminal_rect body = morph_tui_terminal_rect_inset(area, 2, 1);
	EXPECT_EQ(body.x, 3);
	EXPECT_EQ(body.y, 2);
	EXPECT_EQ(body.width, 16);
	EXPECT_EQ(body.height, 8);
	struct morph_tui_terminal_rect title = morph_tui_terminal_rect_take_top(&body, 2);
	EXPECT_EQ(title.height, 2);
	EXPECT_EQ(body.y, 4);
	struct morph_tui_terminal_rect left = morph_tui_terminal_rect_take_left(&body, 100);
	EXPECT_EQ(left.width, 16);
	EXPECT_EQ(body.width, 0);
	EXPECT_EQ(morph_tui_terminal_rect_take_top(&body, -1).height, 0);
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	ASSERT_EQ(morph_tui_draw_region_cell(&commands, title, 0, 15, 8, "abc", ""), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&commands)), "\033[2;18H\033[0m…");
	morph_buf_reset(&commands);
	EXPECT_EQ(morph_tui_draw_region_cell(&commands, body, 0, 0, 2, "x", ""), 0);
	EXPECT_EQ(commands.len, 0U);
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, ListsPreserveSelectionVisibilityAndHandleShrinkingData)
{
	struct morph_tui_widget_list_state list{};
	list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_HORIZONTAL_PAGES |
		     MORPH_TUI_WIDGET_LIST_PAGED;
	morph_tui_widget_list_state_layout(&list, 12, 5);
	EXPECT_TRUE(morph_tui_widget_list_state_update(&list, MORPH_TUI_KEY_PAGE_DOWN));
	EXPECT_EQ(list.index, 5U);
	EXPECT_EQ(list.first, 5U);
	EXPECT_TRUE(morph_tui_widget_list_state_update(&list, MORPH_TUI_KEY_END));
	EXPECT_EQ(list.index, 11U);
	EXPECT_EQ(list.first, 10U);
	morph_tui_widget_list_state_layout(&list, 3, 5);
	EXPECT_EQ(list.index, 2U);
	EXPECT_EQ(list.first, 0U);
	morph_tui_widget_list_state_layout(&list, 0, 0);
	EXPECT_EQ(list.index, 0U);
	EXPECT_EQ(list.first, 0U);
	EXPECT_FALSE(morph_tui_widget_list_state_update(&list, MORPH_TUI_KEY_DOWN));
	EXPECT_EQ(morph_tui_widget_scroll_update(0, 15, 5, MORPH_TUI_KEY_END), 10U);
	EXPECT_EQ(morph_tui_widget_scroll_update(10, 15, 5, MORPH_TUI_KEY_DOWN), 10U);
	EXPECT_EQ(morph_tui_widget_scroll_update(10, 3, 5, 0), 0U);
}

TEST_F(TuiTest, GroupedListRowsSkipHeadersAndMapItems)
{
	struct morph_tui_widget_list_groups groups{};
	const size_t counts[] = {2, 0, 3};

	ASSERT_EQ(morph_tui_widget_list_groups_init(&groups, counts, 3), 0);
	EXPECT_EQ(groups.group_count, 3U);
	EXPECT_EQ(groups.item_count, 5U);
	EXPECT_EQ(groups.row_count, 8U);
	EXPECT_EQ(groups.row_item[0], SIZE_MAX);
	EXPECT_EQ(groups.row_item[1], 0U);
	EXPECT_EQ(groups.row_item[2], 1U);
	EXPECT_EQ(groups.row_item[3], SIZE_MAX);
	EXPECT_EQ(groups.row_item[4], SIZE_MAX);
	EXPECT_EQ(groups.row_item[5], 2U);
	EXPECT_EQ(groups.item_row[4], 7U);
	EXPECT_EQ(groups.row_group[6], 2U);

	struct morph_tui_widget_list_state list{};
	list.flags = MORPH_TUI_WIDGET_LIST_PAGED;
	list.index = groups.item_row[0];
	morph_tui_widget_list_state_layout_groups(&list, &groups, 4);
	EXPECT_EQ(list.count, 8U);
	EXPECT_EQ(list.index, 1U);
	EXPECT_TRUE(morph_tui_widget_list_state_update_groups(
		&list, &groups, 4, MORPH_TUI_KEY_DOWN));
	EXPECT_EQ(list.index, 2U);
	EXPECT_TRUE(morph_tui_widget_list_state_update_groups(
		&list, &groups, 4, MORPH_TUI_KEY_DOWN));
	EXPECT_EQ(list.index, 5U);
	EXPECT_EQ(groups.row_item[list.index], 2U);
	EXPECT_TRUE(morph_tui_widget_list_state_update_groups(
		&list, &groups, 4, MORPH_TUI_KEY_UP));
	EXPECT_EQ(list.index, 2U);
	EXPECT_EQ(groups.row_item[list.index], 1U);
	EXPECT_TRUE(morph_tui_widget_list_state_update_groups(
		&list, &groups, 4, MORPH_TUI_KEY_END));
	EXPECT_EQ(list.index, 7U);
	EXPECT_TRUE(morph_tui_widget_list_state_update_groups(
		&list, &groups, 4, MORPH_TUI_KEY_HOME));
	EXPECT_EQ(list.index, 1U);

	struct morph_tui_widget_menu_state menu{};
	menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM | MORPH_TUI_WIDGET_LIST_PAGED;
	menu.list.index = groups.item_row[0];
	morph_tui_widget_list_state_layout_groups(&menu.list, &groups, 2);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(
			  &menu, &groups, 2, MORPH_TUI_KEY_DOWN),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(menu.list.index, 2U);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(
			  &menu, &groups, 2, MORPH_TUI_KEY_DOWN),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(menu.list.index, 5U);
	EXPECT_EQ(groups.row_item[menu.list.index], 2U);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(
			  &menu, &groups, 2, '\n'),
		  MORPH_TUI_WIDGET_SUBMIT);
	EXPECT_EQ(groups.row_item[menu.list.index], 2U);
	morph_tui_widget_list_groups_cleanup(&groups);
}

TEST_F(TuiTest, GroupedMenusDoNotActivateHeadersOrReportStationarySelectionChanges)
{
	struct morph_tui_widget_list_groups groups{};
	const size_t counts[] = {0, 1, 0};
	ASSERT_EQ(morph_tui_widget_list_groups_init(&groups, counts, 3), 0);
	struct morph_tui_widget_menu_state menu{};
	morph_tui_widget_list_state_layout_groups(&menu.list, &groups, 3);
	ASSERT_EQ(menu.list.index, 2U);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(
			  &menu, &groups, 3, MORPH_TUI_KEY_UP),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(menu.list.index, 2U);
	EXPECT_FALSE(morph_tui_widget_list_state_update_groups(
		&menu.list, &groups, 3, MORPH_TUI_KEY_END));
	EXPECT_EQ(menu.list.index, 2U);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(&menu, &groups, 3, '\r'),
		  MORPH_TUI_WIDGET_SUBMIT);
	morph_tui_widget_list_groups_cleanup(&groups);
	const size_t empty[] = {0, 0};
	ASSERT_EQ(morph_tui_widget_list_groups_init(&groups, empty, 2), 0);
	morph_tui_widget_list_state_layout_groups(&menu.list, &groups, 3);
	EXPECT_EQ(morph_tui_widget_menu_state_update_groups(&menu, &groups, 3, '\r'),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_FALSE(morph_tui_widget_list_state_update_groups(
		&menu.list, &groups, 3, MORPH_TUI_KEY_DOWN));
	morph_tui_widget_list_groups_cleanup(&groups);
}

TEST_F(TuiTest, MenusWrapAndIgnorePastedShortcutsAndConfirmation)
{
	struct morph_tui_widget_menu_state menu{};
	menu.list.flags = MORPH_TUI_WIDGET_LIST_WRAP | MORPH_TUI_WIDGET_LIST_TAB |
			  MORPH_TUI_WIDGET_LIST_DIGITS;
	morph_tui_widget_list_state_layout(&menu.list, 3, 3);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, MORPH_TUI_KEY_UP),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(menu.list.index, 2U);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '\t'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(menu.list.index, 0U);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, MORPH_TUI_KEY_PASTE_BEGIN),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '3'), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '\n'), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, 27), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(menu.list.index, 0U);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, MORPH_TUI_KEY_PASTE_END),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '3'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '\n'), MORPH_TUI_WIDGET_SUBMIT);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, 27), MORPH_TUI_WIDGET_BACK);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, 3), MORPH_TUI_WIDGET_CANCEL);
	morph_tui_widget_list_state_layout(&menu.list, 0, 1);
	EXPECT_EQ(morph_tui_widget_menu_state_update(&menu, '\n'), MORPH_TUI_WIDGET_UNHANDLED);
}

TEST_F(TuiTest, SelectSingleUsesFocusAndPreservesNavigationVisibility)
{
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 12, 0, 1, 1), 0);
	morph_tui_widget_list_state_layout(&selection.menu.list, 12, 3);
	EXPECT_TRUE(morph_tui_widget_select_selected(&selection, 0));
	EXPECT_FALSE(morph_tui_widget_select_selected(&selection, 12));
	EXPECT_EQ(morph_tui_widget_select_update(&selection, MORPH_TUI_KEY_END),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(selection.menu.list.first, 9u);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, MORPH_TUI_KEY_DOWN),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(selection.menu.list.index, 0u);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '3'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(morph_tui_widget_select_selected(&selection, 2));
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '\r'), MORPH_TUI_WIDGET_SUBMIT);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, 27), MORPH_TUI_WIDGET_BACK);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, 3), MORPH_TUI_WIDGET_CANCEL);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, SelectMultiEnforcesLimitsAndIgnoresPastedActions)
{
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 4, 1, 2, 2), 0);
	for (size_t i = 0; i < 4; i++)
		EXPECT_FALSE(morph_tui_widget_select_selected(&selection, i));
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '\r'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(selection.invalid);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, MORPH_TUI_KEY_PASTE_BEGIN),
		  MORPH_TUI_WIDGET_UNHANDLED);
	for (int key : std::initializer_list<int>{' ', 'j', '\r', 27, 3})
		EXPECT_EQ(morph_tui_widget_select_update(&selection, key),
			  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, MORPH_TUI_KEY_PASTE_END),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(selection.selected_count, 0u);
	EXPECT_EQ(selection.menu.list.index, 0u);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, ' '), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, 'j'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, ' '), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, 'j'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, ' '), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(selection.invalid);
	EXPECT_EQ(selection.selected_count, 2u);
	EXPECT_FALSE(morph_tui_widget_select_selected(&selection, 2));
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '\r'), MORPH_TUI_WIDGET_SUBMIT);
	EXPECT_FALSE(selection.invalid);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, 'k'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, ' '), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_FALSE(morph_tui_widget_select_selected(&selection, 1));
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '\r'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(selection.invalid);
	morph_tui_widget_select_cleanup(&selection);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, SelectValidatesConfigurationAndAllowsUnlimitedOrEmptySelection)
{
	struct morph_tui_widget_select selection{};
	EXPECT_EQ(morph_tui_widget_select_init(nullptr, 2, 1, 0, 0), -EINVAL);
	EXPECT_EQ(morph_tui_widget_select_init(&selection, 0, 1, 0, 0), -EINVAL);
	EXPECT_EQ(morph_tui_widget_select_init(&selection, 2, 1, 3, 0), -EINVAL);
	EXPECT_EQ(morph_tui_widget_select_init(&selection, 2, 1, 2, 1), -EINVAL);
	EXPECT_EQ(morph_tui_widget_select_init(&selection, 2, 0, 1, 2), -EINVAL);
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 2, 1, 0, 0), 0);
	EXPECT_EQ(selection.maximum, 2u);
	EXPECT_EQ(morph_tui_widget_select_update(&selection, '\r'), MORPH_TUI_WIDGET_SUBMIT);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, EditorEditsUnicodeInTheMiddleAndKeepsCursorVisible)
{
	struct morph_tui_widget_editor_state editor{};
	ASSERT_EQ(morph_tui_widget_editor_state_init(&editor, "中文ab", 32, 0), 0);
	EXPECT_EQ(morph_tui_widget_editor_state_init(&editor, "other", 32, 0), -EBUSY);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_HOME),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_RIGHT),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(editor.cursor, 3U);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 'x'), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&editor.text)), "中x文ab");
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 127), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 127), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&editor.text)), "文ab");
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_END),
		  MORPH_TUI_WIDGET_CHANGED);
	morph_buf_t visible{};
	ASSERT_EQ(morph_buf_init(&visible, 32), 0);
	size_t column = 0;
	ASSERT_EQ(morph_tui_widget_editor_state_view(&editor, &visible, 3, &column), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&visible)), "ab");
	EXPECT_EQ(column, 2U);
	EXPECT_TRUE(morph_tui_widget_editor_state_valid(&editor));
	morph_tui_widget_editor_state_cleanup(&editor);
	morph_tui_widget_editor_state_cleanup(&editor);
	morph_buf_cleanup(&visible);
}

TEST_F(TuiTest, EditorBoundsPasteAndSecretRendering)
{
	struct morph_tui_widget_editor_state editor{};
	ASSERT_EQ(morph_tui_widget_editor_state_init(&editor, "", 5, 1), 0);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_PASTE_BEGIN),
		  MORPH_TUI_WIDGET_UNHANDLED);
	for (int key : {'a', 'b', 'c', 'd', 'e'})
		EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, key),
			  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(editor.text.len, 4U);
	EXPECT_TRUE(editor.overflow);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 21), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, '\n'), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_PASTE_END),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_FALSE(morph_tui_widget_editor_state_valid(&editor));
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 127), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(morph_tui_widget_editor_state_valid(&editor));
	morph_buf_t visible{};
	ASSERT_EQ(morph_buf_init(&visible, 32), 0);
	size_t column = 0;
	ASSERT_EQ(morph_tui_widget_editor_state_view(&editor, &visible, 3, &column), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&visible)), "•••");
	EXPECT_EQ(column, 3U);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 21), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(editor.text.len, 0U);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 0xe4), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_FALSE(morph_tui_widget_editor_state_valid(&editor));
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 0xb8), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 0xad), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_TRUE(morph_tui_widget_editor_state_valid(&editor));
	morph_tui_widget_editor_state_cleanup(&editor);
	morph_buf_cleanup(&visible);
}

TEST_F(TuiTest, EditorGrowsAcrossCapacityBoundariesAndInsertsInsideUtf8)
{
	struct morph_tui_widget_editor_state editor
		MORPH_CLEANUP(morph_tui_widget_editor_state_cleanup) = {};
	ASSERT_EQ(morph_tui_widget_editor_state_init(&editor, "", 65537, 0), 0);
	std::string original;
	for (size_t i = 0; i < 4000; i++)
		original += "界";
	original += "TAIL";
	for (unsigned char byte : original) {
		ASSERT_EQ(morph_tui_widget_editor_state_update(&editor, byte),
			  MORPH_TUI_WIDGET_CHANGED);
		ASSERT_LT(editor.text.len, editor.text.cap);
		ASSERT_EQ(editor.text.data[editor.text.len], '\0');
	}
	EXPECT_TRUE(morph_tui_widget_editor_state_valid(&editor));
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&editor.text)), original);
	ASSERT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_HOME),
		  MORPH_TUI_WIDGET_CHANGED);
	ASSERT_EQ(morph_tui_widget_editor_state_update(&editor, MORPH_TUI_KEY_RIGHT),
		  MORPH_TUI_WIDGET_CHANGED);
	std::string inserted;
	for (size_t i = 0; i < 4000; i++)
		inserted += "文";
	for (unsigned char byte : inserted) {
		ASSERT_EQ(morph_tui_widget_editor_state_update(&editor, byte),
			  MORPH_TUI_WIDGET_CHANGED);
		ASSERT_LT(editor.text.len, editor.text.cap);
		ASSERT_EQ(editor.text.data[editor.text.len], '\0');
	}
	EXPECT_TRUE(morph_tui_widget_editor_state_valid(&editor));
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&editor.text)),
		  original.substr(0, 3) + inserted + original.substr(3));
}

TEST_F(TuiTest, CardAndDocumentComponentsRespectDisplayWidth)
{
	morph_buf_t row{};
	ASSERT_EQ(morph_buf_init(&row, 32), 0);
	ASSERT_EQ(morph_tui_widget_card_row(&row, 8, "中文abc\033[2J"), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&row)), "│ 中…  │");
	morph_buf_reset(&row);
	ASSERT_EQ(morph_tui_widget_card_border(&row, 8, 0), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&row)), "┌──────┐");
	morph_buf_cleanup(&row);
	struct morph_tui_widget_text_layout view{};
	ASSERT_EQ(morph_tui_widget_text_layout_wrap_borrow(&view, "中文ab\n\nlast", 3), 0);
	ASSERT_EQ(view.lines.nelts, 6U);
	auto line = static_cast<struct morph_tui_widget_text_line *>(
		morph_array_borrow(&view.lines, 0));
	EXPECT_EQ(std::string(view.text + line->offset, line->length), "中");
	ASSERT_EQ(morph_tui_widget_text_layout_wrap_borrow(&view, "中", 1), 0);
	EXPECT_EQ(view.lines.nelts, 1U);
	morph_tui_widget_text_layout_cleanup(&view);
}

class TuiPseudoTerminal
{
      public:
	int master = -1;
	FILE *terminal = nullptr;
	TuiPseudoTerminal()
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0)
			terminal = fopen(ptsname(master), "r+");
	}
	~TuiPseudoTerminal()
	{
		if (terminal)
			fclose(terminal);
		if (master >= 0)
			close(master);
	}
};

TEST_F(TuiTest, SelectPromptRunsWithoutCliAndRestoresTerminal)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct termios before{}, after{};
	ASSERT_EQ(tcgetattr(fileno(pty.terminal), &before), 0);
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 2, 0, 1, 1), 0);
	const char *items[] = {"Blue", "Red\033[2J"};
	struct morph_tui_widget_select_options options{};
	options.items = items;
	ASSERT_EQ(write(pty.master, "j\n", 2), 2);
	EXPECT_EQ(morph_tui_widget_select_run(&selection, &options, pty.terminal, output, nullptr),
		  0);
	EXPECT_TRUE(morph_tui_widget_select_selected(&selection, 1));
	ASSERT_EQ(tcgetattr(fileno(pty.terminal), &after), 0);
	EXPECT_EQ(after.c_lflag & (ICANON | ECHO | ISIG | IEXTEN),
		  before.c_lflag & (ICANON | ECHO | ISIG | IEXTEN));
	std::string rendered = take();
	EXPECT_NE(rendered.find("Blue"), std::string::npos);
	EXPECT_EQ(rendered.find("Other"), std::string::npos);
	EXPECT_EQ(rendered.find("\033[2J"), std::string::npos);
	EXPECT_EQ(rendered.find("\033[0m"), std::string::npos);
	EXPECT_EQ(rendered.find("\033[?1049h"), std::string::npos);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, SelectPromptCustomAnswerUsesSharedEditorWithoutCli)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 2, 0, 1, 1), 0);
	const char *items[] = {"Blue"};
	struct morph_tui_widget_select_options options{};
	options.items = items;
	options.allow_custom = 1;
	const char keys[] = "j\nGold\n";
	ASSERT_EQ(write(pty.master, keys, sizeof(keys) - 1),
		  static_cast<ssize_t>(sizeof(keys) - 1));
	char *custom = nullptr;
	EXPECT_EQ(morph_tui_widget_select_run(&selection, &options, pty.terminal, output, &custom),
		  0);
	EXPECT_STREQ(custom, "Gold");
	EXPECT_TRUE(morph_tui_widget_select_selected(&selection, 1));
	free(custom);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, SelectPromptCustomAnswerLimitRejectsOverflowUntilEdited)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct termios input_mode{};
	ASSERT_EQ(tcgetattr(fileno(pty.terminal), &input_mode), 0);
	/* Queue the editing keys without canonical erase processing. */
	input_mode.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
	ASSERT_EQ(tcsetattr(fileno(pty.terminal), TCSANOW, &input_mode), 0);
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 2, 0, 1, 1), 0);
	const char *items[] = {"Blue"};
	struct morph_tui_widget_select_options options{};
	options.items = items;
	options.allow_custom = 1;
	options.custom_max_bytes = 4;
	const char keys[] = "j\n12345\n\177Z\n";
	ASSERT_EQ(write(pty.master, keys, sizeof(keys) - 1),
		  static_cast<ssize_t>(sizeof(keys) - 1));
	char *custom MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	EXPECT_EQ(morph_tui_widget_select_run(&selection, &options, pty.terminal, output, &custom),
	0);
	EXPECT_STREQ(custom, "123Z");
	EXPECT_NE(take().find("shorten it if needed"), std::string::npos);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, SelectPromptInterruptionClosesSessionWithoutApplicationActions)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct morph_tui_widget_select selection{};
	ASSERT_EQ(morph_tui_widget_select_init(&selection, 1, 0, 1, 1), 0);
	const char *items[] = {"Blue"};
	volatile sig_atomic_t interrupted = 1;
	struct morph_tui_widget_select_options options{};
	options.items = items;
	options.interrupted = &interrupted;
	EXPECT_EQ(morph_tui_widget_select_run(&selection, &options, pty.terminal, output, nullptr),
		  -EINTR);
	EXPECT_NE(take().find("\033[?2004l\033[?25h"), std::string::npos);
	morph_tui_widget_select_cleanup(&selection);
}

TEST_F(TuiTest, ScreenPollReportsResizeAndPasteWithoutConsumingNextScreenInput)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct morph_tui_screen screen{};
	ASSERT_EQ(morph_tui_screen_open(&screen, pty.terminal, pty.terminal,
					MORPH_TUI_TERMINAL_SESSION_ALTERNATE |
						MORPH_TUI_TERMINAL_SESSION_PASTE),
		  0);
	struct winsize size{};
	size.ws_col = 60;
	size.ws_row = 18;
	ASSERT_EQ(ioctl(fileno(pty.terminal), TIOCSWINSZ, &size), 0);
	struct morph_tui_event event{};
	ASSERT_EQ(morph_tui_screen_poll(&screen, &event, 0), 0);
	EXPECT_EQ(event.type, MORPH_TUI_EVENT_RESIZE);
	EXPECT_EQ(screen.area.width, 60);
	EXPECT_EQ(screen.area.height, 18);
	const char keys[] = "\033[200~x\033[201~qz";
	ASSERT_EQ(write(pty.master, keys, sizeof(keys) - 1),
		  static_cast<ssize_t>(sizeof(keys) - 1));
	const int expected[] = {MORPH_TUI_KEY_PASTE_BEGIN, 'x', MORPH_TUI_KEY_PASTE_END, 'q'};
	for (int key : expected) {
		ASSERT_EQ(morph_tui_screen_poll(&screen, &event, 100), 0);
		EXPECT_EQ(event.type, MORPH_TUI_EVENT_KEY);
		EXPECT_EQ(event.key, key);
		EXPECT_EQ(event.pasting, key == 'q' ? 0 : 1);
	}
	unsigned char remaining = 0;
	ASSERT_EQ(read(fileno(pty.terminal), &remaining, 1), 1);
	EXPECT_EQ(remaining, 'z');
	EXPECT_EQ(morph_tui_screen_close(&screen), 0);
	EXPECT_EQ(morph_tui_screen_close(&screen), 0);
}

struct tui_core_screen_test_state {
	struct morph_tui_widget_menu_state menu{};
	int draws = 0;
	int selected = -1;
};

static int tui_core_screen_test_draw(void *data, const struct morph_tui_screen *)
{
	auto state = static_cast<tui_core_screen_test_state *>(data);
	state->draws++;
	state->selected = static_cast<int>(state->menu.list.index);
	return 0;
}

static int tui_core_screen_test_update(void *data, const struct morph_tui_event *event)
{
	auto state = static_cast<tui_core_screen_test_state *>(data);
	if (event->type != MORPH_TUI_EVENT_KEY)
		return MORPH_TUI_SCREEN_CONTINUE;
	int result = morph_tui_widget_menu_state_update(&state->menu, event->key);
	return result == MORPH_TUI_WIDGET_SUBMIT    ? MORPH_TUI_SCREEN_CLOSE
	       : result == MORPH_TUI_WIDGET_CHANGED ? MORPH_TUI_SCREEN_REDRAW
						    : MORPH_TUI_SCREEN_CONTINUE;
}

TEST_F(TuiTest, ScreenLoopOwnsUpdateAndRedrawAndReturnsForCleanup)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct morph_tui_screen screen{};
	ASSERT_EQ(morph_tui_screen_open(&screen, pty.terminal, output, 0), 0);
	struct tui_core_screen_test_state state{};
	state.menu.list.flags = MORPH_TUI_WIDGET_LIST_VIM;
	morph_tui_widget_list_state_layout(&state.menu.list, 3, 3);
	const struct morph_tui_screen_ops ops{tui_core_screen_test_update,
					      tui_core_screen_test_draw};
	ASSERT_EQ(write(pty.master, "j\n", 2), 2);
	EXPECT_EQ(morph_tui_screen_run(&screen, &ops, &state), 0);
	EXPECT_EQ(state.draws, 2);
	EXPECT_EQ(state.selected, 1);
	EXPECT_TRUE(screen.session.active);
	EXPECT_EQ(morph_tui_screen_close(&screen), 0);
}

TEST_F(TuiTest, ScreenFailureAndCancellationReturnWithoutLosingCleanupOwnership)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct morph_tui_screen screen{};
	ASSERT_EQ(morph_tui_screen_open(&screen, pty.terminal, output, 0), 0);
	struct tui_core_screen_test_state state{};
	const struct morph_tui_screen_ops failing{
		tui_core_screen_test_update,
		+[](void *, const struct morph_tui_screen *) { return -EIO; }};
	EXPECT_EQ(morph_tui_screen_run(&screen, &failing, &state), -EIO);
	EXPECT_TRUE(screen.session.active);
	volatile sig_atomic_t cancelled = 1;
	screen.interrupted = &cancelled;
	const struct morph_tui_screen_ops normal{tui_core_screen_test_update,
						 tui_core_screen_test_draw};
	EXPECT_EQ(morph_tui_screen_run(&screen, &normal, &state), -EINTR);
	EXPECT_EQ(morph_tui_screen_close(&screen), 0);
	EXPECT_FALSE(screen.session.active);
}

TEST_F(TuiTest, NumericCellsAlignInsideTheirAllocatedWidth)
{
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	ASSERT_EQ(morph_tui_draw_cell_append_aligned(&commands, 10, 2, 1, 2, 5, "42", "", 1), 0);
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&commands)), "\033[1;2H\033[0m   42");
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, TreeTraversesOutOfOrderChildrenAndCollapses)
{
	struct morph_tui_widget_tree tree{};
	ASSERT_EQ(morph_tui_widget_tree_init(&tree), 0);
	size_t root = morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "Root", "");
	morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "Other", "");
	size_t child = morph_tui_widget_tree_add(&tree, root, "Child", "");
	morph_buf_t text{};
	ASSERT_EQ(morph_buf_init(&text, 128), 0);
	ASSERT_EQ(morph_tui_widget_tree_render(&tree, &text, 40, 0, 10), 0);
	EXPECT_STREQ(morph_buf_cstr_borrow(&text), "  ├ ▾ Root\n  │ └ Child\n  └ Other\n");
	EXPECT_EQ(morph_tui_widget_tree_selected(&tree), root);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_RIGHT, 2),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_tree_selected(&tree), child);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_LEFT, 2),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(morph_tui_widget_tree_selected(&tree), root);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_LEFT, 2),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(tree.visible.nelts, 2u);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_END, 1),
		  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(tree.list.first, 1u);
	morph_tui_widget_tree_cleanup(&tree);
	morph_buf_cleanup(&text);
}

TEST_F(TuiTest, TreeClipsUtf8AndSanitizesTerminalControls)
{
	struct morph_tui_widget_tree tree{};
	ASSERT_EQ(morph_tui_widget_tree_init(&tree), 0);
	morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "中文名称名称\033[2J",
				  "description");
	for (int width : {1, 4, 9, 20, 80}) {
		morph_buf_t text{};
		ASSERT_EQ(morph_buf_init(&text, 128), 0);
		ASSERT_EQ(morph_tui_widget_tree_render(&tree, &text, width, 0, 10), 0);
		std::string row = morph_buf_cstr_borrow(&text);
		row.pop_back();
		EXPECT_LE(utf8_display_width(row.c_str()), static_cast<size_t>(width));
		EXPECT_EQ(row.find('\033'), std::string::npos);
		EXPECT_EQ(utf8valid(row.c_str()), nullptr);
		morph_buf_cleanup(&text);
	}
	morph_tui_widget_tree_cleanup(&tree);
}

TEST_F(TuiTest, TreeRejectsInvalidParentsAndPreservesStickyErrors)
{
	struct morph_tui_widget_tree tree{};
	ASSERT_EQ(morph_tui_widget_tree_init(&tree), 0);
	EXPECT_EQ(morph_tui_widget_tree_add(&tree, 3, "invalid", ""), MORPH_TUI_WIDGET_TREE_ROOT);
	EXPECT_EQ(tree.error, -EINVAL);
	EXPECT_EQ(morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "later", ""),
		  MORPH_TUI_WIDGET_TREE_ROOT);
	EXPECT_EQ(tree.nodes.nelts, 0u);
	morph_tui_widget_tree_cleanup(&tree);
}

TEST_F(TuiTest, TreeViewportScrollsAndErasesUnusedRows)
{
	struct morph_tui_widget_tree tree{};
	ASSERT_EQ(morph_tui_widget_tree_init(&tree), 0);
	for (int i = 0; i < 30; i++) {
		std::string label = "Node " + std::to_string(i);
		ASSERT_NE(morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT,
						    label.c_str(), ""),
			  MORPH_TUI_WIDGET_TREE_ROOT);
	}
	morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_END, 3);
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 256), 0);
	ASSERT_EQ(morph_tui_widget_tree_draw(&tree, &commands, {2, 3, 20, 3}, "\033[0m", "\033[7m"),
		  0);
	std::string text = morph_buf_cstr_borrow(&commands);
	EXPECT_NE(text.find("Node 29"), std::string::npos);
	EXPECT_EQ(text.find("Node 0"), std::string::npos);
	EXPECT_NE(text.find("\033[5;2H\033[0m\033[7m"), std::string::npos);
	morph_buf_cleanup(&commands);
	morph_tui_widget_tree_cleanup(&tree);
}

TEST_F(TuiTest, StreamingTreeRowsBoundLongNamesAndDeepPrefixes)
{
	for (int width : {1, 4, 12, 40, 80}) {
		morph_buf_t text{};
		ASSERT_EQ(morph_buf_init(&text, 128), 0);
		ASSERT_EQ(morph_tui_widget_tree_row(
				  &text, "│ │ │ ", 1, "○", "中文很长的名称名称名称",
				  "description description description", 28, width, "", ""),
			  0);
		std::string value = morph_buf_cstr_borrow(&text);
		size_t start = 0;
		for (size_t end; (end = value.find('\n', start)) != std::string::npos;) {
			std::string row = value.substr(start, end - start);
			EXPECT_LE(utf8_display_width(row.c_str()), static_cast<size_t>(width));
			start = end + 1;
		}
		morph_buf_cleanup(&text);
	}
}

TEST_F(TuiTest, TreeIgnoresPastedNavigationAndSubmitsOnlyRealKeys)
{
	struct morph_tui_widget_tree tree{};
	ASSERT_EQ(morph_tui_widget_tree_init(&tree), 0);
	morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "First", "");
	morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT, "Second", "");
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_PASTE_BEGIN, 2),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_DOWN, 2),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, '\r', 2), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_PASTE_END, 2),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(tree.list.index, 0u);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, MORPH_TUI_KEY_RIGHT, 2),
		  MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, '\r', 2), MORPH_TUI_WIDGET_SUBMIT);
	EXPECT_EQ(morph_tui_widget_tree_update(&tree, 27, 2), MORPH_TUI_WIDGET_CANCEL);
	morph_tui_widget_tree_cleanup(&tree);
}
