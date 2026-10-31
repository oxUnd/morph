#include <gtest/gtest.h>
#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct AppPage {
	std::vector<std::string> *log = nullptr;
	const char *name = "page";
	int messages = 0;
	int keys = 0;
	int ticks = 0;
	int layouts = 0;
	int draws = 0;
	int width = 0;
	int closes = 0;
	int close_result = 0;
	int reentry = 0;
	int last_key = 0;
	int pasting = 0;
	bool dismiss = false;
	bool redraw = false;
	bool write_output = false;
	int failure = 0;
	std::vector<int> codes;
};

static int page_appear(void *data, struct morph_tui_application *)
{
	auto *page = static_cast<AppPage *>(data);
	if (page->log)
		page->log->push_back(std::string(page->name) + ":appear");
	return 0;
}

static void page_disappear(void *data, struct morph_tui_application *)
{
	auto *page = static_cast<AppPage *>(data);
	if (page->log)
		page->log->push_back(std::string(page->name) + ":disappear");
}

static void page_close(void *data, int result)
{
	auto *page = static_cast<AppPage *>(data);
	page->closes++;
	page->close_result = result;
	if (page->log)
		page->log->push_back(std::string(page->name) + ":close");
}

static int page_layout(void *data, const struct morph_tui_terminal_rect *)
{
	static_cast<AppPage *>(data)->layouts++;
	return 0;
}

static int page_draw(void *data, const struct morph_tui_screen *screen)
{
	auto *page = static_cast<AppPage *>(data);
	page->draws++;
	page->width = screen->area.width;
	if (page->write_output)
		(void)fputs("frame", screen->session.output);
	return page->failure;
}

static int page_event(void *data, struct morph_tui_application *app,
		      const struct morph_tui_event *event)
{
	auto *page = static_cast<AppPage *>(data);
	if (event->type == MORPH_TUI_EVENT_MESSAGE) {
		page->messages++;
		page->codes.push_back(event->code);
		page->reentry = morph_tui_application_step(app, 0);
		if (page->redraw)
			morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
		if (page->dismiss) {
			int rc = morph_tui_application_dismiss_controller(app, 19);
			if (rc)
				return rc;
			/* Both requests address this page, never the underlying page. */
			return morph_tui_application_dismiss_controller(app, 23);
		}
	}
	if (event->type == MORPH_TUI_EVENT_KEY) {
		page->keys++;
		page->last_key = event->key;
		page->pasting = event->pasting;
	}
	if (event->type == MORPH_TUI_EVENT_TICK)
		page->ticks++;
	return 0;
}

class TuiApplicationTest : public ::testing::Test
{
      protected:
	FILE *input = nullptr;
	FILE *output = nullptr;
	int writer = -1;
	struct morph_tui_application *app = nullptr;
	std::vector<std::unique_ptr<AppPage>> pages;
	std::vector<std::string> log;

	AppPage &page()
	{
		pages.push_back(std::make_unique<AppPage>());
		return *pages.back();
	}
	const struct morph_tui_controller_ops ops = {page_event,  page_layout,	  page_draw,
						     page_appear, page_disappear, page_close};

	void SetUp() override
	{
		int fds[2];
		ASSERT_EQ(pipe(fds), 0);
		input = fdopen(fds[0], "r");
		writer = fds[1];
		ASSERT_NE(input, nullptr);
		ASSERT_NE(output = tmpfile(), nullptr);
		struct morph_tui_application_options options{};
		options.external_signals = 1;
		options.input = input;
		options.output = output;
		ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
	}

	void TearDown() override
	{
		morph_tui_application_destroy(app);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		if (writer >= 0)
			close(writer);
	}

	int present(AppPage &page, int tick_ms = 0)
	{
		struct morph_tui_controller controller{};
		controller.ops = &ops;
		controller.data = &page;
		controller.tick_ms = tick_ms;
		return morph_tui_application_present_controller(app, &controller);
	}

	void pump_until(const std::function<bool()> &done)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!done() && std::chrono::steady_clock::now() < deadline) {
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
			std::this_thread::yield();
		}
		ASSERT_TRUE(done());
	}
};

