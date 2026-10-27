#include <gtest/gtest.h>
#include "tui/components.h"
#include "tui/tree.h"
#include "util/utf8.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <sys/ioctl.h>
#include <thread>
#include <string>
#include <unistd.h>

class TuiTest : public ::testing::Test {
protected:
	FILE *output = nullptr;
	void SetUp() override { ASSERT_NE(output = tmpfile(), nullptr); }
	void TearDown() override { if (output) fclose(output); }
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
	struct tui_input input_state{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	const char input[] = "a\033[A\033OB\033[C\033[D\033[H\033[4~"
		"\033[5~\033[6~\033[200~\033[201~\xe4\xb8\xad\033[99~z";
	ASSERT_EQ(write(fds[1], input, sizeof(input) - 1), sizeof(input) - 1);
	const int expected[] = {'a', TUI_KEY_UP, TUI_KEY_DOWN, TUI_KEY_RIGHT,
		TUI_KEY_LEFT, TUI_KEY_HOME, TUI_KEY_END, TUI_KEY_PAGE_UP,
		TUI_KEY_PAGE_DOWN, TUI_KEY_PASTE_BEGIN, TUI_KEY_PASTE_END,
		0xe4, 0xb8, 0xad, 0, 'z'};
	for (int key : expected)
		EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), key);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), 0);
	ASSERT_EQ(write(fds[1], "\033", 1), 1);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 100), 27);
	close(fds[1]);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), -ECANCELED);
	close(fds[0]);
	EXPECT_EQ(tui_input_read(&input_state, -1, 0), -EBADF);
}

TEST_F(TuiTest, PartialSequenceSurvivesPollingBoundaries)
{
	struct tui_input input_state{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	ASSERT_EQ(write(fds[1], "\033[", 2), 2);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), 0);
	ASSERT_EQ(write(fds[1], "A", 1), 1);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), TUI_KEY_UP);
	ASSERT_EQ(write(fds[1], "\033[", 2), 2);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 100), 0);
	ASSERT_EQ(write(fds[1], "x", 1), 1);
	EXPECT_EQ(tui_input_read(&input_state, fds[0], 0), 'x');
	close(fds[0]);
	close(fds[1]);
}

TEST_F(TuiTest, CellSanitizesControlsAndClipsCjkToViewport)
{
	ASSERT_EQ(tui_cell(output, 8, 2, 1, 6, 20, "中文abc\033[2J", ""), 0);
	std::string text = take();
	EXPECT_EQ(text.find("\033[1;6H"), 0U);
	EXPECT_EQ(text.find("\033[2J"), std::string::npos);
	EXPECT_NE(text.find("中"), std::string::npos);
	EXPECT_EQ(text.find("文"), std::string::npos);
	ASSERT_EQ(tui_cell(output, 8, 2, 2, 8, 1, "x", ""), 0);
	EXPECT_EQ(take(), "\033[2;8Hx");
	ASSERT_EQ(tui_cell(output, 8, 2, 0, 1, 2, "x", ""), 0);
	EXPECT_TRUE(take().empty());
}

