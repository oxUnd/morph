#include <gtest/gtest.h>
#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/layout.h>
#include <morph/tui/window.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_field.h>
#include "util/utf8.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

struct TuiViewModel {
	struct morph_tui_view *root = nullptr;
	struct morph_tui_view *field = nullptr;
	struct morph_tui_view *list = nullptr;
	struct morph_tui_view *label = nullptr;
	struct morph_tui_view_controller *controller = nullptr;
	int actions = 0, closes = 0, widget_closes = 0, result = 0;
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
	model->root = morph_tui_widget_stack_create(builder, nullptr,
						    MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
	if (model->editor)
		model->field = morph_tui_widget_text_field_create(builder, model->root, "", 32, 1,
								  nullptr);
	const char *items[] = {"Alpha", "Beta", "Gamma"};
	model->list = morph_tui_widget_list_create(builder, model->root, items, 3, 2, &fill);
	model->label = morph_tui_widget_label_create(builder, model->root, "Ready", nullptr);
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
	model->selected = action->text;
	int rc = morph_tui_widget_label_set_text(model->label, action->text);
	if (rc || !model->dialog || action->type == MORPH_TUI_WIDGET_ACTION_CHANGED)
		return rc;
	static const struct morph_tui_view_controller_ops ops = {
		+[](struct morph_tui_view_controller *, struct morph_tui_builder *builder) {
			morph_tui_widget_label_create(builder, nullptr, "Dialog", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &ops;
	return morph_tui_view_controller_present(morph_tui_view_controller_application(controller),
						 &spec);
}

class TuiViewsTest : public ::testing::Test
{
      protected:
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct morph_tui_application *app = nullptr;
	TuiViewModel model;
	std::string frame;
	int frames = 0;
	const struct morph_tui_view_controller_ops ops = {
		tui_views_build, tui_views_action, nullptr, nullptr, nullptr, tui_views_close};
	const struct morph_tui_renderer_ops backend_ops = {
		+[](struct morph_tui_renderer *backend, struct morph_tui_surface *surface, FILE *) {
			auto *self = static_cast<TuiViewsTest *>(backend->data);
			morph_buf_t rows{};
			int rc = morph_buf_init(&rows, BUFSIZ);
			if (!rc)
				rc = morph_tui_surface_rows(surface, &rows);
			char *plain =
				rc ? nullptr : utf8_strip_ansi_dup(rows.data, rows.len, nullptr);
			if (plain)
				self->frame = plain;
			else if (!rc)
				rc = -ENOMEM;
			free(plain);
			morph_buf_cleanup(&rows);
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
		ASSERT_EQ(
			morph_tui_window_set_renderer(morph_tui_application_window(app), &backend),
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
		int rc = morph_tui_view_controller_present(app, &spec);
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
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.field);
	int before = frames;
	ASSERT_EQ(morph_tui_widget_label_set_text(model.label, "Updated"), 0);
	ASSERT_EQ(morph_tui_widget_label_set_text(model.label, "Latest"), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before + 1);
	EXPECT_NE(frame.find("Latest"), std::string::npos);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frames, before + 1);
}

TEST_F(TuiViewsTest, PrintableQuitKeyBelongsToEditorAndUnicodeArrivesWhole)
{
	ASSERT_EQ(present(), 0);
	send("q你好e\xcc\x81");
	until([&] {
		return std::string(morph_tui_widget_text_field_text(model.field)) ==
		       "q你好e\xcc\x81";
	});
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
	EXPECT_EQ(model.actions, 5);
}

TEST_F(TuiViewsTest, PasteCannotNavigateSubmitOrInvokePageShortcuts)
{
	ASSERT_EQ(present(), 0);
	send("\033[200~q\tjk\r\n\033[201~");
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text(model.field)), "qjk");
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.field);
	send("\t");
	send("\033[200~qj\r\033[201~");
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 0U);
	EXPECT_EQ(morph_tui_application_depth(app), 1U);
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
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.list);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 1U);
	send("\033[Z");
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.field);
}

