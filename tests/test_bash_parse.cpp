#include <gtest/gtest.h>

extern "C" {
#include "util/bash_parse.h"
}

TEST(BashParseTest, ExtractsCommandAfterEnvironmentAssignments)
{
	char name[BASH_PARSE_COMMAND_NAME_MAX];
	char program[PATH_MAX];

	ASSERT_EQ(bash_parse_command_name(
		"LARK_NO_UPDATE=1 PROFILE='user profile' "
		"/usr/local/bin/lark-cli auth status",
		name, sizeof(name)), 0);
	EXPECT_STREQ(name, "lark-cli");
	ASSERT_EQ(bash_parse_command_program(
		"LARK_NO_UPDATE=1 /usr/local/bin/lark-cli auth status",
		program, sizeof(program)), 0);
	EXPECT_STREQ(program, "/usr/local/bin/lark-cli");
}

TEST(BashParseTest, AcceptsComplexEnvironmentAssignment)
{
	char name[BASH_PARSE_COMMAND_NAME_MAX];

	ASSERT_EQ(bash_parse_command_name(
		"PROFILE=\"a\\\" b\" lark-cli auth status",
		name, sizeof(name)), 0);
	EXPECT_STREQ(name, "lark-cli");
}

TEST(BashParseTest, QuotedAmpersandIsNotCompound)
{
	struct bash_parse_result result;

	ASSERT_EQ(bash_parse_analyze(
		"lark-cli auth qrcode "
		"\"https://example.test/verify?flow_id=x&user_code=y\" "
		"--output ./qrcode.png",
		&result), 0);
	EXPECT_FALSE(result.has_error);
	EXPECT_FALSE(result.is_compound);
	ASSERT_EQ(result.commands.nelts, 1u);
	const auto *command = static_cast<const struct bash_parse_command *>(
		morph_array_get(&result.commands, 0));
	ASSERT_NE(command, nullptr);
	EXPECT_STREQ(command->name, "lark-cli");
	bash_parse_result_cleanup(&result);
}

TEST(BashParseTest, CollectsPipelineAndCommandSubstitution)
{
	struct bash_parse_result result;

	ASSERT_EQ(bash_parse_analyze(
		"printf '%s' \"$(echo value)\" | rm -f target",
		&result), 0);
	EXPECT_TRUE(result.is_compound);
	ASSERT_EQ(result.commands.nelts, 3u);
	const auto *first = static_cast<const struct bash_parse_command *>(
		morph_array_get(&result.commands, 0));
	const auto *nested = static_cast<const struct bash_parse_command *>(
		morph_array_get(&result.commands, 1));
	const auto *last = static_cast<const struct bash_parse_command *>(
		morph_array_get(&result.commands, 2));
	ASSERT_NE(first, nullptr);
	ASSERT_NE(nested, nullptr);
	ASSERT_NE(last, nullptr);
	EXPECT_STREQ(first->name, "printf");
	EXPECT_STREQ(nested->name, "echo");
	EXPECT_STREQ(last->name, "rm");
	bash_parse_result_cleanup(&result);
}

TEST(BashParseTest, RejectsDynamicCommandName)
{
	char name[BASH_PARSE_COMMAND_NAME_MAX];

	EXPECT_NE(bash_parse_command_name("$COMMAND argument",
					  name, sizeof(name)), 0);
}

TEST(BashParseTest, SemicolonSeparatesCommands)
{
	struct bash_parse_result result;

	ASSERT_EQ(bash_parse_analyze("echo ok; rm target", &result), 0);
	EXPECT_TRUE(result.is_compound);
	ASSERT_EQ(result.commands.nelts, 2u);
	bash_parse_result_cleanup(&result);
}
