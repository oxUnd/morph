#include <gtest/gtest.h>
#include "config/config.h"
#include "util/file.h"
#include "util/error.h"
#include <cstdio>
#include <cstring>
#include <string>

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
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_EQ(cfg.models.vision.timeout_seconds, 300);
	EXPECT_EQ(cfg.models.text.timeout_seconds, 300);
	EXPECT_EQ(cfg.models.image.timeout_seconds, 290);
	EXPECT_EQ(cfg.models.text.retry_count, 3);
	EXPECT_EQ(cfg.models.vision.retry_count, 3);
	EXPECT_EQ(cfg.react.max_iterations, 10);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 300);
	EXPECT_EQ(cfg.react.tool_max_retries, 3);
	EXPECT_EQ(cfg.react.bash_exec_enabled, 0);
	EXPECT_EQ(cfg.react.bash_exec_default_timeout, 60);
	EXPECT_STREQ(cfg.react.bash_exec_mode, "server");
	EXPECT_EQ(cfg.react.bash_exec_allowed_commands_count, 0);
	EXPECT_EQ(cfg.react.bash_exec_allowed_cwds_count, 0);
	ASSERT_EQ(cfg.react.bash_exec_server_read_paths_count, 2);
	EXPECT_STREQ(cfg.react.bash_exec_server_read_paths[0], "@workdir");
	ASSERT_EQ(cfg.react.bash_exec_server_write_paths_count, 1);
	EXPECT_STREQ(cfg.react.bash_exec_server_write_paths[0], "@output");
	EXPECT_EQ(cfg.react.bash_exec_server_delete_paths_count, 0);
	EXPECT_EQ(cfg.react.bash_exec_server_network_access, 0);
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
	ASSERT_EQ(cfg.sync.include_count, 7);
	EXPECT_STREQ(cfg.sync.include[0], "config.toml");
	EXPECT_STREQ(cfg.sync.include[1], "data.db");
	EXPECT_STREQ(cfg.sync.include[6], "ui-history.db");
}

TEST_F(ConfigTest, MigratesLegacyDefaultSyncIncludes) {
	const char *toml = R"(
[sync]
include = ["config.toml", "data.db", "skills", "tools", "exts", "output"]
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	ASSERT_EQ(config_load(&cfg, config_path), 0);
	ASSERT_EQ(cfg.sync.include_count, 7);
	EXPECT_STREQ(cfg.sync.include[6], "ui-history.db");
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
retry_count = 7

[model.vision]
provider = "openai"
model = "gpt-4o"
api_base = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"
max_tokens = 2048
retry_count = 10

[model.image]
provider = "openai"
adapter = "openai-images"
model = "gpt-image-2"

[react]
max_iterations = 5
tool_timeout_seconds = 45
bash_exec_enabled = true
bash_exec_mode = "local"
bash_exec_allowed_commands = ["cmake --build build", "ctest --output-on-failure"]
bash_exec_allowed_cwds = ["/tmp"]

[react.bash_exec_server]
read_paths = ["@workdir", "/srv/reference"]
write_paths = ["@output", "/srv/cache"]
delete_paths = ["/srv/cache"]
network_access = true

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
	EXPECT_EQ(cfg.models.text.retry_count, 7);
	EXPECT_STREQ(cfg.models.vision.provider, "openai");
	EXPECT_STREQ(cfg.models.vision.model, "gpt-4o");
	EXPECT_EQ(cfg.models.vision.max_tokens, 2048);
	EXPECT_EQ(cfg.models.vision.retry_count, 10);
	EXPECT_STREQ(cfg.models.image.provider, "openai");
	EXPECT_STREQ(cfg.models.image.adapter, "openai-images");
	EXPECT_STREQ(cfg.models.image.model, "gpt-image-2");
	EXPECT_EQ(cfg.react.max_iterations, 5);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 45);
	EXPECT_EQ(cfg.react.bash_exec_enabled, 1);
	EXPECT_STREQ(cfg.react.bash_exec_mode, "local");
	EXPECT_EQ(cfg.react.bash_exec_allowed_commands_count, 2);
	EXPECT_STREQ(cfg.react.bash_exec_allowed_commands[0],
		     "cmake --build build");
	EXPECT_EQ(cfg.react.bash_exec_allowed_cwds_count, 1);
	EXPECT_STREQ(cfg.react.bash_exec_allowed_cwds[0], "/tmp");
	ASSERT_EQ(cfg.react.bash_exec_server_read_paths_count, 2);
	EXPECT_STREQ(cfg.react.bash_exec_server_read_paths[1],
		     "/srv/reference");
	ASSERT_EQ(cfg.react.bash_exec_server_write_paths_count, 2);
	EXPECT_STREQ(cfg.react.bash_exec_server_write_paths[1], "/srv/cache");
	ASSERT_EQ(cfg.react.bash_exec_server_delete_paths_count, 1);
	EXPECT_STREQ(cfg.react.bash_exec_server_delete_paths[0], "/srv/cache");
	EXPECT_EQ(cfg.react.bash_exec_server_network_access, 1);
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
cached_input_per_million = 0.0
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
	EXPECT_DOUBLE_EQ(cfg.credits.prices[0].cached_input_per_million, 0.0);
	EXPECT_EQ(cfg.credits.prices[0].cached_input_price_configured, 1);
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

