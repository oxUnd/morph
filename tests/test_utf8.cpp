#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
}

TEST(Utf8EllipsizeTest, KeepsShortValue)
{
	char output[64];

	EXPECT_EQ(morph_terminal_copy_ellipsized_display_width(output, sizeof(output),
							       "deepseek-v4-flash", 24, 0),
		  17u);
	EXPECT_STREQ(output, "deepseek-v4-flash");
}

TEST(Utf8EllipsizeTest, ClipsLongValueAtEnd)
{
	char output[64];

	EXPECT_EQ(morph_terminal_copy_ellipsized_display_width(output, sizeof(output),
							       "very-long-model-name", 10, 0),
		  10u);
	EXPECT_STREQ(output, "very-long…");
}

TEST(Utf8EllipsizeTest, ClipsLongValueAtStart)
{
	char output[64];

	EXPECT_EQ(morph_terminal_copy_ellipsized_display_width(
			  output, sizeof(output), "~/Work/AI/a-very-long-project/morph", 18, 1),
		  18u);
	EXPECT_STREQ(output, "…ong-project/morph");
}

TEST(Utf8EllipsizeTest, DoesNotSplitUtf8)
{
	char output[64];

	EXPECT_EQ(morph_terminal_copy_ellipsized_display_width(output, sizeof(output),
							       "模型名称非常长", 7, 0),
		  7u);
	EXPECT_TRUE(nb_utf8_validate(nb_str_from_cstr(output), NULL) == NB_UTF8_OK);
	EXPECT_EQ(nb_utf8_width_cstr(output), 7u);
}

TEST(Utf8TerminalSanitizeTest, RemovesTerminalSequencesAndShowsControls)
{
	const char input[] =
		"safe\033[31mred\033[0m\033[2J"
		"\033]52;c;secret\aX\033Ppayload\033\\Y\r\nZ\rQ\b\a";
	char *output = morph_terminal_sanitize_dup(input, strlen(input),
						   MORPH_TERMINAL_TEXT_MULTILINE, nullptr);

	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output, "saferedXY\nZ\\rQ\\b\\a");
	EXPECT_EQ(std::strchr(output, '\033'), nullptr);
	free(output);
}

TEST(Utf8TerminalSanitizeTest, HandlesSequencesAndUtf8AcrossChunks)
{
	struct morph_terminal_sanitizer sanitizer;
	nb_buf output{};
	const char first[] = "A\033]52;c;sec";
	const char second[] = "ret\aB \xe4";
	const char third[] = "\xb8\xad\033[";
	const char fourth[] = "2JC";

	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 64) ? 0 : NB_ERR_NOMEM), 0);
	morph_terminal_sanitizer_init(&sanitizer, MORPH_TERMINAL_TEXT_MULTILINE);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, first, sizeof(first) - 1, 0),
		  0);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, second, sizeof(second) - 1, 0),
		  0);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, third, sizeof(third) - 1, 0),
		  0);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, fourth, sizeof(fourth) - 1, 1),
		  0);
	EXPECT_STREQ(nb_buf_cstr(&output), "AB 中C");
	nb_buf_cleanup(&output);
}

TEST(Utf8TerminalSanitizeTest, HandlesC1AndSingleLineText)
{
	const char input[] =
		"A\x9b" "31mB\xc2\x9b\nC\tD\rE\x7f";
	char *output = morph_terminal_sanitize_dup(input, strlen(input),
						   MORPH_TERMINAL_TEXT_SINGLE_LINE, nullptr);

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
	char *output = morph_terminal_sanitize_dup(terminated, strlen(terminated),
						   MORPH_TERMINAL_TEXT_MULTILINE, nullptr);
	char *unfinished = morph_terminal_sanitize_dup(unterminated, strlen(unterminated),
						       MORPH_TERMINAL_TEXT_MULTILINE, nullptr);

	ASSERT_NE(output, nullptr);
	ASSERT_NE(unfinished, nullptr);
	EXPECT_STREQ(output, "A\\rBCD");
	EXPECT_STREQ(unfinished, "visible");
	free(output);
	free(unfinished);
}

TEST(Utf8TerminalSanitizeTest, DropsIncompleteUtf8AtStreamEnd)
{
	struct morph_terminal_sanitizer sanitizer;
	nb_buf output{};
	const char input[] = "valid \xe4\xb8";

	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 32) ? 0 : NB_ERR_NOMEM), 0);
	morph_terminal_sanitizer_init(&sanitizer, MORPH_TERMINAL_TEXT_MULTILINE);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, input, sizeof(input) - 1, 1),
		  0);
	EXPECT_STREQ(nb_buf_cstr(&output), "valid ");
	nb_buf_cleanup(&output);
}

