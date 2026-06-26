#include <gtest/gtest.h>
#include "render/markdown.h"
#include "util/utf8.h"
#include <string.h>
#include <stdlib.h>
#include <sstream>
#include <vector>
#include <string>

static std::string render(const char *md)
{
	size_t len = markdown_render_ansi_to_buf(md, NULL, 0);
	if (len == 0)
		return "";
	size_t buf_len = len + 1;
	char *buf = (char *)malloc(buf_len);
	if (!buf)
		return "";
	size_t actual = markdown_render_ansi_to_buf(md, buf, buf_len);
	std::string result(buf, actual);
	free(buf);
	return result;
}

static bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

static int parse_graphics_param(const std::string &s, const char *name)
{
	std::string key = std::string(name) + "=";
	size_t pos = s.find(key);
	if (pos == std::string::npos)
		return -1;
	pos += key.size();
	return atoi(s.c_str() + pos);
}

static std::string strip_ansi(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
			i += 2;
			while (i < s.size() &&
			       !((s[i] >= 'A' && s[i] <= 'Z') ||
				 (s[i] >= 'a' && s[i] <= 'z')))
				i++;
			if (i < s.size())
				i++;
			continue;
		}
		out += s[i++];
	}
	return out;
}

TEST(MarkdownRender, NullInput)
{
	EXPECT_EQ(markdown_render_ansi_to_buf(NULL, NULL, 0), 0u);
	EXPECT_NO_FATAL_FAILURE(markdown_render_ansi(NULL));
}

TEST(MarkdownRender, EmptyString)
{
	std::string out = render("");
	EXPECT_TRUE(out.empty() || strip_ansi(out).find_first_not_of("\n") == std::string::npos);
}

TEST(MarkdownRender, SizingModeMatchesActual)
{
	const char *md = "# Hello\n\n**bold** text\n";
	size_t sizing = markdown_render_ansi_to_buf(md, NULL, 0);
	ASSERT_GT(sizing, 0u);
	char *buf = (char *)malloc(sizing + 1);
	ASSERT_NE(buf, nullptr);
	size_t actual = markdown_render_ansi_to_buf(md, buf, sizing + 1);
	EXPECT_EQ(actual, sizing);
	EXPECT_EQ(buf[actual], '\0');
	free(buf);
}

TEST(MarkdownRender, BufferTooSmall)
{
	const char *md = "# Hello World";
	size_t sizing = markdown_render_ansi_to_buf(md, NULL, 0);
	ASSERT_GT(sizing, 0u);
	char *buf = (char *)malloc(4);
	memset(buf, 'X', 4);
	size_t actual = markdown_render_ansi_to_buf(md, buf, 4);
	EXPECT_LT(actual, sizing);
	EXPECT_EQ(buf[3], '\0');
	free(buf);
}

TEST(MarkdownRender, BufferExactFit)
{
	const char *md = "plain text";
	size_t sizing = markdown_render_ansi_to_buf(md, NULL, 0);
	char *buf = (char *)malloc(sizing + 1);
	size_t actual = markdown_render_ansi_to_buf(md, buf, sizing + 1);
	EXPECT_EQ(actual, sizing);
	free(buf);
}

TEST(MarkdownRender, HeadingH1)
{
	std::string out = render("# Title");
	EXPECT_TRUE(contains(out, "\033[1m"));
	EXPECT_TRUE(contains(out, "\033[36m"));
	EXPECT_TRUE(contains(out, "# "));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("# Title") != std::string::npos);
}

