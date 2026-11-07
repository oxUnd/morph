#include <gtest/gtest.h>

extern "C" {
#include "agent_ui.h"
#include "util/json.h"
}

#include <cstdlib>
#include <string>

static struct json *parse_ir(const char *input)
{
	char *json = agent_ui_parse_tags_json(input);
	struct json *root = nullptr;

	if (!json)
		return nullptr;
	root = json_parse_prefix(json, NULL, NULL);
	free(json);
	return root;
}

TEST(TestAgentUI, ParsesSupportedTagsToIR)
{
	struct json *root =
		parse_ir("Learn <m:speak text=\"access\" lang=\"en-US\">access</m:speak> "
			 "<m:button label=\"Next\" action=\"practice.next\" />");
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(json_string_value(json_member_ci(root, "kind")), "agent_ui");
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 2);

	struct json *nodes = json_member_ci(root, "nodes");
	ASSERT_TRUE(json_is_array_value(nodes));
	ASSERT_EQ(json_count(nodes), 4);
	struct json *speak = json_at(nodes, 1);
	ASSERT_STREQ(json_string_value(json_member_ci(speak, "component")), "speak");
	EXPECT_STREQ(json_string_value(json_member_ci(speak, "text")), "access");
	struct json *attrs = json_member_ci(speak, "attrs");
	EXPECT_STREQ(json_string_value(json_member_ci(attrs, "text")), "access");
	EXPECT_STREQ(json_string_value(json_member_ci(attrs, "lang")), "en-US");
	json_free(root);
}

TEST(TestAgentUI, AcceptsSmartQuotesAndUnquotedAttrs)
{
	struct json *root = parse_ir("<m:vocab word=“access” phonetic=/ˈækses/ lang=en-US>"
				     "v. 获取；n. 入口</m:vocab>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 1);

	struct json *nodes = json_member_ci(root, "nodes");
	struct json *vocab = json_at(nodes, 0);
	ASSERT_STREQ(json_string_value(json_member_ci(vocab, "component")), "vocab");
	EXPECT_STREQ(json_string_value(json_member_ci(vocab, "text")), "v. 获取；n. 入口");
	struct json *attrs = json_member_ci(vocab, "attrs");
	EXPECT_STREQ(json_string_value(json_member_ci(attrs, "word")), "access");
	EXPECT_STREQ(json_string_value(json_member_ci(attrs, "phonetic")), "/ˈækses/");
	EXPECT_STREQ(json_string_value(json_member_ci(attrs, "lang")), "en-US");
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, AcceptsSingleQuotedAttrsAndBodylessOpenTags)
{
	struct json *root = parse_ir("<m:copy text='hello world' label='Copy'> "
				     "<m:navigate target='word.detail' word='access'>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 2);

	struct json *nodes = json_member_ci(root, "nodes");
	struct json *copy = json_at(nodes, 0);
	ASSERT_STREQ(json_string_value(json_member_ci(copy, "component")), "copy");
	struct json *copyAttrs = json_member_ci(copy, "attrs");
	EXPECT_STREQ(json_string_value(json_member_ci(copyAttrs, "text")), "hello world");
	struct json *navigate = json_at(nodes, 2);
	ASSERT_STREQ(json_string_value(json_member_ci(navigate, "component")), "navigate");
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, MissingClosingTagFallsBackWithWarning)
{
	struct json *root = parse_ir("Say <m:highlight level=\"strong\">important");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 1);
	struct json *nodes = json_member_ci(root, "nodes");
	struct json *highlight = json_at(nodes, 1);
	ASSERT_STREQ(json_string_value(json_member_ci(highlight, "component")), "highlight");
	EXPECT_STREQ(json_string_value(json_member_ci(highlight, "text")), "important");
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, AskUserTagRemainsText)
{
	struct json *root = parse_ir("Question <m:ask_user question=\"Ready?\" /> should not run");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 0);

	struct json *nodes = json_member_ci(root, "nodes");
	std::string text;
	for (int i = 0; i < json_count(nodes); i++) {
		struct json *node = json_at(nodes, i);
		struct json *value = json_member_ci(node, "text");
		if (json_is_string_value(value))
			text += json_string_value(value);
	}
	EXPECT_NE(text.find("<m:ask_user"), std::string::npos);
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, AcceptsMissingColonInKnownTags)
{
	struct json *root = parse_ir("Say <mspeak text=\"hello\" lang=\"en-US\">hello</mspeak> "
				     "<mbutton label=\"Next\" action=\"practice.next\" />");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 2);

	struct json *nodes = json_member_ci(root, "nodes");
	struct json *speak = json_at(nodes, 1);
	ASSERT_STREQ(json_string_value(json_member_ci(speak, "component")), "speak");
	EXPECT_STREQ(json_string_value(json_member_ci(speak, "text")), "hello");
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, AcceptsMixedColonOpeningAndClosingTags)
{
	struct json *root = parse_ir("<mspeak text=\"hello\">hello</m:speak> "
				     "<m:highlight level=\"strong\">重点</mhighlight>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 2);

	struct json *nodes = json_member_ci(root, "nodes");
	struct json *speak = json_at(nodes, 0);
	ASSERT_STREQ(json_string_value(json_member_ci(speak, "component")), "speak");
	EXPECT_STREQ(json_string_value(json_member_ci(speak, "text")), "hello");
	struct json *highlight = json_at(nodes, 2);
	ASSERT_STREQ(json_string_value(json_member_ci(highlight, "component")), "highlight");
	EXPECT_STREQ(json_string_value(json_member_ci(highlight, "text")), "重点");
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

TEST(TestAgentUI, DoesNotTreatHtmlMarkAsMissingColonTag)
{
	struct json *root = parse_ir("Use <mark>highlight</mark> text");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 0);

	struct json *nodes = json_member_ci(root, "nodes");
	std::string text;
	for (int i = 0; i < json_count(nodes); i++) {
		struct json *node = json_at(nodes, i);
		struct json *value = json_member_ci(node, "text");
		if (json_is_string_value(value))
			text += json_string_value(value);
	}
	EXPECT_NE(text.find("<mark>highlight</mark>"), std::string::npos);
	json_free(root);
}

TEST(TestAgentUI, UnsupportedTagsRemainText)
{
	struct json *root = parse_ir("Bad <m:quiz id=\"q1\">Q</m:quiz>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_int_value(json_member_ci(root, "recognized_tags")), 0);
	struct json *nodes = json_member_ci(root, "nodes");
	ASSERT_GE(json_count(nodes), 1);
	std::string text;
	for (int i = 0; i < json_count(nodes); i++) {
		struct json *node = json_at(nodes, i);
		struct json *value = json_member_ci(node, "text");
		if (json_is_string_value(value))
			text += json_string_value(value);
	}
	EXPECT_NE(text.find("<m:quiz"), std::string::npos);
	struct json *warnings = json_member_ci(root, "warnings");
	EXPECT_GT(json_count(warnings), 0);
	json_free(root);
}

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
