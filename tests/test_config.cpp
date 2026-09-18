#include <gtest/gtest.h>
#include "config/config.h"
#include "util/file.h"
#include "util/error.h"
#include <cstdio>
#include <cstring>
#include <fstream>
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
	EXPECT_STREQ(cfg.models.video.provider, "volcengine");
	EXPECT_STREQ(cfg.models.video.adapter, "");
	EXPECT_STREQ(cfg.models.video.model, "doubao-seedance-2-0-260128");
	EXPECT_EQ(cfg.models.video.poll_interval_seconds, 5);
	EXPECT_EQ(cfg.models.video.poll_timeout_seconds, 600);
	EXPECT_EQ(cfg.models.text.retry_count, 3);
	EXPECT_EQ(cfg.models.vision.retry_count, 3);
	EXPECT_EQ(cfg.react.max_iterations, 10);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 300);
	EXPECT_EQ(cfg.react.tool_max_retries, 3);
	EXPECT_EQ(cfg.react.guardrail_max_retries, 2);
	EXPECT_EQ(cfg.react.guardrail_max_empty_rounds, 3);
	EXPECT_EQ(cfg.react.disabled_tools_count, 0);
	EXPECT_STREQ(cfg.exec.shell, "/bin/bash");
	EXPECT_EQ(cfg.exec.default_timeout_ms, 120000);
	EXPECT_EQ(cfg.exec.yield_time_ms, 10000);
	EXPECT_EQ(cfg.exec.max_inline_output, 32768);
	EXPECT_EQ(cfg.exec.max_session_output, 1048576);
	EXPECT_EQ(cfg.exec.kill_grace_ms, 500);
	EXPECT_EQ(cfg.exec.network, 0);
	EXPECT_EQ(cfg.react.request_permissions_enabled, 1);
	EXPECT_STREQ(cfg.react.permission_active_profile, "");
	EXPECT_EQ(cfg.react.permission_profile_count, 0);
	EXPECT_DOUBLE_EQ(cfg.context.summarize_threshold_ratio, 0.8);
	EXPECT_DOUBLE_EQ(cfg.context.compress_target_ratio, 0.5);
	EXPECT_EQ(cfg.context.in_turn_compaction, 1);
	EXPECT_EQ(cfg.context.protocol_reserve_tokens, 4096);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 6);
	EXPECT_EQ(cfg.context.tool_result_max_tokens, 8000);
	EXPECT_EQ(cfg.context.compaction_user_message_tokens, 20000);
	EXPECT_EQ(cfg.context.compaction_summary_max_tokens, 6000);
	EXPECT_STREQ(cfg.context.compaction_prompt_file, "");
	EXPECT_EQ(cfg.context.compaction_warning_count, 3);
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
	EXPECT_STREQ(cfg.prompt.mode, "append");
}

TEST_F(ConfigTest, PromptModesAndIndependentSubAgentSources)
{
	const char *toml = R"(
[prompt]
mode = "replace"
system_prompt_file = "/tmp/main.txt"
system_prompt_dir = "/tmp/main.d"
[[agent.sub_agents]]
name = "custom"
system_prompt_mode = "replace"
system_prompt_file = "/tmp/child.txt"
system_prompt_dir = "/tmp/child.d"
[[agent.sub_agents]]
name = "legacy"
system_prompt_file = "/tmp/legacy.txt"
)";
	ASSERT_EQ(file_write_all(config_path, toml, strlen(toml)), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, config_path), 0);
	EXPECT_STREQ(cfg.prompt.mode, "replace");
	EXPECT_STREQ(cfg.prompt.system_prompt_file, "/tmp/main.txt");
	EXPECT_STREQ(cfg.prompt.system_prompt_dir, "/tmp/main.d");
	ASSERT_EQ(cfg.sub_agents.count, 2);
	EXPECT_STREQ(cfg.sub_agents.entries[0].system_prompt_mode, "replace");
	EXPECT_STREQ(cfg.sub_agents.entries[0].system_prompt_dir, "/tmp/child.d");
	EXPECT_STREQ(cfg.sub_agents.entries[1].system_prompt_mode, "append");
}