TEST(MarkdownRender, HeadingH2)
{
	std::string out = render("## Section");
	EXPECT_TRUE(contains(out, "\033[1m"));
	EXPECT_TRUE(contains(out, "\033[32m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("## Section") != std::string::npos);
}

TEST(MarkdownRender, HeadingH3)
{
	std::string out = render("### Subsection");
	EXPECT_TRUE(contains(out, "\033[33m"));
}

TEST(MarkdownRender, HeadingH4)
{
	std::string out = render("#### H4");
	EXPECT_TRUE(contains(out, "\033[35m"));
}

TEST(MarkdownRender, HeadingH5)
{
	std::string out = render("##### H5");
	EXPECT_TRUE(contains(out, "\033[34m"));
}

TEST(MarkdownRender, HeadingH6)
{
	std::string out = render("###### H6");
	EXPECT_TRUE(contains(out, "\033[90m"));
}

TEST(MarkdownRender, CodeBlockWithLang)
{
	std::string out = render("```python\nprint('hi')\n```");
	EXPECT_TRUE(contains(out, "python"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("print('hi')") != std::string::npos);
}

TEST(MarkdownRender, CodeBlockWithoutLang)
{
	std::string out = render("```\nhello\n```");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("hello") != std::string::npos);
}

TEST(MarkdownRender, CodeBlockMultiline)
{
	std::string out = render("```\nline1\nline2\nline3\n```");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("line1") != std::string::npos);
	EXPECT_TRUE(plain.find("line2") != std::string::npos);
	EXPECT_TRUE(plain.find("line3") != std::string::npos);
}

TEST(MarkdownRender, Blockquote)
{
	std::string out = render("> quoted text");
	EXPECT_TRUE(contains(out, "\342\224\202") || contains(out, "|"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("quoted text") != std::string::npos);
}

TEST(MarkdownRender, BlockquoteNested)
{
	std::string out = render("> outer\n>> inner");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("outer") != std::string::npos);
	EXPECT_TRUE(plain.find("inner") != std::string::npos);
}

TEST(MarkdownRender, UnorderedList)
{
	std::string out = render("- item1\n- item2\n- item3");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("item1") != std::string::npos);
	EXPECT_TRUE(plain.find("item2") != std::string::npos);
	EXPECT_TRUE(plain.find("item3") != std::string::npos);
}

TEST(MarkdownRender, OrderedList)
{
	std::string out = render("1. first\n2. second\n3. third");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("1.") != std::string::npos);
	EXPECT_TRUE(plain.find("2.") != std::string::npos);
	EXPECT_TRUE(plain.find("3.") != std::string::npos);
}

TEST(MarkdownRender, OrderedListStart)
{
	std::string out = render("5. fifth\n6. sixth");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("5.") != std::string::npos);
	EXPECT_TRUE(plain.find("6.") != std::string::npos);
}

TEST(MarkdownRender, NestedList)
{
	std::string out = render("- outer\n  - inner");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("outer") != std::string::npos);
	EXPECT_TRUE(plain.find("inner") != std::string::npos);
}

TEST(MarkdownRender, TaskListChecked)
{
	std::string out = render("- [x] done");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("[x]") != std::string::npos ||
		    plain.find("x") != std::string::npos);
}

TEST(MarkdownRender, TaskListUnchecked)
{
	std::string out = render("- [ ] todo");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("[ ]") != std::string::npos ||
		    plain.find("todo") != std::string::npos);
}

TEST(MarkdownRender, HorizontalRule)
{
	std::string out = render("---");
	std::string plain = strip_ansi(out);
	EXPECT_FALSE(plain.empty());
}

TEST(MarkdownRender, Paragraph)
{
	std::string out = render("Hello world");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Hello world") != std::string::npos);
}

