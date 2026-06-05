#include <gtest/gtest.h>
#include "agent/tool_context.h"
#include "util/file.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

class ToolContextTest : public ::testing::Test {
protected:
	void SetUp() override {}
	void TearDown() override {}
};

static int check_command(struct tool_context *tctx, const char *command,
			 const char *cwd)
{
	struct tool_operation op = {
		.kind = TOOL_OP_COMMAND,
		.tool_name = "bash_exec",
		.action = command,
		.target = NULL,
		.scope = cwd,
		.details_json = NULL,
	};
	return tool_context_check_operation(tctx, &op);
}

static int check_write_path(struct tool_context *tctx, const char *path)
{
	return tool_context_authorize_path(tctx, TOOL_PATH_WRITE, path,
					   NULL, 0);
}

static int check_read_path(struct tool_context *tctx, const char *path)
{
	return tool_context_authorize_path(tctx, TOOL_PATH_READ, path,
					   NULL, 0);
}

TEST_F(ToolContextTest, CreateDestroy) {
	struct tool_context *tctx = tool_context_create("/tmp/test_workdir", "/tmp/test_output");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(tool_context_output_dir(tctx)[0], '\0');
	EXPECT_NE(tool_context_output_dir(tctx)[0], '~');
	EXPECT_NE(tool_context_workdir(tctx)[0], '\0');
	EXPECT_NE(tool_context_workdir(tctx)[0], '~');
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CreateWithTilde) {
	struct tool_context *tctx = tool_context_create("~/test", "~/.morph/output");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(tool_context_output_dir(tctx)[0], '~');
	EXPECT_NE(tool_context_workdir(tctx)[0], '~');
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, OutputDirNull) {
	EXPECT_EQ(tool_context_output_dir(NULL), nullptr);
}

TEST_F(ToolContextTest, CheckWritePathWithinOutputDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(check_write_path(tctx, "/tmp/test_file.txt"), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathOutsideOutputDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(check_write_path(tctx, "/var/test_file.txt"), -EPERM);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(check_write_path(tctx, NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathNullCtx) {
	EXPECT_NE(check_write_path(NULL, "/tmp/file"), 0);
}

static enum tool_operation_verdict op_allow(
	const struct tool_operation *op, void *user_data)
{
	(void)op;
	(void)user_data;
	return TOOL_OP_ALLOW;
}

static enum tool_operation_verdict op_deny(
	const struct tool_operation *op, void *user_data)
{
	(void)op;
	(void)user_data;
	return TOOL_OP_DENY;
}

static int op_always_calls = 0;
static enum tool_operation_verdict op_always(
	const struct tool_operation *op, void *user_data)
{
	(void)op;
	(void)user_data;
	op_always_calls++;
	return TOOL_OP_ALWAYS;
}

TEST_F(ToolContextTest, CheckWritePathApprovalAllow) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_allow, NULL);
	EXPECT_EQ(check_write_path(tctx, "/var/test_file.txt"), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathApprovalDeny) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_deny, NULL);
	EXPECT_EQ(check_write_path(tctx, "/var/test_file.txt"), -EACCES);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathApprovalAlwaysAddsDir) {
	op_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_always, NULL);
	EXPECT_EQ(check_write_path(tctx, "/var/test_file.txt"), 0);
	EXPECT_EQ(op_always_calls, 1);
	EXPECT_EQ(tctx->write_allowed_dirs_count, 1);
	EXPECT_EQ(check_write_path(tctx, "/var/other_file.txt"), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AddWriteAllowedDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_add_write_allowed_dir(tctx, "/var/log");
	EXPECT_EQ(tctx->write_allowed_dirs_count, 1);
	EXPECT_EQ(check_write_path(tctx, "/var/log/app.log"), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AddWriteAllowedDirDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_add_write_allowed_dir(tctx, "/var/log");
	tool_context_add_write_allowed_dir(tctx, "/var/log");
	EXPECT_EQ(tctx->write_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AuthorizeReadRelativeWithinWorkdir) {
	const char *work = "/tmp/morph_tctx_work";
	const char *path = "/tmp/morph_tctx_work/in.txt";
	file_ensure_dir(work);
	file_write_all(path, "ok", 2);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	char resolved[PATH_MAX];
	int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
					     "in.txt", resolved,
					     sizeof(resolved));
	EXPECT_EQ(rc, 0);
	char *expected = file_resolve_path(path);
	ASSERT_NE(expected, nullptr);
	EXPECT_STREQ(resolved, expected);
	free(expected);
	tool_context_destroy(tctx);
	std::remove(path);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadParentTraversalDenied) {
	const char *work = "/tmp/morph_tctx_work";
	const char *secret = "/tmp/morph_tctx_secret.txt";
	file_ensure_dir(work);
	file_write_all(secret, "secret", 6);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	char resolved[PATH_MAX];
	int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
					     "../morph_tctx_secret.txt",
					     resolved, sizeof(resolved));
	EXPECT_EQ(rc, -EPERM);
	tool_context_destroy(tctx);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadSymlinkEscapeDenied) {
	const char *work = "/tmp/morph_tctx_work";
	const char *secret = "/tmp/morph_tctx_secret.txt";
	const char *link_path = "/tmp/morph_tctx_work/link.txt";
	file_ensure_dir(work);
	file_write_all(secret, "secret", 6);
	std::remove(link_path);
	ASSERT_EQ(symlink(secret, link_path), 0);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	char resolved[PATH_MAX];
	int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
					     "link.txt", resolved,
					     sizeof(resolved));
	EXPECT_EQ(rc, -EPERM);
	tool_context_destroy(tctx);
	std::remove(link_path);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadApprovalAllow) {
	const char *work = "/tmp/morph_tctx_work";
	const char *secret = "/tmp/morph_tctx_secret.txt";
	file_ensure_dir(work);
	file_write_all(secret, "secret", 6);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_allow, NULL);
	char resolved[PATH_MAX];
	int rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
					     secret, resolved,
					     sizeof(resolved));
	EXPECT_EQ(rc, 0);
	char *expected = file_resolve_path(secret);
	ASSERT_NE(expected, nullptr);
	EXPECT_STREQ(resolved, expected);
	free(expected);
	tool_context_destroy(tctx);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadApprovalDeny) {
	const char *work = "/tmp/morph_tctx_work";
	const char *secret = "/tmp/morph_tctx_secret.txt";
	file_ensure_dir(work);
	file_write_all(secret, "secret", 6);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_deny, NULL);
	EXPECT_EQ(check_read_path(tctx, secret), -EACCES);
	tool_context_destroy(tctx);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadApprovalAlwaysAddsDir) {
	const char *work = "/tmp/morph_tctx_work";
	const char *dir = "/tmp/morph_tctx_external";
	const char *one = "/tmp/morph_tctx_external/one.txt";
	const char *two = "/tmp/morph_tctx_external/two.txt";
	op_always_calls = 0;
	file_ensure_dir(work);
	file_ensure_dir(dir);
	file_write_all(one, "one", 3);
	file_write_all(two, "two", 3);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_always, NULL);
	EXPECT_EQ(check_read_path(tctx, one), 0);
	EXPECT_EQ(op_always_calls, 1);
	EXPECT_EQ(tctx->read_allowed_dirs_count, 1);
	EXPECT_EQ(check_read_path(tctx, two), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(tctx);
	std::remove(one);
	std::remove(two);
	rmdir(dir);
	rmdir(work);
}