TEST_F(TuiTest, FramesOnlyRedrawChangedRowsAndInvalidateOnResize)
{
	struct tui_frame frame{};
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 3, "one\ntwo\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 3, "one\ntwo\n"), 0);
	EXPECT_TRUE(take().empty());
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 3, "one\nshort\n"), 0);
	EXPECT_EQ(take(), "\033[?2026h\033[2;1Hshort\033[0m\033[K\033[?2026l");
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 3, "one\n"), 0);
	EXPECT_NE(take().find("\033[2;1H\033[0m\033[K"), std::string::npos);
	ASSERT_EQ(tui_frame_present(&frame, output, 40, 2, "one\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	tui_frame_cleanup(&frame);
	tui_frame_cleanup(&frame);
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
	struct tui_session session{};
	ASSERT_EQ(tui_session_enter(&session, input, output,
		TUI_SESSION_ALTERNATE | TUI_SESSION_PASTE), 0);
	ASSERT_EQ(tcgetattr(fileno(input), &during), 0);
	EXPECT_EQ(during.c_lflag & (ICANON | ECHO | ISIG | IEXTEN), 0U);
	EXPECT_EQ(tui_session_enter(&session, input, output, 0), -EBUSY);
	EXPECT_EQ(take(), "\033[?1049h\033[?25l\033[?2004h");
	ASSERT_EQ(tui_session_leave(&session), 0);
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
	EXPECT_EQ(take(), "\033[?2004l\033[0m\033[?25h\033[?1049l");
	EXPECT_EQ(tui_session_leave(&session), 0);
	EXPECT_TRUE(take().empty());
	/* Inline sessions must preserve NO_COLOR's absence of SGR output. */
	ASSERT_EQ(tui_session_enter(&session, input, output, TUI_SESSION_PASTE), 0);
	EXPECT_EQ(take(), "\033[?25l\033[?2004h");
	ASSERT_EQ(tui_session_leave(&session), 0);
	EXPECT_EQ(take(), "\033[?2004l\033[?25h");
	FILE *unwritable = fopen("/dev/null", "r");
	ASSERT_NE(unwritable, nullptr);
	EXPECT_LT(tui_session_enter(&session, input, unwritable, 0), 0);
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
	struct tui_session session{};
	EXPECT_EQ(tui_session_enter(&session, input, output, 0), -ENOTTY);
	EXPECT_EQ(tui_session_leave(&session), 0);
	EXPECT_TRUE(take().empty());
	int columns = 0, rows = 0;
	tui_dimensions(-1, &columns, &rows);
	EXPECT_EQ(columns, 80);
	EXPECT_EQ(rows, 24);
	EXPECT_FALSE(tui_available(input, output));
	fclose(input);
}

TEST_F(TuiTest, PureDecoderDiscardsOverlongSequencesAndResynchronizes)
{
	struct tui_input input{};
	EXPECT_EQ(tui_input_feed(&input, 27), 0);
	EXPECT_EQ(tui_input_feed(&input, '['), 0);
	for (int i = 0; i < TUI_INPUT_SEQUENCE_MAX + 20; i++)
		EXPECT_EQ(tui_input_feed(&input, '1'), 0);
	EXPECT_EQ(tui_input_feed(&input, '~'), 0);
	EXPECT_EQ(tui_input_feed(&input, 'q'), 'q');
	EXPECT_EQ(tui_input_feed(&input, 27), 0);
	EXPECT_EQ(tui_input_expire(&input), 27);
	EXPECT_EQ(tui_input_expire(&input), 0);
	EXPECT_EQ(tui_input_feed(&input, 27), 0);
	EXPECT_EQ(tui_input_feed(&input, '['), 0);
	EXPECT_EQ(tui_input_feed(&input, 27), 0);
	EXPECT_EQ(tui_input_feed(&input, 'O'), 0);
	EXPECT_EQ(tui_input_feed(&input, 'F'), TUI_KEY_END);
}

TEST_F(TuiTest, InterruptedReadPreservesPendingSequence)
{
	struct tui_input input{};
	int fds[2];
	ASSERT_EQ(pipe(fds), 0);
	struct sigaction handler{}, previous{};
	handler.sa_handler = +[](int) {};
	sigemptyset(&handler.sa_mask);
	ASSERT_EQ(sigaction(SIGUSR1, &handler, &previous), 0);
	EXPECT_EQ(tui_input_feed(&input, 27), 0);
	EXPECT_EQ(tui_input_feed(&input, '['), 0);
	pthread_t reader = pthread_self();
	std::thread interrupt([reader] {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		(void)pthread_kill(reader, SIGUSR1);
	});
	int rc = tui_input_read(&input, fds[0], 1000);
	interrupt.join();
	(void)sigaction(SIGUSR1, &previous, nullptr);
	EXPECT_EQ(rc, -EINTR);
	EXPECT_EQ(write(fds[1], "B", 1), 1);
	EXPECT_EQ(tui_input_read(&input, fds[0], 0), TUI_KEY_DOWN);
	close(fds[0]);
	close(fds[1]);
}

TEST_F(TuiTest, CellCompositionHasNoIoAndPropagatesErrorsBeforeOutput)
{
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	EXPECT_EQ(tui_cell_append(&commands, 10, 2, 1, 1, 4, "abc", ""), 0);
	EXPECT_TRUE(take().empty());
	EXPECT_EQ(tui_output_write(output, &commands), 0);
	EXPECT_EQ(take(), "\033[1;1Habc ");
	/* A later drawing error must reject the entire composed screen. */
	EXPECT_EQ(tui_cell_append(&commands, 10, 2, 1, 1, 4, nullptr, ""), -EINVAL);
	EXPECT_EQ(tui_output_write(output, &commands), -EINVAL);
	EXPECT_TRUE(take().empty());
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, FrameInvalidationAndOutputFailureForceFullRepaint)
{
	struct tui_frame frame{};
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 2, "one\n"), 0);
	(void)take();
	tui_frame_invalidate(&frame);
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 2, "one\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	FILE *unwritable = fopen("/dev/null", "r");
	ASSERT_NE(unwritable, nullptr);
	EXPECT_LT(tui_frame_present(&frame, unwritable, 80, 2, "two\n"), 0);
	fclose(unwritable);
	EXPECT_EQ(std::string(morph_buf_cstr(&frame.previous)), "one\n");
	ASSERT_EQ(tui_frame_present(&frame, output, 80, 2, "two\n"), 0);
	EXPECT_NE(take().find("\033[2J"), std::string::npos);
	FILE *other = tmpfile();
	ASSERT_NE(other, nullptr);
	ASSERT_EQ(tui_frame_present(&frame, other, 80, 2, "two\n"), 0);
	EXPECT_GT(ftell(other), 0);
	fclose(other);
	tui_frame_cleanup(&frame);
}

