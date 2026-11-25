#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/adapters/input/focus.h"
#include "sapi/cli/application.h"
#include <nb/cleanup.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/drop_down.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/shortcut.h>
#include <nothing/tui/views/split_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/window.h>
}

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

class CliInputFocusTest : public ::testing::Test {
protected:
	struct nt_application *app = nullptr;
	struct nt_view *root = nullptr, *buttons[3]{}, *inputs[4]{}, *groups[4]{};
	struct nt_view *document = nullptr, *menu = nullptr, *modal = nullptr;
	struct nt_view *modal_button = nullptr, *modal_input = nullptr;
	FILE *input = nullptr, *output = nullptr;
	int master = -1, shortcut_count = 0;
	bool split = false, nested = false, extra_left = false, rich = false;
	bool intercept_shortcut = false;
	struct nt_renderer renderer = {
		+[](struct nt_renderer *, struct nt_surface *, FILE *) { return 0; },
		nullptr, nullptr};

	static int Capture(struct nt_view *view, const struct nt_event *event)
	{
		auto *self = static_cast<CliInputFocusTest *>(nt_view_controller(view)->data);
		if (self->intercept_shortcut && event->type == NT_EVENT_KEY &&
		    event->action != NT_KEY_ACTION_RELEASE && !event->modifiers &&
		    event->key == 'r' && !nt_view_is_focused(self->inputs[0])) {
			self->shortcut_count++;
			return 1;
		}

		return 0;
	}

	static void AddInput(struct nt_builder *builder, struct nt_view *parent,
		CliInputFocusTest *self, size_t index)
	{
		self->groups[index] = nt_column_add(builder, parent, 0, nullptr);
		if (self->rich && index == 1) {
			self->inputs[index] = nt_text_view_add(builder, self->groups[index], "",
				&nt_view_options_for_rich);
			nt_builder_fail(builder, nt_text_view_set_editable(self->inputs[index], 1));
		} else {
			struct nt_text_field_options field{};
			field.limit = 4096;
			self->inputs[index] = nt_text_field_add(builder, self->groups[index],
				&field, nullptr);
			nt_builder_fail(builder, nt_text_field_set_multiline(self->inputs[index], 1));
		}
	}

	static constexpr struct nt_view_options nt_view_options_for_rich = [] {
		struct nt_view_options options{};
		options.height = 3;
		return options;
	}();

	static void Build(struct nt_view_controller *controller, struct nt_builder *builder)
	{
		auto *self = static_cast<CliInputFocusTest *>(controller->data);
		static const struct nt_view_class type = [] {
			struct nt_view_class value{};
			value.superclass = &nt_stack_view_class;
			value.instance_size = sizeof(struct nt_stack_view);
			value.kind = "cli_input_focus_test_root";
			value.capture_event = Capture;
			return value;
		}();
		self->root = nt_view_add(builder, nullptr, &type, nullptr);
		struct nt_view_options fill{};
		fill.width = fill.height = NT_FILL;
		auto *workspace = self->split
			? nt_split_view_add(builder, self->root, NT_SPLIT_VIEW_HORIZONTAL, 50, &fill)
			: self->root;
		struct nt_view *panes[3]{};
		panes[0] = nt_column_add(builder, workspace, 0, &fill);
		if (self->split) {
			auto *right = nt_column_add(builder, workspace, 0, &fill);
			if (self->nested) {
				auto *inner = nt_split_view_add(builder, right,
					NT_SPLIT_VIEW_VERTICAL, 50, &fill);
				panes[1] = nt_column_add(builder, inner, 0, &fill);
				panes[2] = nt_column_add(builder, inner, 0, &fill);
			} else
				panes[1] = right;
		}

		for (size_t i = 0; i < 3; i++) {
			if (!panes[i])
				continue;

			self->buttons[i] = nt_button_add(builder, panes[i], "Focus panel", 0, nullptr);
			AddInput(builder, panes[i], self, i);
		}

		if (self->extra_left)
			AddInput(builder, panes[0], self, 3);

		struct nt_view_options body{};
		body.height = 3;
		self->document = nt_text_view_add(builder, panes[0], "Document content",
			&body);
		static const char *const items[] = {"First", "Second"};
		struct nt_drop_down_options choices{};
		choices.items = items;
		choices.count = 2;
		self->menu = nt_drop_down_add(builder, panes[0], &choices, nullptr);
		auto *hidden = nt_column_add(builder, panes[0], 0, nullptr);
		struct nt_text_field_options field{};
		nt_text_field_add(builder, hidden, &field, nullptr);
		nt_builder_fail(builder, nt_view_set_hidden(hidden, 1));

		struct nt_view_options overlay = fill;
		overlay.position = NT_VIEW_POSITION_ABSOLUTE;
		overlay.z_index = 10;
		self->modal = nt_column_add(builder, self->root, 0, &overlay);
		self->modal_button = nt_button_add(builder, self->modal, "Modal button", 0, nullptr);
		self->modal_input = nt_text_field_add(builder, self->modal, &field, nullptr);
		nt_builder_fail(builder, nt_view_set_hidden(self->modal, 1));
	}

