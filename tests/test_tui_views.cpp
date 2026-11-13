#include <gtest/gtest.h>
#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/layout.h>
#include <morph/tui/window.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/image.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/scroll.h>
#include <morph/tui/widgets/text_field.h>
#include <morph/tui/widgets/text_view.h>
#include "util/utf8.h"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <vector>
#include <unistd.h>

struct TuiViewModel {
	struct morph_tui_view *root = nullptr;
	struct morph_tui_view *field = nullptr;
	struct morph_tui_view *list = nullptr;
	struct morph_tui_view *label = nullptr;
	struct morph_tui_view_controller *controller = nullptr;
	int actions = 0, closes = 0, widget_closes = 0, result = 0, submits = 0, activations = 0;
	bool editor = true, failure = false, dialog = false;
	std::string selected;
};

static void tui_views_build(struct morph_tui_view_controller *controller,
			    struct morph_tui_builder *builder)
{
	auto *model = static_cast<TuiViewModel *>(controller->data);
	model->controller = controller;
	struct morph_tui_view_options fill{};
	fill.grow = 1;
	model->root = morph_tui_widget_stack_add_borrow(
		builder, nullptr, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
	if (model->editor)
		model->field = morph_tui_widget_text_field_add_borrow(builder, model->root, "", 32,
								      1, nullptr);
	const char *items[] = {"Alpha", "Beta", "Gamma"};
	model->list = morph_tui_widget_list_add_borrow(builder, model->root, items, 3, 2, &fill);
	model->label = morph_tui_widget_label_add_borrow(builder, model->root, "Ready", nullptr);
	if (model->failure)
		morph_tui_builder_fail(builder, -EIO);
}

static void tui_views_close(struct morph_tui_view_controller *controller, int result)
{
	auto *model = static_cast<TuiViewModel *>(controller->data);
	model->closes++;
	model->result = result;
	model->controller = nullptr;
}

static int tui_views_action(struct morph_tui_view_controller *controller,
			    const struct morph_tui_widget_action *action)
{
	auto *model = static_cast<TuiViewModel *>(controller->data);
	model->actions++;
	if (action->type == MORPH_TUI_WIDGET_ACTION_SUBMIT)
		model->submits++;
	if (action->type == MORPH_TUI_WIDGET_ACTION_ACTIVATE)
		model->activations++;
	model->selected = action->text;
	int rc = morph_tui_widget_label_set_text_copy(model->label, action->text);
	if (rc || !model->dialog || action->type == MORPH_TUI_WIDGET_ACTION_CHANGED)
		return rc;
	static const struct morph_tui_view_controller_ops ops = {
		+[](struct morph_tui_view_controller *, struct morph_tui_builder *builder) {
			morph_tui_widget_label_add_borrow(builder, nullptr, "Dialog", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &ops;
	return morph_tui_view_controller_present_borrow(
		morph_tui_view_controller_application_borrow(controller), &spec);
}

class TuiViewsTest : public ::testing::Test
{
      protected:
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct morph_tui_application *app = nullptr;
	TuiViewModel model;
	std::string frame, styled_frame;
	int frames = 0;
	int cursor_x = -1, cursor_y = -1, cursor_visible = 0;
	const struct morph_tui_view_controller_ops ops = {
		tui_views_build, tui_views_action, nullptr, nullptr, nullptr, tui_views_close};
	const struct morph_tui_renderer_ops backend_ops = {
		+[](struct morph_tui_renderer *backend, struct morph_tui_surface *surface, FILE *) {
			auto *self = static_cast<TuiViewsTest *>(backend->data);
			morph_buf_t rows{};
			int rc = morph_buf_init(&rows, BUFSIZ);
			if (!rc)
				rc = morph_tui_surface_rows(surface, &rows);
			if (!rc)
				self->styled_frame = morph_buf_cstr_borrow(&rows);
			char *plain =
				rc ? nullptr : utf8_strip_ansi_dup(rows.data, rows.len, nullptr);
			if (plain)
				self->frame = plain;
			else if (!rc)
				rc = -ENOMEM;
			free(plain);
			morph_buf_cleanup(&rows);
			self->cursor_visible = morph_tui_surface_cursor(surface,
				&self->cursor_x, &self->cursor_y);
			self->frames++;
			return rc;
		},
		nullptr};
	struct morph_tui_renderer backend{};

	void SetUp() override
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		ASSERT_NE(input = fopen(ptsname(master), "r+"), nullptr);
		ASSERT_NE(output = fdopen(dup(fileno(input)), "w"), nullptr);
		struct winsize size{};
		size.ws_col = 30;
		size.ws_row = 8;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct morph_tui_application_options options{};
		options.external_signals = 1;
		options.input = input;
		options.output = output;
		ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
		backend.ops = &backend_ops;
		backend.data = this;
		ASSERT_EQ(morph_tui_window_set_renderer_borrow(
				  morph_tui_application_window_borrow(app), &backend),
			  0);
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

	int present()
	{
		struct morph_tui_view_controller_spec spec{};
		spec.ops = &ops;
		spec.data = &model;
		spec.quit_key = 'q';
		int rc = morph_tui_view_controller_present_borrow(app, &spec);
		return rc ? rc : morph_tui_application_step(app, 0);
	}

	void until(const std::function<bool()> &done)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!done() && std::chrono::steady_clock::now() < deadline) {
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_TRUE(done());
	}
	void send(const std::string &bytes)
	{
		ASSERT_EQ(write(master, bytes.data(), bytes.size()),
			  static_cast<ssize_t>(bytes.size()));
		for (size_t n = 0; n < bytes.size() + 3; n++)
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	}
};

TEST_F(TuiViewsTest, LayoutAndControlsNeedNoApplicationDrawOrKeyForwarding)
{
	ASSERT_EQ(present(), 0);
	EXPECT_NE(frame.find("Alpha"), std::string::npos);
	EXPECT_EQ(morph_tui_view_frame(model.field).height, 1);
	EXPECT_EQ(morph_tui_view_frame(model.list).height, 6);
	EXPECT_EQ(morph_tui_view_frame(model.label).y, 7);
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.field);
	int before = frames;
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(model.label, "Updated"), 0);
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(model.label, "Latest"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before + 1);
	EXPECT_NE(frame.find("Latest"), std::string::npos);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before + 1);
}

TEST_F(TuiViewsTest, RepeatedStateUpdatesDoNotScheduleFrames)
{
	ASSERT_EQ(present(), 0);
	int before = frames;

	ASSERT_EQ(morph_tui_widget_label_set_text_copy(model.label, "Ready"), 0);
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(model.label, "\033[31mReady\033[0m"), 0);
	ASSERT_EQ(morph_tui_view_set_hidden(model.list, 0), 0);
	ASSERT_EQ(morph_tui_view_set_focusable(model.field, 1), 0);
	ASSERT_EQ(morph_tui_view_focus(model.field), 0);
	ASSERT_EQ(morph_tui_widget_list_select(model.list, 0), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before);
}