TEST(ConfigValidationTest, ValidatesTextWithoutLoadingRuntime)
{
	struct config_validation_error error = {};

	EXPECT_EQ(config_validate_text("[general]\nlog_level = \"info\"\n", &error), 0);
	EXPECT_EQ(error.line, 0);
	EXPECT_STREQ(error.message, "");
}

TEST(ConfigValidationTest, ReportsTomlLine)
{
	struct config_validation_error error = {};

	EXPECT_LT(config_validate_text("[general]\nlog_level = \"unterminated\n", &error), 0);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_SYNTAX);
	EXPECT_EQ(error.line, 3);
	EXPECT_NE(std::string(error.message).find("line 3"), std::string::npos);
}

struct config_warning_state {
	int count;
	struct config_validation_error last;
};

static void collect_config_warning(
	const struct config_validation_error *warning, void *user_data)
{
	struct config_warning_state *state =
		static_cast<struct config_warning_state *>(user_data);

	state->count++;
	state->last = *warning;
}

TEST(ConfigValidationTest, WarnsAndContinuesForUnknownKeys)
{
	struct config_validation_error error = {};
	struct config_warning_state warnings = {};
	const char *text =
		"[general]\nlog_levle = \"info\"\n"
		"[react]\nmax_iterations = \"ten\"\n";

	EXPECT_EQ(config_validate_text_with_warnings(text, &error,
		collect_config_warning, &warnings), MORPH_ERR_CONFIG);
	EXPECT_EQ(warnings.count, 1);
	EXPECT_EQ(warnings.last.code, CONFIG_VALIDATION_UNKNOWN_KEY);
	EXPECT_STREQ(warnings.last.path, "general.log_levle");
	EXPECT_EQ(error.code, CONFIG_VALIDATION_TYPE);
	EXPECT_STREQ(error.path, "react.max_iterations");
	memset(&error, 0, sizeof(error));
	EXPECT_EQ(config_validate_text(
		"[general]\nlog_levle = \"info\"\n", &error), 0);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_NONE);
}

