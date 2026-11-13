#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/widgets/tool_call.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/notice.h"
#include "sapi/cli/widgets/plan.h"
#include "sapi/cli/widgets/composer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "util/utf8.h"
#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/window.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/text_field.h>
#include <morph/tui/widgets/editor.h>
#include <morph/tui/widgets/list.h>
#include "util/cleanup.h"
}

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

static std::string retained_normalized(const char *text)
{
	char *plain = utf8_strip_ansi_dup(text, strlen(text), nullptr);
	std::string value = plain ? plain : "";
	free(plain);
	std::string result;
	size_t start = 0;
	while (start < value.size()) {
		size_t end = value.find('\n', start);
		if (end == std::string::npos)
			end = value.size();
		size_t trim = end;
		while (trim > start && (value[trim - 1] == ' ' || value[trim - 1] == '\r'))
			trim--;
		result.append(value, start, trim - start);
		result.push_back('\n');
		start = end + 1;
	}
	while (!result.empty() && result.back() == '\n')
		result.pop_back();
	return result;
}

class CliRetainedWidgetsTest : public ::testing::Test
{
      protected:
	enum Kind { Tool, Notice, Plan, Composer } kind = Tool;
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct morph_tui_application *app = nullptr;
	struct morph_tui_view_controller *controller = nullptr;
	struct morph_tui_view *component = nullptr;
	struct morph_tui_view *editor = nullptr;
	struct cli_application ctx{};
	int cursor_x = -1, cursor_y = -1;
	struct cli_transcript_tool tool{};
	struct cli_notice notice{};
	cJSON *plan = nullptr;
	bool details = false;
	std::string frame;
	struct morph_tui_renderer renderer{};
	const struct morph_tui_renderer_ops renderer_ops = {
		+[](struct morph_tui_renderer *backend, struct morph_tui_surface *surface, FILE *) {
			auto *self = static_cast<CliRetainedWidgetsTest *>(backend->data);
			morph_buf_t rows{};
			int rc = morph_tui_surface_rows(surface, &rows);
			if (!rc)
				self->frame = morph_buf_cstr_borrow(&rows);
			(void)morph_tui_surface_cursor(surface, &self->cursor_x, &self->cursor_y);
			morph_buf_cleanup(&rows);
			return rc;
		}, nullptr};
	const struct morph_tui_view_controller_ops ops = {
		+[](struct morph_tui_view_controller *owner, struct morph_tui_builder *builder) {
			auto *self = static_cast<CliRetainedWidgetsTest *>(owner->data);
			self->controller = owner;
			auto *root = morph_tui_widget_stack_add_borrow(builder, nullptr,
				MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			if (self->kind == Tool)
				self->component = cli_tool_call_add_borrow(builder, root,
					&self->tool, 1, self->details);
			else if (self->kind == Notice)
				self->component = cli_notice_add_borrow(builder, root,
					&self->notice, 1);
			else if (self->kind == Composer) {
				self->component = cli_composer_add_borrow(builder, root,
					&self->ctx, &self->editor);
				morph_tui_builder_fail(builder,
					morph_tui_widget_text_field_set_prompt_copy(
							self->editor, "› "));
			}
			else
				self->component = cli_plan_add_borrow(builder, root, self->plan, 1);
		}, nullptr, nullptr, nullptr, nullptr, nullptr};

	void SetUp() override
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		ASSERT_NE(input = fopen(ptsname(master), "r+"), nullptr);
		ASSERT_NE(output = fdopen(dup(fileno(input)), "w"), nullptr);
		struct winsize size{};
		size.ws_col = 120;
		size.ws_row = 48;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct morph_tui_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
		renderer.ops = &renderer_ops;
		renderer.data = this;
		ASSERT_EQ(morph_tui_window_set_renderer_borrow(
			morph_tui_application_window_borrow(app), &renderer), 0);
		tool.id = const_cast<char *>("parity");
		tool.name = const_cast<char *>("exec");
		tool.subject = const_cast<char *>("printf '%s\\n' visual-parity");
		tool.args = const_cast<char *>("{\"command\":\"printf visual-parity\"}");
		tool.result = const_cast<char *>("{\"stdout\":\"first\\nsecond\\nthird\\n"
				"fourth\\nfifth\","
			"\"exit_code\":0}");
		tool.summary = const_cast<char *>("exit 0");
		tool.error = const_cast<char *>("");
		tool.shell = 1;
		tool.state = 1;
		tool.started = 1;
		tool.ended = 2;
	}