TEST_F(TuiTest, LayoutRegionsClampAndClipAtTheirOwnBoundary)
{
	struct tui_rect area{1, 1, 20, 10};
	struct tui_rect body = tui_rect_inset(area, 2, 1);
	EXPECT_EQ(body.x, 3);
	EXPECT_EQ(body.y, 2);
	EXPECT_EQ(body.width, 16);
	EXPECT_EQ(body.height, 8);
	struct tui_rect title = tui_rect_take_top(&body, 2);
	EXPECT_EQ(title.height, 2);
	EXPECT_EQ(body.y, 4);
	struct tui_rect left = tui_rect_take_left(&body, 100);
	EXPECT_EQ(left.width, 16);
	EXPECT_EQ(body.width, 0);
	EXPECT_EQ(tui_rect_take_top(&body, -1).height, 0);
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	ASSERT_EQ(tui_region_cell(&commands, title, 0, 15, 8, "abc", ""), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&commands)), "\033[2;18H…");
	morph_buf_reset(&commands);
	EXPECT_EQ(tui_region_cell(&commands, body, 0, 0, 2, "x", ""), 0);
	EXPECT_EQ(commands.len, 0U);
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, ListsPreserveSelectionVisibilityAndHandleShrinkingData)
{
	struct tui_list list{};
	list.flags = TUI_LIST_VIM | TUI_LIST_HORIZONTAL_PAGES | TUI_LIST_PAGED;
	tui_list_layout(&list, 12, 5);
	EXPECT_TRUE(tui_list_update(&list, TUI_KEY_PAGE_DOWN));
	EXPECT_EQ(list.index, 5U);
	EXPECT_EQ(list.first, 5U);
	EXPECT_TRUE(tui_list_update(&list, TUI_KEY_END));
	EXPECT_EQ(list.index, 11U);
	EXPECT_EQ(list.first, 10U);
	tui_list_layout(&list, 3, 5);
	EXPECT_EQ(list.index, 2U);
	EXPECT_EQ(list.first, 0U);
	tui_list_layout(&list, 0, 0);
	EXPECT_EQ(list.index, 0U);
	EXPECT_EQ(list.first, 0U);
	EXPECT_FALSE(tui_list_update(&list, TUI_KEY_DOWN));
	EXPECT_EQ(tui_scroll_update(0, 15, 5, TUI_KEY_END), 10U);
	EXPECT_EQ(tui_scroll_update(10, 15, 5, TUI_KEY_DOWN), 10U);
	EXPECT_EQ(tui_scroll_update(10, 3, 5, 0), 0U);
}

