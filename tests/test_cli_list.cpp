#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/list_ui.h"
}

#include <cstdlib>
#include <string>

TEST(CliListTest, CompactsWhitespaceWithoutBreakingUtf8)
{
	char output[128];

	ASSERT_GT(cli_list_compact_text(
		output, sizeof(output), "  飞书\n\tMarkdown   renderer  "), 0u);
	EXPECT_STREQ(output, "飞书 Markdown renderer");
}

TEST(CliListTest, UsesConfiguredColumnsWhenStdoutIsNotTerminal)
{
	const char *previous = std::getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	bool had_previous = previous != nullptr;

	ASSERT_EQ(setenv("COLUMNS", "72", 1), 0);
	EXPECT_EQ(cli_list_columns(), 72);
	if (had_previous)
		ASSERT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		ASSERT_EQ(unsetenv("COLUMNS"), 0);
}

TEST(CliListTest, LongItemUsesCompactContinuation)
{
	testing::internal::CaptureStdout();
	cli_list_item("", 1, nullptr, "a_very_long_registered_tool_name",
		      "First line\nSecond line with more details", 12, 40);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("a_very_long_registered_tool_name"),
		  std::string::npos);
	EXPECT_NE(output.find("First line Second line"), std::string::npos);
	EXPECT_NE(output.find("…"), std::string::npos);
}

TEST(CliListTest, JsonFieldRendersNestedTree)
{
	testing::internal::CaptureStdout();
	cli_list_json_field("Input schema",
			    "{\"type\":\"object\",\"properties\":{"
			    "\"query\":{\"type\":\"string\"}}}", 1, 80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Input schema"), std::string::npos);
	EXPECT_NE(output.find("properties"), std::string::npos);
	EXPECT_NE(output.find("query"), std::string::npos);
	EXPECT_NE(output.find("string"), std::string::npos);
}
