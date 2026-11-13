#include <gtest/gtest.h>
#include <morph/tui/application.h>
#include <morph/tui/canvas.h>
#include <morph/tui/renderer.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/scroll.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/window.h>
#include "util/cleanup.h"

extern "C" {
#include "tui/core/view_internal.h"
}

#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <vector>
#include <unistd.h>

class TuiSelectionSurfaceTest : public ::testing::Test
{
      protected:
	struct morph_tui_surface *surface = nullptr;
	struct morph_tui_canvas canvas{};

	void SetUp() override
	{
		ASSERT_EQ(morph_tui_surface_create(&surface), 0);
		ASSERT_EQ(morph_tui_surface_resize(surface, 10, 3), 0);
		morph_tui_canvas_root_borrow(surface, &canvas);
	}
	void TearDown() override { morph_tui_surface_destroy(surface); }
	std::string selected(int x1, int y1, int x2, int y2)
	{
		morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {};
		EXPECT_EQ(morph_tui_surface_selection_text(surface, x1, y1, x2, y2, &text), 0);
		const char *value = morph_buf_cstr_borrow(&text);

		return value ? value : "";
	}
};

TEST_F(TuiSelectionSurfaceTest, WideAndCombiningGlyphsStayWholeInEitherDirection)
{
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 0, 0, "A中éZ", 0), 0);
	EXPECT_EQ(selected(2, 0, 3, 0), "中é");
	EXPECT_EQ(selected(3, 0, 2, 0), "中é");
	EXPECT_EQ(selected(1, 0, 1, 0), "中");
	EXPECT_EQ(selected(2, 0, 2, 0), "中");
}

TEST_F(TuiSelectionSurfaceTest, MultilineRangeTrimsPaddingAndRetainsBlankRows)
{
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 0, 0, "alpha", 0), 0);
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 0, 2, "omega", 0), 0);
	EXPECT_EQ(selected(2, 0, 1, 2), "pha\n\nom");
	EXPECT_EQ(selected(1, 2, 2, 0), "pha\n\nom");
	EXPECT_EQ(selected(INT_MIN, INT_MIN, INT_MAX, INT_MAX), "alpha\n\nomega");
}

TEST_F(TuiSelectionSurfaceTest, CopyContainsVisibleTextWithoutLinksOrTerminalStyles)
{
	struct morph_tui_text_style style{};
	style.attributes = MORPH_TUI_STYLE_BOLD;
	style.foreground = {MORPH_TUI_COLOR_INDEXED, 6};
	ASSERT_EQ(morph_tui_canvas_text_link(&canvas, 0, 0, "linked", &style,
		"https://example.test/private"), 0);
	ASSERT_EQ(morph_tui_surface_selection_style(surface, 0, 0, 5, 0,
		MORPH_TUI_STYLE_SELECTED), 0);
	EXPECT_EQ(selected(0, 0, 5, 0), "linked");
	morph_buf_t rows MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_tui_surface_rows(surface, &rows), 0);
	EXPECT_NE(std::string(rows.data).find("\033[1m\033[7m\033[38;5;6m"),
		std::string::npos);
}

TEST_F(TuiSelectionSurfaceTest, EmptySurfaceAndInvalidAttributesAreSafe)
{
	ASSERT_EQ(morph_tui_surface_resize(surface, 0, 0), 0);
	EXPECT_EQ(selected(INT_MIN, INT_MIN, INT_MAX, INT_MAX), "");
	EXPECT_EQ(morph_tui_surface_selection_style(surface, 0, 0, 1, 1, UINT_MAX), -EINVAL);
}

class TuiWindowSelectionTest : public ::testing::Test
{
      protected:
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct morph_tui_application *app = nullptr;
	struct morph_tui_window *window = nullptr;
	struct morph_tui_view *label = nullptr;
	std::string copied, rendered;
	int copies = 0;
	const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *self = static_cast<TuiWindowSelectionTest *>(controller->data);
			self->build(builder);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	const struct morph_tui_renderer_ops renderer_ops = {
		+[](struct morph_tui_renderer *backend, struct morph_tui_surface *surface, FILE *) {
			auto *self = static_cast<TuiWindowSelectionTest *>(backend->data);
			morph_buf_t rows MORPH_CLEANUP(morph_buf_cleanup) = {};
			int rc = morph_tui_surface_rows(surface, &rows);

			if (!rc)
				self->rendered = morph_buf_cstr_borrow(&rows);
			return rc;
		}, nullptr};
	struct morph_tui_renderer renderer{};
	virtual void build(struct morph_tui_builder *builder)
	{
		label = morph_tui_widget_label_add_borrow(builder, nullptr,
			"Hello中é world", nullptr);
	}