TEST_F(TuiViewsTest, PaddedListNavigationUsesTheContentViewport)
{
	static const struct morph_tui_view_controller_ops padded_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options options{};
			options.padding = morph_tui_view_insets_all(1);
			const char *items[] = {"row0", "row1", "row2", "row3", "row4",
					       "row5", "row6", "row7", "row8", "row9"};
			data->list = morph_tui_widget_list_add_borrow(builder, nullptr, items, 10,
								      0, &options);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &padded_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_view_rect content = morph_tui_view_content_rect(model.list);
	EXPECT_EQ(content.x, 1);
	EXPECT_EQ(content.y, 1);
	EXPECT_EQ(content.width, 28);
	EXPECT_EQ(content.height, 6);
	send("\033[6~");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 6U);
	EXPECT_NE(frame.find("row6"), std::string::npos);
	send("\033[F");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 9U);
	EXPECT_NE(frame.find("row9"), std::string::npos);

	struct winsize size{};
	size.ws_col = 1;
	size.ws_row = 1;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	content = morph_tui_view_content_rect(model.list);
	EXPECT_EQ(content.width, 0);
	EXPECT_EQ(content.height, 0);
	EXPECT_LE(content.x, morph_tui_view_frame(model.list).width);
	EXPECT_LE(content.y, morph_tui_view_frame(model.list).height);
}

TEST_F(TuiViewsTest, InsetsSeparateOuterAllocationFrameAndContentForCustomLayouts)
{
	static const struct morph_tui_view_controller_ops inset_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options root{};
			root.margin = {1, 2, 1, 3};
			root.padding = {1, 2, 1, 1};
			data->root = morph_tui_widget_stack_add_borrow(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &root);
			struct morph_tui_view_options child{};
			child.grow = 1;
			child.margin = {1, 3, 0, 2};
			child.padding = {0, 1, 0, 1};
			data->label =
				morph_tui_widget_label_add_borrow(builder, data->root, "X", &child);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &inset_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_view_rect root = morph_tui_view_frame(model.root);
	EXPECT_EQ(root.x, 3);
	EXPECT_EQ(root.y, 1);
	EXPECT_EQ(root.width, 25);
	EXPECT_EQ(root.height, 6);
	struct morph_tui_view_rect content = morph_tui_view_content_rect(model.root);
	EXPECT_EQ(content.x, 1);
	EXPECT_EQ(content.y, 1);
	EXPECT_EQ(content.width, 22);
	EXPECT_EQ(content.height, 4);
	struct morph_tui_view_rect child = morph_tui_view_frame(model.label);
	EXPECT_EQ(child.x, 3);
	EXPECT_EQ(child.y, 2);
	EXPECT_EQ(child.width, 17);
	EXPECT_EQ(child.height, 3);
	struct morph_tui_view_size measured{};
	ASSERT_EQ(morph_tui_view_measure(model.label, 22, &measured), 0);
	EXPECT_EQ(measured.width, 8);
	EXPECT_EQ(measured.height, 2);
	EXPECT_NE(frame.find("       X"), std::string::npos);

	/* Arrange consumes an outer slot for custom containers just as for Stack. */
	ASSERT_EQ(morph_tui_view_arrange(model.label, {0, 0, 20, 5}), 0);
	child = morph_tui_view_frame(model.label);
	EXPECT_EQ(child.x, 2);
	EXPECT_EQ(child.y, 1);
	EXPECT_EQ(child.width, 15);
	EXPECT_EQ(child.height, 4);
}

TEST_F(TuiViewsTest, HorizontalGrowthAndCrossAlignmentIncludeMarginsOutsideSizeLimits)
{
	static const struct morph_tui_view_controller_ops inset_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			data->root = morph_tui_widget_stack_add_borrow(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_HORIZONTAL, 0, nullptr);
			struct morph_tui_view_options first{};
			first.grow = 1;
			first.max_width = 4;
			first.height = 2;
			first.alignment = MORPH_TUI_VIEW_CENTER;
			first.margin = {1, 2, 3, 1};
			data->label = morph_tui_widget_label_add_borrow(builder, data->root, "one",
									&first);
			struct morph_tui_view_options second{};
			second.grow = 1;
			second.margin = {0, 1, 0, 2};
			data->field = morph_tui_widget_label_add_borrow(builder, data->root, "two",
									&second);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &inset_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_view_rect first = morph_tui_view_frame(model.label);
	struct morph_tui_view_rect second = morph_tui_view_frame(model.field);
	EXPECT_EQ(first.x, 1);
	EXPECT_EQ(first.y, 2);
	EXPECT_EQ(first.width, 4);
	EXPECT_EQ(first.height, 2);
	EXPECT_EQ(second.x, 9);
	EXPECT_EQ(second.width, 20);
	EXPECT_EQ(second.height, 8);
	ASSERT_EQ(morph_tui_view_set_hidden(model.label, 1), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	second = morph_tui_view_frame(model.field);
	EXPECT_EQ(second.x, 2);
	EXPECT_EQ(second.width, 27);
}

TEST_F(TuiViewsTest, InsetSettersInvalidateLayoutAndRejectInvalidOrWorkerChanges)
{
	ASSERT_EQ(present(), 0);
	struct morph_tui_view_insets margin{1, 2, 0, 3};
	struct morph_tui_view_insets padding{0, 1, 0, 2};
	ASSERT_EQ(morph_tui_view_set_margin(model.list, margin), 0);
	ASSERT_EQ(morph_tui_view_set_padding(model.list, padding), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_view_frame(model.list).x, 3);
	EXPECT_EQ(morph_tui_view_frame(model.list).width, 25);
	EXPECT_EQ(morph_tui_view_content_rect(model.list).width, 22);
	int before = frames;
	ASSERT_EQ(morph_tui_view_set_margin(model.list, margin), 0);
	ASSERT_EQ(morph_tui_view_set_padding(model.list, padding), 0);
	EXPECT_EQ(morph_tui_view_set_margin(model.list, {-1, 0, 0, 0}), -EINVAL);
	EXPECT_EQ(morph_tui_view_set_padding(model.list, {0, INT_MAX, 0, 1}), -EINVAL);
	std::thread worker([&] {
		EXPECT_EQ(morph_tui_view_set_margin(model.list, {}), -EPERM);
		EXPECT_EQ(morph_tui_view_set_padding(model.list, {}), -EPERM);
	});
	worker.join();
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before);
	EXPECT_EQ(morph_tui_view_get_options(model.list).margin.top, 1);
	/* Excessive but valid insets yield bounded empty frames on tiny terminals. */
	ASSERT_EQ(morph_tui_view_set_margin(model.list, morph_tui_view_insets_all(20)), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_view_frame(model.list).width, 0);
	EXPECT_EQ(morph_tui_view_frame(model.list).height, 0);
	EXPECT_EQ(morph_tui_view_content_rect(model.list).width, 0);
}