TEST(Utf8DecodeTest, RejectsNonScalarCodepoints)
{
	unsigned cp = 0;
	size_t count = 0;

	for (const char *input : {"\xed\xa0\x80", "\xed\xbf\xbf",
				  "\xf4\x90\x80\x80", "\xf7\xbf\xbf\xbf"})
		EXPECT_EQ(nb_utf8_read(input, strlen(input), &cp, &count), 0);
	EXPECT_EQ(nb_utf8_read("\xf4\x8f\xbf\xbf", 4, &cp, &count), 1);
	EXPECT_EQ(cp, 0x10ffffu);
	EXPECT_EQ(count, 4u);
}

TEST(Utf8ProcTest, StrictValidationReportsTheBadByte)
{
	const char invalid[] = "ok\xed\xa0\x80";
	const char truncated[] = "\xe4\xb8\xad";
	size_t offset = 0;
	EXPECT_NE(nb_utf8_validate(nb_str_from_cstr(invalid), &offset), NB_UTF8_OK);
	EXPECT_EQ(offset, 2u);
	EXPECT_NE(nb_utf8_validate(nb_str_from_parts(truncated, 2), &offset), NB_UTF8_OK);
	EXPECT_EQ(offset, 0u);
	EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr("hello 中文😀"), NULL), NB_UTF8_OK);
	for (const char *text : {"\xc0\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80"})
		EXPECT_NE(nb_utf8_validate(nb_str_from_cstr(text), NULL), NB_UTF8_OK);
}

TEST(Utf8ProcTest, EncodingAndDecodingRoundTripScalarBoundaries)
{
	for (unsigned value : {1u, 0x7fu, 0x80u, 0x7ffu, 0x800u, 0xd7ffu,
			       0xe000u, 0xffffu, 0x10000u, 0x1f600u, 0x10ffffu}) {
		char text[4];
		size_t size = nb_utf8_encode(value, text);
		ASSERT_GE(size, 1u);
		ASSERT_LE(size, sizeof(text));
		unsigned decoded = 0;
		size_t consumed = 0;
		ASSERT_EQ(nb_utf8_read(text, size, &decoded, &consumed), 1);
		EXPECT_EQ(decoded, value);
		EXPECT_EQ(consumed, size);
	}
	char text[4];
	for (unsigned value : {0xd800u, 0xdfffu, 0x110000u, 0xffffffffu})
		EXPECT_EQ(nb_utf8_encode(value, text), 0u);
	EXPECT_EQ(nb_utf8_encode(0, text), 1u);
	EXPECT_EQ(text[0], '\0');
}

TEST(Utf8ProcTest, BoundedHelpersDoNotReadPastInputOrSplitCharacters)
{
	const char partial[] = {'\xf0', '\x9f', '\x98'};
	unsigned cp = 0;
	size_t count = 0;
	EXPECT_EQ(nb_utf8_read(partial, sizeof(partial), &cp, &count), 0);
	size_t offset = 1;
	EXPECT_NE(nb_utf8_validate(nb_str_from_parts(partial, sizeof(partial)), &offset),
		  NB_UTF8_OK);
	EXPECT_EQ(offset, 0u);
	const char text[] = "A中😀Z";
	EXPECT_EQ(nb_utf8_clamp_bytes(text, 3), 1u);
	EXPECT_EQ(nb_utf8_clamp_bytes(text, 4), 4u);
	EXPECT_EQ(nb_utf8_clamp_bytes(text, 7), 4u);
	EXPECT_EQ(nb_utf8_clamp_bytes(text, 8), 8u);
	EXPECT_EQ(nb_utf8_count(nb_str_from_cstr(text)), 4u);
	EXPECT_EQ(nb_utf8_prev(text, text + 8), text + 4);
	EXPECT_EQ(nb_utf8_prev(text, text + 4), text + 1);
}

TEST(Utf8ProcTest, WidthComesFromUnicodeDataWithoutLocaleSetup)
{
	EXPECT_EQ(nb_utf8_width_cp('A'), 1);
	EXPECT_EQ(nb_utf8_width_cp(0x4e2d), 2);
	EXPECT_EQ(nb_utf8_width_cp(0x1f600), 2);
	EXPECT_EQ(nb_utf8_width_cp(0x0301), 0);
	EXPECT_EQ(nb_utf8_width_cp(0x200d), 0);
	EXPECT_EQ(nb_utf8_width_cp(0x1b), 0);
	EXPECT_EQ(nb_utf8_width_cp(0xd800), -1);
	EXPECT_EQ(nb_utf8_width_cstr("e\xcc\x81中"), 3u);
	EXPECT_EQ(nb_utf8_is_unicode_space_cp(0x202f), 1);
	EXPECT_EQ(nb_utf8_is_unicode_space_cp(' '), 0);
}

