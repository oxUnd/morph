#include <gtest/gtest.h>
#include "config.h"
#include "util/file.h"
#include <cstdio>
#include <cstring>

class ConfigTest : public ::testing::Test {
protected:
	char config_path[256];
	void SetUp() override {
		snprintf(config_path, sizeof(config_path), "/tmp/ma_test_config_%d.toml", getpid());
	}
	void TearDown() override {
		std::remove(config_path);
	}
};

TEST_F(ConfigTest, DefaultValues) {
	struct config cfg;
	config_set_defaults(&cfg);
	EXPECT_STREQ(cfg.general.default_session, "default");
	EXPECT_STREQ(cfg.general.log_level, "info");
	EXPECT_STREQ(cfg.models.text.provider, "openai");
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
	EXPECT_EQ(cfg.models.text.context_limit, 128000);
	EXPECT_EQ(cfg.models.text.timeout_seconds, 300);
	EXPECT_EQ(cfg.react.max_iterations, 10);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 300);
	EXPECT_EQ(cfg.react.tool_max_retries, 3);
	EXPECT_EQ(cfg.react.bash_exec_enabled, 0);
	EXPECT_EQ(cfg.react.bash_exec_default_timeout, 60);
	EXPECT_EQ(cfg.react.bash_exec_allowed_commands_count, 0);
	EXPECT_EQ(cfg.react.bash_exec_allowed_cwds_count, 0);
	EXPECT_DOUBLE_EQ(cfg.context.summarize_threshold_ratio, 0.8);
	EXPECT_DOUBLE_EQ(cfg.context.compress_target_ratio, 0.5);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 6);
	EXPECT_EQ(cfg.credits.daily_limit, -1);
	EXPECT_STREQ(cfg.credits.currency, "USD");
	EXPECT_DOUBLE_EQ(cfg.credits.cost_to_credit_coef, 1000.0);
	EXPECT_EQ(cfg.credits.price_count, 0);
	EXPECT_EQ(cfg.sync.enabled, 0);
	EXPECT_EQ(cfg.sync.interval_seconds, 300);
	EXPECT_EQ(cfg.sync.retention_days, 30);
	ASSERT_EQ(cfg.sync.include_count, 6);
	EXPECT_STREQ(cfg.sync.include[0], "config.toml");
	EXPECT_STREQ(cfg.sync.include[1], "data.db");
}

TEST_F(ConfigTest, LoadFromFile) {
	const char *toml = R"(
[general]
default_session = "my_session"
log_level = "debug"

[model.text]
provider = "deepseek"
model = "deepseek-chat"
context_limit = 200000

[react]
max_iterations = 5
tool_timeout_seconds = 45
bash_exec_enabled = true
bash_exec_allowed_commands = ["cmake --build build", "ctest --output-on-failure"]
bash_exec_allowed_cwds = ["/tmp"]

[context]
keep_recent_rounds = 10

[sync]
enabled = true
dir = "/tmp/morph-sync"
interval_seconds = 60
retention_days = 14
include = ["config.toml", "output"]
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "my_session");
	EXPECT_STREQ(cfg.general.log_level, "debug");
	EXPECT_STREQ(cfg.models.text.provider, "deepseek");
	EXPECT_STREQ(cfg.models.text.model, "deepseek-chat");
	EXPECT_EQ(cfg.models.text.context_limit, 200000);
	EXPECT_EQ(cfg.react.max_iterations, 5);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 45);
	EXPECT_EQ(cfg.react.bash_exec_enabled, 1);
	EXPECT_EQ(cfg.react.bash_exec_allowed_commands_count, 2);
	EXPECT_STREQ(cfg.react.bash_exec_allowed_commands[0],
		     "cmake --build build");
	EXPECT_EQ(cfg.react.bash_exec_allowed_cwds_count, 1);
	EXPECT_STREQ(cfg.react.bash_exec_allowed_cwds[0], "/tmp");
	EXPECT_EQ(cfg.context.keep_recent_rounds, 10);
	EXPECT_EQ(cfg.sync.enabled, 1);
	EXPECT_STREQ(cfg.sync.dir, "/tmp/morph-sync");
	EXPECT_EQ(cfg.sync.interval_seconds, 60);
	EXPECT_EQ(cfg.sync.retention_days, 14);
	ASSERT_EQ(cfg.sync.include_count, 2);
	EXPECT_STREQ(cfg.sync.include[1], "output");
}

