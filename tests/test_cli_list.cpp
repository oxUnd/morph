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
#include <fstream>
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

TEST(CliListTest, SyncQueriesUseGroupedTrees)
{
	char pattern[] = "/tmp/morph-cli-sync-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	struct morph_sync_backup *backups = nullptr;
	int backup_count = 0;
	std::string root;
	std::string database;
	std::string config_path;
	std::string remote;
	std::ofstream config;
	std::ofstream payload;

	ASSERT_NE(directory, nullptr);
	root = directory;
	database = root + "/data.db";
	config_path = root + "/config.toml";
	remote = root + "/remote";
	config.open(config_path);
	ASSERT_TRUE(config.is_open());
	config << "[sync]\n"
	       << "enabled = true\n"
	       << "dir = \"" << remote << "\"\n"
	       << "interval_seconds = 0\n"
	       << "retention_days = 30\n"
	       << "include = [\"data.db\", \"payload.txt\"]\n";
	config.close();
	ASSERT_TRUE(config.good());
	payload.open(root + "/payload.txt");
	payload << "first";
	payload.close();
	ASSERT_TRUE(payload.good());
	options.db_path = database.c_str();
	options.config_path = config_path.c_str();
	options.workdir = root.c_str();
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	cli_command_registry_clear();
	ASSERT_EQ(cli_register_sync_commands(), 0);

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/sync now");
	std::string status_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(status_output.find("sync status"), std::string::npos);
	EXPECT_NE(status_output.find("state"), std::string::npos);
	EXPECT_NE(status_output.find("files"), std::string::npos);
	EXPECT_NE(status_output.find("database"), std::string::npos);
	EXPECT_NE(status_output.find("├"), std::string::npos);
	EXPECT_NE(status_output.find("└"), std::string::npos);
	EXPECT_NE(status_output.find("snapshots"), std::string::npos);

	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	std::string conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (0)"), std::string::npos);
	EXPECT_NE(conflict_output.find("└ none"), std::string::npos);
	payload.open(root + "/payload.txt", std::ios::trunc);
	payload << "local change";
	payload.close();
	payload.open(remote + "/data/payload.txt", std::ios::trunc);
	payload << "remote change";
	payload.close();
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync now");
	(void)testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (1)"), std::string::npos);
	EXPECT_NE(conflict_output.find("#1"), std::string::npos);
	EXPECT_NE(conflict_output.find("payload.txt"), std::string::npos);

	ASSERT_EQ(runtime_sync_backups(runtime, "data.db", &backups,
		&backup_count), 0);
	ASSERT_EQ(backup_count, 1);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync backups");
	std::string backup_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(backup_output.find("database backups (1)"),
		  std::string::npos);
	EXPECT_NE(backup_output.find("data.db"), std::string::npos);
	EXPECT_NE(backup_output.find(backups[0].snapshot_id), std::string::npos);
	EXPECT_NE(backup_output.find("created"), std::string::npos);
	morph_sync_backups_free(backups);

	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(root, error);
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
