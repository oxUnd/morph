extern "C" {
#include <string.h>
#include "nb/cleanup.h"
#include "sapi/cli/services/configuration/model.h"
#include "nb/err.h"
#include "sapi/cli/services/configuration/role.h"
}
#include <gtest/gtest.h>
#include "sapi/cli/pages/setup/wizard.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/mcp.h"
#include "agent/system_prompt.h"
#include <filesystem>
#include "config/config.h"
#include "nb/file.h"
#include "nb/toml.h"
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
	int run_custom(const std::string &answers) {
		return run_custom_raw("\n" + answers + "\n");
	}
	int run_custom_raw(const std::string &answers, bool update = false) {
		auto role = answers.find('\n');
		if (role == std::string::npos) return run_input(answers, update);
		return run_input(answers.substr(0, role + 1) + "2\n" + answers.substr(role + 1), update);
	}
	int run_input(const std::string &answers, bool update = false) {
		struct cli_application _cli_output_context = {0};
		struct cli_application *_cli_output_context_ptr = &_cli_output_context;
		FILE *input = tmpfile();
		FILE *output = tmpfile();
		if (!input || !output) {
			if (input) fclose(input);
			if (output) fclose(output);
			return NB_ERR_NOMEM;
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	ASSERT_GE(run_custom(std::string(9, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
	EXPECT_EQ(cfg.exec.network, 1);
	EXPECT_EQ(cfg.models.text.context_limit, 128000);
	EXPECT_EQ(cfg.models.text.max_tokens, 16384);
	EXPECT_EQ(cfg.models.text.timeout_seconds, 600);
	EXPECT_EQ(cfg.models.text.retry_count, 3);
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_STREQ(cfg.models.image.model, "");
	EXPECT_STREQ(cfg.models.video.model, "");
	EXPECT_TRUE(cfg.exec.allow_host_execution);
	EXPECT_TRUE(cfg.react.hitl_auto_approve_readonly);
	EXPECT_TRUE(cfg.react.hitl_auto_approve_media);
	char *saved =
		(nb_file_read_all(path.c_str(), &file_value_1, 0, NULL) == NB_OK &&
				 (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				   nb_buf_len(&file_value_1) + 1),
			    nb_charp_steal(&file_copy_1))
			 : NULL);
	ASSERT_NE(saved, nullptr);
	nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(saved), nullptr);
	ASSERT_TRUE(parsed);
	for (const char *kind : {"image", "video"}) {
		const nb_toml_value *entry = nb_toml_table_get_cstr(
			nb_toml_table_get_cstr(nb_toml_root(parsed), "model"), kind);
		EXPECT_EQ(nb_toml_type_of(nb_toml_table_get_cstr(entry, "api_key")), NB_TOML_NONE);
		EXPECT_EQ(nb_toml_type_of(nb_toml_table_get_cstr(entry, "api_key_env")),
			  NB_TOML_NONE);
	}
	nb_toml_free(parsed);
	free(saved);
}

TEST_F(CliSetupTest, DefaultsNeedOnlyRoleAndSaveWithoutWalkingModelSettings) {
	ASSERT_GE(run_input("\n\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "gpt-4o");
	EXPECT_STREQ(cfg.models.vision.model, "");
	EXPECT_EQ(transcript.find("Provider:"), std::string::npos);
	EXPECT_EQ(transcript.find("Context window (tokens):"), std::string::npos);
}

TEST_F(CliSetupTest, VideoDefaultsConfigureAllModelsWithoutPerModelQuestions) {
	ASSERT_GE(run_input("7\n\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STRNE(cfg.models.text.model, "");
	EXPECT_STRNE(cfg.models.vision.model, "");
	EXPECT_STRNE(cfg.models.image.model, "");
	EXPECT_STRNE(cfg.models.video.model, "");
	EXPECT_EQ(transcript.find("Provider:"), std::string::npos);
}

TEST_F(CliSetupTest, DefaultsStillRequireSaveAndCannotHideIncompleteCustomModels) {
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	EXPECT_EQ(run_input("\n"), NB_ERR_CANCELLED);
	EXPECT_FALSE(nb_file_exists(path.c_str()));
	const std::string old = "[model.text]\nprovider='custom'\nmodel=''\napi_base=''\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), old.c_str(), old.size(), NULL), 0);
	EXPECT_EQ(run_input("\n\n", true), NB_ERR_CANCELLED);
	char *saved =
		(nb_file_read_all(path.c_str(), &file_value_2, 0, NULL) == NB_OK &&
				 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				   nb_buf_len(&file_value_2) + 1),
			    nb_charp_steal(&file_copy_2))
			 : NULL);
	ASSERT_NE(saved, nullptr);
	EXPECT_EQ(std::string(saved), old);
	free(saved);
}

TEST(CliSetupDefaults, SharesOnlyMissingCredentialsFromTheSameConfiguredSource) {
	struct cli_setup_state state{};
	state.role = CLI_SETUP_VIDEO_CREATOR;
	cli_setup_role_defaults(&state);
	const char *env = "MORPH_SETUP_TEST_MISSING_SHARED_KEY";
	for (int i = 0; i <= CLI_SETUP_IMAGE; i++) strcpy(state.entries[i].api_key_env, env);
	strcpy(state.entries[CLI_SETUP_IMAGE].api_key, "keep-existing-account");
	strcpy(state.entries[CLI_SETUP_TEXT].api_key, "shared-fixture-key");
	state.entries[CLI_SETUP_TEXT].api_key_env[0] = '\0';
	cli_setup_share_credentials(&state, CLI_SETUP_TEXT, env);
	EXPECT_STREQ(state.entries[CLI_SETUP_VISION].api_key, "shared-fixture-key");
	EXPECT_STREQ(state.entries[CLI_SETUP_VISION].api_key_env, "");
	EXPECT_STREQ(state.entries[CLI_SETUP_IMAGE].api_key, "keep-existing-account");
	EXPECT_STREQ(state.entries[CLI_SETUP_VIDEO].api_key, "");
}

TEST(CliSetupDefaults, NewlyRequiredModelsReuseAnExistingProviderAccount) {
	struct cli_setup_state state{};
	cli_setup_model_defaults(CLI_SETUP_TEXT, 1, &state.entries[CLI_SETUP_TEXT]);
	strcpy(state.entries[CLI_SETUP_TEXT].api_key, "existing-fixture-key");
	state.entries[CLI_SETUP_TEXT].api_key_env[0] = '\0';
	state.role = CLI_SETUP_PHOTO_EDITOR;
	cli_setup_role_defaults(&state);
	for (int kind : {CLI_SETUP_VISION, CLI_SETUP_IMAGE}) {
		EXPECT_STREQ(state.entries[kind].api_key, "existing-fixture-key");
		EXPECT_STREQ(state.entries[kind].api_key_env, "");
	}
	state.role = CLI_SETUP_VIDEO_CREATOR;
	cli_setup_role_defaults(&state);
	EXPECT_STREQ(state.entries[CLI_SETUP_VIDEO].api_key, "");
	EXPECT_STREQ(state.entries[CLI_SETUP_TEXT].api_key, "existing-fixture-key");
}

TEST_F(CliSetupTest, SetupWritesOnlyTheSelectedCredentialSource) {
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

	ASSERT_GE(run_custom_raw("7\n" + std::string(21, '\n')), 0);
	char *saved =
		(nb_file_read_all(path.c_str(), &file_value_3, 0, NULL) == NB_OK &&
				 (file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				   nb_buf_len(&file_value_3) + 1),
			    nb_charp_steal(&file_copy_3))
			 : NULL);
	ASSERT_NE(saved, nullptr);
	nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(saved), nullptr);
	ASSERT_TRUE(parsed);
	for (const char *kind : {"text", "vision", "image", "video"}) {
		const nb_toml_value *entry = nb_toml_table_get_cstr(
			nb_toml_table_get_cstr(nb_toml_root(parsed), "model"), kind);
		EXPECT_EQ(nb_toml_type_of(nb_toml_table_get_cstr(entry, "api_key")), NB_TOML_NONE)
			<< kind;
		EXPECT_EQ(nb_toml_type_of(nb_toml_table_get_cstr(entry, "api_key_env")),
			  NB_TOML_STRING)
			<< kind;
	}
	nb_toml_free(parsed);
	free(saved);
}

TEST_F(CliSetupTest, ReconfigureRemovesEnvironmentFieldWhenKeepingDirectKey) {
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_4 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const std::string old = "[model.text]\nprovider='custom'\nmodel='private'\n"
		"api_base='http://localhost:1234/v1'\napi_key='fixture-secret'\n"
		"api_key_env='OLD_KEY' # retain this comment\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), old.c_str(), old.size(), NULL), 0);
	ASSERT_GE(run_custom_raw(std::string(11, '\n'), true), 0);
	char *saved =
		(nb_file_read_all(path.c_str(), &file_value_4, 0, NULL) == NB_OK &&
				 (file_copy_4 = (char *)malloc(nb_buf_len(&file_value_4) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_4, nb_buf_cstr(&file_value_4),
				   nb_buf_len(&file_value_4) + 1),
			    nb_charp_steal(&file_copy_4))
			 : NULL);
	ASSERT_NE(saved, nullptr);
	nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(saved), nullptr);
	ASSERT_TRUE(parsed);
	const nb_toml_value *entry = nb_toml_table_get_cstr(
		nb_toml_table_get_cstr(nb_toml_root(parsed), "model"), "text");
	EXPECT_STREQ(nb_toml_string(nb_toml_table_get_cstr(entry, "api_key")).ptr,
		     "fixture-secret");
	EXPECT_EQ(nb_toml_type_of(nb_toml_table_get_cstr(entry, "api_key_env")), NB_TOML_NONE);
	EXPECT_NE(std::string(saved).find("# retain this comment"), std::string::npos);
	EXPECT_EQ(transcript.find("fixture-secret"), std::string::npos);
	nb_toml_free(parsed);
	free(saved);
}

TEST_F(CliSetupTest, CredentialCleanupPreservesOtherValuesAndAllTomlLayouts) {
	const char *fixtures[] = {
		"[model.text]\napi_key=''\napi_key_env='KEY'\nmodel='name'\n",
		"[model.text]\r\napi_key = '''\r\n''' # comment\r\napi_key_env='KEY'\r\n",
		"'model'.text.'api_key' = ''\nmodel.text.api_key_env='KEY'\n",
		"model={text={api_key='',api_key_env='KEY',timeout_seconds=77}}\n",
		"model={text={api_key_env='KEY',api_key=''}}\n",
		"model={text={api_key='',api_key_env=''}}\n",
		"model={text={api_key_env='',api_key=''}}\n",
		"model={text={api_key='',api_key_env='',model='name'}}\n",
		"model={text={model='name',api_key='',api_key_env=''}}\n",
		"model={text.api_key='',text.api_key_env='KEY'}\n",
		"model={text={api_key='', # retain\napi_key_env='KEY'}}\n",
		"model={text={api_key_env='KEY', # retain\napi_key='' # keep too\n}}\n",
		"model={text={api_key='' # retain\n, api_key_env='KEY'}}\n",
		"[model.text]\napi_key='secret'\napi_key_env='KEY'\n",
		"[model.vision]\napi_key=''\n[model.image]\napi_key_env=''\n"
		"[model.video]\napi_key='secret'\napi_key_env='KEY'\n",
	};
	const std::string unrelated = "\n[[mcp.servers]]\nname='keep'\ntransport='stdio'\n"
		"command='server'\nargs=['api_key=keep', '[model.text]']\n";
	for (const char *fixture : fixtures) {
		SCOPED_TRACE(fixture);
		char *clean = nullptr;
		ASSERT_EQ(cli_setup_config_credentials_copy((fixture + unrelated).c_str(), &clean),
			  0);
		ASSERT_NE(clean, nullptr);
		EXPECT_NE(std::string(clean).find(unrelated), std::string::npos);
		nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(clean), nullptr);
		ASSERT_TRUE(parsed);
		for (const char *kind : {"text", "vision", "image", "video"}) {
			const nb_toml_value *entry = nb_toml_table_get_cstr(
				nb_toml_table_get_cstr(nb_toml_root(parsed), "model"), kind);
			const nb_toml_value *direct = nb_toml_table_get_cstr(entry, "api_key");
			const nb_toml_value *env = nb_toml_table_get_cstr(entry, "api_key_env");
			EXPECT_TRUE(nb_toml_type_of(direct) == NB_TOML_NONE ||
				    nb_toml_type_of(env) == NB_TOML_NONE);
			if (nb_toml_type_of(direct) == NB_TOML_STRING)
				EXPECT_NE(nb_toml_string(direct).ptr[0], '\0');
			if (nb_toml_type_of(env) == NB_TOML_STRING)
				EXPECT_NE(nb_toml_string(env).ptr[0], '\0');
		}
		char *again = nullptr;
		EXPECT_EQ(cli_setup_config_credentials_copy(clean, &again), 0);
		if (again) EXPECT_STREQ(again, clean);
		free(again);
		nb_toml_free(parsed);
		free(clean);
	}
}

