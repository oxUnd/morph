#include <gtest/gtest.h>
#include <nb/err.h>

extern "C" {
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/component/tool_call.h"
#include "sapi/cli/component/transcript.h"
#include "sapi/cli/component/notice.h"
#include "sapi/cli/component/plan.h"
#include "sapi/cli/component/recap.h"
#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/component/composer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include <nb/utf8.h>
#include <morph/tui/terminal/text.h>
#include <nothing/tui/application.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/window.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/models/text_editor.h>
#include <nothing/tui/models/list_navigation.h>
#include <nb/cleanup.h>
}

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <chrono>
#include <regex>
#include <nothing/tui/capabilities.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* NT packs a cell's reset, attributes, and colors into one SGR. Compare the
 * individual SGR operations so visual checks are independent of batching. */
static std::string retained_sgr(const std::string &frame)
{
	static const std::regex packed("\\x1b\\[0((;[0-9]+)+)m");
	static const std::regex operation("(?:38|48);5;[0-9]+|(?:38|48);2;[0-9]+;[0-9]+;[0-9]+|[0-9]+");
	std::string result;
	std::size_t start = 0;
	for (std::sregex_iterator i(frame.begin(), frame.end(), packed), end; i != end; ++i) {
		result.append(frame, start, static_cast<std::size_t>(i->position()) - start);
		result += "\033[0m";
		const std::string parameters = (*i)[1];
		for (std::sregex_iterator part(parameters.begin(), parameters.end(), operation);
		     part != end; ++part)
			result += "\033[" + part->str() + "m";
		start = static_cast<std::size_t>(i->position() + i->length());
	}
	return result.append(frame, start);
}

static std::string retained_normalized(const char *text)
{
	char *plain = morph_terminal_strip_ansi_dup(text, strlen(text), nullptr);
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
	enum Kind { Tool, Notice, Plan, Composer, Recap } kind = Tool;
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct nt_application *app = nullptr;
	struct nt_view_controller *controller = nullptr;
	struct nt_view *component = nullptr;
	struct nt_view *editor = nullptr;
	struct cli_application ctx{};
	int cursor_x = -1, cursor_y = -1, columns_rendered = 0;
	struct cli_transcript_tool tool{};
	struct cli_notice notice{};
	std::string recap_summary, recap_next;
	cJSON *plan = nullptr;
	bool details = false;
	std::string frame;
	struct nt_renderer renderer = {
		+[](struct nt_renderer *backend, struct nt_surface *surface, FILE *) {
			auto *self = static_cast<CliRetainedWidgetsTest *>(backend->data);
			struct nt_terminal_capabilities caps{};
			caps.color_profile = NT_COLOR_PROFILE_256;
			nt_surface_set_capabilities(surface, &caps);
			nt_surface_size(surface, &self->columns_rendered, nullptr);
			nb_buf rows{};
			int rc = nt_surface_rows(surface, &rows);
			if (!rc)
				self->frame = retained_sgr(nb_buf_cstr(&rows));
			(void)nt_surface_cursor(surface, &self->cursor_x, &self->cursor_y);
			nb_buf_cleanup(&rows);
			return rc;
		}, nullptr, this};
	static void Build(struct nt_view_controller *owner, struct nt_builder *builder)
	{
			auto *self = static_cast<CliRetainedWidgetsTest *>(owner->data);
			self->controller = owner;
			auto *root = nt_stack_view_add(builder, nullptr,
				NT_STACK_VIEW_VERTICAL, 0, nullptr);
			if (self->kind == Tool)
				self->component = cli_tool_call_add_borrow(builder, root,
					&self->tool, 1, self->details);
			else if (self->kind == Notice)
				self->component = cli_notice_add_borrow(builder, root,
					&self->notice, 1);
			else if (self->kind == Recap)
				self->component = cli_recap_add_borrow(builder, root,
					self->recap_summary.c_str(), self->recap_next.c_str(), 1);
			else if (self->kind == Composer) {
				self->component = cli_composer_add_borrow(builder, root,
					&self->ctx, &self->editor);
				nt_builder_fail(builder,
					nt_text_field_set_prompt(
							self->editor, "› "));
			}
			else
				self->component = cli_plan_add_borrow(builder, root, self->plan, 1);
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
		size.ws_col = 120;
		size.ws_row = 48;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct nt_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(nt_application_create(&options, &app), 0);
		ASSERT_EQ(nt_window_set_renderer(
			nt_application_window(app), &renderer), 0);
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
		nt_application_destroy(app);
		cli_composer_model_cleanup(&ctx.composer);
		cJSON_Delete(plan);
		nb_buf_cleanup(&tool.stream);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		if (master >= 0)
			close(master);
	}

	void Present()
	{
		struct nt_view_controller_class type{};
		type.superclass = &nt_view_controller_base_class;
		type.instance_size = sizeof(struct nt_view_controller);
		type.load_view = Build;
		static const struct nt_view_controller_class page_class = type;
		ASSERT_EQ(cli_application_present_page(app, &page_class, this, 0), 0);
		ASSERT_EQ(nt_application_step(app, 0), 0);
		if (editor)
			ASSERT_EQ(nt_window_focus_view(nt_application_window(app), editor), 0);
	}

	void Resize(int columns)
	{
		struct winsize size{};
		size.ws_col = static_cast<unsigned short>(columns);
		size.ws_row = 48;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (columns_rendered != columns && std::chrono::steady_clock::now() < deadline)
			ASSERT_EQ(nt_application_step(app, 1), 0);
		ASSERT_EQ(columns_rendered, columns);
	}

	void ExpectLegacy(int columns)
	{
		EXPECT_EQ(nt_view_options(component).padding.right, 2);
		EXPECT_EQ(nt_view_content_rect(component).width, columns - 2);
		nb_buf expected{};
		if (kind == Tool)
			ASSERT_EQ(cli_transcript_tool_append(&expected, &tool, columns, 1, details),
					  0);
		else if (kind == Notice)
			ASSERT_EQ(cli_notice_append(&expected, &notice, columns, 1), 0);
		else if (kind == Recap)
			ASSERT_EQ(cli_recap_append(&expected, recap_summary.c_str(), recap_next.c_str(),
				columns, 1), 0);
		else {
			const struct cli_plan_style style = {columns, 1, 1};
			ASSERT_EQ(cli_plan_append(&expected, plan, &style), 0);
		}
		EXPECT_EQ(nt_view_frame(component).y, 1);
		EXPECT_EQ(retained_normalized(frame.c_str()).substr(1),
			  retained_normalized(nb_buf_cstr(&expected)))
			<< columns;
		nb_buf_cleanup(&expected);
	}
};