TEST_F(TuiViewsTest, PaddedDocumentScrollReachesTheLastLineAndPreservesOffsetOnRefresh)
{
	static const char text[] =
		"line0\nline1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9";
	static const struct morph_tui_view_controller_ops padded_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options options{};
			options.padding = morph_tui_view_insets_all(1);
			data->field = morph_tui_widget_text_view_add_borrow(builder, nullptr, text,
									    &options);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &padded_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	send("\033[F");
	EXPECT_NE(frame.find("line9"), std::string::npos);
	EXPECT_EQ(frame.find("line0"), std::string::npos);
	int before = frames;
	std::string previous = frame;
	ASSERT_EQ(morph_tui_widget_text_view_set_text_copy(model.field, text), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before);
	EXPECT_EQ(frame, previous);
	send("\033[H\033[6~");
	EXPECT_NE(frame.find("line6"), std::string::npos);
	EXPECT_NE(frame.find("line9"), std::string::npos);
	send("\033[F");
	before = frames;
	send("\033[B");
	EXPECT_EQ(frames, before);
}

TEST_F(TuiViewsTest, PrintableQuitKeyBelongsToEditorAndUnicodeArrivesWhole)
{
	ASSERT_EQ(present(), 0);
	send("q你好e\xcc\x81");
	until([&] {
		return std::string(morph_tui_widget_text_field_text_borrow(model.field)) ==
		       "q你好e\xcc\x81";
	});
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
	EXPECT_EQ(model.actions, 5);
}

TEST_F(TuiViewsTest, PasteCannotNavigateSubmitOrInvokePageShortcuts)
{
	ASSERT_EQ(present(), 0);
	send("\033[200~q\tjk\r\n\033[201~");
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text_borrow(model.field)), "qjk");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.field);
	send("\t");
	send("\033[200~qj\r\033[201~");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 0U);
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
}

TEST_F(TuiViewsTest, PasteAfterHidingFocusUsesTheNextAvailableField)
{
	static const struct morph_tui_view_controller_ops fields_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			data->root = morph_tui_widget_stack_add_borrow(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			data->field = morph_tui_widget_text_field_add_borrow(builder, data->root,
									     "", 32, 0, nullptr);
			data->list = morph_tui_widget_text_field_add_borrow(builder, data->root, "",
									    32, 0, nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &fields_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	ASSERT_EQ(morph_tui_view_set_hidden(model.field, 1), 0);
	send("\033[200~q你好\033[201~");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.list);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.list), "q你好");
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "");
}

TEST_F(TuiViewsTest, FocusTraversalAndModalRestoreTheSameControl)
{
	model.dialog = true;
	ASSERT_EQ(present(), 0);
	send("\t\033[B\r");
	until([&] { return morph_tui_application_depth(app) == 2; });
	EXPECT_NE(frame.find("Dialog"), std::string::npos);
	send("\033");
	until([&] { return morph_tui_application_depth(app) == 1; });
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.list);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 1U);
	send("\033[Z");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.field);
}

TEST_F(TuiViewsTest, HiddenFocusAndTinyTerminalHaveBoundedLayout)
{
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_view_set_hidden(model.field, 1), 0);
	send("j");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.list);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 1U);
	struct winsize size{};
	size.ws_col = 1;
	size.ws_row = 1;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_LE(morph_tui_view_frame(model.list).height, 1);
	EXPECT_GE(morph_tui_view_frame(model.label).height, 0);
}

TEST_F(TuiViewsTest, MouseListActivationRespectsPaddingScrollAndDragging)
{
	model.editor = false;
	ASSERT_EQ(present(), 0);
	const char *items[] = {"row0", "row1", "row2", "row3", "row4",
			       "row5", "row6", "row7", "row8", "row9"};
	ASSERT_EQ(morph_tui_widget_list_set_items_copy(model.list, items, 10), 0);
	ASSERT_EQ(morph_tui_view_set_padding(model.list, morph_tui_view_insets_all(1)), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	send("\033[F");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 9U);
	send("\033[<0;3;3M");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 6U);
	EXPECT_EQ(model.activations, 0);
	send("\033[<0;3;3m");
	EXPECT_EQ(model.activations, 1);
	EXPECT_EQ(model.selected, "row6");
	send("\033[<0;3;4M\033[<32;4;4M\033[<0;3;4m");
	EXPECT_EQ(model.activations, 1);
	send("\033[<0;3;4M\033[<32;40;20M\033[<0;3;4m");
	EXPECT_EQ(model.activations, 1);
	send("\r");
	EXPECT_EQ(model.activations, 2);
	EXPECT_EQ(model.selected, "row7");
	ASSERT_EQ(morph_tui_widget_list_set_items_copy(model.list, items, 2), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	send("\033[<0;3;5M\033[<0;3;5m");
	EXPECT_EQ(model.activations, 2);
	send("\033[<0;1;1M\033[<0;1;1m");
	EXPECT_EQ(model.activations, 2);
}

TEST_F(TuiViewsTest, BuildFailureClosesControllerAndLeavesNoPresentation)
{
	model.failure = true;
	EXPECT_EQ(present(), -EIO);
	EXPECT_EQ(model.closes, 1);
	EXPECT_EQ(model.result, -EIO);
	EXPECT_EQ(morph_tui_application_depth(app), 0U);
}

TEST_F(TuiViewsTest, CopiedItemsSurviveCallerChangesAndReplacementIsTransactional)
{
	ASSERT_EQ(present(), 0);
	char text[] = "Borrowed";
	const char *items[] = {text};
	ASSERT_EQ(morph_tui_widget_list_set_items_copy(model.list, items, 1), 0);
	text[0] = 'X';
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("Borrowed"), std::string::npos);
	const char *invalid[] = {"first", nullptr};
	EXPECT_EQ(morph_tui_widget_list_set_items_copy(model.list, invalid, 2), -EINVAL);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("Borrowed"), std::string::npos);
}

struct TuiDerivedWidget {
	struct morph_tui_widget base;
	int *destroyed;
};