TEST(MarkdownRender, Bold)
{
	std::string out = render("**bold**");
	EXPECT_TRUE(contains(out, "\033[1m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("bold") != std::string::npos);
}

TEST(MarkdownRender, Italic)
{
	std::string out = render("*italic*");
	EXPECT_TRUE(contains(out, "\033[3m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("italic") != std::string::npos);
}

TEST(MarkdownRender, BoldItalic)
{
	std::string out = render("***bold italic***");
	EXPECT_TRUE(contains(out, "\033[1m"));
	EXPECT_TRUE(contains(out, "\033[3m"));
}

TEST(MarkdownRender, InlineCode)
{
	std::string out = render("`code`");
	EXPECT_TRUE(contains(out, "\033[36m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("code") != std::string::npos);
}

TEST(MarkdownRender, InlineLatexMathWithCjkText)
{
	const char *md = "formula $\\boldsymbol{\\mu}\xef\xbc\x8c"
		"\xe5\x8d\x8f\xe6\x96\xb9\xe5\xb7\xae"
		"\xe7\x9f\xa9\xe9\x98\xb5\\Sigma$\xef\xbc\x9a";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
        EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
        EXPECT_TRUE(plain.find("formula") != std::string::npos);
        EXPECT_TRUE(plain.find("\xef\xbc\x9a") != std::string::npos);
	EXPECT_TRUE(plain.find("\\boldsymbol") == std::string::npos);
	EXPECT_TRUE(plain.find("\\Sigma") == std::string::npos);
	EXPECT_TRUE(plain.find("$") == std::string::npos);
}

TEST(MarkdownRender, SimpleInlineLatexMathStaysInline)
{
        std::string out = render("latex $xx$ end");
        EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
        EXPECT_TRUE(contains(out, ",c="));
        EXPECT_TRUE(contains(out, ",r="));
        EXPECT_EQ(out.find('\n'), std::string::npos);

        std::string plain = strip_ansi(out);
        EXPECT_TRUE(plain.find("latex ") != std::string::npos);
        EXPECT_TRUE(plain.find(" end") != std::string::npos);
        EXPECT_TRUE(plain.find("$xx$") == std::string::npos);
}

TEST(MarkdownRender, TallInlineLatexMathPromotesToBlock)
{
        std::string out = render("latex $\\frac{a}{b}$ end");
        EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
        EXPECT_NE(out.find('\n'), std::string::npos);
        bool text_centered_on_formula_row = false;
        std::stringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
                if (line.find("latex ") != std::string::npos &&
                    line.find(" end") != std::string::npos) {
                        text_centered_on_formula_row = true;
                        break;
                }
        }
        EXPECT_TRUE(text_centered_on_formula_row);

        std::string plain = strip_ansi(out);
        EXPECT_TRUE(plain.find("latex ") != std::string::npos);
        EXPECT_TRUE(plain.find(" end") != std::string::npos);
        EXPECT_TRUE(plain.find("\\frac") == std::string::npos);
}

TEST(MarkdownRender, TableInlineLatexMathRendersInCell)
{
	const char *md =
		"| Formula | Meaning |\n"
		"|---------|---------|\n"
		"| $x+y$ | sum |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
	EXPECT_TRUE(contains(out, "C=1") || contains(out, "U=1"));
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Formula") != std::string::npos);
	EXPECT_TRUE(plain.find("sum") != std::string::npos);
	EXPECT_TRUE(plain.find("$x+y$") == std::string::npos);
}

TEST(MarkdownRender, TableTallLatexMathRendersMultilineCell)
{
	const char *md =
		"| Formula | Meaning |\n"
		"|---------|---------|\n"
		"| $\\frac{a}{b}$ | ratio |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
	EXPECT_TRUE(contains(out, "C=1") || contains(out, "U=1"));
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	EXPECT_NE(out.find('\n'), std::string::npos);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Formula") != std::string::npos);
	EXPECT_TRUE(plain.find("ratio") != std::string::npos);
	EXPECT_TRUE(plain.find("\\frac") == std::string::npos);
}

TEST(MarkdownRender, KittyPlaceholderLatexMath)
{
	const char *old = getenv("MORPH_MARKDOWN_KITTY_PLACEHOLDER");
	char *saved = old ? strdup(old) : nullptr;
	setenv("MORPH_MARKDOWN_KITTY_PLACEHOLDER", "1", 1);

	std::string out = render("latex $x$ end");
	EXPECT_TRUE(contains(out, "U=1"));
	EXPECT_TRUE(contains(out, "q=2"));
	EXPECT_TRUE(contains(out, "\xf4\x8e\xbb\xae"));
	EXPECT_TRUE(contains(out, "\xcc\x85"));

	if (saved) {
		setenv("MORPH_MARKDOWN_KITTY_PLACEHOLDER", saved, 1);
		free(saved);
	} else {
		unsetenv("MORPH_MARKDOWN_KITTY_PLACEHOLDER");
	}
}

TEST(MarkdownRender, LatexInlineScaleControlsRenderedSize)
{
	const char *old = getenv("MORPH_MATH_INLINE_SCALE");
	const char *old_max = getenv("MORPH_MATH_INLINE_MAX_SIZE");
	char *saved = old ? strdup(old) : nullptr;
	char *saved_max = old_max ? strdup(old_max) : nullptr;

	unsetenv("MORPH_MATH_INLINE_MAX_SIZE");
	setenv("MORPH_MATH_INLINE_SCALE", "0.80", 1);
	std::string small = render("latex $x$");
	setenv("MORPH_MATH_INLINE_SCALE", "1.40", 1);
	std::string large = render("latex $x$");
	setenv("MORPH_MATH_INLINE_SCALE", "4.00", 1);
	std::string huge = render("latex $x$");
	setenv("MORPH_MATH_INLINE_MAX_SIZE", "20", 1);
	std::string capped = render("latex $x$");

	EXPECT_GT(parse_graphics_param(large, "s"),
		  parse_graphics_param(small, "s"));
	EXPECT_GE(parse_graphics_param(large, "v"),
		  parse_graphics_param(small, "v"));
	EXPECT_GT(parse_graphics_param(huge, "s"),
		  parse_graphics_param(large, "s"));
	EXPECT_LT(parse_graphics_param(capped, "s"),
		  parse_graphics_param(huge, "s"));

	if (saved) {
		setenv("MORPH_MATH_INLINE_SCALE", saved, 1);
		free(saved);
	} else {
		unsetenv("MORPH_MATH_INLINE_SCALE");
	}
	if (saved_max) {
		setenv("MORPH_MATH_INLINE_MAX_SIZE", saved_max, 1);
		free(saved_max);
	} else {
		unsetenv("MORPH_MATH_INLINE_MAX_SIZE");
	}
}

TEST(MarkdownRender, DisplayLatexMathRendersAsBlock)
{
	std::string out = render("before\n\n$$\\sum_i x_i$$\n\nafter");
	EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
	EXPECT_NE(out.find('\n'), std::string::npos);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("before") != std::string::npos);
	EXPECT_TRUE(plain.find("after") != std::string::npos);
	EXPECT_TRUE(plain.find("\\sum") == std::string::npos);
}

TEST(MarkdownRender, TableDisplayLatexMathRendersInCell)
{
	const char *md =
		"| Formula | Meaning |\n"
		"|---------|---------|\n"
		"| $$\\sum_i x_i$$ | total |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\033_Ga=T,f=32"));
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Formula") != std::string::npos);
	EXPECT_TRUE(plain.find("total") != std::string::npos);
	EXPECT_TRUE(plain.find("\\sum") == std::string::npos);
}

TEST(MarkdownRender, Strikethrough)
{
	std::string out = render("~~deleted~~");
	EXPECT_TRUE(contains(out, "\033[9m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("deleted") != std::string::npos);
}

TEST(MarkdownRender, LinkWithHref)
{
	std::string out = render("[click](http://example.com)");
	EXPECT_TRUE(contains(out, "\033[4m"));
	EXPECT_TRUE(contains(out, "\033[34m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("click") != std::string::npos);
	EXPECT_TRUE(plain.find("http://example.com") != std::string::npos);
}

TEST(MarkdownRender, Image)
{
	std::string out = render("![alt text](image.png)");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("image") != std::string::npos);
}

TEST(MarkdownRender, NestedBoldItalic)
{
	std::string out = render("**bold _italic_**");
	EXPECT_TRUE(contains(out, "\033[1m"));
	EXPECT_TRUE(contains(out, "\033[3m"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("bold") != std::string::npos);
	EXPECT_TRUE(plain.find("italic") != std::string::npos);
}

TEST(MarkdownRender, SimpleTable)
{
	const char *md = "| Name  | Age |\n|-------|-----|\n| Alice | 30  |\n| Bob   | 25  |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Name") != std::string::npos);
	EXPECT_TRUE(plain.find("Age") != std::string::npos);
	EXPECT_TRUE(plain.find("Alice") != std::string::npos);
	EXPECT_TRUE(plain.find("Bob") != std::string::npos);
}

TEST(MarkdownRender, TableAlignment)
{
	const char *md = "| Left | Center | Right |\n|:-----|:------:|------:|\n| a    | b      | c     |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Left") != std::string::npos);
	EXPECT_TRUE(plain.find("Center") != std::string::npos);
	EXPECT_TRUE(plain.find("Right") != std::string::npos);
}

TEST(MarkdownRender, TableHeaderSeparator)
{
	const char *md = "| H1 | H2 |\n|----|----|\n| a  | b  |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\342\224\234") || contains(out, "|"));
}