TEST_F(ConfigTest, PromptRejectsUnknownModes)
{
	for (const char *toml : {
	     "[prompt]\nmode = \"replcae\"\n",
	     "[[agent.sub_agents]]\nname = \"child\"\nsystem_prompt_mode = \"off\"\n"}) {
		ASSERT_EQ(file_write_all(config_path, toml, strlen(toml)), 0);
		struct config cfg;
		EXPECT_NE(config_load(&cfg, config_path), 0);
	}
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

TEST_F(ConfigTest, EmptyDisabledToolsLeavesAllToolsEnabled) {
	const char *toml = R"(
[react]
disabled_tools = []
)";
	file_write_all(config_path, toml, strlen(toml));

	struct config cfg;
	ASSERT_EQ(config_load(&cfg, config_path), 0);
	EXPECT_EQ(cfg.react.disabled_tools_count, 0);
	EXPECT_STREQ(cfg.react.disabled_tools[0], "");
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
extra_body_json = '''{"reasoning_effort":"high","chat_template_kwargs":{"enable_thinking":true}}'''

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

[model.video]
provider = "volcengine"
adapter = "volcengine-videos"
model = "doubao-seedance-2-0-fast-260128"

[exec]
shell = "/bin/zsh"
default_timeout_ms = 0
yield_time_ms = 2500
max_inline_output = 16384
max_session_output = 524288
kill_grace_ms = 750
network = true

[react]
max_iterations = 5
tool_timeout_seconds = 45

[react.permissions]
active_profile = "developer"
request_tool_enabled = false

[[react.permission_profiles]]
name = "developer"
workspace_roots = ["/tmp"]
write_paths = ["/tmp"]
delete_paths = ["/tmp"]

[context]
keep_recent_rounds = 10
tool_result_max_tokens = 4096
compaction_user_message_tokens = 12000
compaction_summary_max_tokens = 3000
compaction_prompt_file = "/tmp/compact-prompt.md"
compaction_warning_count = 5

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
	EXPECT_STREQ(cfg.models.text.extra_body_json,
		     "{\"reasoning_effort\":\"high\",\"chat_template_kwargs\":"
		     "{\"enable_thinking\":true}}");
	EXPECT_STREQ(cfg.models.vision.provider, "openai");
	EXPECT_STREQ(cfg.models.vision.model, "gpt-4o");
	EXPECT_EQ(cfg.models.vision.max_tokens, 2048);
	EXPECT_EQ(cfg.models.vision.retry_count, 10);
	EXPECT_STREQ(cfg.models.image.provider, "openai");
	EXPECT_STREQ(cfg.models.image.adapter, "openai-images");
	EXPECT_STREQ(cfg.models.image.model, "gpt-image-2");
	EXPECT_STREQ(cfg.models.video.adapter, "volcengine-videos");
	EXPECT_STREQ(cfg.models.video.model,
		     "doubao-seedance-2-0-fast-260128");
	EXPECT_EQ(cfg.react.max_iterations, 5);
	EXPECT_EQ(cfg.react.tool_timeout_seconds, 45);
	EXPECT_STREQ(cfg.exec.shell, "/bin/zsh");
	EXPECT_EQ(cfg.exec.default_timeout_ms, 0);
	EXPECT_EQ(cfg.exec.yield_time_ms, 2500);
	EXPECT_EQ(cfg.exec.max_inline_output, 16384);
	EXPECT_EQ(cfg.exec.max_session_output, 524288);
	EXPECT_EQ(cfg.exec.kill_grace_ms, 750);
	EXPECT_EQ(cfg.exec.network, 1);
	EXPECT_EQ(cfg.react.request_permissions_enabled, 0);
	EXPECT_STREQ(cfg.react.permission_active_profile, "developer");
	ASSERT_EQ(cfg.react.permission_profile_count, 1);
	EXPECT_STREQ(cfg.react.permission_profiles[0].name, "developer");
	ASSERT_EQ(cfg.react.permission_profiles[0].workspace_roots_count, 1);
	EXPECT_STREQ(cfg.react.permission_profiles[0].workspace_roots[0],
		     "/tmp");
	ASSERT_EQ(cfg.react.permission_profiles[0].write_paths_count, 1);
	EXPECT_STREQ(cfg.react.permission_profiles[0].write_paths[0],
		     "/tmp");
	ASSERT_EQ(cfg.react.permission_profiles[0].delete_paths_count, 1);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 10);
	EXPECT_EQ(cfg.context.tool_result_max_tokens, 4096);
	EXPECT_EQ(cfg.context.compaction_user_message_tokens, 12000);
	EXPECT_EQ(cfg.context.compaction_summary_max_tokens, 3000);
	EXPECT_STREQ(cfg.context.compaction_prompt_file,
		     "/tmp/compact-prompt.md");
	EXPECT_EQ(cfg.context.compaction_warning_count, 5);
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
		"[exec]\nmax_inline_output = 100\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_RANGE);
}

