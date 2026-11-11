#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/widgets/composer_panel.h"
#include "util/error.h"
#include "util/utf8.h"
}

#include <string>

class CliComposerPanelTest : public testing::Test
{
      protected:
	struct cli_composer_panel_frame frame{};

	void SetUp() override
	{
		ASSERT_EQ(morph_buf_init(&frame.content, 128), 0);
		frame.columns = 8;
		frame.rows = 2;
		frame.color = 1;
	}

	void TearDown() override
	{
		morph_buf_cleanup(&frame.content);
	}

	std::string Content()
	{
		return std::string(morph_buf_cstr_borrow(&frame.content), frame.content.len);
	}

	std::string Plain()
	{
		std::string plain(frame.content.len + 1, '\0');
		const size_t size = utf8_strip_ansi(
			plain.data(), morph_buf_cstr_borrow(&frame.content), frame.content.len);
		plain.resize(size);
		return plain;
	}
};

TEST_F(CliComposerPanelTest, RowBudgetAndCellClippingKeepOnePhysicalLinePerEntry)
{
	frame.columns = 5;
	ASSERT_EQ(cli_composer_panel_line(&frame, "中文ab", "\033[44;97m"), 0);
	ASSERT_EQ(cli_composer_panel_line(&frame, "second-line", nullptr), 0);
	const std::string painted = Content();
	ASSERT_EQ(cli_composer_panel_line(&frame, "outside-owned-region", nullptr), 0);
	EXPECT_EQ(frame.line_count, 2);
	EXPECT_EQ(Content(), painted);
	EXPECT_EQ(Plain(), "中文a\nsecon\n");
	EXPECT_EQ(utf8_display_width_ansi("\033[44;97m中文a\033[0m"), 5u);
	EXPECT_EQ(utf8valid(Content().c_str()), nullptr);
}

TEST_F(CliComposerPanelTest, WideCodepointsNeverOverflowTheLastCellAndPaddingKeepsBackground)
{
	frame.columns = 1;
	ASSERT_EQ(cli_composer_panel_line(&frame, "中", "\033[48;2;28;28;28m"), 0);
	ASSERT_EQ(cli_composer_panel_line(&frame, "A中", nullptr), 0);
	EXPECT_EQ(Plain(), " \nA\n");
	EXPECT_NE(Content().find("\033[48;2;28;28;28m \033[0m"), std::string::npos);
}

TEST_F(CliComposerPanelTest, UntrustedTextAndStyleCannotInjectCursorOrOscControls)
{
	frame.columns = 80;
	const char *text = "safe\033]0;INJECTED_TITLE\a\033[2J\033[H visible\nnext";
	const char *style = "\033[31m\033[999A\033]0;OTHER_TITLE\a";
	ASSERT_EQ(cli_composer_panel_line(&frame, text, style), 0);
	EXPECT_NE(Content().find("\033[31m"), std::string::npos);
	EXPECT_EQ(Content().find("\033]"), std::string::npos);
	EXPECT_EQ(Content().find("\033[2J"), std::string::npos);
	EXPECT_EQ(Content().find("\033[H"), std::string::npos);
	EXPECT_EQ(Content().find("\033[999A"), std::string::npos);
	EXPECT_EQ(Content().find("INJECTED_TITLE"), std::string::npos);
	EXPECT_EQ(Content().find("OTHER_TITLE"), std::string::npos);
	EXPECT_EQ(frame.line_count, 1);
	EXPECT_NE(Plain().find("safe visible next"), std::string::npos);
}

TEST_F(CliComposerPanelTest, TrustedRendererSgrSurvivesClippingAndOtherControlsAreRemoved)
{
	frame.columns = 7;
	ASSERT_EQ(cli_composer_panel_styled_line(
			  &frame, "\033[31mRed \033[1m中xy\033[0m\033[999A\033]0;title\a"),
		  0);
	EXPECT_EQ(Plain(), "Red 中x\n");
	EXPECT_NE(Content().find("\033[31m"), std::string::npos);
	EXPECT_NE(Content().find("\033[1m"), std::string::npos);
	EXPECT_EQ(Content().find("\033[999A"), std::string::npos);
	EXPECT_EQ(Content().find("\033]"), std::string::npos);
	EXPECT_EQ(utf8valid(Content().c_str()), nullptr);
}

TEST_F(CliComposerPanelTest, DisabledColorKeepsTextWithoutRendererStyles)
{
	frame.color = 0;
	ASSERT_EQ(cli_composer_panel_styled_line(&frame, "\033[38;2;10;20;30mtext\033[0m"), 0);
	EXPECT_EQ(Plain(), "text\n");
	EXPECT_EQ(Content().find("\033[38;2;10;20;30m"), std::string::npos);
}

TEST_F(CliComposerPanelTest, BufferFailuresPropagateWithoutAddingRows)
{
	frame.content.failed = -ENOMEM;
	EXPECT_EQ(cli_composer_panel_line(&frame, "text", nullptr), -ENOMEM);
	EXPECT_EQ(frame.line_count, 0);
	EXPECT_EQ(frame.content.failed, -ENOMEM);
}

TEST(CliComposerPanelLifecycleTest, FailedPresentationPreservesCallerOwnedData)
{
	int completions = 0;
	struct cli_composer_panel_options options{10, 100};
	struct cli_composer_panel_ops ops{};
	ops.draw = [](void *, struct cli_composer_panel_frame *) { return 0; };
	ops.update = [](void *, const struct morph_tui_event *) { return 0; };
	ops.complete = [](void *data, int) { ++*static_cast<int *>(data); };
	EXPECT_EQ(cli_composer_panel_show(nullptr, &options, &ops, &completions), -EINVAL);
	EXPECT_EQ(completions, 0);
}