	void TearDown() override
	{
		morph_tui_application_destroy(app);
		cli_composer_model_cleanup(&ctx.composer);
		cJSON_Delete(plan);
		morph_buf_cleanup(&tool.stream);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		if (master >= 0)
			close(master);
	}

	void Present()
	{
		struct morph_tui_view_controller_spec spec{};
		spec.ops = &ops;
		spec.data = this;
		ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
		ASSERT_EQ(morph_tui_application_step(app, 0), 0);
		if (editor)
			ASSERT_EQ(morph_tui_view_focus(editor), 0);
	}

	void Resize(int columns)
	{
		struct winsize size{};
		size.ws_col = static_cast<unsigned short>(columns);
		size.ws_row = 48;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	}

	void ExpectLegacy(int columns)
	{
		EXPECT_EQ(morph_tui_view_get_options(component).padding.right, 2);
		EXPECT_EQ(morph_tui_view_content_rect(component).width, columns - 2);
		morph_buf_t expected{};
		if (kind == Tool)
			ASSERT_EQ(cli_transcript_tool_append(&expected, &tool, columns, 1, details),
					  0);
		else if (kind == Notice)
			ASSERT_EQ(cli_notice_append(&expected, &notice, columns, 1), 0);
		else {
			const struct cli_plan_style style = {columns, 1, 1};
			ASSERT_EQ(cli_plan_append(&expected, plan, &style), 0);
		}
		EXPECT_EQ(morph_tui_view_frame(component).y, 1);
		EXPECT_EQ(retained_normalized(frame.c_str()).substr(1),
			retained_normalized(morph_buf_cstr_borrow(&expected))) << columns;
		morph_buf_cleanup(&expected);
	}
};