TEST(MarkdownRender, MixedBlocks)
{
	const char *md = "# Title\n\nParagraph text\n\n```\ncode\n```\n\n- item1\n- item2";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Title") != std::string::npos);
	EXPECT_TRUE(plain.find("Paragraph text") != std::string::npos);
	EXPECT_TRUE(plain.find("code") != std::string::npos);
	EXPECT_TRUE(plain.find("item1") != std::string::npos);
	EXPECT_TRUE(plain.find("item2") != std::string::npos);
}

TEST(MarkdownRender, Autolink)
{
	std::string out = render("Visit http://example.com for info");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("example.com") != std::string::npos);
}

TEST(MarkdownRender, MultipleParagraphs)
{
	std::string out = render("First paragraph\n\nSecond paragraph");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("First paragraph") != std::string::npos);
	EXPECT_TRUE(plain.find("Second paragraph") != std::string::npos);
}

TEST(MarkdownRender, HardBreak)
{
	std::string out = render("line1  \nline2");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("line1") != std::string::npos);
	EXPECT_TRUE(plain.find("line2") != std::string::npos);
}

TEST(MarkdownRender, CodeBlockInBlockquote)
{
	std::string out = render("> ```\n> code\n> ```");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("code") != std::string::npos);
}

TEST(MarkdownRender, ListInBlockquote)
{
	std::string out = render("> - item1\n> - item2");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("item1") != std::string::npos);
	EXPECT_TRUE(plain.find("item2") != std::string::npos);
}