TEST_F(CliSetupTest, ConfigureBothMediaModels) {
	ASSERT_GE(run_custom("2\n\n\n\n\n\n\n1\n2\n2\n\n\n\n2\n1\n\n\n\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_STREQ(cfg.models.text.model, "deepseek-flash");
	EXPECT_STREQ(cfg.models.text.api_base, "https://api.deepseek.com");
	EXPECT_EQ(cfg.models.text.context_limit, 1048576);
	EXPECT_EQ(cfg.models.text.max_tokens, 65536);
	EXPECT_STREQ(cfg.models.text.extra_body_json,
		     "{\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"high\"}");
	EXPECT_STREQ(cfg.models.image.adapter, "volcengine-images");
	EXPECT_STREQ(cfg.models.video.adapter, "volcengine-videos");
	EXPECT_STREQ(cfg.models.image.model, "doubao-seedream-5-0-flash-260915");
	EXPECT_EQ(cfg.models.image.timeout_seconds, 300);
	EXPECT_STREQ(cfg.models.video.model, "doubao-seedance-2-0-mini-260615");
	EXPECT_EQ(cfg.models.video.poll_interval_seconds, 5);
	EXPECT_EQ(cfg.models.video.poll_timeout_seconds, 1800);
}

TEST_F(CliSetupTest, CustomAndInvalidInputsRoundTrip) {
	ASSERT_GE(run_custom("9\n4\nmy-\"model\\name\n64000\n8000\ninvalid\nhttp://localhost:8080/v1\n"
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
	EXPECT_EQ(run_custom("\n\n"), NB_ERR_CANCELLED);
	EXPECT_FALSE(nb_file_exists(path.c_str()));
}

TEST_F(CliSetupTest, NeverOverwriteExistingConfig) {
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_5 NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	ASSERT_EQ(nb_file_write_all(path.c_str(), "# keep\n", 7, NULL), 0);
	EXPECT_EQ(run_custom(std::string(9, '\n')), NB_ERR_EXISTS);
	char *contents =
		(nb_file_read_all(path.c_str(), &file_value_5, 0, NULL) == NB_OK &&
				 (file_copy_5 = (char *)malloc(nb_buf_len(&file_value_5) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_5, nb_buf_cstr(&file_value_5),
				   nb_buf_len(&file_value_5) + 1),
			    nb_charp_steal(&file_copy_5))
			 : NULL);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "# keep\n");
	free(contents);
	EXPECT_EQ(cli_setup_if_missing(&_cli_output_context, path.c_str(), 0), 0);
}

TEST_F(CliSetupTest, NonInteractiveDoesNotCreateConfig) {
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	EXPECT_EQ(cli_setup_if_missing(&_cli_output_context, path.c_str(), 0), NB_ERR_NOT_FOUND);
	EXPECT_FALSE(nb_file_exists(path.c_str()));
}

TEST_F(CliSetupTest, VisionUsesChatAdapterIndependentlyOfImageGeneration) {
	ASSERT_GE(run_custom("2\n\n\n\n\n\n\n2\n1\n\n\n\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_EQ(cfg.models.text.context_limit, 1048576);
	EXPECT_EQ(cfg.models.text.max_tokens, 65536);
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
	ASSERT_GE(run_custom("\n\n\n\n\n\n2\n3\nep-vision-test\n256000\n16000\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.vision.provider, "volcengine");
	EXPECT_STREQ(cfg.models.vision.adapter, "openai-chat-compatible");
	EXPECT_STREQ(cfg.models.vision.model, "ep-vision-test");
}

TEST_F(CliSetupTest, VisionAcceptsDeepSeekWithDeepSeekAdapter) {
	ASSERT_GE(run_custom("\n\n\n\n\n\n2\n2\n\n\n\n\n\n\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.vision.provider, "deepseek");
	EXPECT_STREQ(cfg.models.vision.adapter, "deepseek");
	EXPECT_STREQ(cfg.models.vision.model, "deepseek-flash");
	EXPECT_EQ(cfg.models.vision.context_limit, 1048576);
	EXPECT_EQ(cfg.models.vision.max_tokens, 16384);
	EXPECT_STREQ(cfg.models.vision.api_base, "https://api.deepseek.com");
	EXPECT_STREQ(cfg.models.vision.api_key_env, "DEEPSEEK_API_KEY");
	EXPECT_STREQ(cfg.models.vision.extra_body_json,
		     "{\"thinking\":{\"type\":\"disabled\"}}");
}

TEST(CliSetupTokens, ModelChangesResetLimitsToEditableDefaults) {
	struct config_model_entry entry = {};
	cli_setup_model_defaults(CLI_SETUP_TEXT, 1, &entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	strcpy(entry.model, "gpt-4o-unknown-version");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	cli_setup_model_defaults(CLI_SETUP_TEXT, 2, &entry);
	EXPECT_EQ(entry.context_limit, 1048576);
	EXPECT_EQ(entry.max_tokens, 65536);
	EXPECT_EQ(cli_setup_token_ceiling(&entry, 0), 1048576);
	EXPECT_EQ(cli_setup_token_ceiling(&entry, 1), 393216);
	cli_setup_model_defaults(CLI_SETUP_VISION, 4, &entry);
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
	ASSERT_GE(run_custom("4\nmy-model\n0\n2147483648\n32000\n32000\n-1\n4096\n"
		"http://localhost:8080/v1\nMORPH_SETUP_KEY\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_EQ(cfg.models.text.context_limit, 32000);
	EXPECT_EQ(cfg.models.text.max_tokens, 4096);
}

TEST_F(CliSetupTest, DeepSeekThinkingCanBeEnabledInPlainSetup) {
	ASSERT_GE(run_custom("2\n\n\n\n2\n\n\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.adapter, "deepseek");
	EXPECT_TRUE(cli_setup_thinking_enabled(&cfg.models.text));
	EXPECT_STREQ(cfg.models.text.extra_body_json,
		"{\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"high\"}");
}

TEST_F(CliSetupTest, ThinkingPromptValidatesAndCanBeCancelled) {
	EXPECT_EQ(run_custom("2\n\n\n\ninvalid\n"), NB_ERR_CANCELLED);
	EXPECT_FALSE(nb_file_exists(path.c_str()));
	ASSERT_GE(run_custom("2\n\n\n\ninvalid\n1\n\n\n1\n1\n1\n"), 0);
	struct config cfg;
	ASSERT_EQ(config_load(NULL, &cfg, path.c_str()), 0);
	EXPECT_FALSE(cli_setup_thinking_enabled(&cfg.models.text));
}

TEST_F(CliSetupTest, VideoRoleRequiresAndConfiguresAllFourModels) {
	ASSERT_GE(run_custom_raw("7\n" + std::string(21, '\n')), 0);
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
	EXPECT_EQ(run_custom_raw("7\n" + std::string(12, '\n')), NB_ERR_CANCELLED);
	EXPECT_FALSE(nb_file_exists(path.c_str()));
	EXPECT_TRUE(std::filesystem::is_empty(dir));
}

TEST_F(CliSetupTest, ReconfigureKeepsValuesCredentialsAndCustomPromptAndCreatesBackup) {
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_6 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const std::string old = "# user comments survive in the backup\n"
		"[model.text]\nprovider='custom'\nadapter='openai-chat-compatible'\n"
		"model='private-model'\napi_base='http://localhost:1234/v1'\napi_key='secret'\n"
		"context_limit=64000\nmax_tokens=4096\ntimeout_seconds=77\n"
		"[memory]\nenabled=false\n[exec]\nnetwork=false\n"
		"[prompt]\nmode='append'\nsystem_prompt_file='/keep/custom.md'\n"
		"[[mcp.servers]]\nname='custom'\ntransport='stdio'\ncommand='custom-command'\n"
		"[context]\nkeep_recent_rounds=3\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), old.c_str(), old.size(), NULL), 0);
	ASSERT_GE(run_custom_raw(std::string(11, '\n'), true), 0);
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
	EXPECT_EQ(cfg.mcp.server_count, 2);
	EXPECT_STREQ(cfg.mcp.servers[0].command, "custom-command");
	EXPECT_STREQ(cfg.mcp.servers[1].name, "exa");
	EXPECT_STREQ(cfg.prompt.system_prompt_file, "/keep/custom.md");
	EXPECT_STREQ(cfg.prompt.mode, "append");
	bool found = false;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().filename().string().find(".backup-") == std::string::npos) continue;
		char *backup =
			(nb_file_read_all(entry.path().c_str(), &file_value_6, 0, NULL) == NB_OK &&
					 (file_copy_6 = (char *)malloc(nb_buf_len(&file_value_6) +
								       1)) != NULL
				 ? (memcpy(file_copy_6, nb_buf_cstr(&file_value_6),
					   nb_buf_len(&file_value_6) + 1),
				    nb_charp_steal(&file_copy_6))
				 : NULL);
		ASSERT_NE(backup, nullptr);
		EXPECT_EQ(std::string(backup), old);
		free(backup);
		found = true;
	}
	EXPECT_TRUE(found);
}

TEST_F(CliSetupTest, ReconfigureCancelAndInvalidConfigLeaveFilesUntouched) {
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_7 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const std::string old = "[memory]\nenabled=false\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), old.c_str(), old.size(), NULL), 0);
	EXPECT_EQ(run_custom_raw("", true), NB_ERR_CANCELLED);
	char *text =
		(nb_file_read_all(path.c_str(), &file_value_7, 0, NULL) == NB_OK &&
				 (file_copy_7 = (char *)malloc(nb_buf_len(&file_value_7) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_7, nb_buf_cstr(&file_value_7),
				   nb_buf_len(&file_value_7) + 1),
			    nb_charp_steal(&file_copy_7))
			 : NULL);
	ASSERT_NE(text, nullptr);
	EXPECT_EQ(std::string(text), old);
	free(text);
	ASSERT_EQ(nb_file_write_all(path.c_str(), "[broken", 7, NULL), 0);
	EXPECT_LT(run_custom_raw(std::string(30, '\n'), true), 0);
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
		std::filesystem::directory_iterator()), 1);
}

TEST_F(CliSetupTest, RolePromptReplacementPreservesOriginalPromptFiles) {
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_8 NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_9 NB_CLEANUP(nb_charp_cleanup) = NULL;

	std::string custom = dir + "/custom.md";
	ASSERT_EQ(nb_file_write_all(custom.c_str(), "Customized instructions", 23, NULL), 0);
	std::string old = "[prompt]\nmode='replace'\nsystem_prompt_file='" + custom + "'\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), old.c_str(), old.size(), NULL), 0);
	ASSERT_GE(run_custom_raw("5\n2\n" + std::string(10, '\n'), true), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STRNE(cfg.prompt.system_prompt_file, custom.c_str());
	char *original =
		(nb_file_read_all(custom.c_str(), &file_value_8, 0, NULL) == NB_OK &&
				 (file_copy_8 = (char *)malloc(nb_buf_len(&file_value_8) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_8, nb_buf_cstr(&file_value_8),
				   nb_buf_len(&file_value_8) + 1),
			    nb_charp_steal(&file_copy_8))
			 : NULL);
	ASSERT_NE(original, nullptr);
	EXPECT_STREQ(original, "Customized instructions");
	free(original);
	char *generated =
		(nb_file_read_all(cfg.prompt.system_prompt_file, &file_value_9, 0, NULL) == NB_OK &&
				 (file_copy_9 = (char *)malloc(nb_buf_len(&file_value_9) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_9, nb_buf_cstr(&file_value_9),
				   nb_buf_len(&file_value_9) + 1),
			    nb_charp_steal(&file_copy_9))
			 : NULL);
	ASSERT_NE(generated, nullptr);
	EXPECT_NE(std::string(generated).find("author's voice"), std::string::npos);
	free(generated);
}

TEST_F(CliSetupTest, EveryRoleRendersAndUsesExplicitModelRequirements) {
	for (int i = 0; i < CLI_SETUP_ROLE_COUNT; i++) {
		char *text = nullptr;
		ASSERT_EQ(cli_setup_role_render_copy(i, &text), 0);
		ASSERT_NE(text, nullptr);
		EXPECT_EQ(std::string(text).find("{{"), std::string::npos);
		EXPECT_NE(std::string(text).find(cli_setup_role_get(i).label), std::string::npos);
		EXPECT_TRUE(cli_setup_role_requires(i, CLI_SETUP_TEXT));
		free(text);
	}
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		EXPECT_TRUE(cli_setup_role_requires(CLI_SETUP_VIDEO_CREATOR, i));
}

TEST_F(CliSetupTest, ReconfigureUsesSavedVideoRoleAndNeverAddsSkipSteps) {
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_10 NB_CLEANUP(nb_charp_cleanup) = NULL;

	ASSERT_GE(run_custom_raw("7\n" + std::string(21, '\n')), 0);
	ASSERT_GE(run_custom_raw(std::string(23, '\n'), true), 0);
	EXPECT_EQ(transcript.find("1) Skip"), std::string::npos);
	EXPECT_NE(transcript.find("Video model (required)"), std::string::npos);
	char *saved =
		(nb_file_read_all(path.c_str(), &file_value_10, 0, NULL) == NB_OK &&
				 (file_copy_10 = (char *)malloc(nb_buf_len(&file_value_10) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_10, nb_buf_cstr(&file_value_10),
				   nb_buf_len(&file_value_10) + 1),
			    nb_charp_steal(&file_copy_10))
			 : NULL);
	ASSERT_NE(saved, nullptr);
	EXPECT_EQ(cli_setup_config_role(saved), CLI_SETUP_VIDEO_CREATOR);
	free(saved);
}

TEST_F(CliSetupTest, ReconfigureRejectsSymlinksAndPublishesPrivateFiles) {
	std::string target = dir + "/target.toml";
	ASSERT_EQ(nb_file_write_all(target.c_str(), "[memory]\nenabled=false\n", 23, NULL), 0);
	ASSERT_EQ(symlink(target.c_str(), path.c_str()), 0);
	EXPECT_LT(run_custom_raw(std::string(20, '\n'), true), 0);
	ASSERT_EQ(unlink(path.c_str()), 0);
	ASSERT_EQ(nb_file_write_all(path.c_str(), "[memory]\nenabled=false\n", 23, NULL), 0);
	ASSERT_EQ(chmod(path.c_str(), 0644), 0);
	ASSERT_GE(run_custom_raw(std::string(11, '\n'), true), 0);
	struct stat st{};
	ASSERT_EQ(stat(path.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST_F(CliSetupTest, SetupConfigMergeSupportsInlineTablesAndUnrelatedArrays) {
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge_copy(
			  "model={text={provider='custom',model='old',timeout_seconds=77}}\n"
			  "[hooks]\nStop=[{hooks=[{type='command',command='echo keep'}]}]\n",
			  "[model.text]\nmodel='new'\n[setup]\nrole='video_creator'\n", &merged),
		  0);
	ASSERT_NE(merged, nullptr);
	EXPECT_NE(std::string(merged).find("timeout_seconds=77"), std::string::npos);
	EXPECT_NE(std::string(merged).find("echo keep"), std::string::npos);
	struct config_validation_error error{};
	EXPECT_EQ(config_validate_text(merged, &error), 0) << error.message;
	EXPECT_EQ(cli_setup_config_role(merged), CLI_SETUP_VIDEO_CREATOR);
	free(merged);
}

TEST_F(CliSetupTest, McpDefaultsVaryByRoleAndReapplicationKeepsExistingEntries) {
	for (int role = 0; role < CLI_SETUP_ROLE_COUNT; role++) {
		char *configured = nullptr;
		ASSERT_EQ(cli_setup_mcp_apply_copy("# keep\n[exec]\nnetwork=false\n", role,
						   &configured),
			  0);
		ASSERT_NE(configured, nullptr);
		char *saved = configured;
		EXPECT_EQ(cli_setup_mcp_apply_copy("", role, &configured), NB_ERR_EXISTS);
		EXPECT_EQ(configured, saved);
		nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(configured), nullptr);
		ASSERT_TRUE(parsed);
		const nb_toml_value *servers = nb_toml_table_get_cstr(
			nb_toml_table_get_cstr(nb_toml_root(parsed), "mcp"), "servers");
		ASSERT_EQ(nb_toml_type_of(servers), NB_TOML_ARRAY);
		EXPECT_STREQ(
			nb_toml_string(nb_toml_table_get_cstr(
					       nb_toml_array_get(servers, (size_t)(0)), "name"))
				.ptr,
			"exa");
		EXPECT_STREQ(nb_toml_string(nb_toml_table_get_cstr(
						    nb_toml_array_get(servers, (size_t)(0)), "url"))
				     .ptr,
			     "https://mcp.exa.ai/mcp");
		int expected = 1 + (role == CLI_SETUP_DEVELOPER || role == CLI_SETUP_RESEARCHER ? 1 : 0) +
			(role == CLI_SETUP_DESIGNER ? 1 : 0);
#ifdef __APPLE__
		if (role == CLI_SETUP_DEVELOPER || role == CLI_SETUP_PHOTOGRAPHER ||
		    role == CLI_SETUP_PHOTO_EDITOR || role == CLI_SETUP_DESIGNER ||
		    role == CLI_SETUP_VIDEO_CREATOR) expected++;
#endif
		EXPECT_EQ(nb_toml_array_size(servers), expected);
		char *again = nullptr;
		ASSERT_EQ(cli_setup_mcp_apply_copy(configured, role, &again), 0);
		EXPECT_STREQ(again, configured);
		EXPECT_EQ(std::string(configured).find("# keep\n[exec]\nnetwork=false\n"), 0u);
		free(again);
		nb_toml_free(parsed);
		free(configured);
	}
}

TEST_F(CliSetupTest, McpAdditionPreservesInlineArraysCommentsAndCredentials) {
	const char *server = "[[mcp.servers]]\nname='exa'\ntransport='http'\n"
		"url='https://mcp.exa.ai/mcp'\nauto_connect=true\n";
	const char *fixtures[] = {
		"# keep\n[mcp]\nservers=[]\n",
		"mcp={servers=[]}\n",
		"mcp={}\n",
		"[mcp]\nservers=[{name='old',transport='stdio',command='server',env={TOKEN='keep'}}]\n",
		"[mcp]\nservers=[\n{name='old',transport='stdio',command='server'}, # keep\n]\n",
		"[[mcp.servers]]\r\nname='old' # keep\r\ntransport='stdio'\r\ncommand='server'\r\n",
	};
	for (const char *fixture : fixtures) {
		SCOPED_TRACE(fixture);
		char *configured = nullptr;
		ASSERT_EQ(cli_setup_config_mcp_add_copy(fixture, server, &configured), 0);
		ASSERT_NE(configured, nullptr);
		if (std::string(fixture).find("TOKEN='keep'") != std::string::npos)
			EXPECT_NE(std::string(configured).find("TOKEN='keep'"), std::string::npos);
		if (std::string(fixture).find("# keep") != std::string::npos)
			EXPECT_NE(std::string(configured).find("# keep"), std::string::npos);
		char *again = nullptr;
		ASSERT_EQ(cli_setup_config_mcp_add_copy(configured, server, &again), 0);
		EXPECT_STREQ(again, configured);
		free(again);
		free(configured);
	}
	const std::string existing = "[[mcp.servers]]\nname='my-search'\ntransport='http'\n"
		"url='https://mcp.exa.ai/mcp?tools=web_search_exa'\nauto_connect=false\n"
		"auth_token_env='PRIVATE_TOKEN'\n";
	char *kept = nullptr;
	ASSERT_EQ(cli_setup_config_mcp_add_copy(existing.c_str(), server, &kept), 0);
	EXPECT_EQ(std::string(kept), existing);
	free(kept);
}

TEST_F(CliSetupTest, LocalMcpDefaultsReflectLaunchersWithoutRunningOrInstallingThem) {
	struct PathGuard {
		bool present = getenv("PATH") != nullptr;
		std::string previous = present ? getenv("PATH") : "";
		~PathGuard() {
			if (present) setenv("PATH", previous.c_str(), 1);
			else unsetenv("PATH");
		}
	} guard;
	ASSERT_EQ(setenv("PATH", dir.c_str(), 1), 0);
	for (int installed : {0, 1}) {
		if (installed) {
			for (const char *name : {"npx", "python3", "uvx"}) {
				std::string executable = dir + "/" + name;
				ASSERT_EQ(nb_file_write_all(executable.c_str(), "fixture", 7, NULL),
					  0);
				ASSERT_EQ(chmod(executable.c_str(), 0700), 0);
			}
		}
		char *configured = nullptr;
		ASSERT_EQ(cli_setup_mcp_apply_copy("", CLI_SETUP_DEVELOPER, &configured), 0);
		nb_toml *parsed = nb_toml_parse(nb_str_from_cstr(configured), nullptr);
		ASSERT_TRUE(parsed);
		const nb_toml_value *servers = nb_toml_table_get_cstr(
			nb_toml_table_get_cstr(nb_toml_root(parsed), "mcp"), "servers");
		for (size_t i = 0; i < nb_toml_array_size(servers); i++) {
			const nb_toml_value *entry = nb_toml_array_get(servers, (size_t)(i));
			const char *name =
				nb_toml_string(nb_toml_table_get_cstr(entry, "name")).ptr;
			EXPECT_EQ(nb_toml_boolean(nb_toml_table_get_cstr(entry, "auto_connect")),
				  strcmp(name, "exa") == 0 || installed != 0)
				<< name;
		}
		nb_toml_free(parsed);
		free(configured);
	}
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
	ASSERT_EQ(cli_setup_config_merge_copy((model + "\n" + mcp).c_str(),
					      "[model.text]\nmodel=\"new\"\nmax_tokens=4096\n",
					      &merged),
		  0);
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
	ASSERT_EQ(cli_setup_config_merge_copy(original.c_str(),
					      "[model.text]\nmodel=\"custom\"\nmax_tokens=4096\n["
					      "prompt]\nmode=\"replace\"\n",
					      &merged),
		  0);
	EXPECT_EQ(std::string(merged), original);
	free(merged);
}

TEST_F(CliSetupTest, SetupEditsMultilineValuesWithoutTouchingDecoyHeadersOrComments) {
	const std::string original = "[model.text]\nmodel = '''old\n[mcp]\n''' # keep comment\n"
		"api_base = \"old\\\"value\" # keep URL comment\n\n[mcp]\nservers = []\n";
	char *merged = nullptr;
	ASSERT_EQ(cli_setup_config_merge_copy(
			  original.c_str(),
			  "[model.text]\nmodel='new'\napi_base='https://new.example'\n", &merged),
		  0);
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
		ASSERT_EQ(
			cli_setup_config_merge_copy(
				original,
				"[prompt]\nmode='replace'\n[model.text]\nmodel='new'\nprovider='"
				"custom'\n"
				"api_base='https://example.test'\n[model.vision]\nmodel='vision'\n",
				&merged),
			0);
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
		ASSERT_EQ(cli_setup_config_merge_copy(original.c_str(),
						      "[model.text]\nmodel='new'\n", &merged),
			  0);
		EXPECT_STREQ(merged, "[model.text]\nmodel = 'new' # keep comment\n\n[mcp]\nservers = []\n");
		free(merged);
	}
}

TEST_F(CliSetupTest, RoleSnapshotsHaveReadableNamesAndReuseUnchangedContent) {
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_11 NB_CLEANUP(nb_charp_cleanup) = NULL;

	ASSERT_GE(run_custom(std::string(9, '\n')), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	std::string first = cfg.prompt.system_prompt_file;
	EXPECT_EQ(first, path + ".prompts/general.md");
	ASSERT_GE(run_custom_raw("1\n2\n" + std::string(10, '\n'), true), 0);
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_EQ(std::string(cfg.prompt.system_prompt_file), first);
	ASSERT_EQ(nb_file_write_all(first.c_str(), "Custom instructions", 19, NULL), 0);
	ASSERT_GE(run_custom_raw("1\n2\n" + std::string(10, '\n'), true), 0);
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	std::filesystem::path next = cfg.prompt.system_prompt_file;
	EXPECT_NE(next.string(), first);
	EXPECT_EQ(next.extension(), ".md");
	EXPECT_EQ(next.filename().string().find("general-"), 0u);
	char *custom =
		(nb_file_read_all(first.c_str(), &file_value_11, 0, NULL) == NB_OK &&
				 (file_copy_11 = (char *)malloc(nb_buf_len(&file_value_11) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_11, nb_buf_cstr(&file_value_11),
				   nb_buf_len(&file_value_11) + 1),
			    nb_charp_steal(&file_copy_11))
			 : NULL);
	ASSERT_NE(custom, nullptr);
	EXPECT_STREQ(custom, "Custom instructions");
	free(custom);
}

TEST(CliSetupThinking, OnlyDeepSeekChangesAndOtherOptionsSurvive) {
	struct config_model_entry entry{};
	cli_setup_model_defaults(CLI_SETUP_TEXT, 1, &entry);
	EXPECT_EQ(cli_setup_set_thinking(&entry, 1), NB_ERR_INVAL);
	EXPECT_STREQ(entry.extra_body_json, "");
	cli_setup_model_defaults(CLI_SETUP_TEXT, 2, &entry);
	strcpy(entry.extra_body_json,
		"{\"reasoning_effort\":\"high\",\"thinking\":{\"type\":\"disabled\"}}");
	ASSERT_EQ(cli_setup_set_thinking(&entry, 1), 0);
	EXPECT_TRUE(cli_setup_thinking_enabled(&entry));
	EXPECT_NE(strstr(entry.extra_body_json, "\"reasoning_effort\":\"high\""), nullptr);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 0), 0);
	EXPECT_FALSE(cli_setup_thinking_enabled(&entry));
}

TEST(CliSetupTokens, ThinkingBudgetsFollowModeWithoutReplacingCustomLimits) {
	struct config_model_entry entry{};
	cli_setup_model_defaults(CLI_SETUP_TEXT, 2, &entry);
	EXPECT_TRUE(cli_setup_thinking_enabled(&entry));
	EXPECT_EQ(entry.max_tokens, 65536);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 0), 0);
	EXPECT_EQ(entry.max_tokens, 16384);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 1), 0);
	EXPECT_EQ(entry.max_tokens, 65536);
	entry.max_tokens = 8192;
	ASSERT_EQ(cli_setup_set_thinking(&entry, 0), 0);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 1), 0);
	EXPECT_EQ(entry.max_tokens, 8192);
	strcpy(entry.extra_body_json,
		"{\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"max\"}");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.max_tokens, 131072);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 0), 0);
	EXPECT_EQ(entry.max_tokens, 16384);
	ASSERT_EQ(cli_setup_set_thinking(&entry, 1), 0);
	EXPECT_EQ(entry.max_tokens, 131072);
}

TEST(CliSetupTokens, VerifiedAliasesAndArkLimitsUseExactModelMetadata) {
	struct config_model_entry entry{};
	for (const char *model : {"deepseek-flash", "deepseek-v4-flash",
				  "deepseek-v4-flash-vision-exp", "deepseek-v4-pro"}) {
		cli_setup_model_defaults(CLI_SETUP_TEXT, 2, &entry);
		strcpy(entry.model, model);
		cli_setup_reset_token_limits(&entry);
		EXPECT_EQ(entry.context_limit, 1048576) << model;
		EXPECT_EQ(entry.max_tokens, 65536) << model;
		int value = 0;
		EXPECT_EQ(cli_setup_parse_token_count("393216", 1,
			cli_setup_token_ceiling(&entry, 1), &value), 0);
		EXPECT_LT(cli_setup_parse_token_count("393217", 1,
			cli_setup_token_ceiling(&entry, 1), &value), 0);
	}
	cli_setup_model_defaults(CLI_SETUP_TEXT, 3, &entry);
	EXPECT_EQ(entry.context_limit, 262144);
	EXPECT_EQ(entry.max_tokens, 16384);
	EXPECT_EQ(cli_setup_token_ceiling(&entry, 1), 131072);
	EXPECT_STREQ(entry.extra_body_json, "{\"thinking\":{\"type\":\"disabled\"}}");
	strcpy(entry.model, "ep-private-deployment");
	cli_setup_reset_token_limits(&entry);
	EXPECT_EQ(entry.context_limit, 128000);
	EXPECT_EQ(entry.max_tokens, 16384);
	EXPECT_EQ(cli_setup_token_ceiling(&entry, 1), INT_MAX);
}

TEST_F(CliSetupTest, ReconfigureKeepsExistingDeepSeekModeBudgetAndTimeout) {
	const std::string original =
		"[model.text]\nprovider='deepseek'\nadapter='deepseek'\n"
		"model='deepseek-v4-flash'\napi_base='https://api.deepseek.com/v1'\n"
		"api_key_env='DEEPSEEK_API_KEY'\ncontext_limit=1048576\nmax_tokens=8192\n"
		"timeout_seconds=120\nretry_count=0\n"
		"extra_body_json='{\"thinking\":{\"type\":\"disabled\"}}'\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), original.c_str(), original.size(), NULL), 0);
	ASSERT_GE(run_custom_raw(std::string(20, '\n'), true), 0);
	struct config cfg;
	ASSERT_EQ(config_load(nullptr, &cfg, path.c_str()), 0);
	EXPECT_STREQ(cfg.models.text.model, "deepseek-v4-flash");
	EXPECT_EQ(cfg.models.text.max_tokens, 8192);
	EXPECT_EQ(cfg.models.text.timeout_seconds, 120);
	EXPECT_EQ(cfg.models.text.retry_count, 0);
	EXPECT_FALSE(cli_setup_thinking_enabled(&cfg.models.text));
}

TEST_F(CliSetupTest, ConfigurationCopiesPreserveExistingOwner)
{
	char *text = strdup("[exec]\nnetwork=false\n");
	ASSERT_NE(text, nullptr);
	char *saved = text;
	EXPECT_EQ(cli_setup_config_merge_copy("", "", &text), NB_ERR_EXISTS);
	EXPECT_EQ(cli_setup_config_credentials_copy("", &text), NB_ERR_EXISTS);
	EXPECT_EQ(cli_setup_config_mcp_add_copy("", "", &text), NB_ERR_EXISTS);
	EXPECT_EQ(text, saved);
	EXPECT_STREQ(text, "[exec]\nnetwork=false\n");
	free(text);
}