	void SetUp() override
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		input = fopen(ptsname(master), "r+");
		ASSERT_NE(input, nullptr);
		output = fdopen(dup(fileno(input)), "w");
		ASSERT_NE(output, nullptr);
		struct winsize size{};
		size.ws_col = 120;
		size.ws_row = 30;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct nt_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(nt_application_create(&options, &app), 0);
		ASSERT_EQ(nt_window_set_renderer(nt_application_window(app), &renderer), 0);
		ASSERT_EQ(cli_input_focus_install(app), 0);
	}

	void TearDown() override
	{
		nt_application_destroy(app);
		if (output)
			fclose(output);

		if (input)
			fclose(input);

		if (master >= 0)
			close(master);
	}

	void Present()
	{
		static const struct nt_view_controller_class type = [] {
			struct nt_view_controller_class value{};
			value.superclass = &nt_view_controller_base_class;
			value.instance_size = sizeof(struct nt_view_controller);
			value.load_view = Build;
			return value;
		}();
		struct nt_view_controller *page NB_CLEANUP(cli_application_controller_cleanup) = nullptr;
		ASSERT_EQ(nt_view_controller_create(&type, &page), 0);
		page->data = this;
		ASSERT_EQ(nt_view_controller_set_tick_interval(page, 5), 0);
		ASSERT_EQ(nt_application_present_view_controller(app, &page), 0);
		ASSERT_EQ(nt_application_step(app, 0), 0);
	}

	void Focus(struct nt_view *view)
	{
		ASSERT_EQ(nt_view_focus(view), 0);
		ASSERT_EQ(nt_application_step(app, 0), 0);
	}

	void Type(const std::string &text)
	{
		ASSERT_EQ(write(master, text.data(), text.size()), static_cast<ssize_t>(text.size()));
		for (int i = 0; i < 10; i++)
			ASSERT_EQ(nt_application_step(app, 1), 0);
	}
};

TEST_F(CliInputFocusTest, UniqueEditorReceivesTheFirstCharacterUtf8AndPaste)
{
	Present();
	Focus(buttons[0]);
	Type("Z中文🙂");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "Z中文🙂");
	EXPECT_EQ(nt_window_focused_view(nt_application_window(app)), inputs[0]);
	Focus(document);
	Type("\033[200~ pasted\n文本\033[201~");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "Z中文🙂 pasted\n文本");
}

TEST_F(CliInputFocusTest, MultipleEditorsStayInTheFocusedSplitPane)
{
	split = true;
	Present();
	Focus(buttons[1]);
	Type("RIGHT");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	EXPECT_STREQ(nt_text_field_text(inputs[1]), "RIGHT");
	Focus(buttons[0]);
	Type("LEFT");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "LEFT");
	EXPECT_STREQ(nt_text_field_text(inputs[1]), "RIGHT");
}

TEST_F(CliInputFocusTest, NestedSplitsChooseTheInnermostPane)
{
	split = nested = true;
	Present();
	Focus(buttons[2]);
	Type("INNER");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	EXPECT_STREQ(nt_text_field_text(inputs[1]), "");
	EXPECT_STREQ(nt_text_field_text(inputs[2]), "INNER");
}

TEST_F(CliInputFocusTest, HiddenDisabledAndReadOnlyEditorsDoNotBecomeTargets)
{
	split = extra_left = true;
	Present();
	ASSERT_EQ(nt_view_set_hidden(groups[3], 1), 0);
	ASSERT_EQ(nt_view_set_read_only(inputs[1], 1), 0);
	Focus(buttons[1]);
	Type("SOLE");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "SOLE");
	EXPECT_STREQ(nt_text_field_text(inputs[1]), "");
	ASSERT_EQ(nt_view_set_hidden(groups[3], 0), 0);
	ASSERT_EQ(nt_view_set_enabled(groups[3], 0), 0);
	Focus(buttons[1]);
	Type("!");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "SOLE!");
	EXPECT_STREQ(nt_text_field_text(inputs[3]), "");
}

TEST_F(CliInputFocusTest, AmbiguousInputsInOnePaneKeepTheirExplicitFocus)
{
	split = extra_left = true;
	Present();
	Focus(buttons[0]);
	Type("Z");
	EXPECT_EQ(nt_window_focused_view(nt_application_window(app)), buttons[0]);
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	EXPECT_STREQ(nt_text_field_text(inputs[3]), "");
}

TEST_F(CliInputFocusTest, EditableDocumentCanBeThePaneInput)
{
	split = rich = true;
	Present();
	Focus(buttons[1]);
	Type("富文本");
	EXPECT_STREQ(nt_text_view_text(inputs[1]), "富文本");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
}

TEST_F(CliInputFocusTest, NativeSearchPopupAndShortcutsConsumeTheirOwnInput)
{
	intercept_shortcut = true;
	Present();
	Focus(buttons[0]);
	Type("r");
	EXPECT_EQ(shortcut_count, 1);
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	Type("\033[122;1:3u");
	EXPECT_EQ(nt_window_focused_view(nt_application_window(app)), buttons[0]);
	Focus(document);
	struct nt_event find{};
	find.type = NT_EVENT_KEY;
	find.key = 'f';
	find.modifiers = nt_shortcut_primary_modifier();
	ASSERT_GE(nt_view_class_methods(document)->handle_event(document, &find), 0);
	Type("Document");
	EXPECT_EQ(nt_window_focused_view(nt_application_window(app)), document);
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	Type("\033");
	Focus(menu);
	ASSERT_EQ(nt_drop_down_open(menu), 0);
	Type("Z");
	EXPECT_TRUE(nt_drop_down_is_open(menu));
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	Type("\033");
	Type("Z");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "Z");
}

TEST_F(CliInputFocusTest, ModalScopeCannotSendTextToTheUnderlyingPage)
{
	Present();
	ASSERT_EQ(nt_view_set_hidden(modal, 0), 0);
	ASSERT_EQ(nt_view_set_focus_scope(modal, 1), 0);
	Focus(modal_button);
	Type("MODAL");
	EXPECT_STREQ(nt_text_field_text(modal_input), "MODAL");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	ASSERT_EQ(nt_view_set_enabled(modal_input, 0), 0);
	Focus(modal_button);
	Type("Z");
	EXPECT_STREQ(nt_text_field_text(inputs[0]), "");
	EXPECT_EQ(nt_window_focused_view(nt_application_window(app)), modal_button);
}