TEST_F(CliRetainedWidgetsTest, ToolKeepsLegacyTreePreviewMixedStylesAndResize)
{
	Present();
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		ExpectLegacy(columns);
	}
	EXPECT_NE(frame.find("\033[1m"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;2m"), std::string::npos);
	EXPECT_NE(frame.find("\033[2m"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("Ctrl+T to expand"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, ExpandedToolKeepsFormattedPayloadAndOwnsSnapshot)
{
	details = true;
	Present();
	tool.subject = const_cast<char *>("printf source-has-changed");
	Resize(80);
	EXPECT_EQ(frame.find("source-has-changed"), std::string::npos);
	tool.subject = const_cast<char *>("printf '%s\\n' visual-parity");
	ExpectLegacy(80);
	Resize(60);
	ExpectLegacy(60);
}

TEST_F(CliRetainedWidgetsTest, NoticeKeepsWrappedBranchesAndContinuation)
{
	kind = Notice;
	notice.status = CLI_NOTICE_WARNING;
	notice.title = "A warning whose original word wrapping must remain exactly the same "
			"when resized";
	notice.detail = "A detail row with enough words to wrap over the available terminal "
		"width\nSecond row";
	notice.footer = "Original footer";
	Present();
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		ExpectLegacy(columns);
	}
}

TEST_F(CliRetainedWidgetsTest, PlanKeepsSingleLineClippingAndMarkerOnlyColors)
{
	kind = Plan;
	plan = cJSON_Parse("{\"selected_plan_id\":\"one\",\"plans\":[{\"id\":\"one\",\"steps\":["
		"{\"description\":\"A completed step with a description long enough to require "
		"truncation at narrow widths\",\"status\":\"completed\"},"
		"{\"description\":\"Running step\",\"status\":\"in_progress\"}]}]}");
	ASSERT_NE(plan, nullptr);
	Present();
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		ExpectLegacy(columns);
	}
	EXPECT_NE(frame.find("\033[38;5;2m"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;6m"), std::string::npos);
	EXPECT_EQ(morph_tui_view_frame(component).height, 3);
}

TEST_F(CliRetainedWidgetsTest, LiveToolKeepsThreeTailRowsAndBranchPrefix)
{
	tool.state = 0;
	tool.ended = tool.started;
	ASSERT_EQ(morph_buf_puts(&tool.stream, "discarded\none\ntwo\nthree\n"), 0);
	Present();
	std::string plain = retained_normalized(frame.c_str());
	EXPECT_EQ(plain.find("discarded"), std::string::npos);
	EXPECT_NE(plain.find("  └ one\n    two\n    three"), std::string::npos);
	Resize(60);
	EXPECT_EQ(retained_normalized(frame.c_str()), plain);
}

TEST_F(CliRetainedWidgetsTest, LiveAgentToolsMayOmitTheirBodyWithoutFailingLayout)
{
	tool.name = const_cast<char *>("spawn_agent");
	tool.args = const_cast<char *>("{\"task_name\":\"worker\"}");
	tool.result = const_cast<char *>("");
	tool.subject = const_cast<char *>("");
	tool.shell = 0;
	tool.state = 0;
	tool.ended = tool.started;
	Present();
	EXPECT_NE(retained_normalized(frame.c_str()).find("Spawning"), std::string::npos);
	EXPECT_EQ(frame.find("Running…"), std::string::npos);
	tool.name = const_cast<char *>("wait_agent");
	tool.args = const_cast<char *>("{\"timeout_ms\":10000}");
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("Waiting for agents"),
		  std::string::npos);
	EXPECT_EQ(frame.find("Running…"), std::string::npos);
	tool.state = 1;
	tool.ended = tool.started + 1;
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("Finished waiting"),
		  std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, AnalyticsToolsHideRunningAndCompletedRowsUntilDetailsAreRequested)
{
	tool.name = const_cast<char *>("send_message");
	tool.args = const_cast<char *>(
		"{\"target\":\"/root/worker\",\"message\":\"DETAIL_SENTINEL\"}");
	tool.result = const_cast<char *>("{\"data\":{\"agent_id\":\"/root/worker\"}}");
	tool.subject = const_cast<char *>("");
	tool.shell = 0;
	Present();
	for (const char *name :
	     {"send_message", "followup_task", "interrupt_agent", "list_agents"}) {
		tool.name = const_cast<char *>(name);
		for (int state : {0, 1}) {
			tool.state = state;
			tool.ended = tool.started + state;
			ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
			EXPECT_TRUE(morph_tui_view_hidden(component));
			struct morph_tui_view_size size{};
			ASSERT_EQ(morph_tui_view_measure(component, 120, &size), 0);
			EXPECT_EQ(size.width, 0);
			EXPECT_EQ(size.height, 0);
			EXPECT_TRUE(retained_normalized(frame.c_str()).empty());
			ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 1), 0);
			ASSERT_EQ(morph_tui_application_step(app, 0), 0);
			EXPECT_FALSE(morph_tui_view_hidden(component));
			EXPECT_NE(frame.find("DETAIL_SENTINEL"), std::string::npos);
			EXPECT_NE(frame.find("Arguments"), std::string::npos);
			EXPECT_EQ(frame.find("Running…"), std::string::npos);
		}
	}
}

TEST_F(CliRetainedWidgetsTest, ComposerUltraWavePaintsOnlyTruePaddingAndPreservesDraftCursor)
{
	kind = Composer;
	Present();
	ASSERT_NE(editor, nullptr);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, "draft中文"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	const std::string initial = frame;
	const int initial_x = cursor_x, initial_y = cursor_y;
	EXPECT_NE(frame.find("\033[48;5;237m"), std::string::npos);
	const struct morph_tui_view_options options = morph_tui_view_get_options(component);
	EXPECT_EQ(options.padding.top, 1);
	EXPECT_EQ(options.padding.bottom, 1);
	EXPECT_EQ(options.padding.right, 0);
	EXPECT_EQ(morph_tui_view_frame(component).height, 3);
	cli_composer_model_set_ultra(&ctx.composer, 0, 0);
	cli_composer_model_set_ultra(&ctx.composer, 1, 1);
	for (int64_t now : {1000, 1495}) {
		ASSERT_EQ(cli_composer_update(component, now), 0);
		ASSERT_EQ(morph_tui_application_render(app), 0);
		EXPECT_NE(frame.find("\033[48;5;141m"), std::string::npos);
		EXPECT_NE(frame.find("\033[48;5;98m"), std::string::npos);
		EXPECT_EQ(retained_normalized(frame.c_str()), retained_normalized(initial.c_str()));
		EXPECT_EQ(cursor_x, initial_x);
		EXPECT_EQ(cursor_y, initial_y);
		EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor), "draft中文");
	}
	ASSERT_EQ(cli_composer_update(component, 1990), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_FALSE(ctx.composer.mode_transition);
	EXPECT_EQ(frame, initial);
}