TEST(MarkdownRender, HeadingResetAfterLeave)
{
	std::string out = render("# Heading\n\nnormal text");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Heading") != std::string::npos);
	EXPECT_TRUE(plain.find("normal text") != std::string::npos);
}

TEST(MarkdownRender, MultipleHeadings)
{
	std::string out = render("# H1\n\n## H2\n\n### H3");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("H1") != std::string::npos);
	EXPECT_TRUE(plain.find("H2") != std::string::npos);
	EXPECT_TRUE(plain.find("H3") != std::string::npos);
}

TEST(MarkdownRender, InlineCodeInParagraph)
{
	std::string out = render("Use `printf` to output");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("printf") != std::string::npos);
	EXPECT_TRUE(plain.find("Use") != std::string::npos);
}

TEST(MarkdownRender, BoldInList)
{
	std::string out = render("- **bold item**\n- normal item");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("bold item") != std::string::npos);
	EXPECT_TRUE(plain.find("normal item") != std::string::npos);
}

TEST(MarkdownRender, LinkInParagraph)
{
	std::string out = render("Check [this](http://test.com) out");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("this") != std::string::npos);
	EXPECT_TRUE(plain.find("http://test.com") != std::string::npos);
}

TEST(MarkdownRender, TableCjkContent)
{
	const char *md = "| \xe5\x90\x8d\xe5\x89\x8d | \xe5\xb9\xb4\xe9\xbd\xa2 |\n|------|------|\n| \xe5\xa4\xaa\xe9\x83\x8e | 30 |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("30") != std::string::npos);
}

TEST(MarkdownRender, EmptyTableNoCrash)
{
	const char *md = "| A |\n|---|\n";
	std::string out = render(md);
	SUCCEED();
}

TEST(MarkdownRender, HtmlBlock)
{
	std::string out = render("<div>raw html</div>");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("raw html") != std::string::npos ||
		    plain.find("div") != std::string::npos);
}

TEST(MarkdownRender, Entity)
{
	std::string out = render("5 &amp; 3 = 8");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("&") != std::string::npos ||
		    plain.find("amp") != std::string::npos);
}

TEST(MarkdownRender, MultipleCodeBlocks)
{
	const char *md = "```c\nint x = 1;\n```\n\n```python\nx = 1\n```";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("int x = 1;") != std::string::npos);
	EXPECT_TRUE(plain.find("x = 1") != std::string::npos);
}

TEST(MarkdownRender, DeeplyNestedList)
{
	const char *md = "- a\n  - b\n    - c";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("a") != std::string::npos);
	EXPECT_TRUE(plain.find("b") != std::string::npos);
	EXPECT_TRUE(plain.find("c") != std::string::npos);
}

TEST(MarkdownRender, TableEmptyCell)
{
	const char *md = "| A | B |\n|---|---|\n| 1 |   |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("1") != std::string::npos);
}

TEST(MarkdownRender, HeadingWithInlineCode)
{
	std::string out = render("# Using `printf`");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("printf") != std::string::npos);
}

