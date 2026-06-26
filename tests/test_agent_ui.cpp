#include <gtest/gtest.h>

extern "C" {
#include "agent_ui.h"
#include "cJSON.h"
}

#include <cstdlib>
#include <string>

static cJSON *parse_ir(const char *input)
{
	char *json = agent_ui_parse_tags_json(input);
	cJSON *root = nullptr;

	if (!json)
		return nullptr;
	root = cJSON_Parse(json);
	free(json);
	return root;
}

TEST(TestAgentUI, ParsesSupportedTagsToIR)
{
	cJSON *root = parse_ir(
		"Learn <m:speak text=\"access\" lang=\"en-US\">access</m:speak> "
		"<m:button label=\"Next\" action=\"practice.next\" />");
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(root, "kind")->valuestring, "agent_ui");
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 2);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	ASSERT_TRUE(cJSON_IsArray(nodes));
	ASSERT_EQ(cJSON_GetArraySize(nodes), 4);
	cJSON *speak = cJSON_GetArrayItem(nodes, 1);
	ASSERT_STREQ(cJSON_GetObjectItem(speak, "component")->valuestring, "speak");
	EXPECT_STREQ(cJSON_GetObjectItem(speak, "text")->valuestring, "access");
	cJSON *attrs = cJSON_GetObjectItem(speak, "attrs");
	EXPECT_STREQ(cJSON_GetObjectItem(attrs, "text")->valuestring, "access");
	EXPECT_STREQ(cJSON_GetObjectItem(attrs, "lang")->valuestring, "en-US");
	cJSON_Delete(root);
}

TEST(TestAgentUI, AcceptsSmartQuotesAndUnquotedAttrs)
{
	cJSON *root = parse_ir(
		"<m:vocab word=“access” phonetic=/ˈækses/ lang=en-US>"
		"v. 获取；n. 入口</m:vocab>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 1);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	cJSON *vocab = cJSON_GetArrayItem(nodes, 0);
	ASSERT_STREQ(cJSON_GetObjectItem(vocab, "component")->valuestring, "vocab");
	EXPECT_STREQ(cJSON_GetObjectItem(vocab, "text")->valuestring,
		     "v. 获取；n. 入口");
	cJSON *attrs = cJSON_GetObjectItem(vocab, "attrs");
	EXPECT_STREQ(cJSON_GetObjectItem(attrs, "word")->valuestring, "access");
	EXPECT_STREQ(cJSON_GetObjectItem(attrs, "phonetic")->valuestring,
		     "/ˈækses/");
	EXPECT_STREQ(cJSON_GetObjectItem(attrs, "lang")->valuestring, "en-US");
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, AcceptsSingleQuotedAttrsAndBodylessOpenTags)
{
	cJSON *root = parse_ir(
		"<m:copy text='hello world' label='Copy'> "
		"<m:navigate target='word.detail' word='access'>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 2);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	cJSON *copy = cJSON_GetArrayItem(nodes, 0);
	ASSERT_STREQ(cJSON_GetObjectItem(copy, "component")->valuestring,
		     "copy");
	cJSON *copyAttrs = cJSON_GetObjectItem(copy, "attrs");
	EXPECT_STREQ(cJSON_GetObjectItem(copyAttrs, "text")->valuestring,
		     "hello world");
	cJSON *navigate = cJSON_GetArrayItem(nodes, 2);
	ASSERT_STREQ(cJSON_GetObjectItem(navigate, "component")->valuestring,
		     "navigate");
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, MissingClosingTagFallsBackWithWarning)
{
	cJSON *root = parse_ir("Say <m:highlight level=\"strong\">important");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 1);
	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	cJSON *highlight = cJSON_GetArrayItem(nodes, 1);
	ASSERT_STREQ(cJSON_GetObjectItem(highlight, "component")->valuestring,
		     "highlight");
	EXPECT_STREQ(cJSON_GetObjectItem(highlight, "text")->valuestring,
		     "important");
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, AskUserTagRemainsText)
{
	cJSON *root = parse_ir(
		"Question <m:ask_user question=\"Ready?\" /> should not run");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 0);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	std::string text;
	for (int i = 0; i < cJSON_GetArraySize(nodes); i++) {
		cJSON *node = cJSON_GetArrayItem(nodes, i);
		cJSON *value = cJSON_GetObjectItem(node, "text");
		if (cJSON_IsString(value))
			text += value->valuestring;
	}
	EXPECT_NE(text.find("<m:ask_user"), std::string::npos);
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, AcceptsMissingColonInKnownTags)
{
	cJSON *root = parse_ir(
		"Say <mspeak text=\"hello\" lang=\"en-US\">hello</mspeak> "
		"<mbutton label=\"Next\" action=\"practice.next\" />");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 2);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	cJSON *speak = cJSON_GetArrayItem(nodes, 1);
	ASSERT_STREQ(cJSON_GetObjectItem(speak, "component")->valuestring,
		     "speak");
	EXPECT_STREQ(cJSON_GetObjectItem(speak, "text")->valuestring,
		     "hello");
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, AcceptsMixedColonOpeningAndClosingTags)
{
	cJSON *root = parse_ir(
		"<mspeak text=\"hello\">hello</m:speak> "
		"<m:highlight level=\"strong\">重点</mhighlight>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 2);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	cJSON *speak = cJSON_GetArrayItem(nodes, 0);
	ASSERT_STREQ(cJSON_GetObjectItem(speak, "component")->valuestring,
		     "speak");
	EXPECT_STREQ(cJSON_GetObjectItem(speak, "text")->valuestring,
		     "hello");
	cJSON *highlight = cJSON_GetArrayItem(nodes, 2);
	ASSERT_STREQ(cJSON_GetObjectItem(highlight, "component")->valuestring,
		     "highlight");
	EXPECT_STREQ(cJSON_GetObjectItem(highlight, "text")->valuestring,
		     "重点");
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
}

TEST(TestAgentUI, DoesNotTreatHtmlMarkAsMissingColonTag)
{
	cJSON *root = parse_ir("Use <mark>highlight</mark> text");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 0);

	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	std::string text;
	for (int i = 0; i < cJSON_GetArraySize(nodes); i++) {
		cJSON *node = cJSON_GetArrayItem(nodes, i);
		cJSON *value = cJSON_GetObjectItem(node, "text");
		if (cJSON_IsString(value))
			text += value->valuestring;
	}
	EXPECT_NE(text.find("<mark>highlight</mark>"), std::string::npos);
	cJSON_Delete(root);
}

TEST(TestAgentUI, UnsupportedTagsRemainText)
{
	cJSON *root = parse_ir("Bad <m:quiz id=\"q1\">Q</m:quiz>");
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(root, "recognized_tags")->valueint, 0);
	cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
	ASSERT_GE(cJSON_GetArraySize(nodes), 1);
	std::string text;
	for (int i = 0; i < cJSON_GetArraySize(nodes); i++) {
		cJSON *node = cJSON_GetArrayItem(nodes, i);
		cJSON *value = cJSON_GetObjectItem(node, "text");
		if (cJSON_IsString(value))
			text += value->valuestring;
	}
	EXPECT_NE(text.find("<m:quiz"), std::string::npos);
	cJSON *warnings = cJSON_GetObjectItem(root, "warnings");
	EXPECT_GT(cJSON_GetArraySize(warnings), 0);
	cJSON_Delete(root);
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
