#include <gtest/gtest.h>
#include "sapi/cli/setup.h"
#include "sapi/cli/cli.h"
#include "sapi/cli/setup_ui.h"
#include "sapi/cli/setup_config.h"
#include "agent/system_prompt.h"
#include <filesystem>
#include "config/config.h"
#include "util/file.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class CliSetupTest : public ::testing::Test {
protected:
	std::string dir;
	std::string path;
	std::string transcript;
	void SetUp() override {
		char pattern[] = "/tmp/morph-setup-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		dir = pattern;
		path = dir + "/config.toml";
	}
	void TearDown() override {
		std::filesystem::remove_all(dir);
	}
	int run(const std::string &answers) {
		return run_raw("\n" + answers + "\n");
	}
	int run_raw(const std::string &answers, bool update = false) {
		struct cli_context _cli_output_context = {0};
		struct cli_context *_cli_output_context_ptr = &_cli_output_context;
		FILE *input = tmpfile();
		FILE *output = tmpfile();
		if (!input || !output) {
			if (input) fclose(input);
			if (output) fclose(output);
			return -ENOMEM;
		}
		fputs(answers.c_str(), input);
		rewind(input);
		int rc = update ? cli_setup_reconfigure(_cli_output_context_ptr, path.c_str(), input, output) :
			cli_setup(_cli_output_context_ptr, path.c_str(), input, output);
		rewind(output);
		transcript.clear();
		int ch;
		while ((ch = fgetc(output)) != EOF) transcript.push_back(static_cast<char>(ch));
		fclose(input);
		fclose(output);
		return rc;
	}
};