TEST(MarkdownRender, StrikethroughInParagraph)
{
	std::string out = render("This is ~~wrong~~ correct");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("wrong") != std::string::npos);
	EXPECT_TRUE(plain.find("correct") != std::string::npos);
}

TEST(MarkdownRender, MultipleLinks)
{
	std::string out = render("[a](http://a.com) and [b](http://b.com)");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("http://a.com") != std::string::npos);
	EXPECT_TRUE(plain.find("http://b.com") != std::string::npos);
}

TEST(MarkdownRender, BlockquoteWithParagraph)
{
	std::string out = render("> line1\n>\n> line2");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("line1") != std::string::npos);
	EXPECT_TRUE(plain.find("line2") != std::string::npos);
}

TEST(MarkdownRender, CodeBlockWithSpecialChars)
{
	std::string out = render("```\n<a href=\"#\">link</a>\n```");
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("href") != std::string::npos);
}

TEST(MarkdownRender, ListWithCodeBlock)
{
	const char *md = "- item1\n\n```\ncode\n```";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("item1") != std::string::npos);
	EXPECT_TRUE(plain.find("code") != std::string::npos);
}

/* ============================================= */
/* Media collection tests (markdown_render_ansi_with_media) */
/* ============================================= */

struct media_entry {
	std::string type;
	std::string path;
};

struct media_collector {
	std::vector<media_entry> entries;
};

static void test_media_cb(const char *type, const char *path, void *user)
{
	auto *col = (media_collector *)user;
	col->entries.push_back({type, path});
}

TEST(MarkdownMedia, NullInput)
{
	media_collector col;
	EXPECT_NO_FATAL_FAILURE(markdown_render_ansi_with_media(NULL, test_media_cb, &col));
	EXPECT_NO_FATAL_FAILURE(markdown_render_ansi_with_media("hello", NULL, &col));
	EXPECT_EQ(col.entries.size(), 0u);
}