TEST(ConfigValidationTest, RejectsWrongTypesAndRanges)
{
	struct config_validation_error error = {};

	EXPECT_EQ(config_validate_text("[react]\nmax_iterations = \"ten\"\n",
		&error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_TYPE);
	EXPECT_EQ(config_validate_text("[model.text]\nretry_count = 11\n",
		&error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_RANGE);
	EXPECT_EQ(config_validate_text(
		"[react]\nbash_exec_mode = \"remote\"\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
}

TEST(ConfigValidationTest, ValidatesRelationsAgainstDefaults)
{
	struct config_validation_error error = {};

	EXPECT_EQ(config_validate_text(
		"[context]\ncompress_target_ratio = 0.9\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_CONFLICT);
	EXPECT_STREQ(error.path, "context.compress_target_ratio");
	EXPECT_EQ(config_validate_text(
		"[context]\nsummarize_threshold_ratio = 0.4\n", &error),
		MORPH_ERR_CONFIG);
}

TEST(ConfigValidationTest, RejectsRelativeBashExecServerPaths)
{
	struct config_validation_error error = {};
	const char *toml = R"(
[react.bash_exec_server]
read_paths = ["@workdir", "relative/path"]
)";

	EXPECT_EQ(config_validate_text(toml, &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
	EXPECT_STREQ(error.path, "react.bash_exec_server.read_paths");
}

TEST(ConfigValidationTest, ValidatesMcpRequirements)
{
	struct config_validation_error error = {};
	const char *invalid =
		"[[mcp.servers]]\nname = \"remote\"\ntransport = \"http\"\n";
	const char *valid =
		"[[mcp.servers]]\nname = \"remote\"\ntransport = \"http\"\n"
		"url = \"https://example.test/mcp\"\n"
		"env = { TOKEN = \"secret\" }\n";

	EXPECT_EQ(config_validate_text(invalid, &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_REQUIRED);
	EXPECT_STREQ(error.path, "mcp.servers[].url");
	EXPECT_EQ(config_validate_text(valid, &error), 0);
}

TEST_F(ConfigTest, IgnoresUnknownKeysInExistingFile)
{
	const char *toml = "[general]\nlog_levle = \"info\"\n";
	struct config_validation_error error = {};
	struct config cfg;

	ASSERT_EQ(file_write_all(config_path, toml, strlen(toml)), 0);
	EXPECT_EQ(config_validate_file(config_path, &error), 0);
	EXPECT_EQ(config_load(&cfg, config_path), 0);
	EXPECT_STREQ(cfg.general.log_level, "info");
}

TEST(ConfigValidationTest, DescribesStableSemanticPaths)
{
	const char *text = R"(
[model.text]
model = "gpt-test"
max_tokens = 42
features = ["vision", "tools"]

[[mcp.servers]]
name = "github"
transport = "http"
url = "https://example.test/mcp"

[[credits.prices]]
provider = "openai"
model = "gpt-test"
kind = "model_text"
input_per_million = 1.25
cached_input_per_million = 0.25
)";
	struct config_validation_error error = {};
	char *json = config_describe_text(text, &error);

	ASSERT_NE(json, nullptr);
	std::string result(json);
	free(json);
	EXPECT_NE(result.find("model.text.model"), std::string::npos);
	EXPECT_NE(result.find("\"path\":\"model.text.max_tokens\",\"kind\":\"int\",\"stable\":true,\"value\":42"), std::string::npos)
		<< result;
	EXPECT_NE(result.find("model.text.features"), std::string::npos);
	EXPECT_NE(result.find("mcp.servers[name=github].url"), std::string::npos);
	EXPECT_NE(result.find("credits.prices[provider=openai,model=gpt-test,kind=model_text].input_per_million"), std::string::npos);
	EXPECT_NE(result.find("credits.prices[provider=openai,model=gpt-test,kind=model_text].cached_input_per_million"), std::string::npos);
	EXPECT_NE(result.find("\"stable\":true"), std::string::npos);
}

TEST(ConfigValidationTest, ReportsUnstableUnknownArrayTables)
{
	const char *text = "[[custom.items]]\nvalue = 1\n";
	struct config_validation_error error = {};
	char *json = config_describe_text(text, &error);

	ASSERT_NE(json, nullptr);
	std::string result(json);
	free(json);
	EXPECT_NE(result.find("custom.items[index=0].value"), std::string::npos);
	EXPECT_NE(result.find("\"stable\":false"), std::string::npos);
}
