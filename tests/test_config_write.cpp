#include <gtest/gtest.h>
#include "agent/tools/config_write.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/error.h"
#include "util/file.h"
#include "cJSON.h"
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

class ConfigEditTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	struct tool_context *tctx;
	char config_path[256];
	char other_path[256];

	void SetUp() override {
		tool_registry_init(&reg);
		snprintf(config_path, sizeof(config_path),
			 "/tmp/morph_config_write_%d.toml", getpid());
		snprintf(other_path, sizeof(other_path),
			 "/tmp/morph_config_write_other_%d.toml", getpid());
		std::remove(config_path);
		std::remove(other_path);
		std::remove((std::string(config_path) + ".bak").c_str());
		std::remove((std::string(config_path) + ".lock").c_str());
		tctx = tool_context_create("/tmp", "/tmp");
	}

	void TearDown() override {
		tool_context_destroy(tctx);
		tool_registry_cleanup(&reg);
		std::remove(config_path);
		std::remove(other_path);
		std::remove((std::string(config_path) + ".bak").c_str());
		std::remove((std::string(config_path) + ".lock").c_str());
	}
};

struct approval_state {
	enum tool_operation_verdict verdict;
	int calls;
	std::string target;
};

static enum tool_operation_verdict approval_cb(
	const struct tool_operation *op, void *user_data)
{
	struct approval_state *state =
		static_cast<struct approval_state *>(user_data);
	state->calls++;
	if (op && op->target)
		state->target = op->target;
	return state->verdict;
}

struct mutating_approval_state {
	const char *path;
	const char *content;
	int calls;
};

static enum tool_operation_verdict mutating_approval_cb(
	const struct tool_operation *op, void *user_data)
{
	auto *state = static_cast<struct mutating_approval_state *>(user_data);

	(void)op;
	state->calls++;
	(void)file_write_all(state->path, state->content,
		std::strlen(state->content));
	return TOOL_OP_ALLOW;
}

static std::string json_escape(const char *s)
{
	cJSON *str = cJSON_CreateString(s);
	char *printed = cJSON_PrintUnformatted(str);
	std::string out = printed ? printed : "\"\"";
	free(printed);
	cJSON_Delete(str);
	return out;
}

static std::string exec_config_edit(struct tool_registry *reg,
				    const std::string &args, int *out_rc)
{
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(reg, "config_edit", args.c_str(), &result);
	if (out_rc)
		*out_rc = rc;
	std::string text(result.text.data ? result.text.data : "");
	tool_result_cleanup(&result);
	return text;
}

TEST_F(ConfigEditTest, InitRegistersTool)
{
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	struct tool_entry *e = tool_lookup(&reg, "config_edit");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "config_edit");
	EXPECT_EQ(tool_lookup(&reg, "config_write"), nullptr);
	EXPECT_TRUE(e->flags & TOOL_FLAG_INTERNAL_APPROVAL);
	EXPECT_NE(std::strstr(e->desc.description, "atomically"), nullptr);
}

TEST_F(ConfigEditTest, WritesApprovedValidToml)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"docs\"\n";
	std::string args = std::string("{\"reason\":\"set default session\",")
		+ "\"content\":" + json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, 0) << out;
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(state.target, config_path);
	FILE *f = fopen(config_path, "rb");
	ASSERT_NE(f, nullptr);
	char buf[128] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	EXPECT_GT(n, 0u);
	EXPECT_NE(strstr(buf, "default_session"), nullptr);
	EXPECT_NE(out.find("\"validated\":true"), std::string::npos);
	EXPECT_NE(out.find("\"restart_required\":true"), std::string::npos);
	EXPECT_NE(out.find("\"hot_reloaded\":false"), std::string::npos);
}

TEST_F(ConfigEditTest, DeniesOtherPath)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"bad\"\n";
	std::string args = std::string("{\"path\":")
		+ json_escape(other_path)
		+ ",\"reason\":\"write elsewhere\",\"content\":"
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("active Morph config"), std::string::npos);
}

TEST_F(ConfigEditTest, RejectsInvalidToml)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	std::string args = std::string("{\"reason\":\"bad toml\",\"content\":")
		+ json_escape("[general\nbad = true\n") + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, MORPH_ERR_PARSE);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("invalid configuration"), std::string::npos);
}

TEST_F(ConfigEditTest, RejectsSemanticallyInvalidConfigBeforeApproval)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	const char *toml = "[react]\nmax_iterations = \"many\"\n";
	std::string args = std::string("{\"reason\":\"bad config\",\"content\":")
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, MORPH_ERR_CONFIG);
	EXPECT_EQ(state.calls, 0);
	EXPECT_EQ(access(config_path, F_OK), -1);
	EXPECT_NE(out.find("react.max_iterations"), std::string::npos);
}

TEST_F(ConfigEditTest, AllowsUnknownKeysForForwardCompatibility)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	const char *toml = "[future]\nenabled = true\n";
	std::string args = std::string(
		"{\"reason\":\"future config\",\"content\":") +
		json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_NE(out.find("configuration edited safely"), std::string::npos);
}