TEST(MarkdownMedia, ImageCollected)
{
	media_collector col;
	markdown_render_ansi_with_media("![alt](photo.png)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "image");
	EXPECT_EQ(col.entries[0].path, "photo.png");
}

TEST(MarkdownMedia, VideoFromImageSyntax)
{
	media_collector col;
	markdown_render_ansi_with_media("![video](clip.mp4)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "video");
	EXPECT_EQ(col.entries[0].path, "clip.mp4");
}

TEST(MarkdownMedia, VideoFromLinkSyntax)
{
	media_collector col;
	markdown_render_ansi_with_media("[watch](movie.mov)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "video");
	EXPECT_EQ(col.entries[0].path, "movie.mov");
}

TEST(MarkdownMedia, NonVideoLinkNotCollected)
{
	media_collector col;
	markdown_render_ansi_with_media("[click](http://example.com)", test_media_cb, &col);
	EXPECT_EQ(col.entries.size(), 0u);
}

TEST(MarkdownMedia, MultipleImages)
{
	media_collector col;
	markdown_render_ansi_with_media("![a](1.png) and ![b](2.jpg)", test_media_cb, &col);
	EXPECT_EQ(col.entries.size(), 2u);
	EXPECT_EQ(col.entries[0].type, "image");
	EXPECT_EQ(col.entries[1].type, "image");
}

TEST(MarkdownMedia, MixedImageAndVideo)
{
	media_collector col;
	markdown_render_ansi_with_media("![pic](img.png)\n\n![vid](out.webm)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 2u);
	EXPECT_EQ(col.entries[0].type, "image");
	EXPECT_EQ(col.entries[1].type, "video");
}

TEST(MarkdownMedia, VideoExtensions)
{
	const char *exts[] = {"mp4", "mov", "avi", "mkv", "webm", "m4v", "mpeg"};
	for (const char *ext : exts) {
		media_collector col;
		std::string filename = std::string("test.") + ext;
		std::string md = std::string("![v](") + filename + ")";
		markdown_render_ansi_with_media(md.c_str(), test_media_cb, &col);
		ASSERT_EQ(col.entries.size(), 1u) << "Failed for extension: " << ext
			<< " path='" << (col.entries.size() > 0 ? col.entries[0].path : "") << "'";
		EXPECT_EQ(col.entries[0].type, "video") << "Failed for extension: " << ext
			<< " path='" << col.entries[0].path << "'";
	}
}

TEST(MarkdownMedia, FileURIStripped)
{
	media_collector col;
	markdown_render_ansi_with_media("![img](file:///tmp/photo.png)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "image");
	EXPECT_EQ(col.entries[0].path, "/tmp/photo.png");
}

TEST(MarkdownMedia, VideoFromLinkWithVideoExt)
{
	media_collector col;
	markdown_render_ansi_with_media("[download](archive.mkv)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "video");
}

TEST(MarkdownMedia, ImageInParagraph)
{
	media_collector col;
	markdown_render_ansi_with_media("Here is a picture:\n\n![cat](cat.png)", test_media_cb, &col);
	ASSERT_EQ(col.entries.size(), 1u);
	EXPECT_EQ(col.entries[0].type, "image");
	EXPECT_EQ(col.entries[0].path, "cat.png");
}

TEST(MarkdownMedia, NoMediaInPlainMarkdown)
{
	media_collector col;
	markdown_render_ansi_with_media("# Hello\n\n**bold** text\n\n- item1\n- item2", test_media_cb, &col);
	EXPECT_EQ(col.entries.size(), 0u);
}

TEST(MarkdownRender, TableWrapDoesNotCrash)
{
	const char *md = "| Name | Description |\n|------|-------------|\n| Alice | This is a very long description that should definitely wrap across multiple lines when the terminal is narrow |\n| Bob | Short |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Alice") != std::string::npos);
	EXPECT_TRUE(plain.find("Bob") != std::string::npos);
}

TEST(MarkdownRender, TableWideContentRenders)
{
	std::string long_text(200, 'X');
	std::string md_str = "| Key | Value |\n|-----|-------|\n| a | " + long_text + " |";
	std::string out = render(md_str.c_str());
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Key") != std::string::npos);
	EXPECT_TRUE(plain.find("a") != std::string::npos);
	EXPECT_TRUE(plain.find("X") != std::string::npos);
}

TEST(MarkdownRender, TableNarrowNoOverflow)
{
	const char *md = "| Header1 | Header2 | Header3 |\n|---------|----------|---------|\n| AAAAAAAAAAAAAA | BBBBBBBBBBBBBB | CCCCCCCCCCCCCC |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Header1") != std::string::npos);
	EXPECT_TRUE(plain.find("Header2") != std::string::npos);
	EXPECT_TRUE(plain.find("Header3") != std::string::npos);
}

TEST(MarkdownRender, TableWrapCJKContent)
{
	const char *md = "| \xe5\x90\x8d\xe5\x89\x8d | \xe8\xaa\xac\xe6\x98\x8e |\n|------|----------|\n| \xe5\xa4\xaa\xe9\x83\x8e | \xe3\x81\x93\xe3\x82\x8c\xe3\x81\xaf\xe9\x9d\x9e\xe5\xb8\xb8\xe3\x81\xab\xe9\x95\xb7\xe3\x81\x84\xe8\xaa\xac\xe6\x98\x8e\xe6\x96\x87\xe3\x81\xa7\xe3\x81\x99 |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("\xe5\x90\x8d") != std::string::npos);
	EXPECT_TRUE(plain.find("\xe8\xaa\xac") != std::string::npos);
	EXPECT_TRUE(plain.find("\xe5\xa4\xaa") != std::string::npos);
}

static size_t count_segment_width(const std::string &line, size_t start, size_t end)
{
	std::string seg = line.substr(start, end - start);
	return utf8_display_width(seg.c_str());
}

TEST(MarkdownRender, TableCJKAlignment)
{
	const char *md = "| \xe5\x90\x8d\xe5\x89\x8d | \xe5\x80\xa4 |\n|------|------|\n| ABC | 10 |\n| \xe3\x81\x82\xe3\x81\x84 | 20 |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);

	std::istringstream stream(plain);
	std::string line;
	std::vector<size_t> col_widths;
	bool found_data = false;
	while (std::getline(stream, line)) {
		size_t pos = 0;
		std::vector<size_t> widths;
		while (pos < line.size()) {
			size_t next = line.find("\xe2\x94\x82", pos);
			if (next == std::string::npos) {
				size_t end = line.find('|', pos);
				if (end == std::string::npos) break;
				next = end;
			}
			if (next > pos) {
				widths.push_back(count_segment_width(line, pos, next));
			}
			pos = next + 3;
		}
		if (!widths.empty()) {
			if (col_widths.empty()) {
				col_widths = widths;
				found_data = true;
			} else if (widths.size() == col_widths.size()) {
				for (size_t i = 0; i < widths.size(); i++) {
					EXPECT_EQ(widths[i], col_widths[i])
						<< "Column " << i << " width mismatch";
				}
			}
		}
	}
	EXPECT_TRUE(found_data);
}

TEST(MarkdownRender, TableCJKWrapAtCharBoundary)
{
	std::string cjk_text;
	for (int i = 0; i < 30; i++)
		cjk_text += "\xe6\xbc\xa2";
	const char *md_prefix = "| \xe5\x90\x8d\xe5\x89\x8d | ";
	const char *md_suffix = " |";
	std::string md = md_prefix + cjk_text + md_suffix;
	std::string out = render(md.c_str());
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("\xe6\xbc\xa2") != std::string::npos);
}

TEST(MarkdownRender, TableNonCJK3ByteWidth)
{
	const char *md = "| Sym | Val |\n|-----|-----|\n| \xe2\x89\xa5 | 10 |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("\xe2\x89\xa5") != std::string::npos);
	EXPECT_TRUE(plain.find("10") != std::string::npos);
}

TEST(MarkdownRender, TableWordBoundaryWrap)
{
	const char *md = "| Name | Description |\n|------|-------------|\n| Test | This is a very long description that should wrap at word boundaries |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("description") != std::string::npos);
	EXPECT_TRUE(plain.find("Test") != std::string::npos);
}

TEST(MarkdownRender, TableMixedCJKAsciiAlignment)
{
	const char *md = "| \xe5\xb1\xa4\xe7\xb4\x9a | \xe9\x81\xb8\xe5\x9e\x8b | \xe5\x8f\x96\xe8\x88\x8e |\n|------|------|------|\n| core | GPT-4o | best |\n| fast | DeepSeek | cheap |";
	std::string out = render(md);
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("core") != std::string::npos);
	EXPECT_TRUE(plain.find("GPT-4o") != std::string::npos);
	EXPECT_TRUE(plain.find("fast") != std::string::npos);
	EXPECT_TRUE(plain.find("DeepSeek") != std::string::npos);
	EXPECT_TRUE(plain.find("\xe5\xb1\xa4") != std::string::npos);
}

