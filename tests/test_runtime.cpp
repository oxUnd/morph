#include <gtest/gtest.h>

extern "C" {
#include "runtime/runtime.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sqlite3.h>

static int runtime_test_task_runner(const scheduled_task *,
				    scheduled_task_action_result *, void *)
{
	return 0;
}

class RuntimeLifecycleTest : public ::testing::Test {
protected:
	char directory[PATH_MAX] = {0};
	char database[PATH_MAX] = {0};

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-runtime-test-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		std::snprintf(directory, sizeof(directory), "%s", created);
		std::snprintf(database, sizeof(database), "%s/data.db", created);
	}

	void TearDown() override
	{
		std::remove(database);
		rmdir(directory);
	}
};

TEST_F(RuntimeLifecycleTest, OwnsSessionAndServices)
{
	runtime_options options{};
	runtime *instance = nullptr;
	session current{};
	session created{};
	session *sessions = nullptr;
	int count = 0;

	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_NE(instance, nullptr);
	EXPECT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_GT(current.id, 0);
	EXPECT_EQ(runtime_session_create_and_select(instance, "second", &created),
		  0);
	EXPECT_STREQ(created.name, "second");
	EXPECT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	EXPECT_GE(count, 2);
	runtime_session_list_free(sessions);
	runtime_close(instance);
}

TEST_F(RuntimeLifecycleTest, RejectsInvalidOpenArguments)
{
	runtime *instance = nullptr;
	runtime_options options{};

	EXPECT_EQ(runtime_open(nullptr, &instance), -EINVAL);
	EXPECT_EQ(runtime_open(&options, &instance), -EINVAL);
}

TEST_F(RuntimeLifecycleTest, CloseStopsOwnedTaskWorker)
{
	runtime_options options{};
	runtime *instance = nullptr;

	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_task_scheduler_start(instance, runtime_test_task_runner,
					       nullptr,
					       nullptr, nullptr, 10), 0);
	EXPECT_EQ(runtime_task_scheduler_running(instance), 1);
	EXPECT_EQ(runtime_task_scheduler_start(instance, runtime_test_task_runner,
					       nullptr,
					       nullptr, nullptr, 10), 0);
	runtime_task_scheduler_stop(instance);
	EXPECT_EQ(runtime_task_scheduler_running(instance), 0);
	runtime_task_scheduler_stop(instance);
	ASSERT_EQ(runtime_task_scheduler_start(instance, runtime_test_task_runner,
					       nullptr,
					       nullptr, nullptr, 10), 0);
	runtime_close(instance);
}

TEST_F(RuntimeLifecycleTest, ReportsConfiguredPathsAndRejectsInvalidDatabase)
{
	runtime_options options{};
	runtime *instance = nullptr;
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	char resolved[PATH_MAX];
	ASSERT_NE(realpath(directory, resolved), nullptr);
	EXPECT_STREQ(runtime_workdir_get(instance), resolved);
	EXPECT_NE(runtime_config_get(instance), nullptr);
	std::string expected_output = std::string(resolved) + "/output";
	EXPECT_STREQ(runtime_config_get(instance)->general.output_dir,
		     expected_output.c_str());
	runtime_close(instance);
	instance = nullptr;
	options.db_path = "/definitely/missing/morph/data.db";
	EXPECT_NE(runtime_open(&options, &instance), 0);
	EXPECT_EQ(instance, nullptr);
}

TEST_F(RuntimeLifecycleTest, KeepsConfiguredOutputWithoutExplicitWorkdir)
{
	runtime_options options{};
	runtime *instance = nullptr;
	std::string config_path = std::string(directory) + "/config.toml";
	std::string configured_output = std::string(directory) + "/configured-output";
	FILE *config = std::fopen(config_path.c_str(), "w");
	ASSERT_NE(config, nullptr);
	std::fprintf(config, "[general]\noutput_dir = \"%s\"\n",
		     configured_output.c_str());
	ASSERT_EQ(std::fclose(config), 0);

	options.config_path = config_path.c_str();
	options.db_path = database;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_NE(runtime_config_get(instance), nullptr);
	EXPECT_STREQ(runtime_config_get(instance)->general.output_dir,
		     configured_output.c_str());
	runtime_close(instance);
	instance = nullptr;
	std::remove(config_path.c_str());
}

TEST_F(RuntimeLifecycleTest, WorkdirOverridesConfiguredOutputWithOutputChild)
{
	runtime_options options{};
	runtime *instance = nullptr;
	std::string config_path = std::string(directory) + "/config.toml";
	FILE *config = std::fopen(config_path.c_str(), "w");
	ASSERT_NE(config, nullptr);
	std::fprintf(config,
		     "[general]\noutput_dir = \"/tmp/ignored-output\"\n");
	ASSERT_EQ(std::fclose(config), 0);

	options.config_path = config_path.c_str();
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	char resolved[PATH_MAX];
	ASSERT_NE(realpath(directory, resolved), nullptr);
	std::string expected_output = std::string(resolved) + "/output";
	ASSERT_NE(runtime_config_get(instance), nullptr);
	EXPECT_STREQ(runtime_config_get(instance)->general.output_dir,
		     expected_output.c_str());
	runtime_close(instance);
	instance = nullptr;
	std::remove(config_path.c_str());
}

TEST_F(RuntimeLifecycleTest, RestoresMostRecentSession)
{
	runtime_options options{};
	runtime *instance = nullptr;
	struct session created{};
	struct session restored{};
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_session_create_and_select(instance, "restore-me", &created), 0);
	runtime_close(instance);
	instance = nullptr;
	sqlite3 *db = nullptr;
	ASSERT_EQ(sqlite3_open(database, &db), SQLITE_OK);
	std::string sql = "UPDATE sessions SET updated_at=4102444800 WHERE id=" +
		std::to_string(created.id);
	ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	options.restore_recent_session = 1;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_session_current(instance, &restored), 0);
	EXPECT_EQ(restored.id, created.id);
	EXPECT_STREQ(restored.name, "restore-me");
	runtime_close(instance);
}
