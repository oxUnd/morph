#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tools/file_read.h"
#include "agent/tools/file_list.h"
#include "agent/tools/file_info.h"
#include "util/file.h"
#include <string.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

static int mock_tool_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_take_text(result, strdup("{\"status\":\"ok\"}"));
	return 0;
}

static int error_tool_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)result;
	(void)user_data;
	return -EIO;
}

class ToolTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	void SetUp() override { tool_registry_init(&reg); }
	void TearDown() override { tool_registry_cleanup(&reg); }
};

TEST_F(ToolTest, Init) {
	EXPECT_EQ(reg.count, 0);
}

TEST_F(ToolTest, RegisterAndLookup) {
	int rc = tool_register(TOOL_ORIGIN_BUILTIN, &reg, "test_tool", "A test tool",
			       "{\"type\":\"object\"}", mock_tool_exec, nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "test_tool");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "test_tool");
	EXPECT_EQ(morph_strmap_get(&reg.by_name, "test_tool"), e);
}

TEST_F(ToolTest, RegisterMultiple) {
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "tool1", "First", nullptr, mock_tool_exec, nullptr, nullptr);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "tool2", "Second", nullptr, mock_tool_exec, nullptr, nullptr);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "tool3", "Third", nullptr, mock_tool_exec, nullptr, nullptr);
	EXPECT_EQ(reg.count, 3);
}

TEST_F(ToolTest, DuplicateName) {
	int rc1 = tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dup", "First", nullptr, mock_tool_exec, nullptr, nullptr);
	EXPECT_EQ(rc1, 0);
	int rc2 = tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dup", "Second", nullptr, mock_tool_exec, nullptr, nullptr);
	EXPECT_NE(rc2, 0);
}

TEST_F(ToolTest, DisableUsesNameIndex) {
	EXPECT_EQ(tool_disable(&reg, "blocked"), 0);
	EXPECT_TRUE(tool_is_disabled(&reg, "blocked"));
	EXPECT_TRUE(morph_strmap_contains(&reg.disabled_by_name, "blocked"));
	EXPECT_EQ(tool_disable(&reg, "blocked"), 0);
	EXPECT_EQ(reg.disabled_count, 1);
}

TEST_F(ToolTest, LookupNotFound) {
	struct tool_entry *e = tool_lookup(&reg, "nonexistent");
	EXPECT_EQ(e, nullptr);
}

TEST_F(ToolTest, ExecTool) {
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "exec_test", "Exec test", nullptr, mock_tool_exec, nullptr, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "exec_test", "{}", &result);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ToolTest, ExecNotFound) {
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "nonexistent", "{}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(ToolTest, ExecError) {
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "error_tool", "Error tool", nullptr, error_tool_exec, nullptr, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "error_tool", "{}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(ToolTest, StructuredResultFieldsAreOwnedAndCleared) {
	struct tool_result result;
	tool_result_init(&result);

	cJSON *data = cJSON_CreateObject();
	ASSERT_NE(data, nullptr);
	ASSERT_NE(cJSON_AddStringToObject(data, "kind", "test"), nullptr);

	cJSON *ui = cJSON_CreateObject();
	ASSERT_NE(ui, nullptr);
	ASSERT_NE(cJSON_AddStringToObject(ui, "component", "test"), nullptr);

	ASSERT_EQ(tool_result_take_data(&result, data), 0);
	ASSERT_EQ(tool_result_take_ui(&result, ui), 0);
	ASSERT_EQ(tool_result_add_image(&result, "/tmp/out.png", 320, 240), 0);
	ASSERT_NE(result.data, nullptr);
	ASSERT_NE(result.ui, nullptr);
	ASSERT_EQ(result.artifacts.count, 1);
	EXPECT_STREQ(result.artifacts.items[0].path, "/tmp/out.png");
	EXPECT_EQ(result.artifacts.items[0].kind, TOOL_ARTIFACT_IMAGE);
	EXPECT_EQ(result.artifacts.items[0].width, 320);

	tool_result_clear(&result);
	EXPECT_EQ(result.data, nullptr);
	EXPECT_EQ(result.ui, nullptr);
	EXPECT_EQ(result.artifacts.count, 0);

	tool_result_cleanup(&result);
}

TEST_F(ToolTest, NullParams) {
	EXPECT_NE(tool_register(TOOL_ORIGIN_BUILTIN, nullptr, "x", "x", nullptr, mock_tool_exec, nullptr, nullptr), 0);
	EXPECT_NE(tool_register(TOOL_ORIGIN_BUILTIN, &reg, nullptr, "x", nullptr, mock_tool_exec, nullptr, nullptr), 0);
	EXPECT_EQ(tool_lookup(nullptr, "x"), nullptr);
	EXPECT_EQ(tool_lookup(&reg, nullptr), nullptr);
	EXPECT_NE(tool_exec(nullptr, "x", "{}", nullptr), 0);
}