TEST_F(ConfigTest, LoadNonexistent) {
	struct config cfg;
	int rc = config_load(&cfg, "/tmp/nonexistent_config_xyz.toml");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "default");
}

TEST_F(ConfigTest, LoadCreditsConfig) {
	const char *toml = R"(
[credits]
daily_limit = 100
currency = "CNY"
cost_to_credit_coef = 7.5
input_token_credit_coef = 0.01
output_token_credit_coef = 0.02
image_unit_credit_coef = 3.0
video_second_credit_coef = 4.0

[[credits.prices]]
provider = "openai"
model = "gpt-test"
kind = "model_text"
input_per_million = 2.0
output_per_million = 10.0
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.credits.daily_limit, 100);
	EXPECT_STREQ(cfg.credits.currency, "CNY");
	EXPECT_DOUBLE_EQ(cfg.credits.cost_to_credit_coef, 7.5);
	EXPECT_DOUBLE_EQ(cfg.credits.input_token_credit_coef, 0.01);
	EXPECT_DOUBLE_EQ(cfg.credits.output_token_credit_coef, 0.02);
	EXPECT_DOUBLE_EQ(cfg.credits.image_unit_credit_coef, 3.0);
	EXPECT_DOUBLE_EQ(cfg.credits.video_second_credit_coef, 4.0);
	ASSERT_EQ(cfg.credits.price_count, 1);
	EXPECT_STREQ(cfg.credits.prices[0].provider, "openai");
	EXPECT_STREQ(cfg.credits.prices[0].model, "gpt-test");
	EXPECT_STREQ(cfg.credits.prices[0].kind, "model_text");
	EXPECT_DOUBLE_EQ(cfg.credits.prices[0].input_per_million, 2.0);
	EXPECT_DOUBLE_EQ(cfg.credits.prices[0].output_per_million, 10.0);
}

TEST_F(ConfigTest, LoadNull) {
	struct config cfg;
	int rc = config_load(&cfg, nullptr);
	EXPECT_NE(rc, 0);
}

TEST_F(ConfigTest, PrintConfig) {
	struct config cfg;
	config_set_defaults(&cfg);
	EXPECT_NO_FATAL_FAILURE(config_print(&cfg));
}

TEST_F(ConfigTest, PrintNull) {
	EXPECT_NO_FATAL_FAILURE(config_print(nullptr));
}

TEST_F(ConfigTest, PartialConfig) {
	const char *toml = R"(
[general]
default_session = "partial_test"

[react]
max_iterations = 20
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "partial_test");
	EXPECT_EQ(cfg.react.max_iterations, 20);
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
}

TEST_F(ConfigTest, CommentLines) {
	const char *toml = R"(
# this is a comment
[general]
# another comment
default_session = "comment_test"
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "comment_test");
}

TEST_F(ConfigTest, McpServerAutoConnect) {
	const char *toml = R"(
[[mcp.servers]]
name = "auto-srv"
transport = "stdio"
command = "npx"
args = ["-y", "some-server"]
auto_connect = true
connect_timeout = 30

[[mcp.servers]]
name = "lazy-srv"
transport = "stdio"
command = "npx"
args = ["-y", "other-server"]
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.mcp.server_count, 2);
	EXPECT_EQ(cfg.mcp.servers[0].auto_connect, 1);
	EXPECT_EQ(cfg.mcp.servers[0].connect_timeout, 30);
	EXPECT_EQ(cfg.mcp.servers[1].auto_connect, 0);
	EXPECT_EQ(cfg.mcp.servers[1].connect_timeout, 0);
}
