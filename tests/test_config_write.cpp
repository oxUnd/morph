#include <gtest/gtest.h>
#include "agent/tools/config_write.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/error.h"
#include "cJSON.h"
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

class ConfigWriteTest : public ::testing::Test {
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
		tctx = tool_context_create("/tmp", "/tmp");
	}

	void TearDown() override {
		tool_context_destroy(tctx);
		tool_registry_cleanup(&reg);
		std::remove(config_path);
		std::remove(other_path);
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

static std::string json_escape(const char *s)
{
	cJSON *str = cJSON_CreateString(s);
	char *printed = cJSON_PrintUnformatted(str);
	std::string out = printed ? printed : "\"\"";
	free(printed);
	cJSON_Delete(str);
	return out;
}

static std::string exec_config_write(struct tool_registry *reg,
				     const std::string &args, int *out_rc)
{
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(reg, "config_write", args.c_str(), &result);
	if (out_rc)
		*out_rc = rc;
	std::string text(result.text.data ? result.text.data : "");
	tool_result_cleanup(&result);
	return text;
}

TEST_F(ConfigWriteTest, InitRegistersTool)
{
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);
	struct tool_entry *e = tool_lookup(&reg, "config_write");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "config_write");
	EXPECT_TRUE(e->flags & TOOL_FLAG_INTERNAL_APPROVAL);
}

TEST_F(ConfigWriteTest, WritesApprovedValidToml)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"docs\"\n";
	std::string args = std::string("{\"reason\":\"set default session\",")
		+ "\"content\":" + json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

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
}

TEST_F(ConfigWriteTest, DeniesOtherPath)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"bad\"\n";
	std::string args = std::string("{\"path\":")
		+ json_escape(other_path)
		+ ",\"reason\":\"write elsewhere\",\"content\":"
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("active Morph config"), std::string::npos);
}

TEST_F(ConfigWriteTest, RejectsInvalidToml)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	std::string args = std::string("{\"reason\":\"bad toml\",\"content\":")
		+ json_escape("[general\nbad = true\n") + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

	EXPECT_EQ(rc, MORPH_ERR_PARSE);
	EXPECT_EQ(state.calls, 0);
	EXPECT_NE(out.find("invalid configuration"), std::string::npos);
}

TEST_F(ConfigWriteTest, RejectsSemanticallyInvalidConfigBeforeApproval)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	const char *toml = "[react]\nmax_iterations = \"many\"\n";
	std::string args = std::string("{\"reason\":\"bad config\",\"content\":")
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

	EXPECT_EQ(rc, MORPH_ERR_CONFIG);
	EXPECT_EQ(state.calls, 0);
	EXPECT_EQ(access(config_path, F_OK), -1);
	EXPECT_NE(out.find("react.max_iterations"), std::string::npos);
}

TEST_F(ConfigWriteTest, AllowsUnknownKeysForForwardCompatibility)
{
	struct approval_state state = { TOOL_OP_ALLOW, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	const char *toml = "[future]\nenabled = true\n";
	std::string args = std::string(
		"{\"reason\":\"future config\",\"content\":") +
		json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_NE(out.find("configuration written"), std::string::npos);
}

TEST_F(ConfigWriteTest, DeniedApprovalDoesNotWrite)
{
	struct approval_state state = { TOOL_OP_DENY, 0, "" };
	tool_context_set_operation_approval(tctx, approval_cb, &state);
	ASSERT_EQ(config_write_init(&reg, tctx, config_path), 0);

	const char *toml = "[general]\ndefault_session = \"deny\"\n";
	std::string args = std::string("{\"reason\":\"deny write\",\"content\":")
		+ json_escape(toml) + "}";
	int rc = 0;
	std::string out = exec_config_write(&reg, args, &rc);

	EXPECT_EQ(rc, -EACCES);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(access(config_path, F_OK), -1);
	EXPECT_NE(out.find("denied"), std::string::npos);
}