TEST(Utf8ProcTest, InvalidPrefixesDoNotEraseValidSanitizedSuffixes)
{
	char output[64];
	const char input[] = "A\xed\xa0\x80" "B\xf4\x90\x80\x80" "C";
	size_t length = nb_utf8_sanitize_into(output, input, sizeof(input) - 1);
	output[length] = '\0';
	EXPECT_STREQ(output, "ABC");
	EXPECT_EQ(nb_utf8_copy_sanitized_clamped(output, sizeof(output), input, sizeof(input)), 3u);
	EXPECT_STREQ(output, "ABC");
}

TEST(Utf8TerminalSanitizeTest, InvalidChunkPrefixPreservesFollowingText)
{
	struct morph_terminal_sanitizer sanitizer;
	nb_buf output = {};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 32) ? 0 : NB_ERR_NOMEM), 0);
	morph_terminal_sanitizer_init(&sanitizer, MORPH_TERMINAL_TEXT_MULTILINE);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, "\xe4", 1, 0), 0);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, "X\xc3\xa9Y", 4, 1), 0);
	EXPECT_STREQ(nb_buf_cstr(&output), "XéY");
	nb_buf_clear(&output);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, "\xe4", 1, 0), 0);
	ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &output, "X", 1, 1), 0);
	EXPECT_STREQ(nb_buf_cstr(&output), "X");
	nb_buf_cleanup(&output);
}

TEST(Utf8TerminalSanitizeTest, EveryChunkBoundaryMatchesWholeInput)
{
	for (const char *input : {"A中😀éZ", "A\xe4X", "\xe4X\xc3\xa9Y",
		"\xe4X\xe4X\xe4X", "A\033[31m中\033[0m\r\nB", "end\xf0\x9f\x98"}) {
		SCOPED_TRACE(input);
		size_t len = strlen(input);
		char *expected = morph_terminal_sanitize_dup(
			input, len, MORPH_TERMINAL_TEXT_MULTILINE, nullptr);
		ASSERT_NE(expected, nullptr);
		for (size_t split = 0; split <= len; split++) {
			SCOPED_TRACE(split);
			struct morph_terminal_sanitizer sanitizer;
			nb_buf out = {};
			ASSERT_EQ((nb_buf_init(&out), nb_buf_reserve(&out, 64) ? 0 : NB_ERR_NOMEM),
				  0);
			morph_terminal_sanitizer_init(&sanitizer, MORPH_TERMINAL_TEXT_MULTILINE);
			ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &out, input, split, 0),
				  0);
			ASSERT_EQ(morph_terminal_sanitize_feed(&sanitizer, &out, input + split,
							       len - split, 1),
				  0);
			EXPECT_STREQ(nb_buf_cstr(&out), expected);
			nb_buf_cleanup(&out);
		}
		free(expected);
	}
}

TEST(Utf8ProcTest, ClassificationPreservesUnicodeSemanticsAtTheAdapterBoundary)
{
	EXPECT_EQ(nb_utf8_classify_codepoint(0x0391), NB_UTF8_CLASS_UPPER);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x01c5), NB_UTF8_CLASS_UPPER);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x0431), NB_UTF8_CLASS_LOWER);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x4e2d), NB_UTF8_CLASS_LETTER);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x0301), NB_UTF8_CLASS_MARK);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x09e7), NB_UTF8_CLASS_NUMBER);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x202f), NB_UTF8_CLASS_SEPARATOR);
	EXPECT_EQ(nb_utf8_classify_codepoint(0x2028), NB_UTF8_CLASS_SEPARATOR);
	for (unsigned cp : {0xd800u, 0x110000u, 0xffffffffu}) {
		EXPECT_EQ(nb_utf8_classify_codepoint(cp), NB_UTF8_CLASS_OTHER);
		EXPECT_EQ(nb_utf8_is_emoji_component_cp(cp), 0);
	}
}

TEST(Utf8ProcTest, ContinuationBytesAreDistinctFromMalformedLeadingBytes)
{
	for (unsigned byte = 0; byte <= 0xff; byte++) {
		SCOPED_TRACE(byte);
		EXPECT_EQ(nb_utf8_is_continuation_byte(static_cast<unsigned char>(byte)),
			  byte >= 0x80 && byte <= 0xbf);
	}
}
