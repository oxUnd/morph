#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "util/utf8.h"
}

TEST(Utf8EllipsizeTest, KeepsShortValue)
{
	char output[64];

	EXPECT_EQ(utf8_copy_ellipsized_display_width(
			  output, sizeof(output), "deepseek-v4-flash", 24, 0),
		  17u);
	EXPECT_STREQ(output, "deepseek-v4-flash");
}

TEST(Utf8EllipsizeTest, ClipsLongValueAtEnd)
{
	char output[64];

	EXPECT_EQ(utf8_copy_ellipsized_display_width(
			  output, sizeof(output), "very-long-model-name", 10, 0),
		  10u);
	EXPECT_STREQ(output, "very-long…");
}

TEST(Utf8EllipsizeTest, ClipsLongValueAtStart)
{
	char output[64];

	EXPECT_EQ(utf8_copy_ellipsized_display_width(
			  output, sizeof(output),
			  "~/Work/AI/a-very-long-project/morph", 18, 1),
		  18u);
	EXPECT_STREQ(output, "…ong-project/morph");
}

TEST(Utf8EllipsizeTest, DoesNotSplitUtf8)
{
	char output[64];

	EXPECT_EQ(utf8_copy_ellipsized_display_width(
			  output, sizeof(output), "模型名称非常长", 7, 0),
		  7u);
	EXPECT_TRUE(utf8valid(output) == nullptr);
	EXPECT_EQ(utf8_display_width(output), 7u);
}

TEST(Utf8TerminalSanitizeTest, RemovesTerminalSequencesAndShowsControls)
{
	const char input[] =
		"safe\033[31mred\033[0m\033[2J"
		"\033]52;c;secret\aX\033Ppayload\033\\Y\r\nZ\rQ\b\a";
	char *output = utf8_terminal_sanitize_dup(input, strlen(input),
		UTF8_TERMINAL_TEXT_MULTILINE, nullptr);

	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output, "saferedXY\nZ\\rQ\\b\\a");
	EXPECT_EQ(std::strchr(output, '\033'), nullptr);
	free(output);
}

TEST(Utf8TerminalSanitizeTest, HandlesSequencesAndUtf8AcrossChunks)
{
	struct utf8_terminal_sanitizer sanitizer;
	morph_buf_t output{};
	const char first[] = "A\033]52;c;sec";
	const char second[] = "ret\aB \xe4";
	const char third[] = "\xb8\xad\033[";
	const char fourth[] = "2JC";

	ASSERT_EQ(morph_buf_init(&output, 64), 0);
	utf8_terminal_sanitizer_init(&sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	ASSERT_EQ(utf8_terminal_sanitize_feed(&sanitizer, &output, first,
		sizeof(first) - 1, 0), 0);
	ASSERT_EQ(utf8_terminal_sanitize_feed(&sanitizer, &output, second,
		sizeof(second) - 1, 0), 0);
	ASSERT_EQ(utf8_terminal_sanitize_feed(&sanitizer, &output, third,
		sizeof(third) - 1, 0), 0);
	ASSERT_EQ(utf8_terminal_sanitize_feed(&sanitizer, &output, fourth,
		sizeof(fourth) - 1, 1), 0);
	EXPECT_STREQ(morph_buf_cstr(&output), "AB 中C");
	morph_buf_cleanup(&output);
}

TEST(Utf8TerminalSanitizeTest, HandlesC1AndSingleLineText)
{
	const char input[] =
		"A\x9b" "31mB\xc2\x9b\nC\tD\rE\x7f";
	char *output = utf8_terminal_sanitize_dup(input, strlen(input),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, nullptr);

	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output, "AB\\x9b C D E\\x7f");
	free(output);
}

TEST(Utf8TerminalSanitizeTest, RemovesC1StringsAndUnterminatedSequences)
{
	const char terminated[] =
		"A\r\x9b" "2JB\x90" "payload\x9c"
		"C\x9d" "title\033\033\\D";
	const char unterminated[] = "visible\033]52;c;hidden";
	char *output = utf8_terminal_sanitize_dup(terminated,
		strlen(terminated), UTF8_TERMINAL_TEXT_MULTILINE, nullptr);
	char *unfinished = utf8_terminal_sanitize_dup(unterminated,
		strlen(unterminated), UTF8_TERMINAL_TEXT_MULTILINE, nullptr);

	ASSERT_NE(output, nullptr);
	ASSERT_NE(unfinished, nullptr);
	EXPECT_STREQ(output, "A\\rBCD");
	EXPECT_STREQ(unfinished, "visible");
	free(output);
	free(unfinished);
}

TEST(Utf8TerminalSanitizeTest, DropsIncompleteUtf8AtStreamEnd)
{
	struct utf8_terminal_sanitizer sanitizer;
	morph_buf_t output{};
	const char input[] = "valid \xe4\xb8";

	ASSERT_EQ(morph_buf_init(&output, 32), 0);
	utf8_terminal_sanitizer_init(&sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	ASSERT_EQ(utf8_terminal_sanitize_feed(&sanitizer, &output, input,
		sizeof(input) - 1, 1), 0);
	EXPECT_STREQ(morph_buf_cstr(&output), "valid ");
	morph_buf_cleanup(&output);
}