TEST_F(TuiViewsTest, CustomWidgetBaseUsesCanvasAndDestroysExactlyOnceAfterFailedBuild)
{
	static const struct morph_tui_view_ops custom = {
		nullptr, nullptr,
		+[](struct morph_tui_view *, struct morph_tui_canvas *canvas) {
			return morph_tui_canvas_text(canvas, 0, 0, "Custom", 0);
		},
		nullptr,
		+[](struct morph_tui_view *view) {
			(*reinterpret_cast<TuiDerivedWidget *>(view)->destroyed)++;
		}, nullptr};
	static const struct morph_tui_view_controller_ops controller = {
		+[](struct morph_tui_view_controller *page, struct morph_tui_builder *builder) {
			auto *widget =
				reinterpret_cast<TuiDerivedWidget *>(morph_tui_widget_add_borrow(
					builder, nullptr, sizeof(TuiDerivedWidget), &custom,
					nullptr));
			if (widget)
				widget->destroyed = static_cast<int *>(page->data);
			morph_tui_builder_fail(builder, -EIO);
			morph_tui_widget_label_add_borrow(builder, nullptr, "Never created",
							  nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller;
	spec.data = &model.widget_closes;
	EXPECT_EQ(morph_tui_view_controller_present_borrow(app, &spec), -EIO);
	EXPECT_EQ(model.widget_closes, 1);
}

TEST(TuiCanvasTest, ClipsWideCellsCombiningMarksAndUntrustedControls)
{
	struct morph_tui_surface *surface = nullptr;
	ASSERT_EQ(morph_tui_surface_create(&surface), 0);
	auto *saved = surface;
	EXPECT_EQ(morph_tui_surface_create(&surface), -EEXIST);
	EXPECT_EQ(surface, saved);
	ASSERT_EQ(morph_tui_surface_resize(surface, 8, 2), 0);
	struct morph_tui_canvas root{}, child{};
	morph_tui_canvas_root_borrow(surface, &root);
	morph_tui_canvas_child_borrow(&root, 1, 0, 4, 1, &child);
	ASSERT_EQ(morph_tui_canvas_text(&child, -1, 0, "界ab界", 0), 0);
	ASSERT_EQ(morph_tui_canvas_text(&root, 0, 1, "e\xcc\x81\033[2JX", 0), 0);
	morph_buf_t rows{};
	ASSERT_EQ(morph_buf_init(&rows, BUFSIZ), 0);
	ASSERT_EQ(morph_tui_surface_rows(surface, &rows), 0);
	char *plain = utf8_strip_ansi_dup(rows.data, rows.len, nullptr);
	ASSERT_NE(plain, nullptr);
	EXPECT_EQ(std::string(plain), "  ab    \ne\xcc\x81X      \n");
	free(plain);
	morph_buf_cleanup(&rows);
	morph_tui_surface_destroy(surface);
}

TEST(TuiCanvasTest, OverwritingEitherHalfOfWideCellClearsTheWholeGlyph)
{
	struct morph_tui_surface *surface = nullptr;
	ASSERT_EQ(morph_tui_surface_create(&surface), 0);
	ASSERT_EQ(morph_tui_surface_resize(surface, 4, 1), 0);
	struct morph_tui_canvas canvas{};
	morph_tui_canvas_root_borrow(surface, &canvas);
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 0, 0, "界ab", 0), 0);
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 1, 0, "X", 0), 0);
	morph_buf_t rows{};
	ASSERT_EQ(morph_buf_init(&rows, 64), 0);
	ASSERT_EQ(morph_tui_surface_rows(surface, &rows), 0);
	char *plain = utf8_strip_ansi_dup(rows.data, rows.len, nullptr);
	ASSERT_NE(plain, nullptr);
	EXPECT_EQ(std::string(plain), " Xab\n");
	free(plain);
	morph_buf_cleanup(&rows);
	morph_tui_surface_destroy(surface);
}

TEST_F(TuiViewsTest, ApplicationSubclassUsesOptionalLaunchEventAndTerminationMethods)
{
	morph_tui_application_destroy(app);
	app = nullptr;
	struct DerivedApplication {
		struct morph_tui_application base;
		int launches = 0, messages = 0, terminations = 0, preparations = 0;
	} application{};
	static const struct morph_tui_application_ops application_ops = {
		+[](struct morph_tui_application *base) {
			auto *derived = reinterpret_cast<DerivedApplication *>(base);
			derived->launches++;
			return morph_tui_application_post(base, 77);
		},
		+[](struct morph_tui_application *base, const struct morph_tui_event *event) {
			if (event->type != MORPH_TUI_EVENT_MESSAGE)
				return 0;
			auto *derived = reinterpret_cast<DerivedApplication *>(base);
			derived->messages++;
			morph_tui_application_stop(base, 0);
			return 1;
		},
		+[](struct morph_tui_application *base, int result) {
			auto *derived = reinterpret_cast<DerivedApplication *>(base);
			derived->terminations++;
			EXPECT_EQ(result, 0);
			EXPECT_EQ(morph_tui_application_window_borrow(base), nullptr);
		},
		+[](struct morph_tui_application *base, int result) {
			auto *derived = reinterpret_cast<DerivedApplication *>(base);
			derived->preparations++;
			EXPECT_EQ(result, 0);
			auto *runtime = base;
			EXPECT_NE(runtime, nullptr);
			EXPECT_EQ(morph_tui_application_post(runtime, 1), -ECANCELED);
		}};
	application.base.ops = &application_ops;
	struct morph_tui_application_options options{};
	options.external_signals = 1;
	options.input = input;
	options.output = output;
	options.external_signals = 1;
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_application_init(&application.base, &options), 0);
	ASSERT_EQ(morph_tui_view_controller_present_borrow(&application.base, &spec), 0);
	EXPECT_EQ(morph_tui_application_run(&application.base), 0);
	morph_tui_application_deinit(&application.base);
	EXPECT_EQ(application.launches, 1);
	EXPECT_EQ(application.messages, 1);
	EXPECT_EQ(application.terminations, 1);
	EXPECT_EQ(application.preparations, 1);
	EXPECT_EQ(application.base.state, nullptr);
	EXPECT_EQ(model.closes, 1);
}

TEST_F(TuiViewsTest, LayoutSubclassAndWidgetSubclassComposeThroughPublicInterfaces)
{
	struct DerivedContainer {
		struct morph_tui_widget base;
		struct morph_tui_layout layout;
	};
	static const struct morph_tui_layout_ops layout_ops = {
		+[](struct morph_tui_layout *, struct morph_tui_view *, int width,
		    struct morph_tui_view_size *size) {
			*size = {width, 2};
			return 0;
		},
		+[](struct morph_tui_layout *, struct morph_tui_view *view) {
			auto frame = morph_tui_view_frame(view);
			return morph_tui_view_arrange(
				morph_tui_view_child_borrow(view, 0),
				{2, 1, frame.width > 2 ? frame.width - 2 : 0, 1});
		}};
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *, struct morph_tui_builder *builder) {
			DerivedContainer *container = nullptr;
			container =
				reinterpret_cast<DerivedContainer *>(morph_tui_widget_add_borrow(
					builder, nullptr, sizeof(*container), nullptr, nullptr));
			if (!container)
				return;
			container->layout.ops = &layout_ops;
			morph_tui_view_set_layout_borrow(&container->base.view, &container->layout);
			morph_tui_widget_label_add_borrow(builder, &container->base.view,
							  "Custom layout", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.substr(31, 15), "  Custom layout");
}