TEST_F(TuiApplicationTest, PresentationIsDeferredAndRestoresUnderlyingController)
{
	AppPage &root = page();
	AppPage &panel = page();
	root.log = panel.log = &log;
	root.name = "root";
	panel.name = "panel";
	ASSERT_EQ(present(root), 0);
	EXPECT_TRUE(log.empty());
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(root.layouts, 1);
	EXPECT_EQ(root.draws, 1);
	ASSERT_EQ(present(panel), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_application_depth(app), 2U);
	panel.dismiss = true;
	ASSERT_EQ(morph_tui_application_post(app, 7, nullptr, nullptr), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
	EXPECT_EQ(panel.closes, 1);
	EXPECT_EQ(panel.close_result, 19);
	EXPECT_EQ(panel.reentry, -EBUSY);
	EXPECT_EQ(root.messages, 0);
	EXPECT_EQ(root.draws, 2);
	EXPECT_EQ(log, (std::vector<std::string>{"root:appear", "root:disappear", "panel:appear",
						 "panel:disappear", "panel:close", "root:appear"}));
}

TEST_F(TuiApplicationTest, BackgroundPostsKeepOrderAndReleaseEachPayload)
{
	AppPage &root = page();
	root.redraw = true;
	std::atomic<int> released{0};
	ASSERT_EQ(present(root), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	int posted = 0;
	std::thread worker([&] {
		for (int i = 0; i < 257; i++) {
			if (morph_tui_application_post(
				    app, i, &released, +[](void *data) {
					    static_cast<std::atomic<int> *>(data)->fetch_add(1);
				    }))
				break;
			posted++;
		}
	});
	worker.join();
	ASSERT_EQ(posted, 257);
	pump_until([&] { return root.messages == posted; });
	EXPECT_EQ(released.load(), posted);
	EXPECT_LT(root.draws, posted);
	for (int i = 0; i < posted; i++)
		EXPECT_EQ(root.codes[static_cast<size_t>(i)], i);
}

TEST_F(TuiApplicationTest, StopRejectsPostsAndReleasesAlreadyQueuedMessages)
{
	AppPage &root = page();
	AppPage &pending = page();
	std::atomic<int> released{0};
	ASSERT_EQ(present(root), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	ASSERT_EQ(present(pending), 0);
	ASSERT_EQ(morph_tui_application_post(
			  app, 1, &released,
			  +[](void *data) { static_cast<std::atomic<int> *>(data)->fetch_add(1); }),
		  0);
	morph_tui_application_stop(app, -EIO);
	EXPECT_EQ(morph_tui_application_wake(app), -ECANCELED);
	EXPECT_EQ(morph_tui_application_post(app, 2, &released, nullptr), -ECANCELED);
	EXPECT_EQ(morph_tui_application_step(app, 0), -EIO);
	morph_tui_application_destroy(app);
	app = nullptr;
	EXPECT_EQ(released.load(), 1);
	EXPECT_EQ(root.closes, 1);
	EXPECT_EQ(root.close_result, -EIO);
	EXPECT_EQ(pending.closes, 1);
	EXPECT_EQ(root.messages, 0);
}

TEST_F(TuiApplicationTest, TimersAndInvalidationHaveSeparateLayoutAndDrawPhases)
{
	AppPage &root = page();
	ASSERT_EQ(present(root), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_DISPLAY);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(root.layouts, 1);
	EXPECT_EQ(root.draws, 2);
	morph_tui_application_schedule(app, 5);
	ASSERT_EQ(morph_tui_application_step(app, 1), 0);
	EXPECT_EQ(root.ticks, 1);
	EXPECT_EQ(root.draws, 2);
	morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_LAYOUT);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(root.layouts, 2);
	EXPECT_EQ(root.draws, 3);
}

TEST_F(TuiApplicationTest, OwnerThreadIsRequiredForLifecycle)
{
	AppPage &root = page();
	ASSERT_EQ(present(root), 0);
	int step_rc = 0, present_rc = 0, dismiss_rc = 0;
	bool owner = true;
	std::thread worker([&] {
		owner = morph_tui_application_is_owner(app);
		step_rc = morph_tui_application_step(app, 0);
		present_rc = present(root);
		dismiss_rc = morph_tui_application_dismiss_controller(app, 0);
	});
	worker.join();
	EXPECT_FALSE(owner);
	EXPECT_EQ(step_rc, -EPERM);
	EXPECT_EQ(present_rc, -EPERM);
	EXPECT_EQ(dismiss_rc, -EPERM);
}

TEST_F(TuiApplicationTest, DrawFailureStopsAndCleanupRunsOnce)
{
	AppPage &root = page();
	root.failure = -EIO;
	ASSERT_EQ(present(root), 0);
	EXPECT_EQ(morph_tui_application_run(app), -EIO);
	morph_tui_application_destroy(app);
	app = nullptr;
	EXPECT_EQ(root.closes, 1);
	EXPECT_EQ(root.close_result, -EIO);
}

TEST_F(TuiApplicationTest, FailedTerminalEntryClosesMountedAndQueuedPagesOnce)
{
	AppPage &root = page();
	AppPage &panel = page();
	AppPage &queued = page();
	ASSERT_EQ(present(root), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_controller controller{};
	controller.ops = &ops;
	controller.data = &panel;
	controller.input_mode = MORPH_TUI_INPUT_KEYS;
	ASSERT_EQ(morph_tui_application_present_controller(app, &controller), 0);
	ASSERT_EQ(present(queued), 0);
	EXPECT_EQ(morph_tui_application_run(app), -ENOTTY);
	morph_tui_application_destroy(app);
	app = nullptr;
	EXPECT_EQ(root.closes, 1);
	EXPECT_EQ(panel.closes, 1);
	EXPECT_EQ(queued.closes, 1);
	EXPECT_EQ(panel.close_result, -ENOTTY);
}

TEST_F(TuiApplicationTest, OutputFailureStopsTheApplication)
{
	morph_tui_application_destroy(app);
	app = nullptr;
	fclose(output);
	output = fopen("/dev/null", "r");
	ASSERT_NE(output, nullptr);
	struct morph_tui_application_options options{};
	options.external_signals = 1;
	options.input = input;
	options.output = output;
	ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
	AppPage &root = page();
	root.write_output = true;
	ASSERT_EQ(present(root), 0);
	EXPECT_EQ(morph_tui_application_run(app), -EIO);
	morph_tui_application_destroy(app);
	app = nullptr;
	EXPECT_EQ(root.closes, 1);
	EXPECT_EQ(root.close_result, -EIO);
}

TEST_F(TuiApplicationTest, RawPanelRestoresTerminalAndDoesNotReadAhead)
{
	morph_tui_application_destroy(app);
	app = nullptr;
	fclose(input);
	input = nullptr;
	close(writer);
	writer = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(writer, 0);
	ASSERT_EQ(grantpt(writer), 0);
	ASSERT_EQ(unlockpt(writer), 0);
	input = fopen(ptsname(writer), "r+");
	ASSERT_NE(input, nullptr);
	struct termios before{}, after{};
	ASSERT_EQ(tcgetattr(fileno(input), &before), 0);
	int original_flags = fcntl(fileno(input), F_GETFL);
	/* A PTY can expose extra status bits after its first output. Compare
	 * the configurable flags, including the nonblocking bit we restore. */
	int status_mask = O_NONBLOCK | O_APPEND | O_ASYNC;
	struct morph_tui_application_options options{};
	options.external_signals = 1;
	options.input = input;
	options.output = input;
	ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
	AppPage &root = page();
	AppPage &panel = page();
	ASSERT_EQ(present(root), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_controller controller{};
	controller.ops = &ops;
	controller.data = &panel;
	controller.input_mode = MORPH_TUI_INPUT_KEYS;
	controller.terminal_flags =
		MORPH_TUI_TERMINAL_SESSION_ALTERNATE | MORPH_TUI_TERMINAL_SESSION_PASTE;
	ASSERT_EQ(morph_tui_application_present_controller(app, &controller), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(fcntl(fileno(input), F_GETFL) & status_mask, original_flags & status_mask);
	struct winsize size{};
	size.ws_col = 30;
	size.ws_row = 12;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
	ASSERT_EQ(morph_tui_application_wake(app), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(panel.width, 30);
	ASSERT_EQ(write(writer, "\033[", 2), 2);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	ASSERT_EQ(write(writer, "A", 1), 1);
	pump_until([&] { return panel.keys == 1; });
	EXPECT_EQ(panel.last_key, MORPH_TUI_KEY_UP);
	ASSERT_EQ(write(writer, "\033", 1), 1);
	pump_until([&] { return panel.keys == 2; });
	EXPECT_EQ(panel.last_key, 27);
	ASSERT_EQ(morph_tui_application_dismiss_controller(app, 0), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	ASSERT_EQ(tcgetattr(fileno(input), &after), 0);
	EXPECT_EQ(after.c_lflag & (ICANON | ECHO | ISIG | IEXTEN),
		  before.c_lflag & (ICANON | ECHO | ISIG | IEXTEN));
	EXPECT_EQ(fcntl(fileno(input), F_GETFL) & status_mask, original_flags & status_mask);
	morph_tui_application_destroy(app);
	app = nullptr;
}

TEST_F(TuiApplicationTest, MainRejectsInitializedApplicationWithoutDestroyingTheHost)
{
	struct morph_tui_view_controller_spec root{};
	testing::internal::CaptureStderr();
	EXPECT_EQ(morph_tui_application_main(app, &root), EXIT_FAILURE);
	std::string error = testing::internal::GetCapturedStderr();
	EXPECT_FALSE(error.empty());
	EXPECT_TRUE(morph_tui_application_is_owner(app));
	AppPage &root_page = page();
	ASSERT_EQ(present(root_page), 0);
	EXPECT_EQ(morph_tui_application_step(app, 0), 0);
}

TEST_F(TuiApplicationTest, DestroyIsDeferredDuringCallbacksAndDirectStopKeepsItsResult)
{
	static const struct morph_tui_controller_ops guarded = {
		+[](void *, struct morph_tui_application *host,
		    const struct morph_tui_event *event) {
			if (event->type == MORPH_TUI_EVENT_MESSAGE) {
				morph_tui_application_destroy(host);
				EXPECT_TRUE(morph_tui_application_is_owner(host));
			}
			return 0;
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_controller controller{};
	controller.ops = &guarded;
	ASSERT_EQ(morph_tui_application_present_controller(app, &controller), 0);
	ASSERT_EQ(morph_tui_application_post(app, 1, nullptr, nullptr), 0);
	EXPECT_EQ(morph_tui_application_step(app, 0), 0);
	morph_tui_application_stop(app, -EIO);
	EXPECT_EQ(morph_tui_application_step(app, 0), -EIO);
}