TEST_F(ToolTest, MaxEntries) {
	for (int i = 0; i < TOOL_MAX_ENTRIES; i++) {
		char name[32];
		snprintf(name, sizeof(name), "tool_%d", i);
		int rc = tool_register(TOOL_ORIGIN_BUILTIN, &reg, name, "desc", nullptr, mock_tool_exec, nullptr, nullptr);
		EXPECT_EQ(rc, 0);
	}
	int rc = tool_register(TOOL_ORIGIN_BUILTIN, &reg, "overflow", "desc", nullptr, mock_tool_exec, nullptr, nullptr);
	EXPECT_NE(rc, 0);
}

/* ---- user_data_destroy tests ---- */

static int g_destroy_call_count;
static void *g_destroy_last_ud;

static void counting_destroy(void *ud)
{
	g_destroy_call_count++;
	g_destroy_last_ud = ud;
	free(ud);
}

static void dummy_dtor(void *ud)
{
	(void)ud;
}

TEST_F(ToolTest, UserDataDestroyCalled) {
	g_destroy_call_count = 0;
	g_destroy_last_ud = nullptr;
	int *val = (int *)malloc(sizeof(int));
	*val = 42;
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dtor_tool", "has dtor", nullptr, mock_tool_exec,
		      val, counting_destroy);
	tool_registry_cleanup(&reg);
	EXPECT_EQ(g_destroy_call_count, 1);
	EXPECT_EQ(g_destroy_last_ud, val);
}

TEST_F(ToolTest, UserDataDestroyNullUd) {
	g_destroy_call_count = 0;
	g_destroy_last_ud = nullptr;
	static int dummy;
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "null_ud", "null ud", nullptr, mock_tool_exec,
		      nullptr, counting_destroy);
	tool_registry_cleanup(&reg);
	EXPECT_EQ(g_destroy_call_count, 0);
}

TEST_F(ToolTest, UserDataDestroyFnNull) {
	int *val = (int *)malloc(sizeof(int));
	*val = 99;
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "no_dtor", "no dtor", nullptr, mock_tool_exec,
		      val, nullptr);
	tool_registry_cleanup(&reg);
}

TEST_F(ToolTest, RegisterStoresUserDataDestroy) {
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "stored", "test", nullptr, mock_tool_exec,
		      nullptr, dummy_dtor);
	struct tool_entry *e = tool_lookup(&reg, "stored");
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->user_data_destroy, dummy_dtor);
}

TEST_F(ToolTest, MixedUserDataDestroyTypes) {
	g_destroy_call_count = 0;

	int *mcp_ud = (int *)calloc(1, sizeof(int));
	*mcp_ud = 1;
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "mcp_tool", "mcp", nullptr, mock_tool_exec,
		      mcp_ud, (tool_user_data_destroy_fn)free);

	int *ext_ud = (int *)malloc(sizeof(int));
	*ext_ud = 2;
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "ext_tool", "ext", nullptr, mock_tool_exec,
		      ext_ud, counting_destroy);

	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "builtin", "builtin", nullptr, mock_tool_exec,
		      nullptr, nullptr);

	tool_registry_cleanup(&reg);
	EXPECT_EQ(g_destroy_call_count, 1);
	EXPECT_EQ(g_destroy_last_ud, ext_ud);
}

TEST_F(ToolTest, RegisterStoresOrigin) {
	ASSERT_EQ(tool_register(TOOL_ORIGIN_MCP, &reg, "mcp_tool", "mcp",
				nullptr, mock_tool_exec, nullptr, nullptr), 0);
	struct tool_entry *e = tool_lookup(&reg, "mcp_tool");
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->origin, TOOL_ORIGIN_MCP);
	EXPECT_STREQ(tool_origin_name(TOOL_ORIGIN_DYNAMIC_SESSION),
		     "dynamic session");
}