TEST_F(TuiViewsTest, WorkerCannotMutateViewsAndMessagesRemainTheUpdateBoundary)
{
	ASSERT_EQ(present(), 0);
	int label_rc = 0, focus_rc = 0, list_rc = 0, render_rc = 0, terminal_rc = 0;
	std::thread worker([&] {
		label_rc = morph_tui_widget_label_set_text_copy(model.label, "Unsafe");
		focus_rc = morph_tui_view_focus(model.list);
		list_rc = morph_tui_widget_list_select(model.list, 2);
		render_rc = morph_tui_application_render(app);
		terminal_rc = morph_tui_application_restore_terminal(app);
	});
	worker.join();
	EXPECT_EQ(label_rc, -EPERM);
	EXPECT_EQ(focus_rc, -EPERM);
	EXPECT_EQ(list_rc, -EPERM);
	EXPECT_EQ(render_rc, -EPERM);
	EXPECT_EQ(terminal_rc, -EPERM);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 0U);
}

TEST_F(TuiViewsTest, BoundActionCanReplaceSenderItemsWithoutInvalidatingItsPayload)
{
	model.editor = false;
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_widget_set_action_borrow(
			  model.list,
			  +[](void *data, const struct morph_tui_widget_action *action) {
				  auto *model = static_cast<TuiViewModel *>(data);
				  const char *replacement[] = {"Replacement"};
				  int rc = morph_tui_widget_list_set_items_copy(action->sender,
										replacement, 1);
				  model->selected = action->text;
				  return rc;
			  },
			  &model),
		  0);
	send("\r");
	EXPECT_EQ(model.selected, "Alpha");
	EXPECT_EQ(model.actions, 0);
	EXPECT_NE(frame.find("Replacement"), std::string::npos);
}

TEST_F(TuiViewsTest, ActionFailureStopsAndUnwindsTheControllerScope)
{
	model.editor = false;
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_widget_set_action_borrow(
			  model.list,
			  +[](void *, const struct morph_tui_widget_action *) { return -EIO; },
			  nullptr),
		  0);
	ASSERT_EQ(write(master, "\r", 1), 1);
	int rc = 0;
	for (int i = 0; i < 100 && !rc; i++)
		rc = morph_tui_application_step(app, 0);
	EXPECT_EQ(rc, -EIO);
	morph_tui_application_destroy(app);
	app = nullptr;
	EXPECT_EQ(model.closes, 1);
	EXPECT_EQ(model.result, -EIO);
}

TEST_F(TuiViewsTest, StackRedistributesGrowthAfterMaximumAndSupportsManySubviews)
{
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *model = static_cast<TuiViewModel *>(controller->data);
			model->root = morph_tui_widget_stack_add_borrow(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_HORIZONTAL, 0, nullptr);
			struct morph_tui_view_options first{};
			first.grow = 1;
			first.max_width = 4;
			struct morph_tui_view_options rest{};
			rest.grow = 1;
			model->field = morph_tui_widget_label_add_borrow(builder, model->root,
									 "one", &first);
			model->list = morph_tui_widget_label_add_borrow(builder, model->root, "two",
									&rest);
			for (int i = 0; i < 6; i++)
				morph_tui_widget_label_add_borrow(builder, model->root, "",
								  nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_view_child_count(model.root), 8U);
	EXPECT_EQ(morph_tui_view_frame(model.field).width, 4);
	EXPECT_EQ(morph_tui_view_frame(model.list).width, 26);
}

TEST_F(TuiViewsTest, WindowReceivesOnlyInputDeclinedByViewsAndController)
{
	ASSERT_EQ(present(), 0);
	int received = 0;
	ASSERT_EQ(morph_tui_window_set_event_handler_borrow(
			  morph_tui_application_window_borrow(app),
			  +[](void *data, const struct morph_tui_event *event) {
				  if (event->key != 'q')
					  return 0;
				  (*static_cast<int *>(data))++;
				  return 1;
			  },
			  &received),
		  0);
	send("q");
	EXPECT_EQ(received, 0);
	send("\tq");
	EXPECT_EQ(received, 1);
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
}

TEST_F(TuiViewsTest, FocusOrderFollowsTreeRatherThanConstructionOrder)
{
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *model = static_cast<TuiViewModel *>(controller->data);
			model->root = morph_tui_widget_stack_add_borrow(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			auto *container = morph_tui_widget_stack_add_borrow(
				builder, model->root, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			model->list = morph_tui_widget_text_field_add_borrow(
				builder, model->root, "second", 32, 0, nullptr);
			model->field = morph_tui_widget_text_field_add_borrow(
				builder, container, "first", 32, 0, nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.field);
	send("\t");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.list);
}

TEST_F(TuiViewsTest, TextFieldSetterCopiesAndPreservesValueOnValidationFailure)
{
	ASSERT_EQ(present(), 0);
	char value[] = "updated";
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, value), 0);
	value[0] = 'X';
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text_borrow(model.field)), "updated");
	EXPECT_EQ(morph_tui_widget_text_field_set_text_copy(model.field,
							    std::string(32, 'a').c_str()),
		  -E2BIG);
	EXPECT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, nullptr), -EINVAL);
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text_borrow(model.field)), "updated");
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)),
		  model.field);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("updated"), std::string::npos);
}