TEST_F(CliRetainedWidgetsTest, ComposerContinuationRowsMatchOriginalUnindentedInput)
{
	kind = Composer;
	Present();
	Resize(10);
	const int editor_y = cursor_y;
	EXPECT_EQ(morph_tui_view_frame(editor).width, 10);
	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(editor, 0), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, "12345678abcdefghijZ"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› 12345678\nabcdefghij\nZ"),
		  std::string::npos);
	EXPECT_EQ(morph_tui_view_frame(editor).height, 3);
	EXPECT_EQ(cursor_x, 1);
	EXPECT_EQ(cursor_y, editor_y + 2);
	for (const char *text : {"12345678", "12345678\n"}) {
		ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, text), 0);
		ASSERT_EQ(morph_tui_application_render(app), 0);
		EXPECT_EQ(morph_tui_view_frame(editor).height, 2) << text;
		EXPECT_EQ(cursor_x, 0) << text;
		EXPECT_EQ(cursor_y, editor_y + 1) << text;
	}
	for (const char *text : {"12345678\nabcdefghij", "12345678\nabcdefghij\n"}) {
		ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, text), 0);
		ASSERT_EQ(morph_tui_application_render(app), 0);
		EXPECT_EQ(morph_tui_view_frame(editor).height, 3) << text;
		EXPECT_EQ(cursor_x, 0) << text;
		EXPECT_EQ(cursor_y, editor_y + 2) << text;
	}
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, "123456中文abcd"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› 123456中\n文abcd"), std::string::npos);
	EXPECT_EQ(cursor_x, 6);
	EXPECT_EQ(cursor_y, editor_y + 1);
	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(editor, 1), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, "12345678abcdefghij"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› 12345678\n  abcdefgh\n  ij"),
		  std::string::npos);
	EXPECT_EQ(cursor_x, 4);
	EXPECT_EQ(cursor_y, editor_y + 2);
}