	void SetUp() override
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		ASSERT_NE(input = fopen(ptsname(master), "r+"), nullptr);
		ASSERT_NE(output = fdopen(dup(fileno(input)), "w"), nullptr);
		struct winsize size{};
		size.ws_col = 20;
		size.ws_row = 4;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct morph_tui_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
		window = morph_tui_application_window_borrow(app);
		renderer.ops = &renderer_ops;
		renderer.data = this;
		ASSERT_EQ(morph_tui_window_set_renderer_borrow(window, &renderer), 0);
		ASSERT_EQ(morph_tui_window_set_text_copy_handler_borrow(window,
			+[](void *data, const char *text) {
				auto *self = static_cast<TuiWindowSelectionTest *>(data);
				self->copied = text;
				self->copies++;
				return 0;
			}, this), 0);
		struct morph_tui_view_controller_spec spec{};
		spec.ops = &controller_ops;
		spec.data = this;
		ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
		ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	}
	void TearDown() override
	{
		morph_tui_application_destroy(app);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		if (master >= 0)
			close(master);
	}
	int mouse(enum morph_tui_mouse_button button, enum morph_tui_mouse_action action,
		  int x, int y)
	{
		struct morph_tui_event event{};
		event.type = MORPH_TUI_EVENT_MOUSE;
		event.mouse = {button, action, x, y, 0};
		return core_window_mouse_event(window, &event);
	}
	void select()
	{
		ASSERT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_PRESS, 1, 0), 1);
		ASSERT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_MOVE, 7, 0), 1);
		ASSERT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_RELEASE, 7, 0), 1);
		ASSERT_EQ(morph_tui_application_render(app), 0);
	}
	void send(const std::string &bytes)
	{
		ASSERT_EQ(write(master, bytes.data(), bytes.size()),
			static_cast<ssize_t>(bytes.size()));
		for (size_t n = 0; n < bytes.size() + 3; n++)
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	}
};

TEST_F(TuiWindowSelectionTest, DragHighlightsAndRightClickCopiesWithoutChangingFocus)
{
	auto *focus = morph_tui_window_focus_borrow(window);
	select();
	EXPECT_EQ(core_window_selection_dragging(window), 0);
	EXPECT_NE(rendered.find("\033[7mello中é"), std::string::npos);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 10, 2), 1);
	EXPECT_EQ(copied, "ello中é");
	EXPECT_EQ(copies, 1);
	EXPECT_EQ(morph_tui_window_focus_borrow(window), focus);
}

TEST_F(TuiWindowSelectionTest, SingleClickAndRightClickWithoutSelectionDoNotCopy)
{
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
	ASSERT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_PRESS, 1, 0), 1);
	ASSERT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_RELEASE, 1, 0), 1);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
	EXPECT_EQ(copies, 0);
}