TEST_F(TuiViewsTest, HiddenFocusAndTinyTerminalHaveBoundedLayout)
{
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_view_set_hidden(model.field, 1), 0);
	send("j");
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.list);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 1U);
	struct winsize size{};
	size.ws_col = 1;
	size.ws_row = 1;
	ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_LE(morph_tui_view_frame(model.list).height, 1);
	EXPECT_GE(morph_tui_view_frame(model.label).height, 0);
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
	ASSERT_EQ(morph_tui_widget_list_set_items(model.list, items, 1), 0);
	text[0] = 'X';
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("Borrowed"), std::string::npos);
	const char *invalid[] = {"first", nullptr};
	EXPECT_EQ(morph_tui_widget_list_set_items(model.list, invalid, 2), -EINVAL);
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
		}};
	static const struct morph_tui_view_controller_ops controller = {
		+[](struct morph_tui_view_controller *page, struct morph_tui_builder *builder) {
			auto *widget = reinterpret_cast<TuiDerivedWidget *>(morph_tui_widget_create(
				builder, nullptr, sizeof(TuiDerivedWidget), &custom, nullptr));
			if (widget)
				widget->destroyed = static_cast<int *>(page->data);
			morph_tui_builder_fail(builder, -EIO);
			morph_tui_widget_label_create(builder, nullptr, "Never created", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller;
	spec.data = &model.widget_closes;
	EXPECT_EQ(morph_tui_view_controller_present(app, &spec), -EIO);
	EXPECT_EQ(model.widget_closes, 1);
}

TEST(TuiCanvasTest, ClipsWideCellsCombiningMarksAndUntrustedControls)
{
	struct morph_tui_surface *surface = nullptr;
	ASSERT_EQ(morph_tui_surface_create(&surface), 0);
	ASSERT_EQ(morph_tui_surface_resize(surface, 8, 2), 0);
	struct morph_tui_canvas root{}, child{};
	morph_tui_canvas_root(surface, &root);
	morph_tui_canvas_child(&root, 1, 0, 4, 1, &child);
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
	morph_tui_canvas_root(surface, &canvas);
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
			return morph_tui_application_post(base, 77, nullptr, nullptr);
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
			EXPECT_EQ(morph_tui_application_window(base), nullptr);
		},
		+[](struct morph_tui_application *base, int result) {
			auto *derived = reinterpret_cast<DerivedApplication *>(base);
			derived->preparations++;
			EXPECT_EQ(result, 0);
			auto *runtime = base;
			EXPECT_NE(runtime, nullptr);
			EXPECT_EQ(morph_tui_application_post(runtime, 1, nullptr, nullptr),
				  -ECANCELED);
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
	ASSERT_EQ(morph_tui_view_controller_present(&application.base, &spec), 0);
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
				morph_tui_view_child(view, 0),
				{2, 1, frame.width > 2 ? frame.width - 2 : 0, 1});
		}};
	static const struct morph_tui_view_controller_ops controller_ops = {
		+[](struct morph_tui_view_controller *, struct morph_tui_builder *builder) {
			DerivedContainer *container = nullptr;
			container = reinterpret_cast<DerivedContainer *>(morph_tui_widget_create(
				builder, nullptr, sizeof(*container), nullptr, nullptr));
			if (!container)
				return;
			container->layout.ops = &layout_ops;
			morph_tui_view_set_layout(&container->base.view, &container->layout);
			morph_tui_widget_label_create(builder, &container->base.view,
						      "Custom layout", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	ASSERT_EQ(morph_tui_view_controller_present(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(frame.substr(31, 15), "  Custom layout");
}

TEST_F(TuiViewsTest, WorkerCannotMutateViewsAndMessagesRemainTheUpdateBoundary)
{
	ASSERT_EQ(present(), 0);
	int label_rc = 0, focus_rc = 0, list_rc = 0;
	std::thread worker([&] {
		label_rc = morph_tui_widget_label_set_text(model.label, "Unsafe");
		focus_rc = morph_tui_view_focus(model.list);
		list_rc = morph_tui_widget_list_select(model.list, 2);
	});
	worker.join();
	EXPECT_EQ(label_rc, -EPERM);
	EXPECT_EQ(focus_rc, -EPERM);
	EXPECT_EQ(list_rc, -EPERM);
	EXPECT_EQ(morph_tui_widget_list_selected(model.list), 0U);
}

TEST_F(TuiViewsTest, BoundActionCanReplaceSenderItemsWithoutInvalidatingItsPayload)
{
	model.editor = false;
	ASSERT_EQ(present(), 0);
	ASSERT_EQ(morph_tui_widget_set_action(
			  model.list,
			  +[](void *data, const struct morph_tui_widget_action *action) {
				  auto *model = static_cast<TuiViewModel *>(data);
				  const char *replacement[] = {"Replacement"};
				  int rc = morph_tui_widget_list_set_items(action->sender,
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
	ASSERT_EQ(morph_tui_widget_set_action(
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
			model->root = morph_tui_widget_stack_create(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_HORIZONTAL, 0, nullptr);
			struct morph_tui_view_options first{};
			first.grow = 1;
			first.max_width = 4;
			struct morph_tui_view_options rest{};
			rest.grow = 1;
			model->field =
				morph_tui_widget_label_create(builder, model->root, "one", &first);
			model->list =
				morph_tui_widget_label_create(builder, model->root, "two", &rest);
			for (int i = 0; i < 6; i++)
				morph_tui_widget_label_create(builder, model->root, "", nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_view_child_count(model.root), 8U);
	EXPECT_EQ(morph_tui_view_frame(model.field).width, 4);
	EXPECT_EQ(morph_tui_view_frame(model.list).width, 26);
}

TEST_F(TuiViewsTest, WindowReceivesOnlyInputDeclinedByViewsAndController)
{
	ASSERT_EQ(present(), 0);
	int received = 0;
	ASSERT_EQ(morph_tui_window_set_event_handler(
			  morph_tui_application_window(app),
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
			model->root = morph_tui_widget_stack_create(
				builder, nullptr, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			auto *container = morph_tui_widget_stack_create(
				builder, model->root, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			model->list = morph_tui_widget_text_field_create(builder, model->root,
									 "second", 32, 0, nullptr);
			model->field = morph_tui_widget_text_field_create(builder, container,
									  "first", 32, 0, nullptr);
		},
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr};
	struct morph_tui_view_controller_spec spec{};
	spec.ops = &controller_ops;
	spec.data = &model;
	ASSERT_EQ(morph_tui_view_controller_present(app, &spec), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.field);
	send("\t");
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.list);
}

TEST_F(TuiViewsTest, TextFieldSetterCopiesAndPreservesValueOnValidationFailure)
{
	ASSERT_EQ(present(), 0);
	char value[] = "updated";
	ASSERT_EQ(morph_tui_widget_text_field_set_text(model.field, value), 0);
	value[0] = 'X';
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text(model.field)), "updated");
	EXPECT_EQ(morph_tui_widget_text_field_set_text(model.field, std::string(32, 'a').c_str()),
		  -E2BIG);
	EXPECT_EQ(morph_tui_widget_text_field_set_text(model.field, nullptr), -EINVAL);
	EXPECT_EQ(std::string(morph_tui_widget_text_field_text(model.field)), "updated");
	EXPECT_EQ(morph_tui_window_focus(morph_tui_application_window(app)), model.field);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(frame.find("updated"), std::string::npos);
}
