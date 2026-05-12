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
	EXPECT_EQ(cfg.models.text.timeout_seconds, 60);
	EXPECT_EQ(cfg.react.max_iterations, 10);
	EXPECT_EQ(cfg.react.step_timeout_seconds, 60);
	EXPECT_EQ(cfg.react.tool_max_retries, 3);
	EXPECT_DOUBLE_EQ(cfg.context.summarize_threshold_ratio, 0.8);
	EXPECT_DOUBLE_EQ(cfg.context.compress_target_ratio, 0.5);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 6);
}

TEST_F(ConfigTest, LoadFromFile) {
	const char *toml = R"(
[general]
default_session = "my_session"
log_level = "debug"

[model.text]
provider = "anthropic"
model = "claude-3.5"
context_limit = 200000

[react]
max_iterations = 5
step_timeout_seconds = 30

[context]
keep_recent_rounds = 10
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "my_session");
	EXPECT_STREQ(cfg.general.log_level, "debug");
	EXPECT_STREQ(cfg.models.text.provider, "anthropic");
	EXPECT_STREQ(cfg.models.text.model, "claude-3.5");
	EXPECT_EQ(cfg.models.text.context_limit, 200000);
	EXPECT_EQ(cfg.react.max_iterations, 5);
	EXPECT_EQ(cfg.react.step_timeout_seconds, 30);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 10);
}

TEST_F(ConfigTest, LoadNonexistent) {
	struct config cfg;
	int rc = config_load(&cfg, "/tmp/nonexistent_config_xyz.toml");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(cfg.general.default_session, "default");
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
default_session = "comment_test
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
}