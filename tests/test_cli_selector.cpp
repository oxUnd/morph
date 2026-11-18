#include "runtime_test_support.hpp"

extern "C" {
#include "sapi/cli/application.h"
#include "sapi/cli/components/selector.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/services/event_queue.h"
#include <nb/utf8.h>
#include <nothing/tui/canvas.h>
#include <nothing/tui/capabilities.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/window.h>
}

#include <cstdio>

class CliSelectorTest : public RuntimeFacadeTest
{
      protected:
	cli_application ctx{};
	cli_command_job job{};
	cli_chat_page *chat = nullptr;
	FILE *input = nullptr;
	FILE *output = nullptr;
	size_t applied = SIZE_MAX;

	static void Apply(cli_application *app, size_t selected)
	{
		*static_cast<size_t *>(app->event_user_data) = selected;
	}

	void SetUp() override
	{
		RuntimeFacadeTest::SetUp();
		Open();
		ctx.runtime = instance;
		ctx.running = 1;
		ctx.event_user_data = &applied;
		ASSERT_EQ(cli_composer_model_init(&ctx.composer), 0);
		ASSERT_EQ(cli_command_job_init(&job), 0);
		ASSERT_EQ(
			cli_event_queue_init(&ctx, [](cli_application *,
						      const cli_event_queue_item *) { return 0; }),
			0);
		ASSERT_EQ(cli_chat_create(&ctx, &job, &chat), 0);
		input = tmpfile();
		output = tmpfile();
		ASSERT_NE(input, nullptr);
		ASSERT_NE(output, nullptr);
		nt_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(nt_application_init(&ctx.base, &options), 0);
		ASSERT_EQ(cli_chat_present(chat), 0);
		ASSERT_EQ(nt_application_step(&ctx.base, 0), 0);
	}

	void TearDown() override
	{
		if (cli_chat_panel_active(&ctx))
			cli_chat_panel_close(&ctx, NB_ERR_CANCELLED);
		nt_application_deinit(&ctx.base);
		cli_chat_destroy(chat, 0);
		cli_command_job_cleanup(&job);
		cli_composer_model_cleanup(&ctx.composer);
		cli_event_queue_cleanup(&ctx);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		RuntimeFacadeTest::TearDown();
	}

	nt_view *List()
	{
		return nt_window_focused_view(nt_application_window(&ctx.base));
	}

	std::string Frame(int width)
	{
		nt_surface *surface = nullptr;
		nb_buf rows{};
		auto *root = nt_window_root_view(nt_application_window(&ctx.base));
		EXPECT_EQ(nt_surface_create(&surface), 0);
		nt_terminal_capabilities caps{};
		caps.color_profile = NT_COLOR_PROFILE_256;
		nt_surface_set_capabilities(surface, &caps);
		EXPECT_EQ(nt_surface_resize(surface, width, 32), 0);
		EXPECT_EQ(nt_view_render(root, surface), 0);
		EXPECT_EQ(nt_surface_rows(surface, &rows), 0);
		std::string result(nb_buf_cstr(&rows), rows.len);
		nt_surface_destroy(surface);
		nb_buf_destroy(&rows);
		return result;
	}

	static std::string Plain(const std::string &s)
	{
		std::string out(s.size() + 1, '\0');
		out.resize(nt_terminal_strip_ansi(out.data(), s.data(), s.size()));
		return out;
	}
};

TEST_F(CliSelectorTest, NavigationChangesDescriptionAndAppliesOnlyOnActivation)
{
	const cli_selector_choice choices[] = {{"First", "First description"},
					       {"Second", "New description"}};
	char subtitle[] = "Subtitle", detail[] = "Detail";
	ASSERT_EQ(cli_selector_show(&ctx, "Pick one", subtitle, detail, choices, 2, 0, Apply), 0);
	subtitle[0] = detail[0] = 'X';
	std::string first = Plain(Frame(72));
	EXPECT_NE(first.find("Subtitle"), std::string::npos);
	EXPECT_NE(first.find("Detail"), std::string::npos);
	EXPECT_NE(first.find("> 1. First (current)"), std::string::npos);
	EXPECT_NE(first.find("First description"), std::string::npos);
	ASSERT_NE(List(), nullptr);
	ASSERT_EQ(nt_list_view_select(List(), 1), 0);
	std::string next = Plain(Frame(72));
	EXPECT_NE(next.find("> 2. Second"), std::string::npos);
	EXPECT_NE(next.find("New description"), std::string::npos);
	EXPECT_EQ(next.find("First description"), std::string::npos);
	EXPECT_EQ(applied, SIZE_MAX);
	EXPECT_GE(nt_view_emit(List(), NT_VIEW_EVENT_ACTIVATE, 1, nullptr), 0);
	EXPECT_EQ(applied, 1u);
	EXPECT_FALSE(cli_chat_panel_active(&ctx));
}

TEST_F(CliSelectorTest, ClipsUnicodeAndRemovesControlSequences)
{
	const cli_selector_choice choices[] = {{"中文🙂abc\033]0;ROW_INJECTION\a", "details"}};
	ASSERT_EQ(cli_selector_show(&ctx, "Pick", "safe\033]0;UNTRUSTED_TITLE\a text", nullptr,
				    choices, 1, -1, Apply),
		  0);
	const std::string wide = Frame(48);
	EXPECT_NE(Plain(wide).find("safe text"), std::string::npos);
	EXPECT_EQ(wide.find("UNTRUSTED_TITLE"), std::string::npos);
	EXPECT_EQ(wide.find("ROW_INJECTION"), std::string::npos);
	EXPECT_NE(wide.find("48;5;4"), std::string::npos);
	const std::string narrow = Plain(Frame(8));
	EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr(narrow.c_str()), nullptr), NB_UTF8_OK);
	EXPECT_NE(narrow.find("> 1. 中"), std::string::npos);
	EXPECT_EQ(narrow.find("中文🙂abc"), std::string::npos);
}

TEST_F(CliSelectorTest, PlainModeAndCancellationLeaveSelectionUnapplied)
{
	const cli_selector_choice choices[] = {{"One", "Single choice"}};
	cli_output_set_color_enabled(&ctx, 0);
	EXPECT_EQ(cli_selector_show(&ctx, "Pick", nullptr, nullptr, choices, 1, 1, Apply),
		  NB_ERR_INVAL);
	ASSERT_EQ(cli_selector_show(&ctx, "Pick", nullptr, nullptr, choices, 1, 0, Apply), 0);
	EXPECT_EQ(cli_selector_show(&ctx, "Other", nullptr, nullptr, choices, 1, 0, Apply),
		  NB_ERR_BUSY);
	const std::string frame = Frame(42);
	EXPECT_NE(Plain(frame).find("> 1. One (current)"), std::string::npos);
	EXPECT_EQ(frame.find("48;5;4"), std::string::npos);
	EXPECT_GE(cli_chat_panel_close(&ctx, NB_ERR_CANCELLED), 0);
	EXPECT_FALSE(cli_chat_panel_active(&ctx));
	EXPECT_EQ(applied, SIZE_MAX);
}