TEST_F(TuiTest, MenusWrapAndIgnorePastedShortcutsAndConfirmation)
{
	struct tui_menu menu{};
	menu.list.flags = TUI_LIST_WRAP | TUI_LIST_TAB | TUI_LIST_DIGITS;
	tui_list_layout(&menu.list, 3, 3);
	EXPECT_EQ(tui_menu_update(&menu, TUI_KEY_UP), TUI_CHANGED);
	EXPECT_EQ(menu.list.index, 2U);
	EXPECT_EQ(tui_menu_update(&menu, '\t'), TUI_CHANGED);
	EXPECT_EQ(menu.list.index, 0U);
	EXPECT_EQ(tui_menu_update(&menu, TUI_KEY_PASTE_BEGIN), TUI_UNHANDLED);
	EXPECT_EQ(tui_menu_update(&menu, '3'), TUI_UNHANDLED);
	EXPECT_EQ(tui_menu_update(&menu, '\n'), TUI_UNHANDLED);
	EXPECT_EQ(tui_menu_update(&menu, 27), TUI_UNHANDLED);
	EXPECT_EQ(menu.list.index, 0U);
	EXPECT_EQ(tui_menu_update(&menu, TUI_KEY_PASTE_END), TUI_UNHANDLED);
	EXPECT_EQ(tui_menu_update(&menu, '3'), TUI_CHANGED);
	EXPECT_EQ(tui_menu_update(&menu, '\n'), TUI_SUBMIT);
	EXPECT_EQ(tui_menu_update(&menu, 27), TUI_BACK);
	EXPECT_EQ(tui_menu_update(&menu, 3), TUI_CANCEL);
	tui_list_layout(&menu.list, 0, 1);
	EXPECT_EQ(tui_menu_update(&menu, '\n'), TUI_UNHANDLED);
}

TEST_F(TuiTest, EditorEditsUnicodeInTheMiddleAndKeepsCursorVisible)
{
	struct tui_editor editor{};
	ASSERT_EQ(tui_editor_init(&editor, "中文ab", 32, 0), 0);
	EXPECT_EQ(tui_editor_init(&editor, "other", 32, 0), -EBUSY);
	EXPECT_EQ(tui_editor_update(&editor, TUI_KEY_HOME), TUI_CHANGED);
	EXPECT_EQ(tui_editor_update(&editor, TUI_KEY_RIGHT), TUI_CHANGED);
	EXPECT_EQ(editor.cursor, 3U);
	EXPECT_EQ(tui_editor_update(&editor, 'x'), TUI_CHANGED);
	EXPECT_EQ(std::string(morph_buf_cstr(&editor.text)), "中x文ab");
	EXPECT_EQ(tui_editor_update(&editor, 127), TUI_CHANGED);
	EXPECT_EQ(tui_editor_update(&editor, 127), TUI_CHANGED);
	EXPECT_EQ(std::string(morph_buf_cstr(&editor.text)), "文ab");
	EXPECT_EQ(tui_editor_update(&editor, TUI_KEY_END), TUI_CHANGED);
	morph_buf_t visible{};
	ASSERT_EQ(morph_buf_init(&visible, 32), 0);
	size_t column = 0;
	ASSERT_EQ(tui_editor_view(&editor, &visible, 3, &column), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&visible)), "ab");
	EXPECT_EQ(column, 2U);
	EXPECT_TRUE(tui_editor_valid(&editor));
	tui_editor_cleanup(&editor);
	tui_editor_cleanup(&editor);
	morph_buf_cleanup(&visible);
}

TEST_F(TuiTest, EditorBoundsPasteAndSecretRendering)
{
	struct tui_editor editor{};
	ASSERT_EQ(tui_editor_init(&editor, "", 5, 1), 0);
	EXPECT_EQ(tui_editor_update(&editor, TUI_KEY_PASTE_BEGIN), TUI_UNHANDLED);
	for (int key : {'a', 'b', 'c', 'd', 'e'})
		EXPECT_EQ(tui_editor_update(&editor, key), TUI_CHANGED);
	EXPECT_EQ(editor.text.len, 4U);
	EXPECT_TRUE(editor.overflow);
	EXPECT_EQ(tui_editor_update(&editor, 21), TUI_UNHANDLED);
	EXPECT_EQ(tui_editor_update(&editor, '\n'), TUI_UNHANDLED);
	EXPECT_EQ(tui_editor_update(&editor, TUI_KEY_PASTE_END), TUI_CHANGED);
	EXPECT_FALSE(tui_editor_valid(&editor));
	EXPECT_EQ(tui_editor_update(&editor, 127), TUI_CHANGED);
	EXPECT_TRUE(tui_editor_valid(&editor));
	morph_buf_t visible{};
	ASSERT_EQ(morph_buf_init(&visible, 32), 0);
	size_t column = 0;
	ASSERT_EQ(tui_editor_view(&editor, &visible, 3, &column), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&visible)), "•••");
	EXPECT_EQ(column, 3U);
	EXPECT_EQ(tui_editor_update(&editor, 21), TUI_CHANGED);
	EXPECT_EQ(editor.text.len, 0U);
	EXPECT_EQ(tui_editor_update(&editor, 0xe4), TUI_CHANGED);
	EXPECT_FALSE(tui_editor_valid(&editor));
	EXPECT_EQ(tui_editor_update(&editor, 0xb8), TUI_CHANGED);
	EXPECT_EQ(tui_editor_update(&editor, 0xad), TUI_CHANGED);
	EXPECT_TRUE(tui_editor_valid(&editor));
	tui_editor_cleanup(&editor);
	morph_buf_cleanup(&visible);
}