TEST_F(ToolContextTest, AllowCommandPattern) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(tool_context_allow_command_pattern(tctx, "echo"), 0);
	EXPECT_EQ(tctx->allowed_commands_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandPatternDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "echo");
	tool_context_allow_command_pattern(tctx, "echo");
	EXPECT_EQ(tctx->allowed_commands_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandPatternInvalid) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(tool_context_allow_command_pattern(tctx, NULL), 0);
	EXPECT_NE(tool_context_allow_command_pattern(tctx, ""), 0);
	EXPECT_NE(tool_context_allow_command_pattern(NULL, "echo"), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandScope) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(tool_context_allow_command_scope(tctx, "/tmp"), 0);
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandScopeWildcard) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(tool_context_allow_command_scope(tctx, "*"), 0);
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandScopeDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_scope(tctx, "/tmp");
	tool_context_allow_command_scope(tctx, "/tmp");
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandScopeNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(tool_context_allow_command_scope(tctx, NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "echo");
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandDenied) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), -EPERM);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(check_command(tctx, NULL, NULL), 0);
	EXPECT_NE(check_command(NULL, "echo", NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackAllow) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_allow, NULL);
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackDeny) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_deny, NULL);
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), -EACCES);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackAlwaysPersistsProgram) {
	op_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_always, NULL);
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), 0);
	EXPECT_EQ(op_always_calls, 1);
	EXPECT_GE(tctx->allowed_commands_count, 1);
	EXPECT_EQ(check_command(tctx, "echo bye", NULL), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackAlwaysPersistsCwd) {
	op_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "pwd");
	tool_context_set_operation_approval(tctx, op_always, NULL);
	EXPECT_EQ(check_command(tctx, "pwd", "/tmp"), 0);
	EXPECT_EQ(op_always_calls, 1);
	EXPECT_GE(tctx->exec_allowed_dirs_count, 1);
	EXPECT_EQ(check_command(tctx, "pwd", "/tmp"), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, SetOperationApprovalNullTctx) {
	tool_context_set_operation_approval(NULL, op_allow, NULL);
}

TEST_F(ToolContextTest, CheckCommandCwdAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "pwd");
	tool_context_allow_command_scope(tctx, "/tmp");
	EXPECT_EQ(check_command(tctx, "pwd", "/tmp"), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCwdDenied) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "pwd");
	EXPECT_EQ(check_command(tctx, "pwd", "/var"), -EPERM);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandNoCwdAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "echo");
	EXPECT_EQ(check_command(tctx, "echo hi", NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandPatternWildcard) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "*");
	EXPECT_EQ(check_command(tctx, "anything here", NULL), 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandPatternPrefix) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command_pattern(tctx, "cmake*");
	EXPECT_EQ(check_command(tctx, "cmake --build build", NULL), 0);
	tool_context_destroy(tctx);
}