TEST_F(CliRetainedWidgetsTest, ComposerImageChipsKeepColorsWrappingAtomicEditsAndUndo)
{
	kind = Composer;
	ASSERT_EQ(cli_composer_model_init(&ctx.composer), 0);
	const char *label = nullptr;
	ASSERT_EQ(cli_composer_model_add_image_resolved(&ctx.composer, "/fixture/one.png",
		1, 1, &label), 0);
	ASSERT_EQ(cli_composer_model_add_image_resolved(&ctx.composer, "/fixture/two.png",
		1, 1, &label), 0);
	Present();
	Resize(24);
	ASSERT_EQ(morph_tui_widget_text_field_set_prompt_hanging_indent(editor, 0), 0);
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor,
		"Compare [IMAGE#1][IMAGE#2]"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	EXPECT_NE(frame.find("\033[38;5;153m"), std::string::npos);
	EXPECT_NE(frame.find("\033[48;5;24m"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› Compare [IMAGE#1][IMAG\nE#2]"),
		std::string::npos);
	ASSERT_EQ(write(master, "\177", 1), 1);
	ASSERT_EQ(morph_tui_application_step(app, 100), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(write(master, "\037", 1), 1);
	ASSERT_EQ(morph_tui_application_step(app, 100), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor),
		"Compare [IMAGE#1][IMAGE#2]");
	ASSERT_EQ(write(master, "\027", 1), 1);
	ASSERT_EQ(morph_tui_application_step(app, 100), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(morph_tui_widget_text_field_insert_text_copy(editor, "[IMAGE#2]"), 0);
	ASSERT_EQ(write(master, "\033[D", 3), 3);
	ASSERT_EQ(morph_tui_application_step(app, 100), 0);
	EXPECT_EQ(morph_tui_widget_text_field_cursor(editor), strlen("Compare [IMAGE#1]"));
	ASSERT_EQ(write(master, "\033[3~", 4), 4);
	ASSERT_EQ(morph_tui_application_step(app, 100), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(morph_tui_widget_text_field_replace_range_copy(editor, 0, 7, "中文"), 0);
	EXPECT_STREQ(morph_tui_widget_text_field_text_borrow(editor), "中文 [IMAGE#1]");
	EXPECT_EQ(morph_tui_widget_text_field_set_cursor(editor, 1), -EINVAL);
}

TEST_F(CliRetainedWidgetsTest, ComposerNoColorKeepsUltraPaddingSteady)
{
	kind = Composer;
	cli_output_set_color_enabled(&ctx, 0);
	Present();
	ASSERT_EQ(morph_tui_widget_text_field_set_text_copy(editor, "protected draft"), 0);
	ASSERT_EQ(morph_tui_application_render(app), 0);
	const std::string initial = frame;
	const int initial_x = cursor_x, initial_y = cursor_y;
	cli_composer_model_set_ultra(&ctx.composer, 0, 0);
	cli_composer_model_set_ultra(&ctx.composer, 1, 1);
	for (int64_t now : {1000, 1495, 1990}) {
		ASSERT_EQ(cli_composer_update(component, now), 0);
		ASSERT_EQ(morph_tui_application_render(app), 0);
		EXPECT_EQ(frame, initial);
		EXPECT_EQ(cursor_x, initial_x);
		EXPECT_EQ(cursor_y, initial_y);
	}
	EXPECT_EQ(frame.find("\033[48;5;"), std::string::npos);
}

TEST(CliEditorStateTest, AtomicRangeUndoRestoresCompleteUtf8TextAndCursor)
{
	struct morph_tui_widget_editor_state editor
		MORPH_CLEANUP(morph_tui_widget_editor_state_cleanup) = {};
	const char original[] = "A中[IMAGE#1]Z";
	const size_t start = strlen("A中"), end = strlen("A中[IMAGE#1]");
	ASSERT_EQ(morph_tui_widget_editor_state_init(&editor, original, 128, 0), 0);
	ASSERT_EQ(morph_tui_widget_editor_state_set_cursor(&editor, end), 0);
	ASSERT_EQ(morph_tui_widget_editor_state_replace_range_copy(&editor, start, end, ""), 0);
	EXPECT_STREQ(editor.text.data, "A中Z");
	EXPECT_EQ(editor.cursor, start);
	const size_t history = editor.undo.nelts;
	EXPECT_EQ(morph_tui_widget_editor_state_set_cursor(&editor, 2), -EINVAL);
	EXPECT_EQ(morph_tui_widget_editor_state_replace_range_copy(&editor, 2, start, ""), -EINVAL);
	EXPECT_EQ(morph_tui_widget_editor_state_replace_range_copy(&editor, start, start, "\xff"),
		  MORPH_ERR_FORMAT);
	const std::string too_long(128, 'x');
	EXPECT_EQ(morph_tui_widget_editor_state_replace_range_copy(
		&editor, start, start, too_long.c_str()), -E2BIG);
	EXPECT_STREQ(editor.text.data, "A中Z");
	EXPECT_EQ(editor.cursor, start);
	EXPECT_EQ(editor.undo.nelts, history);
	EXPECT_EQ(morph_tui_widget_editor_state_update(&editor, 31), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_STREQ(editor.text.data, original);
	EXPECT_EQ(editor.cursor, end);
	EXPECT_EQ(morph_tui_widget_editor_state_undo(&editor), MORPH_TUI_WIDGET_UNHANDLED);
}

TEST(CliEditorStateTest, TypedUtf8AndSecretEditingKeepUndoOwnershipSafe)
{
	struct morph_tui_widget_editor_state editor
		MORPH_CLEANUP(morph_tui_widget_editor_state_cleanup) = {};
	ASSERT_EQ(morph_tui_widget_editor_state_init(&editor, "A", 64, 0), 0);
	const std::string input = "中";
	for (unsigned char byte : input)
		ASSERT_EQ(morph_tui_widget_editor_state_update(&editor, byte),
			  MORPH_TUI_WIDGET_CHANGED);
	EXPECT_STREQ(editor.text.data, "A中");
	EXPECT_EQ(morph_tui_widget_editor_state_undo(&editor), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_STREQ(editor.text.data, "A");
	EXPECT_EQ(editor.cursor, 1u);
	struct morph_tui_widget_editor_state secret
		MORPH_CLEANUP(morph_tui_widget_editor_state_cleanup) = {};
	ASSERT_EQ(morph_tui_widget_editor_state_init(&secret, "secret", 64, 1), 0);
	ASSERT_EQ(morph_tui_widget_editor_state_replace_range_copy(&secret, 6, 6, "x"), 0);
	ASSERT_EQ(morph_tui_widget_editor_state_update(&secret, 127), MORPH_TUI_WIDGET_CHANGED);
	EXPECT_EQ(secret.undo.nelts, 0u);
	EXPECT_EQ(morph_tui_widget_editor_state_undo(&secret), MORPH_TUI_WIDGET_UNHANDLED);
	EXPECT_STREQ(secret.text.data, "secret");
}
