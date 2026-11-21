#include <gtest/gtest.h>

extern "C" {
#include "agent_ui.h"
}

#include <cstdlib>
#include <string>

TEST(TestAgentUI, NormalizesCommonMarkdownDeviations)
{
	char *out = agent_ui_normalize_markdown(
		"标题\n＃ Heading\n正文\n- item\n```c\nint x = 1;");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("# Heading"), std::string::npos);
	EXPECT_NE(normalized.find("\n\n# Heading"), std::string::npos);
	EXPECT_NE(normalized.find("\n\n- item"), std::string::npos);
	EXPECT_NE(normalized.find("\n```\n"), std::string::npos);
}

TEST(TestAgentUI, RemovesMarkdownHorizontalRules)
{
	char *out = agent_ui_normalize_markdown(
		"Intro\n\n---\n\nDetails\n  * * *  \nMore\n___\nEnd");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_EQ(normalized.find("---"), std::string::npos);
	EXPECT_EQ(normalized.find("* * *"), std::string::npos);
	EXPECT_EQ(normalized.find("___"), std::string::npos);
	EXPECT_NE(normalized.find("Intro\n\nDetails\n\nMore\n\nEnd"),
		  std::string::npos);
}

TEST(TestAgentUI, KeepsHorizontalRuleTextInsideFences)
{
	char *out = agent_ui_normalize_markdown("```md\n---\n```\nAfter");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("```md\n---\n```"), std::string::npos);
}

TEST(TestAgentUI, NormalizesMarkdownWithoutBreakingLists)
{
	char *out = agent_ui_normalize_markdown(
		"intro\n\xef\xbc\x8d one\n\xef\xbc\x8d two\n\n"
		"\xef\xbc\x9e quote\n\xef\xbc\x9e more");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("\n\n- one\n- two\n"), std::string::npos);
	EXPECT_NE(normalized.find("\n\n> quote\n> more\n"), std::string::npos);
	EXPECT_EQ(normalized.find("- one\n\n- two"), std::string::npos);
	EXPECT_EQ(normalized.find("> quote\n\n> more"), std::string::npos);
}

TEST(TestAgentUI, NormalizesCodeFenceButPreservesCodeBody)
{
	char *out = agent_ui_normalize_markdown(
		"\xef\xbd\x80\xef\xbd\x80\xef\xbd\x80\n"
		"\xef\xbc\x83 not heading\n"
		"\xef\xbd\x80\xef\xbd\x80\xef\xbd\x80");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("```\n"), std::string::npos);
	EXPECT_NE(normalized.find("\xef\xbc\x83 not heading"), std::string::npos);
	EXPECT_EQ(normalized.find("# not heading"), std::string::npos);
}

TEST(TestAgentUI, PreservesLiteralCodeWhitespaceAndFenceLikeLines)
{
	const char *input = "````text\nline  \n\n\n\t\n```\n````suffix\n"
		"~~~\n    ````\n＃ literal\n````\nAfter";
	char *out = agent_ui_normalize_markdown(input);
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);
	EXPECT_NE(normalized.find("line  \n\n\n\t\n```\n````suffix\n"
		"~~~\n    ````\n＃ literal\n````\n"), std::string::npos);
	EXPECT_NE(normalized.find("After\n"), std::string::npos);
}

TEST(TestAgentUI, NormalizesFullwidthLinksAndTables)
{
	char *out = agent_ui_normalize_markdown(
		"\xef\xbc\xbb" "click" "\xef\xbc\xbd"
		"\xef\xbc\x88" "http://example.com" "\xef\xbc\x89\n"
		"\xef\xbd\x9c A \xef\xbd\x9c B \xef\xbd\x9c\n"
		"\xef\xbd\x9c \xef\xbc\x8d\xef\xbc\x8d\xef\xbc\x8d "
		"\xef\xbd\x9c \xef\xbc\x8d\xef\xbc\x8d\xef\xbc\x8d "
		"\xef\xbd\x9c\n"
		"\xef\xbd\x9c 1 \xef\xbd\x9c 2 \xef\xbd\x9c");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("[click](http://example.com)"),
		  std::string::npos);
	EXPECT_NE(normalized.find("| A | B |"), std::string::npos);
	EXPECT_NE(normalized.find("| --- | --- |"), std::string::npos);
	EXPECT_NE(normalized.find("| 1 | 2 |"), std::string::npos);
}

TEST(TestAgentUI, SplitsTextAppendedAfterClosedTableRow)
{
	char *out = agent_ui_normalize_markdown(
		"| 常数 | 意义 | 所属领域 |\n"
		"|------|------|----------|\n"
		"| i | 虚数单位，满足 $i^2=-1$，扩展实数到复数域的核心单位 | "
		"代数学 | 这个 table 没有渲染");
	ASSERT_NE(out, nullptr);
	std::string normalized(out);
	free(out);

	EXPECT_NE(normalized.find("| i | 虚数单位，满足 $i^2=-1$，扩展实数到复数域的核心单位 | 代数学 |\n"
				  "这个 table 没有渲染\n"),
		  std::string::npos);
}
