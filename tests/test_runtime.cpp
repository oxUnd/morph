#include <gtest/gtest.h>

extern "C" {
#include "event/event.h"
#include "runtime/runtime.h"
#include "util/error.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
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
		char output[PATH_MAX];

		std::snprintf(output, sizeof(output), "%s/output", directory);
		rmdir(output);
		std::remove(database);
		rmdir(directory);
	}
};

TEST_F(RuntimeLifecycleTest, RejectsInvalidConfigurationAtStartup)
{
	runtime_options options{};
	runtime *instance = nullptr;
	std::string config_path = std::string(directory) + "/config.toml";
	FILE *config = std::fopen(config_path.c_str(), "w");

	ASSERT_NE(config, nullptr);
	std::fprintf(config, "[react]\nmax_iterations = \"ten\"\n");
	ASSERT_EQ(std::fclose(config), 0);
	options.config_path = config_path.c_str();
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	EXPECT_EQ(runtime_open(&options, &instance), MORPH_ERR_CONFIG);
	EXPECT_EQ(instance, nullptr);
}

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

TEST_F(RuntimeLifecycleTest, EmitsStartupProgress)
{
	runtime_options options{};
	runtime *instance = nullptr;
	morph_event_recorder recorder{};
	std::string config_path = std::string(directory) + "/config.toml";
	FILE *config = std::fopen(config_path.c_str(), "w");

	ASSERT_NE(config, nullptr);
	unsetenv("MORPH_TEST_MISSING_MCP_TOKEN");
	std::fprintf(config,
		     "[mcp]\n"
		     "[[mcp.servers]]\n"
		     "name = \"broken-test\"\n"
		     "transport = \"http\"\n"
		     "url = \"https://example.invalid/mcp\"\n"
		     "auth_token_env = \"MORPH_TEST_MISSING_MCP_TOKEN\"\n"
		     "auto_connect = true\n");
	ASSERT_EQ(std::fclose(config), 0);
	ASSERT_EQ(morph_event_recorder_init(&recorder), 0);
	options.config_path = config_path.c_str();
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	options.auto_connect_mcp = 1;
	options.event_cb = morph_event_recorder_cb;
	options.event_user_data = &recorder;
	ASSERT_EQ(runtime_open(&options, &instance), 0);

	std::string events;
	for (size_t i = 0; i < morph_event_recorder_count(&recorder); i++) {
		events += morph_event_recorder_get(&recorder, i);
		events += "\n";
	}
	EXPECT_NE(events.find("\"name\":\"startup.begin\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.config\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.database\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.models\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.tools\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.mcp\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.session\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"startup.ready\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"mcp.connecting\""), std::string::npos);
	EXPECT_NE(events.find("\"name\":\"mcp.failed\""), std::string::npos);
	EXPECT_NE(events.find("\"server\":\"broken-test\""),
		  std::string::npos);
	EXPECT_NE(events.find("\"message\":\"Connecting to broken-test\""),
		  std::string::npos);
	EXPECT_NE(events.find("Missing MCP token: environment variable "
			      "'MORPH_TEST_MISSING_MCP_TOKEN' is not set"),
		  std::string::npos);

	runtime_close(instance);
	morph_event_recorder_cleanup(&recorder);
	std::remove(config_path.c_str());
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
	struct stat output_stat{};
	ASSERT_EQ(stat(expected_output.c_str(), &output_stat), 0);
	EXPECT_TRUE(S_ISDIR(output_stat.st_mode));
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

TEST_F(RuntimeLifecycleTest, DefersNewSessionUntilFirstTurn)
{
	runtime_options options{};
	runtime *instance = nullptr;
	struct session current{};
	struct session *sessions = nullptr;
	int count = 0;

	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	options.create_new_session = 1;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_EQ(current.id, 0);
	EXPECT_EQ(runtime_session_set_model(instance, "deferred-model"), 0);
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_STREQ(current.model, "deferred-model");
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	EXPECT_EQ(count, 0);
	runtime_session_list_free(sessions);
	runtime_close(instance);
}

TEST_F(RuntimeLifecycleTest, FirstTurnCreatesDeferredSession)
{
	runtime_options options{};
	runtime *instance = nullptr;
	runtime_request request{};
	runtime_result result{};
	struct session current{};
	struct session *sessions = nullptr;
	int count = 0;
	std::string config_path = std::string(directory) + "/config.toml";
	FILE *config = std::fopen(config_path.c_str(), "w");

	ASSERT_NE(config, nullptr);
	std::fprintf(config,
		"[model.text]\n"
		"provider = \"openai\"\n"
		"model = \"test-model\"\n"
		"api_key_env = \"MORPH_TEST_MISSING_API_KEY\"\n");
	ASSERT_EQ(std::fclose(config), 0);
	unsetenv("MORPH_TEST_MISSING_API_KEY");
	options.config_path = config_path.c_str();
	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	options.create_new_session = 1;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	request.model_input = "hello";
	request.stored_user_input = "hello";
	EXPECT_EQ(runtime_execute_turn(instance, &request, &result),
		  MORPH_ERR_NOT_CONFIGURED);
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_GT(current.id, 0);
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	EXPECT_EQ(count, 1);
	runtime_session_list_free(sessions);
	runtime_close(instance);
	std::remove(config_path.c_str());
}

TEST_F(RuntimeLifecycleTest, SwitchingBeforeFirstTurnCreatesNoExtraSession)
{
	runtime_options options{};
	runtime *instance = nullptr;
	struct session existing{};
	struct session selected{};
	struct session *sessions = nullptr;
	int count = 0;

	options.db_path = database;
	options.workdir = directory;
	options.front_name = "test";
	options.create_new_session = 1;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_session_create_and_select(instance, "existing",
						    &existing), 0);
	runtime_close(instance);
	instance = nullptr;
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	ASSERT_EQ(runtime_session_select_existing(instance, existing.id,
						  &selected), 0);
	EXPECT_EQ(selected.id, existing.id);
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	EXPECT_EQ(count, 1);
	runtime_session_list_free(sessions);
	runtime_close(instance);
}