TEST_F(TuiTest, CardAndDocumentComponentsRespectDisplayWidth)
{
	morph_buf_t row{};
	ASSERT_EQ(morph_buf_init(&row, 32), 0);
	ASSERT_EQ(tui_card_row(&row, 8, "中文abc\033[2J"), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&row)), "│ 中…  │");
	morph_buf_reset(&row);
	ASSERT_EQ(tui_card_border(&row, 8, 0), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&row)), "┌──────┐");
	morph_buf_cleanup(&row);
	struct tui_text_view view{};
	ASSERT_EQ(tui_text_view_wrap(&view, "中文ab\n\nlast", 3), 0);
	ASSERT_EQ(view.lines.nelts, 6U);
	auto line = static_cast<struct tui_text_line *>(morph_array_get(&view.lines, 0));
	EXPECT_EQ(std::string(view.text + line->offset, line->length), "中");
	ASSERT_EQ(tui_text_view_wrap(&view, "中", 1), 0);
	EXPECT_EQ(view.lines.nelts, 1U);
	tui_text_view_cleanup(&view);
}

class TuiPseudoTerminal {
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
		if (terminal) fclose(terminal);
		if (master >= 0) close(master);
	}
};

TEST_F(TuiTest, ScreenPollReportsResizeAndPasteWithoutConsumingNextScreenInput)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct tui_screen screen{};
	ASSERT_EQ(tui_screen_open(&screen, pty.terminal, pty.terminal,
		TUI_SESSION_ALTERNATE | TUI_SESSION_PASTE), 0);
	struct winsize size{};
	size.ws_col = 60;
	size.ws_row = 18;
	ASSERT_EQ(ioctl(fileno(pty.terminal), TIOCSWINSZ, &size), 0);
	struct tui_event event{};
	ASSERT_EQ(tui_screen_poll(&screen, &event, 0), 0);
	EXPECT_EQ(event.type, TUI_EVENT_RESIZE);
	EXPECT_EQ(screen.area.width, 60);
	EXPECT_EQ(screen.area.height, 18);
	const char keys[] = "\033[200~x\033[201~qz";
	ASSERT_EQ(write(pty.master, keys, sizeof(keys) - 1), sizeof(keys) - 1);
	const int expected[] = {TUI_KEY_PASTE_BEGIN, 'x', TUI_KEY_PASTE_END, 'q'};
	for (int key : expected) {
		ASSERT_EQ(tui_screen_poll(&screen, &event, 100), 0);
		EXPECT_EQ(event.type, TUI_EVENT_KEY);
		EXPECT_EQ(event.key, key);
		EXPECT_EQ(event.pasting, key == 'q' ? 0 : 1);
	}
	unsigned char remaining = 0;
	ASSERT_EQ(read(fileno(pty.terminal), &remaining, 1), 1);
	EXPECT_EQ(remaining, 'z');
	EXPECT_EQ(tui_screen_close(&screen), 0);
	EXPECT_EQ(tui_screen_close(&screen), 0);
}

struct tui_loop_test_state {
	struct tui_menu menu{};
	int draws = 0;
	int selected = -1;
};

static int tui_loop_test_draw(void *data, const struct tui_screen *)
{
	auto state = static_cast<tui_loop_test_state *>(data);
	state->draws++;
	state->selected = static_cast<int>(state->menu.list.index);
	return 0;
}

