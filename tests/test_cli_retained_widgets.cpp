#include <gtest/gtest.h>
#include <nb/err.h>

extern "C" {
#include "sapi/cli/components/message.h"
#include "sapi/cli/components/file_browser.h"
#include "sapi/cli/components/workspace_sidebar.h"
#include "sapi/cli/components/diff_browser.h"
#include "sapi/cli/presentation/diff.h"
#include "sapi/cli/components/rich_document.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/presentation/messages.h"
#include <morph_markdown_highlight.h>
#include "sapi/cli/components/plan.h"
#include "sapi/cli/components/notice.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/components/tool_call.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/transcript.h"
#include "sapi/cli/presentation/notice.h"
#include "sapi/cli/presentation/plan.h"
#include "sapi/cli/components/recap.h"
#include "sapi/cli/presentation/recap.h"
#include "sapi/cli/components/composer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include <nb/utf8.h>
#include "sapi/cli/presentation/terminal_text.h"
#include <nothing/tui/application.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/window.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/controls/tab_bar.h>
#include <nothing/tui/models/text_editor.h>
#include <nothing/tui/models/list_navigation.h>
#include <nb/cleanup.h>
}

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include <regex>
#include <nothing/tui/capabilities.h>
#include <sys/ioctl.h>
#include <termios.h>
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
	char *plain = cli_terminal_text_strip_ansi_dup(text, strlen(text), nullptr);
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
	enum Kind { Tool, Notice, Plan, Composer, Recap, Feed, Files, Sidebar, RichDocument,
		Unstable } kind = Tool;
	struct cli_files_model files{};
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct termios original_modes{};
	struct nt_application *app = nullptr;
	struct nt_view_controller *controller = nullptr;
	struct nt_view *component = nullptr;
	struct nt_view *editor = nullptr;
	struct cli_application ctx{};
	int cursor_x = -1, cursor_y = -1, columns_rendered = 0;
	int rendered_frames = 0;
	int layout_passes = 0, layout_retries = 0, display_retries = 0;
	struct cli_transcript_tool tool{};
	struct cli_notice notice{};
	std::string recap_summary, recap_next;
	cJSON *plan = nullptr;
	bool details = false;
	bool native_footer = false;
	std::string frame;
	std::string file_directory;

	static struct nt_view *FindKind(struct nt_view *view, const char *kind)
	{
		if (!strcmp(nt_view_kind(view), kind))
			return view;

		for (size_t i = 0; i < nt_view_child_count(view); i++) {
			if (auto *found = FindKind(nt_view_child(view, i), kind))
				return found;
		}

		return nullptr;
	}

	void OpenFile(const char *name, const char *source)
	{
		if (file_directory.empty()) {
			char directory[] = "/tmp/morph-file-widget-XXXXXX";
			ASSERT_NE(mkdtemp(directory), nullptr);
			file_directory = directory;
		}

		const std::string path = file_directory + "/" + name;
		std::ofstream(path) << source;
		ASSERT_EQ(cli_files_add(&files, "/workspace", path.c_str(), CLI_FILES_MODIFIED), 0);
		ASSERT_EQ(cli_file_browser_open(component, path.c_str(), 0), 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		do {
			ASSERT_EQ(cli_file_browser_sync(component), 0);
			ASSERT_EQ(nt_application_step(app, 1), 0);
			auto *document = FindKind(component, "text_view");
			if (document && strcmp(nt_text_view_text(document), "Loading file preview..."))
				return;
		} while (std::chrono::steady_clock::now() < deadline);

		FAIL() << "file preview did not finish";
	}
	struct nt_renderer renderer = {
		+[](struct nt_renderer *backend, struct nt_surface *surface, FILE *) {
			auto *self = static_cast<CliRetainedWidgetsTest *>(backend->data);
			self->rendered_frames++;
			/* Keep the regression finite even without the framework guard. */
			if (self->kind == Unstable && self->rendered_frames > 128)
				nt_application_stop(self->app, NB_ERR_CANCELLED);

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
			if (self->kind == Unstable) {
				static const nt_view_class type = [] {
					nt_view_class value{};
					value.superclass = &nt_view_base_class;
					value.instance_size = sizeof(struct nt_view);
					value.layout = +[](struct nt_view *view) {
						auto *self = static_cast<CliRetainedWidgetsTest *>(
							nt_view_controller(view)->data);
						if (self->layout_passes++ < self->layout_retries)
							nt_view_invalidate(view, 1);

						return 0;
					};
					value.draw = +[](struct nt_view *view, struct nt_canvas *) {
						auto *self = static_cast<CliRetainedWidgetsTest *>(
							nt_view_controller(view)->data);
						if (self->rendered_frames < self->display_retries)
							nt_view_invalidate(view, 0);

						return 0;
					};
					return value;
				}();
				self->component = nt_view_add(builder, nullptr, &type, nullptr);
				return;
			}

			if (self->kind == Feed) {
				auto *scroll = nt_scroll_view_add(builder, nullptr, nullptr);
				struct nt_view_options fill{};
				fill.width = NT_FILL;
				auto *feed = nt_column_add(builder, scroll, 0, &fill);
				for (auto kind : {CLI_MESSAGE_USER, CLI_MESSAGE_REASONING,
						  CLI_MESSAGE_ASSISTANT})
					cli_message_add_borrow(builder, feed, kind, "Short text", 1);

				self->component = cli_tool_call_add_borrow(builder, feed,
					&self->tool, 1, 0);
				return;
			}

			auto *root = nt_stack_view_add(builder, nullptr,
				NT_STACK_VIEW_VERTICAL, 0, nullptr);
			if (self->kind == Tool)
				self->component = cli_tool_call_add_borrow(builder, root,
					&self->tool, 1, self->details);
			else if (self->kind == Files)
				self->component = cli_file_browser_add(builder, root, &self->files,
					"/workspace", 1);
			else if (self->kind == Sidebar)
				self->component = cli_workspace_sidebar_add(builder, root, &self->files,
					"/workspace", 1);
			else if (self->kind == RichDocument)
				self->component = cli_rich_document_add_borrow(builder, root);
			else if (self->kind == Notice)
				self->component = cli_notice_add_borrow(builder, root,
					&self->notice, 1);
			else if (self->kind == Recap)
				self->component = cli_recap_add_borrow(builder, root,
					self->recap_summary.c_str(), self->recap_next.c_str(), 1);
			else if (self->kind == Composer) {
				self->component = cli_composer_add_borrow(
					builder, root, &self->ctx.composer,
					cli_output_color_enabled(&self->ctx), &self->editor);
				nt_builder_fail(builder,
					nt_text_field_set_prompt(
							self->editor, "› "));
			}
			else
				self->component = cli_plan_add_borrow(builder, root, self->plan, 1);

			if (self->native_footer && self->component) {
				auto *report = reinterpret_cast<struct cli_report *>(self->component);
				report->footer = nt_label_add(builder, self->component,
					"NATIVE_REPORT_FOOTER", nullptr);
			}
		}

	void SetUp() override
	{
		cli_files_init(&files);
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		ASSERT_NE(input = fopen(ptsname(master), "r+"), nullptr);
		ASSERT_NE(output = fdopen(dup(fileno(input)), "w"), nullptr);
		ASSERT_EQ(tcgetattr(fileno(input), &original_modes), 0);
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
		if (!file_directory.empty())
			std::filesystem::remove_all(file_directory);

		cli_files_cleanup(&files);
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

	void Present(int expected = 0)
	{
		struct nt_view_controller_class type{};
		type.superclass = &nt_view_controller_base_class;
		type.instance_size = sizeof(struct nt_view_controller);
		type.load_view = Build;
		static const struct nt_view_controller_class page_class = type;
		struct nt_view_controller *page NB_CLEANUP(cli_application_controller_cleanup) =
			nullptr;
		ASSERT_EQ(nt_view_controller_create(&page_class, &page), 0);
		page->data = this;
		ASSERT_EQ(nt_application_present_view_controller(app, &page), 0);
		ASSERT_EQ(nt_application_step(app, 0), expected);
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

	void ExpectTerminalRestored()
	{
		nt_application_destroy(app);
		app = nullptr;
		struct termios modes{};
		ASSERT_EQ(tcgetattr(fileno(input), &modes), 0);
		EXPECT_EQ(modes.c_iflag, original_modes.c_iflag);
		EXPECT_EQ(modes.c_oflag, original_modes.c_oflag);
		EXPECT_EQ(modes.c_cflag, original_modes.c_cflag);
		/* BSD may mark queued input for retyping after a mode switch. */
		tcflag_t local_mask = ~tcflag_t{0};
#ifdef PENDIN
		local_mask &= ~static_cast<tcflag_t>(PENDIN);
#endif
		EXPECT_EQ(modes.c_lflag & local_mask, original_modes.c_lflag & local_mask);
		EXPECT_EQ(memcmp(modes.c_cc, original_modes.c_cc, sizeof(modes.c_cc)), 0);
	}

	void Click(struct nt_view *view)
	{
		const std::string before = nt_label_text(view);
		const struct nt_view_rect rect = nt_view_frame(view);
		char input[128];
		int length = snprintf(input, sizeof(input), "\033[<0;%d;%dM\033[<0;%d;%dm",
			rect.x + 1, rect.y + 1, rect.x + 1, rect.y + 1);
		ASSERT_EQ(write(master, input, static_cast<size_t>(length)), length);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (before == nt_label_text(view) && std::chrono::steady_clock::now() < deadline)
			ASSERT_EQ(nt_application_step(app, 1), 0);

		ASSERT_NE(before, nt_label_text(view));
		ASSERT_EQ(nt_application_step(app, 0), 0);
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
		std::string expected_text = retained_normalized(nb_buf_cstr(&expected));
		if (kind == Tool && !details) {
			const std::string hint = "(Ctrl+T to expand)";
			size_t offset = expected_text.find(hint);
			if (offset != std::string::npos)
				expected_text.replace(offset, hint.size(), "· show more");
		}

		EXPECT_EQ(retained_normalized(frame.c_str()).substr(1),
			  expected_text)
			<< columns;
		nb_buf_cleanup(&expected);
	}
};

TEST_F(CliRetainedWidgetsTest, NonconvergingLayoutReturnsAnErrorInsteadOfSpinning)
{
	kind = Unstable;
	layout_retries = 256;
	Present(NB_ERR_STATE);
	EXPECT_GT(layout_passes, 1);
	EXPECT_LT(rendered_frames, 128);
	ExpectTerminalRestored();
}

TEST_F(CliRetainedWidgetsTest, NonconvergingDisplayReturnsAnErrorInsteadOfSpinning)
{
	kind = Unstable;
	display_retries = 256;
	Present(NB_ERR_STATE);
	EXPECT_GT(rendered_frames, 1);
	EXPECT_LT(rendered_frames, 128);
	ExpectTerminalRestored();
}

TEST_F(CliRetainedWidgetsTest, FiniteLayoutUpdatesSettleAndLeaveTheApplicationIdle)
{
	kind = Unstable;
	layout_retries = 3;
	display_retries = 5;
	Present();
	EXPECT_GT(layout_passes, layout_retries);
	EXPECT_GT(rendered_frames, display_retries);
	const int published = rendered_frames;
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_EQ(rendered_frames, published);
}

TEST_F(CliRetainedWidgetsTest, NativeReportFooterKeepsItsOwnMeasurementAcrossWidths)
{
	kind = Notice;
	native_footer = true;
	notice.title = "Report body";
	Present();
	auto *report = reinterpret_cast<struct cli_report *>(component);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		EXPECT_NE(frame.find("NATIVE_REPORT_FOOTER"), std::string::npos);
		const auto footer = nt_view_frame(report->footer);
		const auto content = nt_view_content_rect(component);
		EXPECT_EQ(footer.height, 1);
		EXPECT_EQ(footer.y, content.y + content.height - 1);
	}
}

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
	EXPECT_NE(retained_normalized(frame.c_str()).find("+ 2 lines · show more"),
		std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, ToolOutputExpandsInPlaceAndKeepsItsStateAcrossUpdatesAndResize)
{
	Present();
	auto *footer = nt_view_child(component, 1);
	ASSERT_NE(footer, nullptr);
	auto *toggle = nt_view_child(footer, 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		std::string plain = retained_normalized(frame.c_str());
		EXPECT_NE(plain.find("first\n    second\n    third\n    fourth\n    fifth"),
			std::string::npos);
		EXPECT_NE(plain.find("fifth\n    show less"), std::string::npos);
		EXPECT_EQ(plain.find("Arguments"), std::string::npos);
		EXPECT_EQ(plain.find("show more"), std::string::npos);
	}

	tool.result = const_cast<char *>("{\"stdout\":\"first\\nsecond\\nthird\\nUPDATED_LAST\"}");
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(frame.find("UPDATED_LAST"), std::string::npos);
	EXPECT_NE(frame.find("show less"), std::string::npos);
	Click(toggle);
	EXPECT_EQ(frame.find("UPDATED_LAST"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("+ 1 line · show more"),
		std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, ToolOutputExpandsClippedLinesAndHidesToggleForShortOutput)
{
	const std::string line = "LONG_BEGIN" + std::string(180, 'x') + "中文🙂LONG_END";
	std::string result = "{\"stdout\":\"" + line + "\"}";
	tool.result = result.data();
	Present();
	auto *footer = nt_view_child(component, 1);
	ASSERT_NE(footer, nullptr);
	auto *toggle = nt_view_child(footer, 1);
	ASSERT_NE(toggle, nullptr);
	EXPECT_EQ(frame.find("LONG_END"), std::string::npos);
	EXPECT_NE(frame.find("show more"), std::string::npos);
	Click(toggle);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		std::string plain = retained_normalized(frame.c_str());
		EXPECT_NE(plain.find("LONG_END"), std::string::npos);
		EXPECT_NE(plain.find("show less"), std::string::npos);
		EXPECT_EQ(plain.find("…"), std::string::npos);
	}

	tool.result = const_cast<char *>("{\"stdout\":\"short\"}");
	ASSERT_EQ(cli_tool_call_update(component, &tool, 0, 0), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(frame.find("short"), std::string::npos);
	EXPECT_TRUE(nt_view_is_hidden(footer));
	EXPECT_EQ(frame.find("show more"), std::string::npos);
	EXPECT_EQ(frame.find("show less"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, MeasuringToolAtOtherWidthsPreservesPublishedFooterAndIdleFrame)
{
	const std::string result = "{\"stdout\":\"" + std::string(100, 'x') + "\"}";
	tool.result = const_cast<char *>(result.c_str());
	Present();
	auto *footer = nt_view_child(component, 1);
	ASSERT_NE(footer, nullptr);
	EXPECT_TRUE(nt_view_is_hidden(footer));

	for (int columns : {120, 80, 60, 120}) {
		Resize(columns);
		const bool hidden = nt_view_is_hidden(footer);
		EXPECT_EQ(hidden, columns >= 106);
		const int published = rendered_frames;
		for (int measured_columns : {60, 200, 80, 120}) {
			struct nt_view_size size{};
			ASSERT_EQ(nt_view_measure(component, measured_columns, &size), 0);
			EXPECT_EQ(size.height, measured_columns < 106 ? 4 : 3);
			EXPECT_EQ(nt_view_is_hidden(footer), hidden);
			ASSERT_EQ(nt_application_render(app), 0);
			EXPECT_EQ(rendered_frames, published);
		}

		/* A fresh layout must restore its own width after speculative measures. */
		nt_application_invalidate(app, NT_INVALIDATE_LAYOUT);
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_EQ(nt_view_is_hidden(footer), hidden);
		ASSERT_EQ(nt_application_step(app, 0), 0);
		const int settled = rendered_frames;
		ASSERT_EQ(nt_application_render(app), 0);
		EXPECT_EQ(rendered_frames, settled);
	}
}

TEST_F(CliRetainedWidgetsTest, LiveToolOutputExpandsItsStreamAndKeepsFollowingUpdates)
{
	tool.state = 0;
	tool.ended = tool.started;
	ASSERT_TRUE(nb_buf_append_cstr(&tool.stream, "FIRST_STREAM\none\ntwo\nthree\n"));
	Present();
	EXPECT_EQ(frame.find("FIRST_STREAM"), std::string::npos);
	EXPECT_NE(frame.find("show more"), std::string::npos);
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	EXPECT_NE(frame.find("FIRST_STREAM"), std::string::npos);
	ASSERT_TRUE(nb_buf_append_cstr(&tool.stream, "NEW_STREAM\n"));
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(frame.find("FIRST_STREAM"), std::string::npos);
	EXPECT_NE(frame.find("NEW_STREAM"), std::string::npos);
	EXPECT_NE(frame.find("show less"), std::string::npos);
	Click(toggle);
	EXPECT_EQ(frame.find("FIRST_STREAM"), std::string::npos);
	EXPECT_NE(frame.find("NEW_STREAM"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, InlineShellOutputIncludesBothStreamsAndFailureStyle)
{
	tool.state = -1;
	tool.result = const_cast<char *>("{\"stdout\":\"out-one\\nout-two\\nout-three\","
		"\"stderr\":\"ERROR_DETAIL\"}");
	Present();
	EXPECT_NE(frame.find("out-one"), std::string::npos);
	EXPECT_EQ(frame.find("ERROR_DETAIL"), std::string::npos);
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	EXPECT_NE(frame.find("out-one"), std::string::npos);
	EXPECT_NE(frame.find("ERROR_DETAIL"), std::string::npos);
	EXPECT_NE(frame.find("show less"), std::string::npos);
	EXPECT_NE(frame.find("\033[1m"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;1m"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, PatchWithoutPreviewShowsStoredChangesAndExpandsEveryFile)
{
	tool.name = const_cast<char *>("apply_patch");
	tool.shell = 0;
	tool.subject = const_cast<char *>("example.c");
	tool.summary = const_cast<char *>("+2 -1");
	tool.patch_preview = const_cast<char *>("");
	tool.args = const_cast<char *>("{\"input\":\"*** Begin Patch\\n"
		"*** Update File: example.c\\n@@\\n context before\\n-old line\\n+new line\\n"
		" context after\\n*** Add File: extra.txt\\n+extra line\\n*** End Patch\"}");
	tool.result = const_cast<char *>("{\"data\":{\"changed\":true,\"files\":[]}}");
	tool.historical = 1;
	Present();
	EXPECT_NE(frame.find("-old line"), std::string::npos);
	EXPECT_NE(frame.find("+new line"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("    ?       -old line"),
		std::string::npos);
	EXPECT_NE(frame.find("show more"), std::string::npos);
	EXPECT_EQ(frame.find("extra line"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;1m"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;2m"), std::string::npos);
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		std::string plain = retained_normalized(frame.c_str());
		EXPECT_NE(plain.find("*** Update File: example.c"), std::string::npos);
		EXPECT_NE(plain.find("context before"), std::string::npos);
		EXPECT_NE(plain.find("-old line"), std::string::npos);
		EXPECT_NE(plain.find("+new line"), std::string::npos);
		EXPECT_NE(plain.find("*** Add File: extra.txt"), std::string::npos);
		EXPECT_NE(plain.find("+extra line"), std::string::npos);
		EXPECT_NE(plain.find("show less"), std::string::npos);
		EXPECT_EQ(plain.find("*** Begin Patch"), std::string::npos);
		EXPECT_EQ(plain.find("*** End Patch"), std::string::npos);
	}

	Click(toggle);
	EXPECT_EQ(frame.find("extra line"), std::string::npos);
	EXPECT_NE(frame.find("show more"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, PatchPrefersCapturedDiffAndShowsChangedLinesBeforeContext)
{
	tool.name = const_cast<char *>("apply_patch");
	tool.shell = 0;
	tool.args = const_cast<char *>("{\"input\":\"-RAW_OLD\\n+RAW_NEW\"}");
	tool.patch_preview = const_cast<char *>("*** Update File: example.c\n@@ -1,4 +1,4 @@\n"
		" context one\n context two\n-CAPTURED_OLD\n+CAPTURED_NEW\n context end\n");
	tool.result = const_cast<char *>("{\"data\":{\"changed\":true}}");
	Present();
	EXPECT_NE(frame.find("-CAPTURED_OLD"), std::string::npos);
	EXPECT_NE(frame.find("+CAPTURED_NEW"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("    3       -CAPTURED_OLD"),
		std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("          3 +CAPTURED_NEW"),
		std::string::npos);
	EXPECT_EQ(frame.find("RAW_OLD"), std::string::npos);
	EXPECT_EQ(frame.find("context one"), std::string::npos);
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	EXPECT_NE(frame.find("context one"), std::string::npos);
	EXPECT_NE(frame.find("@@ -1,4 +1,4 @@"), std::string::npos);
	EXPECT_EQ(frame.find("RAW_NEW"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, PatchNumbersFollowOldAndNewRangesAcrossHunksAndWrappedRows)
{
	tool.name = const_cast<char *>("apply_patch");
	tool.shell = 0;
	const std::string long_line = "-WRAPPED_OLD_" + std::string(95, 'x') + "_END\n";
	std::string diff = "*** Update File: first.c\n@@ -40,2 +70,3 @@\n context\n" + long_line +
		"+FIRST_NEW\n+SECOND_NEW\n@@ -90,1 +121,1 @@\n-NEXT_OLD\n+NEXT_NEW\n"
		"*** Add File: second.c\n@@ -0,0 +1,1 @@\n+ADDED_FILE\n";
	tool.patch_preview = diff.data();
	tool.result = const_cast<char *>("{\"data\":{\"changed\":true}}");
	Present();
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		const std::string plain = retained_normalized(frame.c_str());
		EXPECT_NE(plain.find("   40    70  context"), std::string::npos);
		EXPECT_NE(plain.find("   41       -WRAPPED_OLD_"), std::string::npos);
		EXPECT_NE(plain.find("         71 +FIRST_NEW"), std::string::npos);
		EXPECT_NE(plain.find("         72 +SECOND_NEW"), std::string::npos);
		EXPECT_NE(plain.find("   90       -NEXT_OLD"), std::string::npos);
		EXPECT_NE(plain.find("        121 +NEXT_NEW"), std::string::npos);
		EXPECT_NE(plain.find("          1 +ADDED_FILE"), std::string::npos);
	}
}

TEST_F(CliRetainedWidgetsTest, ShortPatchKeepsCollapseLinkAndFailureKeepsItsError)
{
	tool.name = const_cast<char *>("apply_patch");
	tool.shell = 0;
	tool.patch_preview = const_cast<char *>("");
	tool.args = const_cast<char *>("{\"input\":\"*** Begin Patch\\n"
		"*** Add File: extra.txt\\n+ONLY_CHANGE\\n*** End Patch\"}");
	tool.result = const_cast<char *>("{\"data\":{\"changed\":true}}");
	Present();
	EXPECT_NE(frame.find("+ONLY_CHANGE"), std::string::npos);
	EXPECT_NE(frame.find("show more"), std::string::npos);
	auto *toggle = nt_view_child(nt_view_child(component, 1), 1);
	ASSERT_NE(toggle, nullptr);
	Click(toggle);
	EXPECT_NE(frame.find("*** Add File: extra.txt"), std::string::npos);
	EXPECT_NE(frame.find("show less"), std::string::npos);
	Click(toggle);
	tool.state = -1;
	tool.error = const_cast<char *>("Permission denied");
	ASSERT_EQ(cli_tool_call_update(component, &tool, 1, 0), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(frame.find("Permission denied"), std::string::npos);
	EXPECT_EQ(frame.find("ONLY_CHANGE"), std::string::npos);
	EXPECT_EQ(frame.find("show more"), std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, ScrollingReusesTheFormattedFeedAtTheViewportWidth)
{
	kind = Feed;
	Present();
	auto *scroll = nt_view_controller_root_view(controller);
	auto *feed = nt_view_child(scroll, 0);
	ASSERT_NE(feed, nullptr);
	ASSERT_EQ(nt_view_arrange(scroll, {0, 0, 80, 4}), 0);
	for (size_t i = 0; i < nt_view_child_count(feed); i++) {
		struct nt_view_size size{};
		ASSERT_EQ(nt_view_measure(nt_view_child(feed, i), 80, &size), 0);
		EXPECT_EQ(size.width, 80);
	}

	/* Moving the viewport must succeed without calling the tool formatter again. */
	auto *report = reinterpret_cast<struct cli_report *>(component);
	report->format = [](struct cli_report *, nb_buf *, int) { return int(NB_ERR_STATE); };
	for (int offset : {1, 2, 1, 0}) {
		ASSERT_EQ(nt_scroll_view_set_offset(scroll, offset), 0);
		EXPECT_EQ(nt_scroll_view_offset(scroll), offset);
	}
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

TEST_F(CliRetainedWidgetsTest, FileSidebarRendersMarkdownAndSourceAcrossWidths)
{
	kind = Files;
	Present();
	OpenFile("readme.md",
		"# Preview heading\n\nSome **document content** and [source](../main.c:2).\n\n"
		"```c\nint value = 42;\n```\n");
	EXPECT_NE(retained_normalized(frame.c_str()).find("Preview heading"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("readme.md"), std::string::npos);
	auto *document = FindKind(component, "text_view");
	auto *viewer = FindKind(component, "cli_file_viewer");
	ASSERT_NE(document, nullptr);
	ASSERT_NE(viewer, nullptr);
	EXPECT_EQ(std::string(nt_text_view_text(document)).find("```c"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;6m"), std::string::npos);
	for (int columns : {80, 60, 120}) {
		Resize(columns);
		EXPECT_NE(retained_normalized(frame.c_str()).find("document content"),
			std::string::npos);
	}

	struct nt_event toggle{};
	toggle.type = NT_EVENT_KEY;
	toggle.key = 'm';
	ASSERT_EQ(nt_view_class_methods(viewer)->handle_event(viewer, &toggle), 1);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(std::string(nt_text_view_text(document)).find("# Preview heading"),
		std::string::npos);
	EXPECT_NE(std::string(nt_text_view_text(document)).find("```c"), std::string::npos);
	ASSERT_EQ(nt_view_class_methods(viewer)->handle_event(viewer, &toggle), 1);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_EQ(std::string(nt_text_view_text(document)).find("```c"), std::string::npos);
	EXPECT_NE(frame.find("\033[38;5;6m"), std::string::npos);
	ASSERT_EQ(cli_file_browser_focus(component), 0);
	ASSERT_EQ(nt_window_focused_view(nt_application_window(app)), document);
	const char find[] = "\006";
	ASSERT_EQ(write(master, find, sizeof(find) - 1), sizeof(find) - 1);
	for (int i = 0; i < 5; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	ASSERT_NE(retained_normalized(frame.c_str()).find("Find:"), std::string::npos)
		<< retained_normalized(frame.c_str());
	ASSERT_EQ(write(master, "document", 8), 8);
	for (int i = 0; i < 5; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	EXPECT_NE(retained_normalized(frame.c_str()).find("Find: document"), std::string::npos)
		<< retained_normalized(frame.c_str());
}

TEST_F(CliRetainedWidgetsTest, DiffSidebarDefaultsToSessionAndRetainsItsNativeDocument)
{
	kind = Sidebar;
	struct cli_transcript_tool patch{};
	patch.id = const_cast<char *>("session-diff");
	patch.name = const_cast<char *>("apply_patch");
	patch.summary = const_cast<char *>("completed");
	patch.state = 1;
	patch.result = const_cast<char *>(R"({"ok":true,"data":{"files":[]},"ui":{"patch_diff":
		"*** Update File: source.c\n@@ -12,1 +14,1 @@\n-old value\n+NEW_VALUE\n@@ -40,1 +42,1 @@\n-old second\n+NEW_SECOND\n"}})");
	ASSERT_EQ(cli_files_tool(&files, "/workspace", &patch), 0);
	Present();
	auto *tabs = FindKind(component, "tab_bar");
	ASSERT_NE(tabs, nullptr);
	ASSERT_GE(nt_tab_bar_select(tabs, 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("Session patches (1)"), std::string::npos);
	auto *browser = FindKind(component, "cli_diff_browser");
	ASSERT_NE(browser, nullptr);
	ASSERT_EQ(cli_diff_browser_focus(browser), 0);
	ASSERT_EQ(write(master, "\r", 1), 1);
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	auto *document = FindKind(browser, "text_view");
	ASSERT_NE(document, nullptr);
	std::string text = nt_text_view_text(document);
	EXPECT_NE(text.find("   12       - old value"), std::string::npos);
	EXPECT_NE(text.find("         14 + NEW_VALUE"), std::string::npos);
	ASSERT_EQ(write(master, "n", 1), 1);
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	EXPECT_EQ(nt_text_view_cursor(document), text.find("@@ -12"));
	ASSERT_EQ(write(master, "n", 1), 1);
	for (int i = 0; i < 3; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	const size_t cursor = nt_text_view_cursor(document);
	EXPECT_EQ(cursor, text.find("@@ -40"));
	ASSERT_GE(nt_tab_bar_select(tabs, 0), 0);
	ASSERT_GE(nt_tab_bar_select(tabs, 1), 0);
	EXPECT_EQ(nt_text_view_cursor(document), cursor);
	for (int columns : {120, 80, 60}) {
		Resize(columns);
		EXPECT_EQ(std::string(nt_text_view_text(document)), text);
		EXPECT_EQ(nt_text_view_cursor(document), cursor);
	}

	ASSERT_EQ(write(master, "\006NEW", 4), 4);
	for (int i = 0; i < 4; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	EXPECT_NE(retained_normalized(frame.c_str()).find("Find: NEW"), std::string::npos);
	ASSERT_EQ(write(master, "\033", 1), 1);
	for (int i = 0; i < 4; i++)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	EXPECT_EQ(retained_normalized(frame.c_str()).find("Find: NEW"), std::string::npos);
	EXPECT_EQ(std::string(nt_text_view_text(document)), text);
	nb_vec hunks NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&hunks, sizeof(size_t));
	ASSERT_EQ(cli_diff_present(document, "@@ -1 +1 @@\n-removed\n+added\n", &hunks, 0), 0);
	EXPECT_EQ(hunks.len, 1u);
	EXPECT_EQ(std::string(nt_text_view_text(document)).find("\033"), std::string::npos);
	auto *sources = FindKind(browser, "tab_bar");
	ASSERT_NE(sources, nullptr);
	ASSERT_GE(nt_tab_bar_select(sources, 1), 0);
	ASSERT_EQ(nt_view_set_hidden(component, 1), 0);
	cli_workspace_sidebar_reset(component);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_EQ(nt_tab_bar_selected(tabs), 0u);
	EXPECT_EQ(nt_tab_bar_selected(sources), 0u);
}

TEST_F(CliRetainedWidgetsTest, FileSidebarCodePreservesWhitespaceSelectionAndNoColor)
{
	kind = Files;
	Present();
	const char *source = "\n\n/* 中文 */\n\tint value = 42;\n"
		"\t\treturn value;\n  \t/* mixed */\n"
		"int\tfield;\n/* 中 */\tint aligned;\n    /* spaces */  \n\n";
	OpenFile("main.c", source);
	auto *document = FindKind(component, "text_view");
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(nt_text_view_text(document), source);
	EXPECT_NE(frame.find("\033[38;5;6m"), std::string::npos);
	ASSERT_EQ(nt_text_view_set_cursor(document, 2), 0);
	for (int columns : {80, 60, 120}) {
		Resize(columns);
		EXPECT_STREQ(nt_text_view_text(document), source);
		EXPECT_EQ(nt_text_view_cursor(document), 2u);
		const std::string visible = retained_normalized(frame.c_str());
		for (const char *line : {"        int value = 42;",
			"                return value;", "        /* mixed */",
			"int     field;", "/* 中 */        int aligned;",
			"    /* spaces */"})
			EXPECT_NE(visible.find(line), std::string::npos) << visible;
	}

	char *highlighted = nullptr;
	ASSERT_EQ(morph_md_highlight_ansi("c", source, strlen(source), &highlighted, nullptr), 0);
	ASSERT_EQ(cli_messages_set_source_view(document, highlighted, 0), 0);
	free(highlighted);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_STREQ(nt_text_view_text(document), source);
	EXPECT_EQ(frame.find("\033[38;5;6m"), std::string::npos);
	EXPECT_NE(retained_normalized(frame.c_str()).find("        int value = 42;"),
		std::string::npos);
}

TEST_F(CliRetainedWidgetsTest, FileSidebarTabSelectionUsesSourceOffsetsAfterHorizontalScroll)
{
	kind = Files;
	Present();
	const std::string source = "\tint first;\n\t\tint second;\n\t" +
		std::string(160, 'x') + "\tEND\n";
	OpenFile("tabs.c", source.c_str());
	auto *document = FindKind(component, "text_view");
	ASSERT_NE(document, nullptr);
	const auto *methods = nt_view_class_methods(document);
	struct nt_view_rect content = nt_view_content_rect(document);
	struct nt_event mouse{};
	mouse.type = NT_EVENT_MOUSE;
	mouse.mouse.button = NT_MOUSE_LEFT;
	mouse.mouse.action = NT_MOUSE_PRESS;
	mouse.mouse.x = content.x + 4;
	mouse.mouse.y = content.y;
	ASSERT_EQ(methods->handle_event(document, &mouse), 1);
	EXPECT_EQ(nt_text_view_cursor(document), 0u);
	mouse.mouse.action = NT_MOUSE_MOVE;
	mouse.mouse.x = content.x + 11;
	ASSERT_EQ(methods->handle_event(document, &mouse), 1);
	size_t start = 0, end = 0;
	ASSERT_EQ(nt_text_view_selection_range(document, &start, &end), 1);
	EXPECT_EQ(source.substr(start, end - start), "\tint");
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find("        int first;"),
		std::string::npos);
	mouse.mouse.action = NT_MOUSE_RELEASE;
	ASSERT_EQ(methods->handle_event(document, &mouse), 1);

	struct nt_event scroll{};
	scroll.type = NT_EVENT_SCROLL;
	scroll.mouse.delta_x = 5;
	ASSERT_EQ(methods->handle_event(document, &scroll), 1);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	const std::string scrolled_line = "\n" + std::string(
		static_cast<size_t>(content.x + 3), ' ') + "int first;\n";
	EXPECT_NE(retained_normalized(frame.c_str()).find(scrolled_line),
		std::string::npos);
	mouse.mouse.action = NT_MOUSE_PRESS;
	mouse.mouse.x = content.x + 3;
	ASSERT_EQ(methods->handle_event(document, &mouse), 1);
	EXPECT_EQ(nt_text_view_cursor(document), 1u);
	mouse.mouse.action = NT_MOUSE_MOVE;
	mouse.mouse.x = content.x + 6;
	ASSERT_EQ(methods->handle_event(document, &mouse), 1);
	ASSERT_EQ(nt_text_view_selection_range(document, &start, &end), 1);
	EXPECT_EQ(source.substr(start, end - start), "int");
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_NE(retained_normalized(frame.c_str()).find(scrolled_line),
		std::string::npos);
	EXPECT_STREQ(nt_text_view_text(document), source.c_str());
}

TEST(CliSourceTextLayoutTest, TabsUseDisplayColumnsAndKeepOriginalLineOffsets)
{
	const char *source = "\tA\t中\te\u0301\tZ\n\t\tend\n\n";
	struct nt_text_layout layout NB_CLEANUP(nt_text_layout_deinit) = {};
	nt_text_layout_init(&layout);
	ASSERT_EQ(nt_text_layout_build(&layout, source, 120, NT_TEXT_WRAP_NONE), 0);
	ASSERT_EQ(layout.lines.len, 4u);
	auto *first = static_cast<nt_text_line *>(nb_vec_at(&layout.lines, 0));
	auto *second = static_cast<nt_text_line *>(nb_vec_at(&layout.lines, 1));
	EXPECT_EQ(first->width, 33);
	EXPECT_EQ(second->width, 19);
	EXPECT_EQ(first->length, strlen("\tA\t中\te\u0301\tZ"));
	EXPECT_EQ(second->offset, first->length + 1);
	EXPECT_EQ(second->source_line, 2u);
	EXPECT_STREQ(layout.text, source);
	ASSERT_EQ(nt_text_layout_build(&layout, source, 16, NT_TEXT_WRAP_CHAR), 0);
	const int widths[] = {16, 16, 1, 16, 3, 0, 0};
	ASSERT_EQ(layout.lines.len, sizeof(widths) / sizeof(widths[0]));
	for (size_t i = 0; i < layout.lines.len; i++) {
		auto *line = static_cast<nt_text_line *>(nb_vec_at(&layout.lines, i));
		EXPECT_EQ(line->width, widths[i]);
	}

	EXPECT_STREQ(layout.text, source);
}

TEST_F(CliRetainedWidgetsTest, RichDocumentLinksUseTrimmedDisplayOffsets)
{
	kind = RichDocument;
	Present();
	ASSERT_EQ(cli_rich_document_set_rendered_copy(component,
		"\n\n\033]8;;/workspace/main.c:12\033\\File link\033]8;;\033\\\n", 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	std::string opened;
	ASSERT_NE(nt_view_on(component, NT_VIEW_EVENT_OPEN,
		+[](void *data, const struct nt_view_event *event) -> int {
			*static_cast<std::string *>(data) = event->text ? event->text : "";
			return NT_VIEW_EVENT_HANDLED;
		}, &opened), 0u);
	auto *text = nt_view_child(component, 0);
	const struct nt_view_rect rect = nt_view_frame(text);
	char mouse[128];
	int length = snprintf(mouse, sizeof(mouse), "\033[<0;%d;%dM\033[<0;%d;%dm",
		rect.x + 3, rect.y + 1, rect.x + 3, rect.y + 1);
	ASSERT_EQ(write(master, mouse, static_cast<size_t>(length)), length);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (opened.empty() && std::chrono::steady_clock::now() < deadline)
		ASSERT_EQ(nt_application_step(app, 10), 0);

	EXPECT_EQ(opened, "/workspace/main.c:12");
}