TEST_F(ConfigEditTest, DeniedApprovalDoesNotWrite)
{
	struct approval_state state = { TOOL_OP_DENY, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"deny\"\n";
	std::string args = std::string("{\"reason\":\"deny write\",\"content\":")
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(access(config_path, F_OK), -1);
	EXPECT_NE(out.find("denied"), std::string::npos);
}

TEST_F(ConfigEditTest, PatchPreservesFormattingAndCreatesBackup)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	const char *original =
		"# keep this comment\n[model.text]\nmax_tokens = 4096\n";
	ASSERT_EQ(file_write_all(config_path, original, std::strlen(original)), 0);
	const char *patch =
		"*** Begin Patch\n"
		"*** Update File: config.toml\n"
		"@@\n"
		"-max_tokens = 4096\n"
		"+max_tokens = 32768\n"
		"*** End Patch";
	std::string args = std::string("{\"reason\":\"raise output budget\",")
		+ "\"patch\":" + json_escape(patch) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, 0) << out;
	EXPECT_EQ(state.calls, 1);
	char *edited = file_read_all(config_path, nullptr);
	char *backup = file_read_all(
		(std::string(config_path) + ".bak").c_str(), nullptr);
	ASSERT_NE(edited, nullptr);
	ASSERT_NE(backup, nullptr);
	EXPECT_NE(std::strstr(edited, "# keep this comment"), nullptr);
	EXPECT_NE(std::strstr(edited, "max_tokens = 32768"), nullptr);
	EXPECT_STREQ(backup, original);
	EXPECT_NE(out.find("\"atomic\":true"), std::string::npos);
	EXPECT_NE(out.find("\"backup_created\":true"), std::string::npos);
	std::free(edited);
	std::free(backup);
}

TEST_F(ConfigEditTest, RejectsPatchForAnotherFile)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	const char *original = "[general]\ndefault_session = \"before\"\n";
	ASSERT_EQ(file_write_all(config_path, original, std::strlen(original)), 0);
	const char *patch =
		"*** Begin Patch\n*** Update File: other.toml\n@@\n"
		"-default_session = \"before\"\n"
		"+default_session = \"after\"\n*** End Patch";
	std::string args = std::string("{\"reason\":\"wrong file\",\"patch\":")
		+ json_escape(patch) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("only update config.toml"), std::string::npos);
	char *current = file_read_all(config_path, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_STREQ(current, original);
	std::free(current);
}

TEST_F(ConfigEditTest, RejectsInvalidPatchedConfigBeforeApproval)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	const char *original = "[react]\nmax_iterations = 10\n";
	ASSERT_EQ(file_write_all(config_path, original, std::strlen(original)), 0);
	const char *patch =
		"*** Begin Patch\n*** Update File: config.toml\n@@\n"
		"-max_iterations = 10\n"
		"+max_iterations = \"many\"\n*** End Patch";
	std::string args = std::string("{\"reason\":\"bad value\",\"patch\":")
		+ json_escape(patch) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, MORPH_ERR_CONFIG);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("react.max_iterations"), std::string::npos);
	char *current = file_read_all(config_path, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_STREQ(current, original);
	std::free(current);
}

TEST_F(ConfigEditTest, DetectsConcurrentConfigChange)
{
	const char *original = "[general]\ndefault_session = \"before\"\n";
	const char *concurrent = "[general]\ndefault_session = \"other\"\n";
	struct mutating_approval_state state = {
		config_path, concurrent, 0
	};
	tool_context_set_operation_approval(tctx, mutating_approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	ASSERT_EQ(file_write_all(config_path, original, std::strlen(original)), 0);
	const char *desired = "[general]\ndefault_session = \"after\"\n";
	std::string args = std::string("{\"reason\":\"change session\",")
		+ "\"content\":" + json_escape(desired) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -EAGAIN);
	EXPECT_EQ(state.calls, 1);
	EXPECT_NE(out.find("changed during approval"), std::string::npos);
	char *current = file_read_all(config_path, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_STREQ(current, concurrent);
	std::free(current);
}

TEST_F(ConfigEditTest, RequiresApprovalContext)
{
	ASSERT_EQ(config_edit_init(&reg, nullptr, config_path), 0);
	const char *toml = "[general]\ndefault_session = \"unsafe\"\n";
	std::string args = std::string("{\"reason\":\"unsafe\",\"content\":")
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(access(config_path, F_OK), -1);
	EXPECT_NE(out.find("approval context"), std::string::npos);
}

TEST_F(ConfigEditTest, RejectsSymlinkConfig)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_edit_init(&reg, tctx, config_path), 0);
	const char *original = "[general]\ndefault_session = \"target\"\n";
	ASSERT_EQ(file_write_all(other_path, original, std::strlen(original)), 0);
	ASSERT_EQ(symlink(other_path, config_path), 0);
	const char *desired = "[general]\ndefault_session = \"changed\"\n";
	std::string args = std::string("{\"reason\":\"edit link\",\"content\":")
		+ json_escape(desired) + "}";
	int rc = 0;
	std::string out = exec_config_edit(&reg, args, &rc);

	EXPECT_EQ(rc, -ELOOP);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("not a symlink"), std::string::npos);
	char *current = file_read_all(other_path, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_STREQ(current, original);
	std::free(current);
}