TEST_F(TuiViewsTest, ScrollContainerComposesWrappedTextAndMultilineComposer)
{
	static const struct morph_tui_view_controller_ops scroll_ops = {
		+[](struct morph_tui_view_controller *controller, struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options inset{};
			inset.padding = {0, 1, 0, 1};
			data->root = morph_tui_widget_stack_add_borrow(builder, nullptr,
				MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &inset);
			morph_tui_widget_label_add_borrow(builder, data->root, "Header", nullptr);
			struct morph_tui_view_options fill{};
			fill.grow = 1;
			data->list = morph_tui_widget_scroll_add_borrow(builder, data->root, &fill);
			auto *content = morph_tui_widget_stack_add_borrow(builder, data->list,
				MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			data->label = morph_tui_widget_text_view_add_borrow(builder, content,
				"one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten", nullptr);
			morph_tui_widget_text_view_set_content_sizing(data->label, 1);
			morph_tui_view_set_focusable(data->label, 0);
			struct morph_tui_view_options editor{};
			editor.padding = {1, 1, 1, 1};
			editor.max_height = 5;
			data->field = morph_tui_widget_text_field_add_borrow(builder, data->root,
				"", 4096, 1, &editor);
			morph_tui_widget_text_field_set_multiline(data->field, 1);
			morph_tui_widget_text_field_set_prompt_copy(data->field, "> ");
			morph_tui_widget_label_add_borrow(builder, data->root, "Footer", nullptr);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &scroll_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("ten"), std::string::npos);
	EXPECT_EQ(frame.find("one"), std::string::npos);
	EXPECT_GT(morph_tui_widget_scroll_offset(model.list), 0);
	ASSERT_EQ(morph_tui_widget_scroll_handle_key(model.list, MORPH_TUI_KEY_HOME), 1);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("one"), std::string::npos);
	EXPECT_EQ(frame.find("ten"), std::string::npos);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "first\nsecond"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("first"), std::string::npos);
	EXPECT_NE(frame.find("second"), std::string::npos);
	EXPECT_EQ(morph_tui_view_frame(model.field).height, 4);
	ASSERT_EQ(morph_tui_widget_scroll_follow(model.list, 1), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("ten"), std::string::npos);
}

TEST_F(TuiViewsTest, RichTextMeasuresWrappedContentAndPaintsPaddingInsideMargins)
{
	static const struct morph_tui_view_controller_ops rich_ops = {
		+[](struct morph_tui_view_controller *controller, struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options options{};
			options.margin = {1, 2, 1, 3};
			options.padding = {1, 1, 1, 2};
			options.style.background = {MORPH_TUI_COLOR_RGB, 0x112233};
			data->label = morph_tui_widget_text_view_add_borrow(builder, nullptr, "", &options);
			struct morph_tui_text_span spans[3]{};
			spans[0].text = "中ab";
			spans[0].style.attributes = MORPH_TUI_STYLE_BOLD | MORPH_TUI_STYLE_UNDERLINE;
			spans[0].style.foreground = {MORPH_TUI_COLOR_RGB, 0xabcdef};
			spans[1].text = "文c";
			spans[1].style.attributes = MORPH_TUI_STYLE_ITALIC;
			spans[1].style.foreground = {MORPH_TUI_COLOR_INDEXED, 3};
			spans[2].text = "!";
			spans[2].style.foreground.kind = MORPH_TUI_COLOR_DEFAULT;
			spans[2].style.background.kind = MORPH_TUI_COLOR_DEFAULT;
			morph_tui_widget_text_view_set_spans_copy(data->label, spans, 3);
			morph_tui_widget_text_view_set_content_sizing(data->label, 1);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &rich_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	struct morph_tui_view_size size{};
	ASSERT_EQ(morph_tui_view_measure(model.label, 12, &size), 0);
	EXPECT_EQ(size.width, 12);
	EXPECT_EQ(size.height, 6);
	EXPECT_NE(styled_frame.find("\033[48;2;17;34;51m"), std::string::npos);
	EXPECT_NE(styled_frame.find("\033[38;2;171;205;239m"), std::string::npos);
	EXPECT_NE(styled_frame.find("\033[38;5;3m"), std::string::npos);
	EXPECT_NE(styled_frame.find("\033[4m"), std::string::npos);
	EXPECT_NE(styled_frame.find("\033[3m"), std::string::npos);
	EXPECT_NE(styled_frame.find("\033[0m!"), std::string::npos);
	EXPECT_EQ(frame.substr(0, 30), std::string(30, ' '));
	EXPECT_EQ(styled_frame.substr(0, styled_frame.find('\n')).find("48;2;"), std::string::npos);
	struct morph_tui_text_span invalid{};
	invalid.text = "replacement";
	invalid.style.foreground = {MORPH_TUI_COLOR_INDEXED, 256};
	EXPECT_EQ(morph_tui_widget_text_view_set_spans_copy(model.label, &invalid, 1), -EINVAL);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("中ab文c"), std::string::npos);
	ASSERT_EQ(morph_tui_widget_text_view_set_text_copy(model.label, "a\n"), 0);
	ASSERT_EQ(morph_tui_view_measure(model.label, 12, &size), 0);
	EXPECT_EQ(size.height, 6);
}

TEST_F(TuiViewsTest, MultilinePasteDoesNotSubmitAndInsertionPreservesUtf8Cursor)
{
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_multiline(model.field, 1), 0);
	send("\033[200~first\n中文\033[201~");
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "first\n中文");
	EXPECT_EQ(model.submits, 0);
	send("\033[D");
	ASSERT_EQ(morph_tui_widget_text_field_insert_text_copy(model.field, "🙂"), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "first\n中🙂文");
	send("\033[3~");
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "first\n中🙂");
	int actions = model.actions;
	send("\nnext");
	EXPECT_GT(model.actions, actions);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "first\n中🙂\nnext");
}

TEST_F(TuiViewsTest, ClearingDynamicChildrenRemovesFocusAndCanBeRepeated)
{
	ASSERT_EQ(present(), 0);
	EXPECT_EQ(morph_tui_view_child_count(model.root), 3u);
	ASSERT_EQ(morph_tui_view_clear_children(model.root), 0);
	EXPECT_EQ(morph_tui_view_child_count(model.root), 0u);
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)), nullptr);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("Alpha"), std::string::npos);
	EXPECT_EQ(frame.find("Ready"), std::string::npos);
	ASSERT_EQ(morph_tui_view_clear_children(model.root), 0);
}

TEST_F(TuiViewsTest, ActionCanClearItsOwnSubtreeDuringDispatch)
{
	static const struct morph_tui_view_controller_ops clear_ops = {
		tui_views_build,
		+[](struct morph_tui_view_controller *controller, const struct morph_tui_widget_action *) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			return morph_tui_view_clear_children(data->root);
		}, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &clear_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	send("x");
	EXPECT_EQ(morph_tui_view_child_count(model.root), 0u);
	EXPECT_EQ(morph_tui_window_focus_borrow(morph_tui_application_window_borrow(app)), nullptr);
}