TEST_F(CliRetainedWidgetsTest, RecapKeepsInlineHeadingHangingIndentAndStylesAcrossResize)
{
	kind = Recap;
	for (int n = 0; n < 5; n++)
		recap_summary += "中文🙂 recap retains completed work and preserves the draft. ";
	recap_next = "Verify the installation.";
	Present();
	for (int columns : {120, 80, 60, 120}) {
		Resize(columns);
		ExpectLegacy(columns);
		EXPECT_NE(frame.find("\033[1m"), std::string::npos);
		EXPECT_NE(frame.find("\033[38;5;245m"), std::string::npos);
		std::string plain = retained_normalized(frame.c_str());
		EXPECT_EQ(plain.find("\n  ↳ Recap: 中文🙂"), 0u);
		EXPECT_NE(plain.find("\n           "), std::string::npos);
		EXPECT_NE(plain.find("Next: Verify the installation."), std::string::npos);
	}
}

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
	EXPECT_EQ(nt_view_frame(component).height, 3);
}

TEST_F(CliRetainedWidgetsTest, LiveToolKeepsThreeTailRowsAndBranchPrefix)
{
	tool.state = 0;
	tool.ended = tool.started;
	ASSERT_EQ((nb_buf_append_cstr(&tool.stream, "discarded\none\ntwo\nthree\n") ? 0
										    : NB_ERR_NOMEM),
		  0);
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
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("Waiting for agents"),
		  std::string::npos);
	EXPECT_EQ(frame.find("Running…"), std::string::npos);
	tool.state = 1;
	tool.ended = tool.started + 1;
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
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
			ASSERT_EQ(nt_application_step(app, 0), 0);
			EXPECT_TRUE(nt_view_is_hidden(component));
			struct nt_view_size size{};
			ASSERT_EQ(nt_view_measure(component, 120, &size), 0);
			EXPECT_EQ(size.width, 0);
			EXPECT_EQ(size.height, 0);
			EXPECT_TRUE(retained_normalized(frame.c_str()).empty());
			ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 1), 0);
			ASSERT_EQ(nt_application_step(app, 0), 0);
			EXPECT_FALSE(nt_view_is_hidden(component));
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
	ASSERT_EQ(nt_text_field_set_text(editor, "draft中文"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	const std::string initial = frame;
	const int initial_x = cursor_x, initial_y = cursor_y;
	EXPECT_NE(frame.find("\033[48;5;237m"), std::string::npos);
	const struct nt_view_options options = nt_view_options(component);
	EXPECT_EQ(options.padding.top, 1);
	EXPECT_EQ(options.padding.bottom, 1);
	EXPECT_EQ(options.padding.right, 0);
	EXPECT_EQ(nt_view_frame(component).height, 3);
	cli_composer_model_set_ultra(&ctx.composer, 0, 0);
	cli_composer_model_set_ultra(&ctx.composer, 1, 1);
	for (int64_t now : {1000, 1495}) {
		ASSERT_EQ(cli_composer_update(component, now), 0);
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_NE(frame.find("\033[48;5;141m"), std::string::npos);
		EXPECT_NE(frame.find("\033[48;5;98m"), std::string::npos);
		EXPECT_EQ(retained_normalized(frame.c_str()), retained_normalized(initial.c_str()));
		EXPECT_EQ(cursor_x, initial_x);
		EXPECT_EQ(cursor_y, initial_y);
		EXPECT_STREQ(nt_text_field_text(editor), "draft中文");
	}
	ASSERT_EQ(cli_composer_update(component, 1990), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	EXPECT_FALSE(ctx.composer.mode_transition);
	EXPECT_EQ(frame, initial);
}

TEST_F(CliRetainedWidgetsTest, ComposerContinuationRowsMatchOriginalUnindentedInput)
{
	kind = Composer;
	Present();
	Resize(10);
	const int editor_y = cursor_y;
	EXPECT_EQ(nt_view_frame(editor).width, 10);
	ASSERT_EQ(nt_text_field_set_prompt_hanging_indent(editor, 0), 0);
	ASSERT_EQ(nt_text_field_set_text(editor, "12345678abcdefghijZ"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› 12345678\nabcdefghij\nZ"),
		  std::string::npos);
	EXPECT_EQ(nt_view_frame(editor).height, 3);
	EXPECT_EQ(cursor_x, 1);
	EXPECT_EQ(cursor_y, editor_y + 2);
	for (const char *text : {"12345678", "12345678\n"}) {
		ASSERT_EQ(nt_text_field_set_text(editor, text), 0);
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_EQ(nt_view_frame(editor).height, 2) << text;
		EXPECT_EQ(cursor_x, 0) << text;
		EXPECT_EQ(cursor_y, editor_y + 1) << text;
	}
	for (const char *text : {"12345678\nabcdefghij", "12345678\nabcdefghij\n"}) {
		ASSERT_EQ(nt_text_field_set_text(editor, text), 0);
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_EQ(nt_view_frame(editor).height, 3) << text;
		EXPECT_EQ(cursor_x, 0) << text;
		EXPECT_EQ(cursor_y, editor_y + 2) << text;
	}
	ASSERT_EQ(nt_text_field_set_text(editor, "123456中文abcd"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› 123456中\n文abcd"), std::string::npos);
	EXPECT_EQ(cursor_x, 6);
	EXPECT_EQ(cursor_y, editor_y + 1);
	ASSERT_EQ(nt_text_field_set_prompt_hanging_indent(editor, 1), 0);
	ASSERT_EQ(nt_text_field_set_text(editor, "12345678abcdefghij"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
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
	ASSERT_EQ(nt_text_field_set_prompt_hanging_indent(editor, 0), 0);
	ASSERT_EQ(nt_text_field_set_text(editor,
		"Compare [IMAGE#1][IMAGE#2]"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	EXPECT_NE(frame.find("\033[38;5;153m"), std::string::npos);
	EXPECT_NE(frame.find("\033[48;5;24m"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("› Compare [IMAGE#1][IMAG\nE#2]"),
		std::string::npos);
	ASSERT_EQ(write(master, "\177", 1), 1);
	ASSERT_EQ(nt_application_step(app, 100), 0);
	EXPECT_STREQ(nt_text_field_text(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(write(master, "\037", 1), 1);
	ASSERT_EQ(nt_application_step(app, 100), 0);
	EXPECT_STREQ(nt_text_field_text(editor),
		"Compare [IMAGE#1][IMAGE#2]");
	ASSERT_EQ(write(master, "\027", 1), 1);
	ASSERT_EQ(nt_application_step(app, 100), 0);
	EXPECT_STREQ(nt_text_field_text(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(nt_text_field_insert_text(editor, "[IMAGE#2]"), 0);
	ASSERT_EQ(write(master, "\033[D", 3), 3);
	ASSERT_EQ(nt_application_step(app, 100), 0);
	EXPECT_EQ(nt_text_field_cursor(editor), strlen("Compare [IMAGE#1]"));
	ASSERT_EQ(write(master, "\033[3~", 4), 4);
	ASSERT_EQ(nt_application_step(app, 100), 0);
	EXPECT_STREQ(nt_text_field_text(editor), "Compare [IMAGE#1]");
	ASSERT_EQ(nt_text_field_replace_range(editor, 0, 7, "中文"), 0);
	EXPECT_STREQ(nt_text_field_text(editor), "中文 [IMAGE#1]");
	EXPECT_EQ(nt_text_field_set_cursor(editor, 1), NT_ERR_RANGE);
}

TEST_F(CliRetainedWidgetsTest, ComposerNoColorKeepsUltraPaddingSteady)
{
	kind = Composer;
	cli_output_set_color_enabled(&ctx, 0);
	Present();
	ASSERT_EQ(nt_text_field_set_text(editor, "protected draft"), 0);
	ASSERT_EQ(nt_application_render(app), 0);
	const std::string initial = frame;
	const int initial_x = cursor_x, initial_y = cursor_y;
	cli_composer_model_set_ultra(&ctx.composer, 0, 0);
	cli_composer_model_set_ultra(&ctx.composer, 1, 1);
	for (int64_t now : {1000, 1495, 1990}) {
		ASSERT_EQ(cli_composer_update(component, now), 0);
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_EQ(frame, initial);
		EXPECT_EQ(cursor_x, initial_x);
		EXPECT_EQ(cursor_y, initial_y);
	}
	EXPECT_EQ(frame.find("\033[48;5;"), std::string::npos);
}

TEST(CliEditorStateTest, AtomicRangeUndoRestoresCompleteUtf8TextAndCursor)
{
	struct nt_text_editor_state editor NB_CLEANUP(
		nt_text_editor_state_deinit) = {};
	const char original[] = "A中[IMAGE#1]Z";
	const size_t start = strlen("A中"), end = strlen("A中[IMAGE#1]");
	ASSERT_EQ(nt_text_editor_state_init(&editor, original, 128, 0), 0);
	ASSERT_EQ(nt_text_editor_state_set_cursor(&editor, end), 0);
	ASSERT_EQ(nt_text_editor_state_replace_range(&editor, start, end, ""), 0);
	EXPECT_STREQ(editor.text.ptr, "A中Z");
	EXPECT_EQ(editor.cursor, start);
	const size_t history = editor.undo.len;
	EXPECT_EQ(nt_text_editor_state_set_cursor(&editor, 2), NT_ERR_RANGE);
	EXPECT_EQ(nt_text_editor_state_replace_range(&editor, 2, start, ""),
		  NT_ERR_RANGE);
	EXPECT_EQ(nt_text_editor_state_replace_range(&editor, start, start, "\xff"),
		  NT_ERR_FORMAT);
	const std::string too_long(128, 'x');
	EXPECT_EQ(nt_text_editor_state_replace_range(&editor, start, start,
								   too_long.c_str()),
		  NT_ERR_RANGE);
	EXPECT_STREQ(editor.text.ptr, "A中Z");
	EXPECT_EQ(editor.cursor, start);
	EXPECT_EQ(editor.undo.len, history);
	EXPECT_EQ(nt_text_editor_prompt_key(&editor, 31), NT_NAVIGATION_CHANGED);
	EXPECT_STREQ(editor.text.ptr, original);
	EXPECT_EQ(editor.cursor, end);
	EXPECT_EQ(nt_text_editor_state_undo(&editor), NT_NAVIGATION_UNHANDLED);
}

TEST(CliEditorStateTest, TypedUtf8AndSecretEditingKeepUndoOwnershipSafe)
{
	struct nt_text_editor_state editor NB_CLEANUP(
		nt_text_editor_state_deinit) = {};
	ASSERT_EQ(nt_text_editor_state_init(&editor, "A", 64, 0), 0);
	const std::string input = "中";
	for (unsigned char byte : input)
		ASSERT_EQ(nt_text_editor_prompt_key(&editor, byte),
			  NT_NAVIGATION_CHANGED);
	EXPECT_STREQ(editor.text.ptr, "A中");
	EXPECT_EQ(nt_text_editor_state_undo(&editor), NT_NAVIGATION_CHANGED);
	EXPECT_STREQ(editor.text.ptr, "A");
	EXPECT_EQ(editor.cursor, 1u);
	struct nt_text_editor_state secret NB_CLEANUP(
		nt_text_editor_state_deinit) = {};
	ASSERT_EQ(nt_text_editor_state_init(&secret, "secret", 64, 1), 0);
	ASSERT_EQ(nt_text_editor_state_replace_range(&secret, 6, 6, "x"), 0);
	ASSERT_EQ(nt_text_editor_prompt_key(&secret, 127), NT_NAVIGATION_CHANGED);
	EXPECT_EQ(secret.undo.len, 0u);
	EXPECT_EQ(nt_text_editor_state_undo(&secret), NT_NAVIGATION_UNHANDLED);
	EXPECT_STREQ(secret.text.ptr, "secret");
}
