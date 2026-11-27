#include <gtest/gtest.h>
#include "agent/tools/config_validate.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <cstdio>
#include <string>
#include <unistd.h>

class ConfigValidateTest : public ::testing::Test {
protected:
	struct tool_registry reg = {};
	struct tool_context *tctx = nullptr;
	std::string path;

	void SetUp() override
	{
		tool_registry_init(nullptr, &reg);
		path = "/tmp/morph_config_validate_" + std::to_string(getpid()) + ".toml";
		tctx = tool_context_create(nullptr, "/tmp", "/tmp");
		ASSERT_NE(tctx, nullptr);
		ASSERT_EQ(config_validate_init(&reg, tctx, path.c_str()), NB_OK);
	}

	void TearDown() override
	{
		tool_registry_cleanup(&reg);
		tool_context_destroy(tctx);
		std::remove(path.c_str());
	}

	std::string run(const std::string &args, int expected = NB_OK)
	{
		struct tool_result result = {};
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&reg, "config_validate", args.c_str(), &result), expected);
		std::string text(result.text.ptr ? result.text.ptr : "");
		tool_result_cleanup(&result);
		return text;
	}
};

TEST_F(ConfigValidateTest, ReplacesEditTool)
{
	EXPECT_NE(tool_borrow(&reg, "config_validate"), nullptr);
	EXPECT_EQ(tool_borrow(&reg, "config_edit"), nullptr);
	EXPECT_TRUE(tool_borrow(&reg, "config_validate")->flags & TOOL_FLAG_READONLY);
}

TEST_F(ConfigValidateTest, ValidatesContentAndReportsFieldError)
{
	EXPECT_NE(run(R"({"content":"[general]\ndefault_session = 'test'\n"})").find(
		"\"valid\":true"), std::string::npos);
	std::string out = run(R"({"content":"[general]\ndefault_session = 42\n"})");
	EXPECT_NE(out.find("\"valid\":false"), std::string::npos);
	EXPECT_NE(out.find("general.default_session"), std::string::npos);
}

TEST_F(ConfigValidateTest, SyntaxErrorDoesNotEchoCredentials)
{
	std::string out = run(R"({"content":"[model.text]\napi_key = 'private-secret\n"})");
	EXPECT_NE(out.find("\"valid\":false"), std::string::npos);
	EXPECT_EQ(out.find("private-secret"), std::string::npos);
	EXPECT_NE(out.find("\"line\":"), std::string::npos);
}

TEST_F(ConfigValidateTest, RejectsAmbiguousAndInvalidArguments)
{
	run(R"({"path":"test","content":""})", NB_ERR_INVAL);
	run(R"({"path":false})", NB_ERR_INVAL);
	run(R"({"path":""})", NB_ERR_INVAL);
	run("[]", NB_ERR_INVAL);
}

TEST_F(ConfigValidateTest, ReadsCurrentAndExternalChangesWithoutWritingInAllModes)
{
	const struct permission_policy policies[] = {
		{PERMISSION_WORKSPACE, PERMISSION_ON_REQUEST, PERMISSION_USER},
		{PERMISSION_WORKSPACE, PERMISSION_ON_REQUEST, PERMISSION_MODEL},
		{PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER},
		{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER}
	};
	for (const auto &policy : policies) {
		ASSERT_EQ(tool_context_set_permission_policy(tctx, &policy), NB_OK);
		const std::string valid = "[general]\ndefault_session = 'external'\n";
		ASSERT_EQ(nb_file_write_all(path.c_str(), valid.data(), valid.size(), nullptr), NB_OK);
		EXPECT_NE(run("{}").find("\"valid\":true"), std::string::npos);
		const std::string invalid = "[general]\ndefault_session = 42\n";
		ASSERT_EQ(nb_file_write_all(path.c_str(), invalid.data(), invalid.size(), nullptr), NB_OK);
		std::string args = "{\"path\":\"" + path + "\"}";
		EXPECT_NE(run(args).find("\"valid\":false"), std::string::npos);
		nb_buf contents NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_EQ(nb_file_read_all(path.c_str(), &contents, 0, nullptr), NB_OK);
		EXPECT_EQ(std::string(nb_buf_cstr(&contents)), invalid);
		EXPECT_NE(access((path + ".bak").c_str(), F_OK), 0);
		EXPECT_NE(access((path + ".lock").c_str(), F_OK), 0);
	}
}

TEST_F(ConfigValidateTest, PreservesCompatibilityWarnings)
{
	std::string out = run(R"({"content":"[general]\nfuture_key = true\n"})");
	EXPECT_NE(out.find("\"valid\":true"), std::string::npos);
	EXPECT_NE(out.find("general.future_key"), std::string::npos);
}

TEST_F(ConfigValidateTest, MissingFileIsAnInspectionFailure)
{
	std::remove(path.c_str());
	run("{}", NB_ERR_NOT_FOUND);
}

TEST_F(ConfigValidateTest, DeniedReadLeavesFileUnchanged)
{
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(nb_file_temp_dir(&directory, "/tmp", "morph-validate-policy-", nullptr), NB_OK);
	tool_registry_cleanup(&reg);
	tool_context_destroy(tctx);
	tctx = tool_context_create(nullptr, nb_buf_cstr(&directory), nb_buf_cstr(&directory));
	ASSERT_NE(tctx, nullptr);
	tool_registry_init(nullptr, &reg);
	ASSERT_EQ(config_validate_init(&reg, tctx, path.c_str()), NB_OK);
	const struct permission_policy policy = {
		PERMISSION_WORKSPACE, PERMISSION_NEVER, PERMISSION_USER
	};
	ASSERT_EQ(tool_context_set_permission_policy(tctx, &policy), NB_OK);
	const std::string source = "[general]\ndefault_session = 'unchanged'\n";
	ASSERT_EQ(nb_file_write_all(path.c_str(), source.data(), source.size(), nullptr), NB_OK);
	run("{}", NB_ERR_PERMISSION);
	nb_buf contents NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(nb_file_read_all(path.c_str(), &contents, 0, nullptr), NB_OK);
	EXPECT_EQ(std::string(nb_buf_cstr(&contents)), source);
	rmdir(nb_buf_cstr(&directory));
}