TEST(TuiCanvasTest, StyledSpansPreserveCombiningMarksAcrossStyleBoundaries)
{
	struct morph_tui_surface *surface = nullptr;
	ASSERT_EQ(morph_tui_surface_create(&surface), 0);
	ASSERT_EQ(morph_tui_surface_resize(surface, 4, 1), 0);
	struct morph_tui_canvas canvas{};
	morph_tui_canvas_root_borrow(surface, &canvas);
	struct morph_tui_text_style first{}, second{};
	first.foreground = {MORPH_TUI_COLOR_RGB, 0xabcdef};
	second.foreground = {MORPH_TUI_COLOR_INDEXED, 2};
	ASSERT_EQ(morph_tui_canvas_text_styled(&canvas, 0, 0, "e", &first), 0);
	ASSERT_EQ(morph_tui_canvas_text_styled(&canvas, 1, 0, "\xcc\x81!", &second), 0);
	morph_buf_t rows{};
	ASSERT_EQ(morph_buf_init(&rows, 128), 0);
	ASSERT_EQ(morph_tui_surface_rows(surface, &rows), 0);
	EXPECT_NE(std::string(rows.data).find("e\xcc\x81"), std::string::npos);
	EXPECT_NE(std::string(rows.data).find("38;5;2m!"), std::string::npos);
	morph_buf_cleanup(&rows);
	morph_tui_surface_destroy(surface);
}

TEST_F(TuiViewsTest, SynchronousRenderFlushesViewsWithoutReadingQueuedInput)
{
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(write(master, "x", 1), 1);
	ASSERT_EQ(morph_tui_widget_label_set_text_copy(model.label, "Visible before prompt"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_NE(frame.find("Visible before prompt"), std::string::npos);
	EXPECT_EQ(model.actions, 0);
	until([&] { return model.actions > 0; });
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(model.field), "x");
}

struct TuiPaddingView {
	struct morph_tui_widget base;
	int calls[4]{};
	struct morph_tui_view_size edges[4]{};
};

TEST_F(TuiViewsTest, PaddingDecorationReceivesOnlyActualEdgesAndNeverMarginOrContent)
{
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			static const struct morph_tui_view_ops view_ops = {
				nullptr,
				+[](struct morph_tui_view *view) {
					auto *child = morph_tui_view_child_borrow(view, 0);
					auto content = morph_tui_view_content_rect(view);
					return morph_tui_view_arrange(child, content);
				},
				+[](struct morph_tui_view *, struct morph_tui_canvas *canvas) {
					std::string line(static_cast<size_t>(canvas->width + 4), 'C');
					for (int y = -1; y <= canvas->height; y++) {
						int rc = morph_tui_canvas_text(canvas, -2, y,
							line.c_str(), 0);
						if (rc)
							return rc;
					}
					return 0;
				},
				nullptr, nullptr,
				+[](struct morph_tui_view *view, enum morph_tui_view_edge edge,
				    struct morph_tui_canvas *canvas) {
					auto *padding = reinterpret_cast<TuiPaddingView *>(view);
					padding->calls[edge]++;
					padding->edges[edge] = {canvas->width, canvas->height};
					const char *markers = "TRBL";
					std::string line(static_cast<size_t>(canvas->width + 4),
						markers[edge]);
					/* Deliberately draw beyond the edge canvas; framework
					 * clipping must protect content and outer margins. */
					for (int y = -1; y <= canvas->height; y++) {
						int rc = morph_tui_canvas_text(canvas, -2, y,
							line.c_str(), 0);
						if (rc)
							return rc;
					}
					return 0;
				}};
			struct morph_tui_view_options options{};
			options.margin = {1, 2, 1, 1};
			options.padding = {1, 2, 2, 3};
			TuiPaddingView *sized = nullptr;
			auto *custom = morph_tui_widget_add_borrow(builder, nullptr,
				sizeof(*sized), &view_ops, &options);
			if (!custom)
				return;
			data->root = &custom->view;
			data->label = morph_tui_widget_label_add_borrow(builder, data->root,
				"child", nullptr);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	auto *padding = reinterpret_cast<TuiPaddingView *>(model.root);
	for (int calls : padding->calls)
		EXPECT_EQ(calls, 1);
	const struct morph_tui_view_size expected[] = {{27, 1}, {2, 3}, {27, 2}, {3, 3}};
	for (size_t i = 0; i < 4; i++) {
		EXPECT_EQ(padding->edges[i].width, expected[i].width) << i;
		EXPECT_EQ(padding->edges[i].height, expected[i].height) << i;
	}
	std::string expected_frame = std::string(30, ' ') + '\n';
	expected_frame += " " + std::string(27, 'T') + "  \n";
	expected_frame += " LLLchild" + std::string(17, 'C') + "RR  \n";
	expected_frame += " LLL" + std::string(22, 'C') + "RR  \n";
	expected_frame += " LLL" + std::string(22, 'C') + "RR  \n";
	expected_frame += " " + std::string(27, 'B') + "  \n";
	expected_frame += " " + std::string(27, 'B') + "  \n";
	expected_frame += std::string(30, ' ');
	if (!frame.empty() && frame.back() == '\n')
		expected_frame += '\n';
	EXPECT_EQ(frame, expected_frame);
	EXPECT_EQ(morph_tui_view_frame(model.label).x, 3);
	EXPECT_EQ(morph_tui_view_frame(model.label).y, 1);
}

static void tui_views_prompt_build(struct morph_tui_view_controller *controller,
				   struct morph_tui_builder *builder)
{
	auto *data = static_cast<TuiViewModel *>(controller->data);
	data->root = morph_tui_widget_stack_add_borrow(builder, nullptr,
		MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
	struct morph_tui_view_options options{};
	options.max_height = data->dialog ? 2 : 0;
	data->field = morph_tui_widget_text_field_add_borrow(builder, data->root,
		"abcdefghij", 1024, 0, &options);
	morph_tui_builder_fail(builder,
		morph_tui_widget_text_field_set_multiline(data->field, 1));
	morph_tui_builder_fail(builder,
		morph_tui_widget_text_field_set_prompt_copy(data->field, "› "));
}

TEST_F(TuiViewsTest, PromptIndentCanMatchTerminalWrappingAndExactContinuationCapacity)
{
	struct winsize terminal_size{};
	terminal_size.ws_col = 10;
	terminal_size.ws_row = 8;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &terminal_size), 0);
	static const struct morph_tui_view_controller_ops controller_ops = {
		tui_views_prompt_build, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› abcdefgh\n  ij      \n"), 0u);
	EXPECT_EQ(cursor_visible, 1);
	EXPECT_EQ(cursor_x, 4);
	EXPECT_EQ(cursor_y, 1);

	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(model.field, 0), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› abcdefgh\nij        \n"), 0u);
	EXPECT_EQ(cursor_x, 2);
	EXPECT_EQ(cursor_y, 1);

	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field,
		"abcdefgh1234567890"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› abcdefgh\n1234567890\n"), 0u);
	EXPECT_EQ(morph_tui_view_frame(model.field).height, 3);
	EXPECT_EQ(cursor_x, 0);
	EXPECT_EQ(cursor_y, 2);

	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "abc\n1234567890"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› abc     \n1234567890\n"), 0u);
	EXPECT_EQ(cursor_x, 0);
	EXPECT_EQ(cursor_y, 2);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "abc\n"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(cursor_x, 0);
	EXPECT_EQ(cursor_y, 1);

	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "abc\n1234567890"), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(model.field, 1), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› abc     \n  12345678\n  90      \n"), 0u);
	EXPECT_EQ(cursor_x, 4);
	EXPECT_EQ(cursor_y, 2);
}

