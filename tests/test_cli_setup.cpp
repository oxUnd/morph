#include <gtest/gtest.h>
#include "sapi/cli/setup.h"
#include "sapi/cli/setup_ui.h"
#include "config/config.h"
#include "util/file.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

class CliSetupTest : public ::testing::Test {
protected:
	std::string dir;
	std::string path;
	void SetUp() override {
		char pattern[] = "/tmp/morph-setup-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		dir = pattern;
		path = dir + "/config.toml";
	}
	void TearDown() override {
		unlink(path.c_str());
		rmdir(dir.c_str());
	}
	int run(const std::string &answers) {
		FILE *input = tmpfile();
		FILE *output = tmpfile();
		if (!input || !output) {
			if (input) fclose(input);
			if (output) fclose(output);
			return -ENOMEM;
		}
		fputs(answers.c_str(), input);
		rewind(input);
		int rc = cli_setup(path.c_str(), input, output);
		fclose(input);
		fclose(output);
		return rc;
	}
};

TEST_F(CliSetupTest, SkipOptionalModels) {
	ASSERT_GE(run(std::string(9, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
	EXPECT_EQ(cfg.models.text.context_limit, 128000);
	EXPECT_EQ(cfg.models.text.max_tokens, 16384);
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_STREQ(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "");
	EXPECT_STREQ(cfg.models.image.api_key_env, "");
	EXPECT_STREQ(cfg.models.video.api_key_env, "");
}

TEST_F(CliSetupTest, ConfigureBothMediaModels) {
	ASSERT_GE(run("2\n\n\n\n\n\n1\n2\n2\n\n\n\n2\n1\n\n\n\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_EQ(cfg.models.text.context_limit, 1000000);
	EXPECT_EQ(cfg.models.text.max_tokens, 384000);
	EXPECT_STREQ(cfg.models.image.adapter, "volcengine-images");
	EXPECT_STREQ(cfg.models.video.adapter, "volcengine-videos");
	EXPECT_STRNE(cfg.models.image.model, "");
	EXPECT_STRNE(cfg.models.video.model, "");
}

TEST_F(CliSetupTest, CustomAndInvalidInputsRoundTrip) {
	ASSERT_GE(run("9\n4\nmy-\"model\\name\n64000\n8000\ninvalid\nhttp://localhost:8080/v1\n"
		"bad-name\nMORPH_SETUP_TEST_KEY\n1\n1\n2\n2\ncustom-video\n"
		"https://example.test/api/v3\nCUSTOM_VIDEO_KEY\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
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
	ASSERT_EQ(file_write_all(path.c_str(), "# keep\n", 7), 0);
	EXPECT_EQ(run(std::string(9, '\n')), -EEXIST);
	char *contents = file_read_all(path.c_str(), nullptr);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "# keep\n");
	free(contents);
	EXPECT_EQ(cli_setup_if_missing(path.c_str(), 0), 0);
}

TEST_F(CliSetupTest, NonInteractiveDoesNotCreateConfig) {
	EXPECT_EQ(cli_setup_if_missing(path.c_str(), 0), -ENOENT);
	EXPECT_FALSE(file_exists(path.c_str()));
}

TEST_F(CliSetupTest, VisionUsesChatAdapterIndependentlyOfImageGeneration) {
	ASSERT_GE(run("2\n\n\n\n\n\n2\n1\n\n\n\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
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
	ASSERT_GE(run("\n\n\n\n\n\n2\n2\nep-vision-test\n256000\n16000\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.vision.provider, "volcengine");
	EXPECT_STREQ(cfg.models.vision.adapter, "openai-chat-compatible");
	EXPECT_STREQ(cfg.models.vision.model, "ep-vision-test");
}

TEST(CliSetupTokens, ModelChangesResetLimitsAndUnknownModelsDoNotInherit) {
	struct config_model_entry entry = {};
	cli_setup_model_defaults(SETUP_TEXT, 1, &entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	strcpy(entry.model, "gpt-4o-unknown-version");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 0);
	EXPECT_EQ(entry.max_tokens, 0);
	cli_setup_model_defaults(SETUP_TEXT, 2, &entry);
	EXPECT_EQ(entry.context_limit, 1000000);
	EXPECT_EQ(entry.max_tokens, 384000);
	cli_setup_model_defaults(SETUP_VISION, 3, &entry);
	strcpy(entry.model, "gpt-4o");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 0);
	EXPECT_EQ(entry.max_tokens, 0);
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
	ASSERT_EQ(config_load(&cfg, path.c_str()), 0);
	EXPECT_EQ(cfg.models.text.context_limit, 32000);
	EXPECT_EQ(cfg.models.text.max_tokens, 4096);
}