TEST_F(ToolTest, UnregisterRemovesEntryAndRebuildsIndex) {
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "first", "first",
				nullptr, mock_tool_exec, nullptr, nullptr), 0);
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "middle", "middle",
				nullptr, mock_tool_exec, nullptr, nullptr), 0);
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "last", "last",
				nullptr, mock_tool_exec, nullptr, nullptr), 0);
	ASSERT_EQ(tool_unregister(&reg, "middle"), 0);
	EXPECT_EQ(reg.count, 2);
	EXPECT_EQ(tool_lookup(&reg, "middle"), nullptr);
	ASSERT_NE(tool_lookup(&reg, "last"), nullptr);
	EXPECT_EQ(morph_strmap_get(&reg.by_name, "last"),
		  tool_lookup(&reg, "last"));

	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "last", "{}", &result), 0);
	tool_result_cleanup(&result);
}

TEST_F(ToolTest, UnregisterCallsDestroyAndClearsDisabled) {
	g_destroy_call_count = 0;
	g_destroy_last_ud = nullptr;
	int *val = (int *)malloc(sizeof(int));
	ASSERT_NE(val, nullptr);
	*val = 7;
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "drop_me", "drop",
				nullptr, mock_tool_exec, val,
				counting_destroy), 0);
	ASSERT_EQ(tool_disable(&reg, "drop_me"), 0);
	ASSERT_EQ(tool_unregister(&reg, "drop_me"), 0);
	EXPECT_EQ(g_destroy_call_count, 1);
	EXPECT_EQ(g_destroy_last_ud, val);
	EXPECT_FALSE(tool_is_disabled(&reg, "drop_me"));
	EXPECT_EQ(reg.disabled_count, 0);
	EXPECT_EQ(tool_unregister(&reg, "drop_me"), -ENOENT);
}

TEST_F(ToolTest, CleanupIdempotent) {
	int *val = (int *)malloc(sizeof(int));
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "once", "once", nullptr, mock_tool_exec,
		      val, (tool_user_data_destroy_fn)free);
	tool_registry_cleanup(&reg);
	tool_registry_cleanup(&reg);
}

TEST_F(ToolTest, FileReadUsesWorkdirPolicy) {
	const char *work = "/tmp/morph_tool_work";
	const char *path = "/tmp/morph_tool_work/read.txt";
	file_ensure_dir(work);
	file_write_all(path, "hello\n", 6);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tool_out");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(file_read_init(&reg, tctx), 0);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "file_read",
			   "{\"file_path\":\"read.txt\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "hello"), nullptr);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(path);
	rmdir(work);
}

TEST_F(ToolTest, FileReadDeniesSymlinkEscape) {
	const char *work = "/tmp/morph_tool_work";
	const char *secret = "/tmp/morph_tool_secret.txt";
	const char *link_path = "/tmp/morph_tool_work/link.txt";
	file_ensure_dir(work);
	file_write_all(secret, "secret\n", 7);
	std::remove(link_path);
	ASSERT_EQ(symlink(secret, link_path), 0);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tool_out");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(file_read_init(&reg, tctx), 0);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "file_read",
			   "{\"file_path\":\"link.txt\"}", &result);
	EXPECT_EQ(rc, -EPERM);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "permission denied"), nullptr);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(link_path);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolTest, FileListDeniesParentTraversal) {
	const char *work = "/tmp/morph_tool_work";
	const char *outside = "/tmp/morph_tool_outside";
	file_ensure_dir(work);
	file_ensure_dir(outside);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tool_out");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(file_list_init(&reg, tctx), 0);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "file_list",
			   "{\"dir_path\":\"../morph_tool_outside\"}",
			   &result);
	EXPECT_EQ(rc, -EPERM);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "permission denied"), nullptr);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	rmdir(outside);
	rmdir(work);
}

TEST_F(ToolTest, FileListAllowsOutputDir) {
	const char *work = "/tmp/morph_tool_work";
	const char *out = "/tmp/morph_tool_out";
	const char *artifact = "/tmp/morph_tool_out/artifact.txt";
	file_ensure_dir(work);
	file_ensure_dir(out);
	file_write_all(artifact, "artifact", 8);
	struct tool_context *tctx = tool_context_create(work, out);
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(file_list_init(&reg, tctx), 0);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "file_list",
			   "{\"dir_path\":\"/tmp/morph_tool_out\"}",
			   &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "artifact.txt"), nullptr);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(artifact);
	rmdir(out);
	rmdir(work);
}

TEST_F(ToolTest, FileInfoUsesResolvedWorkdirPath) {
	const char *work = "/tmp/morph_tool_work";
	const char *path = "/tmp/morph_tool_work/info.txt";
	file_ensure_dir(work);
	file_write_all(path, "info", 4);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tool_out");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(file_info_init(&reg, tctx), 0);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "file_info",
			   "{\"file_path\":\"info.txt\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "\"type\":\"file\""), nullptr);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(path);
	rmdir(work);
}
