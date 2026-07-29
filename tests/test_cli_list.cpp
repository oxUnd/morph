#include <gtest/gtest.h>

extern "C" {
#include "runtime/runtime.h"
#include "sapi/cli/cli.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"
}

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <regex>
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

TEST(CliListTest, SessionRowKeepsRelatedFieldsTogether)
{
	testing::internal::CaptureStdout();
	cli_list_row("ae2c6f9a", "查一下开放平台、前端架构绩效",
		     "3,086 tokens · 07-29 15:49", 0, 0, 80);
	cli_list_row("a6f7b14c", "default", "0 tokens · 07-29 15:52",
		     1, 1, 80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("├"), std::string::npos);
	EXPECT_NE(output.find("└"), std::string::npos);
	EXPECT_NE(output.find("ae2c6f9a"), std::string::npos);
	EXPECT_NE(output.find("3,086 tokens"), std::string::npos);
	EXPECT_NE(output.find("07-29 15:49"), std::string::npos);
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_EQ(output.find("current"), std::string::npos);
	EXPECT_EQ(output.find("Model"), std::string::npos);
}

TEST(CliListTest, SessionCommandShowsUpdatedTimeWithoutCurrentLabel)
{
	char pattern[] = "/tmp/morph-cli-list-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	std::string database;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	cli_command_registry_clear();
	ASSERT_EQ(cli_register_session_commands(), 0);

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/list");
	std::string output = testing::internal::GetCapturedStdout();

	ASSERT_EQ(rc, 0);
	EXPECT_TRUE(std::regex_search(
		output, std::regex("[0-9]{2}-[0-9]{2} "
				   "[0-9]{2}:[0-9]{2}")));
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_EQ(output.find("current"), std::string::npos);

	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, TasksCommandUsesActiveAndCompactHistoryTrees)
{
	char pattern[] = "/tmp/morph-cli-tasks-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task active{};
	struct scheduled_task history{};
	std::string database;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	ASSERT_EQ(runtime_session_current(runtime, &current), 0);
	context.runtime = runtime;
	input.source_session_id = current.id;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) + 3600;
	input.max_attempts = 3;
	input.action_type = "agent_run";
	input.policy_json = "{}";
	input.notify_json = "{}";
	input.title = "active task";
	input.payload_json = "{\"prompt\":\"active task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &active), 0);
	input.title = "old task";
	input.payload_json = "{\"prompt\":\"old task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &history), 0);
	ASSERT_EQ(runtime_task_cancel(runtime, history.id), 0);

	cli_command_registry_clear();
	ASSERT_EQ(cli_register_task_commands(), 0);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/tasks");
	std::string output = testing::internal::GetCapturedStdout();

	ASSERT_EQ(rc, 0);
	EXPECT_NE(output.find("scheduled tasks (2)"), std::string::npos);
	EXPECT_NE(output.find("active"), std::string::npos);
	EXPECT_NE(output.find("history"), std::string::npos);
	EXPECT_NE(output.find("active task details"), std::string::npos);
	EXPECT_NE(output.find("old task"), std::string::npos);
	EXPECT_NE(output.find("cancelled"), std::string::npos);
	EXPECT_EQ(output.find("| ID |"), std::string::npos);

	scheduled_task_cleanup(&active);
	scheduled_task_cleanup(&history);
	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}
