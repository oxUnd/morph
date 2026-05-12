#include <gtest/gtest.h>
#include "agent/tool.h"
#include <string.h>

static int mock_tool_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json;
	(void)user_data;
	*result_json = strdup("{\"status\":\"ok\"}");
	return 0;
}

static int error_tool_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json;
	(void)result_json;
	(void)user_data;
	return -EIO;
}

class ToolTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	void SetUp() override { tool_registry_init(&reg); }
};

TEST_F(ToolTest, Init) {
	EXPECT_EQ(reg.count, 0);
}

TEST_F(ToolTest, RegisterAndLookup) {
	int rc = tool_register(&reg, "test_tool", "A test tool",
			       "{\"type\":\"object\"}", mock_tool_exec, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "test_tool");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "test_tool");
}

TEST_F(ToolTest, RegisterMultiple) {
	tool_register(&reg, "tool1", "First", nullptr, mock_tool_exec, nullptr);
	tool_register(&reg, "tool2", "Second", nullptr, mock_tool_exec, nullptr);
	tool_register(&reg, "tool3", "Third", nullptr, mock_tool_exec, nullptr);
	EXPECT_EQ(reg.count, 3);
}

TEST_F(ToolTest, DuplicateName) {
	int rc1 = tool_register(&reg, "dup", "First", nullptr, mock_tool_exec, nullptr);
	EXPECT_EQ(rc1, 0);
	int rc2 = tool_register(&reg, "dup", "Second", nullptr, mock_tool_exec, nullptr);
	EXPECT_NE(rc2, 0);
}

TEST_F(ToolTest, LookupNotFound) {
	struct tool_entry *e = tool_lookup(&reg, "nonexistent");
	EXPECT_EQ(e, nullptr);
}

TEST_F(ToolTest, ExecTool) {
	tool_register(&reg, "exec_test", "Exec test", nullptr, mock_tool_exec, nullptr);
	char *result = nullptr;
	int rc = tool_exec(&reg, "exec_test", "{}", &result);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(result, nullptr);
	free(result);
}

TEST_F(ToolTest, ExecNotFound) {
	char *result = nullptr;
	int rc = tool_exec(&reg, "nonexistent", "{}", &result);
	EXPECT_NE(rc, 0);
}

TEST_F(ToolTest, ExecError) {
	tool_register(&reg, "error_tool", "Error tool", nullptr, error_tool_exec, nullptr);
	char *result = nullptr;
	int rc = tool_exec(&reg, "error_tool", "{}", &result);
	EXPECT_NE(rc, 0);
}

TEST_F(ToolTest, NullParams) {
	EXPECT_NE(tool_register(nullptr, "x", "x", nullptr, mock_tool_exec, nullptr), 0);
	EXPECT_NE(tool_register(&reg, nullptr, "x", nullptr, mock_tool_exec, nullptr), 0);
	EXPECT_EQ(tool_lookup(nullptr, "x"), nullptr);
	EXPECT_EQ(tool_lookup(&reg, nullptr), nullptr);
	EXPECT_NE(tool_exec(nullptr, "x", "{}", nullptr), 0);
}

TEST_F(ToolTest, MaxEntries) {
	for (int i = 0; i < TOOL_MAX_ENTRIES; i++) {
		char name[32];
		snprintf(name, sizeof(name), "tool_%d", i);
		int rc = tool_register(&reg, name, "desc", nullptr, mock_tool_exec, nullptr);
		EXPECT_EQ(rc, 0);
	}
	int rc = tool_register(&reg, "overflow", "desc", nullptr, mock_tool_exec, nullptr);
	EXPECT_NE(rc, 0);
}