TEST(MarkdownRender, TableNoBlankLineBefore)
{
	const char *md =
		"Some text\n"
		"| A | B |\n"
		"| --- | --- |\n"
		"| 1 | 2 |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Some text") != std::string::npos);
	EXPECT_TRUE(plain.find("A") != std::string::npos);
	EXPECT_TRUE(plain.find("B") != std::string::npos);
}

TEST(MarkdownRender, TableNoBlankLineBeforeCJK)
{
	const char *md =
		"\xe4\xbb\xa5\xe4\xb8\x8b\xe6\x98\xaf\xe5\xaf\xb9\xe6\xaf\x94\xe8\xa1\xa8\xef\xbc\x9a\n"
		"| \xe7\x94\xa8Python | \xe7\x94\xa8Zerolang |\n"
		"| --- | --- |\n"
		"| AI\xe5\x86\x99\xe4\xbb\xa3\xe7\xa0\x81 | \xe8\x87\xaa\xe5\x8a\xa8\xe4\xbf\xae\xe5\xa4\x8d |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Python") != std::string::npos);
	EXPECT_TRUE(plain.find("Zerolang") != std::string::npos);
}

TEST(MarkdownRender, TableInCodeBlockNotAffected)
{
	const char *md =
		"```\n"
		"| A | B |\n"
		"| --- | --- |\n"
		"| 1 | 2 |\n"
		"```";
	std::string out = render(md);
	EXPECT_FALSE(contains(out, "\xe2\x94\x82"));
}

TEST(MarkdownRender, TableWithBlankLineBeforeUnchanged)
{
	const char *md =
		"Some text\n\n"
		"| A | B |\n"
		"| --- | --- |\n"
		"| 1 | 2 |";
	std::string out = render(md);
	EXPECT_TRUE(contains(out, "\xe2\x94\x82"));
	std::string plain = strip_ansi(out);
	EXPECT_TRUE(plain.find("Some text") != std::string::npos);
	EXPECT_TRUE(plain.find("A") != std::string::npos);
}