TEST_F(CliSetupTest, SkipOptionalModels) {
	ASSERT_GE(run(std::string(9, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
	EXPECT_EQ(cfg.exec.network, 1);
	EXPECT_EQ(cfg.models.text.context_limit, 128000);
	EXPECT_EQ(cfg.models.text.max_tokens, 16384);
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_STREQ(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "");
	EXPECT_STREQ(cfg.models.image.api_key_env, "");
	EXPECT_STREQ(cfg.models.video.api_key_env, "");
}

TEST_F(CliSetupTest, ConfigureBothMediaModels) {
	ASSERT_GE(run("2\n\n\n\n\n\n\n1\n2\n2\n\n\n\n2\n1\n\n\n\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_EQ(cfg.models.text.context_limit, 1000000);
	EXPECT_EQ(cfg.models.text.max_tokens, 384000);
	EXPECT_STREQ(cfg.models.text.extra_body_json,
		     "{\"thinking\":{\"type\":\"disabled\"}}");
	EXPECT_STREQ(cfg.models.image.adapter, "volcengine-images");
	EXPECT_STREQ(cfg.models.video.adapter, "volcengine-videos");
	EXPECT_STRNE(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "doubao-seedance-2-0-mini-260615");
}

TEST_F(CliSetupTest, CustomAndInvalidInputsRoundTrip) {
	ASSERT_GE(run("9\n4\nmy-\"model\\name\n64000\n8000\ninvalid\nhttp://localhost:8080/v1\n"
		"bad-name\nMORPH_SETUP_TEST_KEY\n1\n1\n2\n2\ncustom-video\n"
		"https://example.test/api/v3\nCUSTOM_VIDEO_KEY\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "my-\"model\\name");
	EXPECT_STREQ(cfg.models.text.api_base, "http://localhost:8080/v1");
	EXPECT_STREQ(cfg.models.text.api_key_env, "MORPH_SETUP_TEST_KEY");
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_STREQ(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "custom-video");
}

TEST_F(CliSetupTest, CancellationDoesNotCreateFile) {
	EXPECT_EQ(run("\n\n"), -ECANCELED);
	EXPECT_FALSE(file_exists(path.c_str()));
}

TEST_F(CliSetupTest, NeverOverwriteExistingConfig) {
	struct cli_context _cli_output_context = {0};
	struct cli_context *_cli_output_context_ptr = &_cli_output_context;
	ASSERT_EQ(file_write_all(path.c_str(), "# keep\n", 7), 0);
	EXPECT_EQ(run(std::string(9, '\n')), -EEXIST);
	char *contents = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "# keep\n");
	free(contents);
	EXPECT_EQ(cli_setup_if_missing(&_cli_output_context, path.c_str(), 0), 0);
}

TEST_F(CliSetupTest, NonInteractiveDoesNotCreateConfig) {
	struct cli_context _cli_output_context = {0};
	struct cli_context *_cli_output_context_ptr = &_cli_output_context;
	EXPECT_EQ(cli_setup_if_missing(&_cli_output_context, path.c_str(), 0), -ENOENT);
	EXPECT_FALSE(file_exists(path.c_str()));
}

TEST_F(CliSetupTest, VisionUsesChatAdapterIndependentlyOfImageGeneration) {
	ASSERT_GE(run("2\n\n\n\n\n\n\n2\n1\n\n\n\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_EQ(cfg.models.text.context_limit, 1000000);
	EXPECT_EQ(cfg.models.text.max_tokens, 384000);
	EXPECT_STREQ(cfg.models.vision.provider, "openai");
	EXPECT_STREQ(cfg.models.vision.adapter, "openai-chat-compatible");
	EXPECT_STREQ(cfg.models.vision.model, "gpt-4o");
	EXPECT_EQ(cfg.models.vision.context_limit, 128000);
	EXPECT_EQ(cfg.models.vision.max_tokens, 16384);
	EXPECT_STREQ(cfg.models.vision.api_key_env, "OPENAI_API_KEY");
	EXPECT_STREQ(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "");
}

TEST_F(CliSetupTest, VisionAcceptsArkEndpoint) {
	ASSERT_GE(run("\n\n\n\n\n\n2\n3\nep-vision-test\n256000\n16000\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.vision.provider, "volcengine");
	EXPECT_STREQ(cfg.models.vision.adapter, "openai-chat-compatible");
	EXPECT_STREQ(cfg.models.vision.model, "ep-vision-test");
}

TEST_F(CliSetupTest, VisionAcceptsDeepSeekWithDeepSeekAdapter) {
	ASSERT_GE(run("\n\n\n\n\n\n2\n2\n\n\n\n\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.vision.provider, "deepseek");
	EXPECT_STREQ(cfg.models.vision.adapter, "deepseek");
	EXPECT_STREQ(cfg.models.vision.model, "deepseek-flash");
	EXPECT_EQ(cfg.models.vision.context_limit, 128000);
	EXPECT_EQ(cfg.models.vision.max_tokens, 16384);
	EXPECT_STREQ(cfg.models.vision.api_base, "https://api.deepseek.com");
	EXPECT_STREQ(cfg.models.vision.api_key_env, "DEEPSEEK_API_KEY");
	EXPECT_STREQ(cfg.models.vision.extra_body_json,
		     "{\"thinking\":{\"type\":\"disabled\"}}");
}

TEST(CliSetupTokens, ModelChangesResetLimitsToEditableDefaults) {
	struct config_model_entry entry = {};
	cli_setup_model_defaults(SETUP_TEXT, 1, &entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	strcpy(entry.model, "gpt-4o-unknown-version");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	cli_setup_model_defaults(SETUP_TEXT, 2, &entry);
	EXPECT_EQ(entry.context_limit, 1000000);
	EXPECT_EQ(entry.max_tokens, 384000);
	cli_setup_model_defaults(SETUP_VISION, 4, &entry);
	strcpy(entry.model, "gpt-4o");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
}

TEST(CliSetupTokens, RejectsInvalidAndOutOfRangeCountsWithoutChangingValue) {
	const char *invalid[] = {"", "0", "-1", "1.5", "128k", " 10", "12junk",
		"2147483648", "999999999999999999999999999999"};
	for (const char *text : invalid) {
		int value = 42;
		EXPECT_LT(cli_setup_parse_token_count(text, 1, INT_MAX, &value), 0) << text;
		EXPECT_EQ(value, 42);
	}
	int value = 0;
	EXPECT_EQ(cli_setup_parse_token_count("16384", 1, 16384, &value), 0);
	EXPECT_EQ(value, 16384);
	EXPECT_LT(cli_setup_parse_token_count("16385", 1, 16384, &value), 0);
}

TEST_F(CliSetupTest, PlainPromptsValidateAndPersistCustomTokenLimits) {
	ASSERT_GE(run("4\nmy-model\n0\n2147483648\n32000\n32000\n-1\n4096\n"
		"http://localhost:8080/v1\nMORPH_SETUP_KEY\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_EQ(cfg.models.text.context_limit, 32000);
	EXPECT_EQ(cfg.models.text.max_tokens, 4096);
}

TEST_F(CliSetupTest, DeepSeekThinkingCanBeEnabledInPlainSetup) {
	ASSERT_GE(run("2\n\n\n\n2\n\n\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_TRUE(cli_setup_thinking_enabled(&cfg.models.text));
	EXPECT_STREQ(cfg.models.text.extra_body_json,
		"{\"thinking\":{\"type\":\"enabled\"}}");
}

TEST_F(CliSetupTest, ThinkingPromptValidatesAndCanBeCancelled) {
	EXPECT_EQ(run("2\n\n\n\ninvalid\n"), -ECANCELED);
	EXPECT_FALSE(file_exists(path.c_str()));
	ASSERT_GE(run("2\n\n\n\ninvalid\n1\n\n\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_FALSE(cli_setup_thinking_enabled(&cfg.models.text));
}

TEST_F(CliSetupTest, VideoRoleRequiresAndConfiguresAllFourModels) {
	ASSERT_GE(run_raw("7\n" + std::string(21, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STRNE(cfg.models.text.model, "");
	EXPECT_STRNE(cfg.models.vision.model, "");
	EXPECT_STRNE(cfg.models.image.model, "");
	EXPECT_STRNE(cfg.models.video.model, "");
	EXPECT_LT(transcript.find("Choose your role"), transcript.find("Text model"));
	EXPECT_NE(transcript.find("Vision model (required)"), std::string::npos);
	EXPECT_NE(transcript.find("Image model (required)"), std::string::npos);
	EXPECT_NE(transcript.find("Video model (required)"), std::string::npos);
	EXPECT_EQ(transcript.find("1) Skip"), std::string::npos);
	EXPECT_NE(transcript.find("Run morph again"), std::string::npos);
	char *prompt = nullptr;
	ASSERT_EQ(morph_prompt_load(nullptr, cfg.prompt.system_prompt_file, nullptr, &prompt), 0);
	ASSERT_NE(prompt, nullptr);
	EXPECT_NE(std::string(prompt).find("Video creator"), std::string::npos);
	EXPECT_EQ(std::string(prompt).find("{{"), std::string::npos);
	free(prompt);
}

TEST_F(CliSetupTest, VideoRoleCannotSaveBeforeAllRequiredModelsAreCollected) {
	EXPECT_EQ(run_raw("7\n" + std::string(12, '\n')), -ECANCELED);
	EXPECT_FALSE(file_exists(path.c_str()));
	EXPECT_TRUE(std::filesystem::is_empty(dir));
}

TEST_F(CliSetupTest, ReconfigureKeepsValuesCredentialsAndCustomPromptAndCreatesBackup) {
	const std::string old = "# user comments survive in the backup\n"
		"[model.text]\nprovider='custom'\nadapter='openai-chat-compatible'\n"
		"model='private-model'\napi_base='http://localhost:1234/v1'\napi_key='secret'\n"
		"context_limit=64000\nmax_tokens=4096\ntimeout_seconds=77\n"
		"[memory]\nenabled=false\n[exec]\nnetwork=false\n"
		"[prompt]\nmode='append'\nsystem_prompt_file='/keep/custom.md'\n"
		"[[mcp.servers]]\nname='custom'\ntransport='stdio'\ncommand='custom-command'\n"
		"[context]\nkeep_recent_rounds=3\n";
	ASSERT_EQ(file_write_all(path.c_str(), old.c_str(), old.size()), 0);
	ASSERT_GE(run_raw(std::string(11, '\n'), true), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "private-model");
	EXPECT_STREQ(cfg.models.text.api_key, "secret");
	EXPECT_EQ(transcript.find("secret"), std::string::npos);
	EXPECT_EQ(cfg.models.text.context_limit, 64000);
	EXPECT_EQ(cfg.models.text.max_tokens, 4096);
	EXPECT_EQ(cfg.models.text.timeout_seconds, 77);
	EXPECT_FALSE(cfg.memory.enabled);
	EXPECT_FALSE(cfg.exec.network);
	EXPECT_EQ(cfg.context.keep_recent_rounds, 3);
	EXPECT_EQ(cfg.mcp.server_count, 1);
	EXPECT_STREQ(cfg.mcp.servers[0].command, "custom-command");
	EXPECT_STREQ(cfg.prompt.system_prompt_file, "/keep/custom.md");
	EXPECT_STREQ(cfg.prompt.mode, "append");
	bool found = false;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().filename().string().find(".backup-") == std::string::npos) continue;
		char *backup = file_read_all(entry.path().c_str(), nullptr);
		ASSERT_NE(backup, nullptr);
		EXPECT_EQ(std::string(backup), old);
		free(backup);
		found = true;
	}
	EXPECT_TRUE(found);
}

TEST_F(CliSetupTest, ReconfigureCancelAndInvalidConfigLeaveFilesUntouched) {
	const std::string old = "[memory]\nenabled=false\n";
	ASSERT_EQ(file_write_all(path.c_str(), old.c_str(), old.size()), 0);
	EXPECT_EQ(run_raw("", true), -ECANCELED);
	char *text = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(text, nullptr);
	EXPECT_EQ(std::string(text), old);
	free(text);
	ASSERT_EQ(file_write_all(path.c_str(), "[broken", 7), 0);
	EXPECT_LT(run_raw(std::string(30, '\n'), true), 0);
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
		std::filesystem::directory_iterator()), 1);
}

TEST_F(CliSetupTest, RolePromptReplacementPreservesOriginalPromptFiles) {
	std::string custom = dir + "/custom.md";
	ASSERT_EQ(file_write_all(custom.c_str(), "Customized instructions", 23), 0);
	std::string old = "[prompt]\nmode='replace'\nsystem_prompt_file='" + custom + "'\n";
	ASSERT_EQ(file_write_all(path.c_str(), old.c_str(), old.size()), 0);
	ASSERT_GE(run_raw("5\n2\n" + std::string(10, '\n'), true), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STRNE(cfg.prompt.system_prompt_file, custom.c_str());
	char *original = file_read_all(custom.c_str(), nullptr);
	ASSERT_NE(original, nullptr);
	EXPECT_STREQ(original, "Customized instructions");
	free(original);
	char *generated = file_read_all(cfg.prompt.system_prompt_file, nullptr);
	ASSERT_NE(generated, nullptr);
	EXPECT_NE(std::string(generated).find("author's voice"), std::string::npos);
	free(generated);
}

TEST_F(CliSetupTest, EveryRoleRendersAndUsesExplicitModelRequirements) {
	for (int i = 0; i < SETUP_ROLE_COUNT; i++) {
		char *text = nullptr;
		ASSERT_EQ(cli_setup_role_render(i, &text), 0);
		ASSERT_NE(text, nullptr);
		EXPECT_EQ(std::string(text).find("{{"), std::string::npos);
		EXPECT_NE(std::string(text).find(cli_setup_role_get(i).label), std::string::npos);
		EXPECT_TRUE(cli_setup_role_requires(i, SETUP_TEXT));
		free(text);
	}
	for (int i = 0; i < SETUP_MODEL_COUNT; i++)
		EXPECT_TRUE(cli_setup_role_requires(SETUP_VIDEO_CREATOR, i));
}

TEST_F(CliSetupTest, ReconfigureUsesSavedVideoRoleAndNeverAddsSkipSteps) {
	ASSERT_GE(run_raw("7\n" + std::string(21, '\n')), 0);
	ASSERT_GE(run_raw(std::string(23, '\n'), true), 0);
	EXPECT_EQ(transcript.find("1) Skip"), std::string::npos);
	EXPECT_NE(transcript.find("Video model (required)"), std::string::npos);
	char *saved = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(saved, nullptr);
	EXPECT_EQ(cli_setup_config_role(saved), SETUP_VIDEO_CREATOR);
	free(saved);
}

TEST_F(CliSetupTest, ReconfigureRejectsSymlinksAndPublishesPrivateFiles) {
	std::string target = dir + "/target.toml";
	ASSERT_EQ(file_write_all(target.c_str(), "[memory]\nenabled=false\n", 23), 0);
	ASSERT_EQ(symlink(target.c_str(), path.c_str()), 0);
	EXPECT_LT(run_raw(std::string(20, '\n'), true), 0);
	ASSERT_EQ(unlink(path.c_str()), 0);
	ASSERT_EQ(file_write_all(path.c_str(), "[memory]\nenabled=false\n", 23), 0);
	ASSERT_EQ(chmod(path.c_str(), 0644), 0);
	ASSERT_GE(run_raw(std::string(11, '\n'), true), 0);
	struct stat st{};
	ASSERT_EQ(stat(path.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(CliSetupTest, SetupConfigMergeSupportsInlineTablesAndUnrelatedArrays) {
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge(
		"model={text={provider='custom',model='old',timeout_seconds=77}}\n"
		"[hooks]\nStop=[{hooks=[{type='command',command='echo keep'}]}]\n",
		"[model.text]\nmodel='new'\n[setup]\nrole='video_creator'\n", &merged), 0);
	ASSERT_NE(merged, nullptr);
	EXPECT_NE(std::string(merged).find("timeout_seconds=77"), std::string::npos);
	EXPECT_NE(std::string(merged).find("echo keep"), std::string::npos);
	struct config_validation_error error{};
	EXPECT_EQ(config_validate_text(merged, &error), 0) << error.message;
	EXPECT_EQ(cli_setup_config_role(merged), SETUP_VIDEO_CREATOR);
	free(merged);
}

TEST_F(CliSetupTest, SetupConfigMergePreservesMcpCommentsAndUnchangedValuesExactly) {
	const std::string mcp = "# Services stay readable\n"
		"[[mcp.servers]]\nname = 'one' # first service\ntransport = 'http'\n"
		"url = 'https://one.example/mcp'\n\n"
		"[[mcp.servers]]\nname = 'two'\ntransport = 'stdio'\n"
		"command = 'server'\nargs = [\n  'one', # argument\n  'two',\n]\n";
	const std::string model = "# Original model settings\n[model.text]\n"
		"model       = 'old' # chosen model\nmax_tokens  = 4_096\n";
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge((model + "\n" + mcp).c_str(),
		"[model.text]\nmodel=\"new\"\nmax_tokens=4096\n", &merged), 0);
	ASSERT_NE(merged, nullptr);
	std::string expected = model;
	expected.replace(expected.find("'old'"), 5, "\"new\"");
	EXPECT_EQ(std::string(merged), expected + "\n" + mcp);
	free(merged);
}

TEST_F(CliSetupTest, UnchangedSetupPatchDoesNotReformatAnything) {
	const std::string original = "# Hand formatted\r\n[model.text]\r\n"
		"model = 'custom' # keep\r\nmax_tokens = 4_096\r\n"
		"[prompt]\r\nmode = 'replace'\r\n";
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge(original.c_str(),
		"[model.text]\nmodel=\"custom\"\nmax_tokens=4096\n[prompt]\nmode=\"replace\"\n",
		&merged), 0);
	EXPECT_EQ(std::string(merged), original);
	free(merged);
}

TEST_F(CliSetupTest, SetupEditsMultilineValuesWithoutTouchingDecoyHeadersOrComments) {
	const std::string original = "[model.text]\nmodel = '''old\n[mcp]\n''' # keep comment\n"
		"api_base = \"old\\\"value\" # keep URL comment\n\n[mcp]\nservers = []\n";
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge(original.c_str(),
		"[model.text]\nmodel='new'\napi_base='https://new.example'\n", &merged), 0);
	EXPECT_STREQ(merged, "[model.text]\nmodel = 'new' # keep comment\n"
		"api_base = 'https://new.example' # keep URL comment\n\n[mcp]\nservers = []\n");
	free(merged);
}

TEST_F(CliSetupTest, SetupCanInsertIntoInlineDottedAndFinalHeaderTables) {
	for (const char *original : {
		"model={text={model='old', timeout_seconds=77}}\n",
		"model = { text = {} }\n",
		"model.text.model='old'\nmodel.text.timeout_seconds=77\n",
		"['model'.'text']\nmodel='old'\n",
		"[model.text]"}) {
		SCOPED_TRACE(original);
		char *merged = nullptr;
		ASSERT_EQ(cli_setup_config_merge(original,
			"[prompt]\nmode='replace'\n[model.text]\nmodel='new'\nprovider='custom'\n"
			"api_base='https://example.test'\n[model.vision]\nmodel='vision'\n",
			&merged), 0);
		if (std::string(original).find("timeout_seconds=77") != std::string::npos)
			EXPECT_NE(std::string(merged).find("timeout_seconds=77"), std::string::npos);
		free(merged);
	}
}

TEST_F(CliSetupTest, SetupReplacesAllStringFormsWhileRetainingTrailingComments) {
	for (const char *literal : {"''", "\"\"", "'old'", "\"old\\\"text\"",
		"\"\"\"\nold\n\"\"\"", "'''\r\nold\r\n'''", "'''old''''", "\"\"\"old\"\"\"\"\""}) {
		SCOPED_TRACE(literal);
		std::string original = "[model.text]\nmodel = " + std::string(literal) +
			" # keep comment\n\n[mcp]\nservers = []\n";
		char *merged = nullptr;
		ASSERT_EQ(cli_setup_config_merge(original.c_str(), "[model.text]\nmodel='new'\n",
			&merged), 0);
		EXPECT_STREQ(merged, "[model.text]\nmodel = 'new' # keep comment\n\n[mcp]\nservers = []\n");
		free(merged);
	}
}

TEST_F(CliSetupTest, RoleSnapshotsHaveReadableNamesAndReuseUnchangedContent) {
	ASSERT_GE(run(std::string(9, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	std::string first = cfg.prompt.system_prompt_file;
	EXPECT_EQ(first, path + ".prompts/general.md");
	ASSERT_GE(run_raw("1\n2\n" + std::string(10, '\n'), true), 0);
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_EQ(std::string(cfg.prompt.system_prompt_file), first);
	ASSERT_EQ(file_write_all(first.c_str(), "Custom instructions", 19), 0);
	ASSERT_GE(run_raw("1\n2\n" + std::string(10, '\n'), true), 0);
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	std::filesystem::path next = cfg.prompt.system_prompt_file;
	EXPECT_NE(next.string(), first);
	EXPECT_EQ(next.extension(), ".md");
	EXPECT_EQ(next.filename().string().find("general-"), 0u);
	char *custom = file_read_all(first.c_str(), nullptr);
	ASSERT_NE(custom, nullptr);
	EXPECT_STREQ(custom, "Custom instructions");
	free(custom);
}

TEST(CliSetupThinking, OnlyDeepSeekChangesAndOtherOptionsSurvive) {
	struct config_model_entry entry{};
	cli_setup_model_defaults(SETUP_TEXT, 1, &entry);
	EXPECT_EQ(cli_setup_set_thinking(&entry, 1), -EINVAL);
	EXPECT_STREQ(entry.extra_body_json, "");
	cli_setup_model_defaults(SETUP_TEXT, 2, &entry);
	strcpy(entry.extra_body_json,
		"{\"reasoning_effort\":\"high\",\"thinking\":{\"type\":\"disabled\"}}");
	ASSERT_EQ(cli_setup_set_thinking(&entry, 1), 0);
	EXPECT_TRUE(cli_setup_thinking_enabled(&entry));
	EXPECT_NE(strstr(entry.extra_body_json, "\"reasoning_effort\":\"high\""), nullptr);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 0), 0);
	EXPECT_FALSE(cli_setup_thinking_enabled(&entry));
}