TEST(ConfigValidationTest, ValidatesExecOutputLimits)
{
	struct config_validation_error error = {};

	EXPECT_EQ(config_validate_text(
		"[exec]\nmax_inline_output = 65536\n"
		"max_session_output = 32768\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_CONFLICT);
	EXPECT_STREQ(error.path, "exec.max_inline_output");
	EXPECT_EQ(config_validate_text(
		"[exec]\nshell = \"\"\n", &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
	EXPECT_STREQ(error.path, "exec.shell");
}

TEST(ConfigValidationTest, ValidatesModelExtraBodyJson)
{
	struct config_validation_error error = {};

	EXPECT_EQ(config_validate_text(
		"[model.text]\n"
		"extra_body_json = '{\"reasoning_effort\":\"high\","
		"\"chat_template_kwargs\":{\"enable_thinking\":true}}'\n",
		&error), 0);
	EXPECT_EQ(config_validate_text(
		"[model.text]\nextra_body_json = '{invalid}'\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
	EXPECT_STREQ(error.path, "model.text.extra_body_json");
	EXPECT_EQ(config_validate_text(
		"[model.text]\nextra_body_json = '[1, 2]'\n", &error),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_TYPE);
	EXPECT_EQ(config_validate_text(
		"[model.text]\nextra_body_json = '{\"stream\":false}'\n",
		&error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_CONFLICT);
	EXPECT_NE(std::string(error.message).find("reserved field 'stream'"),
		  std::string::npos);
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

TEST(ConfigValidationTest, ValidatesPermissionProfiles)
{
	struct config_validation_error error = {};
	const char *missing = R"(
[react.permissions]
active_profile = "missing"
)";
	const char *relative = R"(
[react.permissions]
active_profile = "developer"
[[react.permission_profiles]]
name = "developer"
write_paths = ["relative/cache"]
)";
	const char *valid = R"(
[react.permissions]
active_profile = "developer"
[[react.permission_profiles]]
name = "developer"
workspace_roots = ["~/Work"]
write_paths = ["/tmp/cache"]
)";

	EXPECT_EQ(config_validate_text(missing, &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_CONFLICT);
	EXPECT_STREQ(error.path, "react.permissions.active_profile");
	EXPECT_EQ(config_validate_text(relative, &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
	EXPECT_STREQ(error.path,
		     "react.permission_profiles[].write_paths");
	EXPECT_EQ(config_validate_text(valid, &error), 0);
}

TEST_F(ConfigTest, FileValidationReportsMissingActivePermissionDirectory)
{
	struct config_validation_error error = {};
	const char *missing =
		"/tmp/morph_directory_that_must_not_exist_for_config_test";
	std::ofstream file(config_path);

	file << "[react.permissions]\n"
	     << "active_profile = \"developer\"\n"
	     << "[[react.permission_profiles]]\n"
	     << "name = \"developer\"\n"
	     << "write_paths = [\"" << missing << "\"]\n";
	file.close();
	std::remove(missing);

	EXPECT_EQ(config_validate_file(config_path, &error), MORPH_ERR_CONFIG);
	EXPECT_EQ(error.code, CONFIG_VALIDATION_VALUE);
	EXPECT_STREQ(error.path,
		     "react.permission_profiles[].write_paths");
	EXPECT_NE(std::string(error.message).find(missing), std::string::npos);
	EXPECT_NE(std::string(error.message).find("No such file or directory"),
		  std::string::npos);
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
