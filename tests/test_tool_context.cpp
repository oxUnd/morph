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
	int rc = tool_context_check_write_path(tctx, "/tmp/test_file.txt");
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathOutsideOutputDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_check_write_path(tctx, "/var/test_file.txt");
	EXPECT_EQ(rc, -1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_check_write_path(tctx, NULL);
	EXPECT_NE(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathNullCtx) {
	int rc = tool_context_check_write_path(NULL, "/tmp/file");
	EXPECT_NE(rc, 0);
}

static enum write_verdict test_approval_allow(const char *path,
					      const char *output_dir,
					      void *user_data)
{
	(void)path;
	(void)output_dir;
	(void)user_data;
	return WRITE_ALLOW;
}

static enum write_verdict test_approval_deny(const char *path,
					     const char *output_dir,
					     void *user_data)
{
	(void)path;
	(void)output_dir;
	(void)user_data;
	return WRITE_DENY;
}

static int always_dir_count = 0;
static enum write_verdict test_approval_always(const char *path,
					       const char *output_dir,
					       void *user_data)
{
	(void)path;
	(void)output_dir;
	(void)user_data;
	always_dir_count++;
	return WRITE_ALWAYS;
}

TEST_F(ToolContextTest, CheckWritePathApprovalAllow) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tctx->approval_fn = test_approval_allow;
	tctx->approval_user_data = NULL;
	int rc = tool_context_check_write_path(tctx, "/var/test_file.txt");
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathApprovalDeny) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tctx->approval_fn = test_approval_deny;
	tctx->approval_user_data = NULL;
	int rc = tool_context_check_write_path(tctx, "/var/test_file.txt");
	EXPECT_EQ(rc, -1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckWritePathApprovalAlwaysAddsDir) {
	always_dir_count = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tctx->approval_fn = test_approval_always;
	tctx->approval_user_data = NULL;
	int rc = tool_context_check_write_path(tctx, "/var/test_file.txt");
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(always_dir_count, 1);
	EXPECT_GE(tctx->allowed_dirs_count, 1);
	int rc2 = tool_context_check_write_path(tctx, "/var/other_file.txt");
	EXPECT_EQ(rc2, 0);
	EXPECT_EQ(always_dir_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AddAllowedDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_add_allowed_dir(tctx, "/var/log");
	EXPECT_EQ(tctx->allowed_dirs_count, 1);
	int rc = tool_context_check_write_path(tctx, "/var/log/app.log");
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AddAllowedDirDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_add_allowed_dir(tctx, "/var/log");
	tool_context_add_allowed_dir(tctx, "/var/log");
	EXPECT_EQ(tctx->allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

static enum tool_path_verdict test_path_allow(enum tool_path_op op,
					      const char *path,
					      const char *root,
					      void *user_data)
{
	(void)op;
	(void)path;
	(void)root;
	(void)user_data;
	return TOOL_PATH_ALLOW;
}

static enum tool_path_verdict test_path_deny(enum tool_path_op op,
					     const char *path,
					     const char *root,
					     void *user_data)
{
	(void)op;
	(void)path;
	(void)root;
	(void)user_data;
	return TOOL_PATH_DENY;
}

static int path_always_calls = 0;
static enum tool_path_verdict test_path_always(enum tool_path_op op,
					       const char *path,
					       const char *root,
					       void *user_data)
{
	(void)op;
	(void)path;
	(void)root;
	(void)user_data;
	path_always_calls++;
	return TOOL_PATH_ALWAYS;
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
	tool_context_set_path_approval(tctx, test_path_allow, NULL);
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
	tool_context_set_path_approval(tctx, test_path_deny, NULL);
	int rc = tool_context_check_read_path(tctx, secret);
	EXPECT_EQ(rc, -EPERM);
	tool_context_destroy(tctx);
	std::remove(secret);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadApprovalAlwaysAddsDir) {
	const char *work = "/tmp/morph_tctx_work";
	const char *dir = "/tmp/morph_tctx_external";
	const char *one = "/tmp/morph_tctx_external/one.txt";
	const char *two = "/tmp/morph_tctx_external/two.txt";
	path_always_calls = 0;
	file_ensure_dir(work);
	file_ensure_dir(dir);
	file_write_all(one, "one", 3);
	file_write_all(two, "two", 3);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_tctx_out");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_path_approval(tctx, test_path_always, NULL);
	EXPECT_EQ(tool_context_check_read_path(tctx, one), 0);
	EXPECT_EQ(path_always_calls, 1);
	EXPECT_EQ(tctx->read_allowed_dirs_count, 1);
	EXPECT_EQ(tool_context_check_read_path(tctx, two), 0);
	EXPECT_EQ(path_always_calls, 1);
	tool_context_destroy(tctx);
	std::remove(one);
	std::remove(two);
	rmdir(dir);
	rmdir(work);
}

/* ---- Command approval tests ---- */

TEST_F(ToolContextTest, AllowCommand) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_command(tctx, "echo");
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(tctx->allowed_commands_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "echo");
	tool_context_allow_command(tctx, "echo");
	EXPECT_EQ(tctx->allowed_commands_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandNullPattern) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_command(tctx, NULL);
	EXPECT_NE(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandEmptyPattern) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_command(tctx, "");
	EXPECT_NE(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowCommandNullTctx) {
	int rc = tool_context_allow_command(NULL, "echo");
	EXPECT_NE(rc, 0);
}

TEST_F(ToolContextTest, AllowExecDir) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_exec_dir(tctx, "/tmp");
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowExecDirWildcard) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_exec_dir(tctx, "*");
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowExecDirDuplicate) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_exec_dir(tctx, "/tmp");
	tool_context_allow_exec_dir(tctx, "/tmp");
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, AllowExecDirNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_allow_exec_dir(tctx, NULL);
	EXPECT_NE(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "echo");
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandDenied) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, -EPERM);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandNull) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int rc = tool_context_check_command(tctx, NULL, NULL);
	EXPECT_NE(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandNullTctx) {
	int rc = tool_context_check_command(NULL, "echo", NULL);
	EXPECT_NE(rc, 0);
}

