#include <gtest/gtest.h>

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