static int tui_loop_test_update(void *data, const struct tui_event *event)
{
	auto state = static_cast<tui_loop_test_state *>(data);
	if (event->type != TUI_EVENT_KEY) return TUI_SCREEN_CONTINUE;
	int result = tui_menu_update(&state->menu, event->key);
	return result == TUI_SUBMIT ? TUI_SCREEN_CLOSE :
		result == TUI_CHANGED ? TUI_SCREEN_REDRAW : TUI_SCREEN_CONTINUE;
}

TEST_F(TuiTest, ScreenLoopOwnsUpdateAndRedrawAndReturnsForCleanup)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct tui_screen screen{};
	ASSERT_EQ(tui_screen_open(&screen, pty.terminal, output, 0), 0);
	struct tui_loop_test_state state{};
	state.menu.list.flags = TUI_LIST_VIM;
	tui_list_layout(&state.menu.list, 3, 3);
	const struct tui_screen_ops ops{tui_loop_test_update, tui_loop_test_draw};
	ASSERT_EQ(write(pty.master, "j\n", 2), 2);
	EXPECT_EQ(tui_screen_run(&screen, &ops, &state), 0);
	EXPECT_EQ(state.draws, 2);
	EXPECT_EQ(state.selected, 1);
	EXPECT_TRUE(screen.session.active);
	EXPECT_EQ(tui_screen_close(&screen), 0);
}

TEST_F(TuiTest, ScreenFailureAndCancellationReturnWithoutLosingCleanupOwnership)
{
	TuiPseudoTerminal pty;
	ASSERT_NE(pty.terminal, nullptr);
	struct tui_screen screen{};
	ASSERT_EQ(tui_screen_open(&screen, pty.terminal, output, 0), 0);
	struct tui_loop_test_state state{};
	const struct tui_screen_ops failing{
		tui_loop_test_update,
		+[](void *, const struct tui_screen *) { return -EIO; }
	};
	EXPECT_EQ(tui_screen_run(&screen, &failing, &state), -EIO);
	EXPECT_TRUE(screen.session.active);
	volatile sig_atomic_t cancelled = 1;
	screen.interrupted = &cancelled;
	const struct tui_screen_ops normal{tui_loop_test_update, tui_loop_test_draw};
	EXPECT_EQ(tui_screen_run(&screen, &normal, &state), -EINTR);
	EXPECT_EQ(tui_screen_close(&screen), 0);
	EXPECT_FALSE(screen.session.active);
}

TEST_F(TuiTest, NumericCellsAlignInsideTheirAllocatedWidth)
{
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 128), 0);
	ASSERT_EQ(tui_cell_append_aligned(&commands, 10, 2, 1, 2, 5,
		"42", "", 1), 0);
	EXPECT_EQ(std::string(morph_buf_cstr(&commands)), "\033[1;2H   42");
	morph_buf_cleanup(&commands);
}

TEST_F(TuiTest, TreeTraversesOutOfOrderChildrenAndCollapses)
{
	struct tui_tree tree{};
	ASSERT_EQ(tui_tree_init(&tree), 0);
	size_t root = tui_tree_add(&tree, TUI_TREE_ROOT, "Root", "");
	tui_tree_add(&tree, TUI_TREE_ROOT, "Other", "");
	size_t child = tui_tree_add(&tree, root, "Child", "");
	morph_buf_t text{};
	ASSERT_EQ(morph_buf_init(&text, 128), 0);
	ASSERT_EQ(tui_tree_render(&tree, &text, 40, 0, 10), 0);
	EXPECT_STREQ(morph_buf_cstr(&text), "  ├ ▾ Root\n  │ └ Child\n  └ Other\n");
	EXPECT_EQ(tui_tree_selected(&tree), root);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_RIGHT, 2), TUI_CHANGED);
	EXPECT_EQ(tui_tree_selected(&tree), child);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_LEFT, 2), TUI_CHANGED);
	EXPECT_EQ(tui_tree_selected(&tree), root);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_LEFT, 2), TUI_CHANGED);
	EXPECT_EQ(tree.visible.nelts, 2u);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_END, 1), TUI_CHANGED);
	EXPECT_EQ(tree.list.first, 1u);
	tui_tree_cleanup(&tree);
	morph_buf_cleanup(&text);
}