static enum command_verdict test_cmd_allow(const char *command,
					   const char *cwd,
					   void *user_data)
{
	(void)command;
	(void)cwd;
	(void)user_data;
	return COMMAND_ALLOW;
}

static enum command_verdict test_cmd_deny(const char *command,
					  const char *cwd,
					  void *user_data)
{
	(void)command;
	(void)cwd;
	(void)user_data;
	return COMMAND_DENY;
}

static int cmd_always_calls = 0;
static enum command_verdict test_cmd_always(const char *command,
					    const char *cwd,
					    void *user_data)
{
	(void)command;
	(void)cwd;
	(void)user_data;
	cmd_always_calls++;
	return COMMAND_ALWAYS;
}

TEST_F(ToolContextTest, CheckCommandCallbackAllow) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_command_approval(tctx, test_cmd_allow, NULL);
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackDeny) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_command_approval(tctx, test_cmd_deny, NULL);
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, -EACCES);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackAlwaysPersistsProgram) {
	cmd_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_command_approval(tctx, test_cmd_always, NULL);
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cmd_always_calls, 1);
	EXPECT_GE(tctx->allowed_commands_count, 1);
	int rc2 = tool_context_check_command(tctx, "echo bye", NULL);
	EXPECT_EQ(rc2, 0);
	EXPECT_EQ(cmd_always_calls, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCallbackAlwaysPersistsCwd) {
	cmd_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "pwd");
	tool_context_set_command_approval(tctx, test_cmd_always, NULL);
	int rc = tool_context_check_command(tctx, "pwd", "/tmp");
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cmd_always_calls, 1);
	EXPECT_GE(tctx->exec_allowed_dirs_count, 1);
	int rc2 = tool_context_check_command(tctx, "pwd", "/tmp");
	EXPECT_EQ(rc2, 0);
	EXPECT_EQ(cmd_always_calls, 1);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, SetCommandApprovalNullTctx) {
	tool_context_set_command_approval(NULL, test_cmd_allow, NULL);
}

TEST_F(ToolContextTest, CheckCommandCwdAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "pwd");
	tool_context_allow_exec_dir(tctx, "/tmp");
	int rc = tool_context_check_command(tctx, "pwd", "/tmp");
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandCwdDenied) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "pwd");
	int rc = tool_context_check_command(tctx, "pwd", "/var");
	EXPECT_EQ(rc, -EPERM);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandNoCwdAllowed) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "echo");
	int rc = tool_context_check_command(tctx, "echo hi", NULL);
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandPatternWildcard) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "*");
	int rc = tool_context_check_command(tctx, "anything here", NULL);
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CheckCommandPatternPrefix) {
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_allow_command(tctx, "cmake*");
	int rc = tool_context_check_command(tctx, "cmake --build build", NULL);
	EXPECT_EQ(rc, 0);
	tool_context_destroy(tctx);
}