TEST_F(TuiViewsTest, PromptIndentOptOutPreservesWideCharactersAndClippedCursor)
{
	model.dialog = true;
	struct winsize terminal_size{};
	terminal_size.ws_col = 10;
	terminal_size.ws_row = 8;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &terminal_size), 0);
	static const struct morph_tui_view_controller_ops controller_ops = {
		tui_views_prompt_build, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(model.field, 0), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field,
		"12345678中🙂abcd中🙂"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("中🙂abcd中\n🙂        \n"), 0u);
	EXPECT_EQ(frame.find("›"), std::string::npos);
	EXPECT_EQ(morph_tui_view_frame(model.field).height, 2);
	EXPECT_EQ(cursor_visible, 1);
	EXPECT_EQ(cursor_x, 2);
	EXPECT_EQ(cursor_y, 1);

	send("\001");
	EXPECT_EQ(frame.find("› 12345678\n中🙂abcd中\n"), 0u);
	EXPECT_EQ(cursor_x, 2);
	EXPECT_EQ(cursor_y, 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "中文🙂ab"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› 中文🙂ab\n"), 0u);
	EXPECT_EQ(cursor_x, 0);
	EXPECT_EQ(cursor_y, 1);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(model.field, "1234567中"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.find("› 1234567 \n中        \n"), 0u);
	EXPECT_EQ(cursor_x, 2);
	EXPECT_EQ(cursor_y, 1);
}

TEST_F(TuiViewsTest, RichHyperlinksAreCopiedWrappedClippedAndRemovedWithTheirText)
{
	struct winsize terminal_size{};
	terminal_size.ws_col = 10;
	terminal_size.ws_row = 8;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &terminal_size), 0);
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			struct morph_tui_view_options options{};
			options.margin = {1, 1, 1, 1};
			options.padding = {1, 1, 1, 1};
			data->label = morph_tui_widget_text_view_add_borrow(builder, nullptr,
				"", &options);
			morph_tui_widget_text_view_set_content_sizing(data->label, 1);
			char uri[] = "https://example.test/docs";
			char text[] = "linked中文";
			struct morph_tui_text_span spans[2]{};
			spans[0].text = text;
			spans[0].hyperlink = uri;
			spans[0].style.attributes = MORPH_TUI_STYLE_UNDERLINE;
			spans[1].text = " plain";
			morph_tui_builder_fail(builder,
				morph_tui_widget_text_view_set_spans_copy(data->label, spans, 2));
			uri[0] = 'X';
			text[0] = 'X';
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	const std::string open = "\033]8;;https://example.test/docs\033\\";
	const std::string close = "\033]8;;\033\\";
	size_t first = styled_frame.find(open);
	ASSERT_NE(first, std::string::npos);
	size_t second = styled_frame.find(open, first + open.size());
	ASSERT_NE(second, std::string::npos);
	EXPECT_EQ(styled_frame.find(open, second + open.size()), std::string::npos);
	EXPECT_LT(styled_frame.find(close, first), styled_frame.find('\n', first));
	EXPECT_LT(styled_frame.find(close, second), styled_frame.find('\n', second));
	EXPECT_NE(frame.find("linked"), std::string::npos);
	EXPECT_NE(frame.find("中文 p"), std::string::npos);
	EXPECT_EQ(styled_frame.find("Xttps"), std::string::npos);
	EXPECT_EQ(frame.substr(0, 10), std::string(10, ' '));
	struct morph_tui_text_span invalid{};
	invalid.text = "replacement";
	invalid.hyperlink = "https://example.test/\033]52;c;private\a";
	EXPECT_EQ(morph_tui_widget_text_view_set_spans_copy(model.label, &invalid, 1), -EINVAL);
	invalid.hyperlink = "https://example.test/\xc2\x9d";
	EXPECT_EQ(morph_tui_widget_text_view_set_spans_copy(model.label, &invalid, 1), -EINVAL);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(styled_frame.find(open), std::string::npos);
	EXPECT_EQ(styled_frame.find("private"), std::string::npos);
	ASSERT_EQ(morph_tui_widget_text_view_set_text_copy(model.label, "clear"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(styled_frame.find("\033]8;"), std::string::npos);
	EXPECT_NE(frame.find("clear"), std::string::npos);
}

TEST_F(TuiViewsTest, ImageAspectRatioUsesFixedAndMaximumWidthConstraints)
{
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *controller,
		    struct morph_tui_builder *builder) {
			auto *data = static_cast<TuiViewModel *>(controller->data);
			data->root = morph_tui_widget_stack_add_borrow(builder, nullptr,
				MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			std::vector<unsigned char> small(64u * 32u * 4u, 255u);
			struct morph_tui_media_asset *small_asset = nullptr;
			morph_tui_builder_fail(builder, morph_tui_media_asset_create_rgba_copy(
				&small_asset, small.data(), 64, 32));
			struct morph_tui_view_options fixed{};
			fixed.width = 40;
			data->field = morph_tui_widget_image_add_borrow(builder, data->root,
				small_asset, &fixed);
			morph_tui_media_asset_destroy(small_asset);
			std::vector<unsigned char> large(640u * 320u * 4u, 255u);
			struct morph_tui_media_asset *large_asset = nullptr;
			morph_tui_builder_fail(builder, morph_tui_media_asset_create_rgba_copy(
				&large_asset, large.data(), 640, 320));
			struct morph_tui_view_options maximum{};
			maximum.max_width = 20;
			data->list = morph_tui_widget_image_add_borrow(builder, data->root,
				large_asset, &maximum);
			morph_tui_media_asset_destroy(large_asset);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
	struct morph_tui_view_size size{};
	ASSERT_EQ(morph_tui_view_measure(model.field, 100, &size), 0);
	EXPECT_EQ(size.width, 40);
	EXPECT_EQ(size.height, 10);
	ASSERT_EQ(morph_tui_view_measure(model.list, 100, &size), 0);
	EXPECT_EQ(size.width, 20);
	EXPECT_EQ(size.height, 5);
	ASSERT_EQ(morph_tui_view_measure(model.list, 12, &size), 0);
	EXPECT_EQ(size.width, 12);
	EXPECT_EQ(size.height, 3);
}