TEST_F(TuiTest, TreeClipsUtf8AndSanitizesTerminalControls)
{
	struct tui_tree tree{};
	ASSERT_EQ(tui_tree_init(&tree), 0);
	tui_tree_add(&tree, TUI_TREE_ROOT, "中文名称名称\033[2J", "description");
	for (int width : {1, 4, 9, 20, 80}) {
		morph_buf_t text{};
		ASSERT_EQ(morph_buf_init(&text, 128), 0);
		ASSERT_EQ(tui_tree_render(&tree, &text, width, 0, 10), 0);
		std::string row = morph_buf_cstr(&text);
		row.pop_back();
		EXPECT_LE(utf8_display_width(row.c_str()), static_cast<size_t>(width));
		EXPECT_EQ(row.find('\033'), std::string::npos);
		EXPECT_EQ(utf8valid(row.c_str()), nullptr);
		morph_buf_cleanup(&text);
	}
	tui_tree_cleanup(&tree);
}

TEST_F(TuiTest, TreeRejectsInvalidParentsAndPreservesStickyErrors)
{
	struct tui_tree tree{};
	ASSERT_EQ(tui_tree_init(&tree), 0);
	EXPECT_EQ(tui_tree_add(&tree, 3, "invalid", ""), TUI_TREE_ROOT);
	EXPECT_EQ(tree.error, -EINVAL);
	EXPECT_EQ(tui_tree_add(&tree, TUI_TREE_ROOT, "later", ""), TUI_TREE_ROOT);
	EXPECT_EQ(tree.nodes.nelts, 0u);
	tui_tree_cleanup(&tree);
}

TEST_F(TuiTest, TreeViewportScrollsAndErasesUnusedRows)
{
	struct tui_tree tree{};
	ASSERT_EQ(tui_tree_init(&tree), 0);
	for (int i = 0; i < 30; i++) {
		std::string label = "Node " + std::to_string(i);
		ASSERT_NE(tui_tree_add(&tree, TUI_TREE_ROOT, label.c_str(), ""), TUI_TREE_ROOT);
	}
	tui_tree_update(&tree, TUI_KEY_END, 3);
	morph_buf_t commands{};
	ASSERT_EQ(morph_buf_init(&commands, 256), 0);
	ASSERT_EQ(tui_tree_draw(&tree, &commands, {2, 3, 20, 3}, "\033[0m", "\033[7m"), 0);
	std::string text = morph_buf_cstr(&commands);
	EXPECT_NE(text.find("Node 29"), std::string::npos);
	EXPECT_EQ(text.find("Node 0"), std::string::npos);
	EXPECT_NE(text.find("\033[5;2H\033[7m"), std::string::npos);
	morph_buf_cleanup(&commands);
	tui_tree_cleanup(&tree);
}

TEST_F(TuiTest, StreamingTreeRowsBoundLongNamesAndDeepPrefixes)
{
	for (int width : {1, 4, 12, 40, 80}) {
		morph_buf_t text{};
		ASSERT_EQ(morph_buf_init(&text, 128), 0);
		ASSERT_EQ(tui_tree_row(&text, "│ │ │ ", 1, "○", "中文很长的名称名称名称",
			"description description description", 28, width, "", ""), 0);
		std::string value = morph_buf_cstr(&text);
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
	struct tui_tree tree{};
	ASSERT_EQ(tui_tree_init(&tree), 0);
	tui_tree_add(&tree, TUI_TREE_ROOT, "First", "");
	tui_tree_add(&tree, TUI_TREE_ROOT, "Second", "");
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_PASTE_BEGIN, 2), TUI_UNHANDLED);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_DOWN, 2), TUI_UNHANDLED);
	EXPECT_EQ(tui_tree_update(&tree, '\r', 2), TUI_UNHANDLED);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_PASTE_END, 2), TUI_UNHANDLED);
	EXPECT_EQ(tree.list.index, 0u);
	EXPECT_EQ(tui_tree_update(&tree, TUI_KEY_RIGHT, 2), TUI_UNHANDLED);
	EXPECT_EQ(tui_tree_update(&tree, '\r', 2), TUI_SUBMIT);
	EXPECT_EQ(tui_tree_update(&tree, 27, 2), TUI_CANCEL);
	tui_tree_cleanup(&tree);
}