TEST_F(TuiWindowSelectionTest, ChangedTextAndResizedViewportClearSelection)
{
	select();
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(label, "Changed"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(label, "Hello中é world"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	select();
	struct morph_tui_screen screen{};
	screen.area = {1, 1, 18, 4};
	screen.session.output = output;
	ASSERT_EQ(core_window_draw(window, &screen), 0);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
	EXPECT_EQ(copies, 0);
}

TEST_F(TuiWindowSelectionTest, ClipboardHandlerIsOwnerOnlyAndCanBeRemoved)
{
	int result = 0;
	std::thread worker([&] {
		result = morph_tui_window_set_text_copy_handler_borrow(window, nullptr, nullptr);
	});
	worker.join();
	EXPECT_EQ(result, -EPERM);
	select();
	ASSERT_EQ(morph_tui_window_set_text_copy_handler_borrow(window, nullptr, nullptr), 0);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_RIGHT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
	EXPECT_EQ(mouse(MORPH_TUI_MOUSE_LEFT, MORPH_TUI_MOUSE_PRESS, 0, 0), 0);
}

class TuiPointerCaptureTest;

struct TuiMouseProbe {
	struct morph_tui_widget base;
	TuiPointerCaptureTest *model;
};

class TuiPointerCaptureTest : public TuiWindowSelectionTest
{
      public:
	struct morph_tui_view *scroll = nullptr, *probe = nullptr;
	struct morph_tui_builder *builder = nullptr;
	std::vector<struct morph_tui_mouse_event> events;
	bool replace_on_press = false;
	const struct morph_tui_view_ops probe_ops = {
		+[](struct morph_tui_view *, int width, struct morph_tui_view_size *size) {
			*size = {width, 8};
			return 0;
		}, nullptr, nullptr,
		+[](struct morph_tui_view *view, const struct morph_tui_event *event) {
			if (event->type != MORPH_TUI_EVENT_MOUSE)
				return 0;
			auto *model = reinterpret_cast<TuiMouseProbe *>(view)->model;
			model->events.push_back(event->mouse);
			if (event->mouse.action != MORPH_TUI_MOUSE_PRESS)
				return 0;
			if (model->replace_on_press) {
				model->replace_on_press = false;
				int rc = morph_tui_view_clear_children(model->scroll);
				if (rc)
					return rc;
				model->add_probe();
			}
			return 1;
		}, nullptr, nullptr};

	void add_probe()
	{
		struct morph_tui_view_options options{};
		options.margin.left = 1;
		options.padding.left = 1;
		options.padding.top = 1;
		struct TuiMouseProbe *node = reinterpret_cast<TuiMouseProbe *>(
			morph_tui_widget_add_borrow(builder, scroll, sizeof(*node), &probe_ops,
				&options));
		if (node)
			node->model = this;
		probe = node ? &node->base.view : nullptr;
	}
	void build(struct morph_tui_builder *scope) override
	{
		builder = scope;
		struct morph_tui_view_options root_options{};
		root_options.padding.left = root_options.padding.right = 1;
		auto *root = morph_tui_widget_stack_add_borrow(builder, nullptr,
			MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &root_options);
		struct morph_tui_view_options options{};
		options.height = 4;
		options.margin.left = options.margin.right = 1;
		options.padding.left = options.padding.right = 1;
		scroll = morph_tui_widget_scroll_add_borrow(builder, root, &options);
		add_probe();
	}
	void press() { send("\033[<0;6;2M"); }
	void release() { send("\033[<0;6;2m"); }
};

TEST_F(TuiPointerCaptureTest, HitTestingUsesPaddingMarginsAndScrolledContentCoordinates)
{
	EXPECT_EQ(morph_tui_widget_scroll_offset(scroll), 5);
	send("\033[<0;3;2M\033[<0;3;2m");
	EXPECT_TRUE(events.empty());
	press();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].action, MORPH_TUI_MOUSE_PRESS);
	EXPECT_EQ(events[0].x, 0);
	EXPECT_EQ(events[0].y, 5);
	release();
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[1].action, MORPH_TUI_MOUSE_RELEASE);
}

TEST_F(TuiPointerCaptureTest, CaptureDeliversOutsideMoveAndReleaseToOriginalWidget)
{
	press();
	send("\033[<32;20;4M\033[<0;20;4m");
	ASSERT_EQ(events.size(), 3u);
	EXPECT_EQ(events[1].action, MORPH_TUI_MOUSE_MOVE);
	EXPECT_EQ(events[1].x, -1);
	EXPECT_EQ(events[1].y, -1);
	EXPECT_EQ(events[2].action, MORPH_TUI_MOUSE_RELEASE);
	EXPECT_EQ(events[2].x, -1);
	release();
	EXPECT_EQ(events.size(), 3u);
}

TEST_F(TuiPointerCaptureTest, KeyboardCancelsCaptureBeforeFurtherMouseRelease)
{
	press();
	send("a");
	ASSERT_EQ(events.size(), 2u);
	EXPECT_EQ(events[1].action, MORPH_TUI_MOUSE_CANCEL);
	release();
	EXPECT_EQ(events.size(), 2u);
}

TEST_F(TuiPointerCaptureTest, ReplacingSenderDuringPressDoesNotCaptureReplacement)
{
	replace_on_press = true;
	press();
	ASSERT_NE(probe, nullptr);
	ASSERT_EQ(events.size(), 1u);
	release();
	EXPECT_EQ(events.size(), 1u);
	press();
	release();
	ASSERT_EQ(events.size(), 3u);
	EXPECT_EQ(events[2].action, MORPH_TUI_MOUSE_RELEASE);
}

TEST_F(TuiPointerCaptureTest, RemovingCapturedSubtreeDoesNotRetainFreedPointer)
{
	press();
	ASSERT_EQ(morph_tui_view_clear_children(scroll), 0);
	add_probe();
	ASSERT_NE(probe, nullptr);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	release();
	EXPECT_EQ(events.size(), 1u);
	press();
	release();
	EXPECT_EQ(events.size(), 3u);
}
