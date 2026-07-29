#include <gtest/gtest.h>
#include "agent/tool_context.h"
#include "db/database.h"
#include "db/permission_grant.h"
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

class ScopedEnvVar {
public:
	explicit ScopedEnvVar(const char *name)
		: name_(name), existed_(getenv(name) != nullptr),
		  value_(getenv(name) ? getenv(name) : "")
	{
	}

	~ScopedEnvVar()
	{
		if (existed_)
			(void)setenv(name_.c_str(), value_.c_str(), 1);
		else
			(void)unsetenv(name_.c_str());
	}

	void Set(const char *value)
	{
		(void)setenv(name_.c_str(), value, 1);
	}

private:
	std::string name_;
	bool existed_;
	std::string value_;
};

static int has_cli_directory(
	const struct tool_directory_capability *directories, int count,
	const char *path)
{
	char *expected = file_resolve_path(path);
	int found = 0;

	if (!expected)
		return 0;
	for (int i = 0; i < count; i++) {
		if (strcmp(directories[i].path, expected) == 0) {
			found = 1;
			break;
		}
	}
	free(expected);
	return found;
}

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

static int check_list_path(struct tool_context *tctx, const char *path)
{
	return tool_context_authorize_path(tctx, TOOL_PATH_LIST, path,
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

TEST_F(ToolContextTest, PersistentGrantReloadRevokesImmediately)
{
	char db_path[] = "/tmp/morph_grants_XXXXXX";
	int fd = mkstemp(db_path);
	struct permission_grant grant = {};
	struct db db = {};
	int deleted = 0;

	ASSERT_GE(fd, 0);
	close(fd);
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(tool_context_set_grant_store(tctx, &db, "/tmp"), 0);
	snprintf(grant.subject, sizeof(grant.subject), "%s", "echo");
	snprintf(grant.resource_kind, sizeof(grant.resource_kind), "%s",
		 "command");
	snprintf(grant.resource, sizeof(grant.resource), "%s", "echo");
	snprintf(grant.project_root, sizeof(grant.project_root), "%s",
		 tctx->grant_project_root);
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);
	snprintf(grant.subject, sizeof(grant.subject), "%s", "printf");
	snprintf(grant.resource, sizeof(grant.resource), "%s", "printf");
	ASSERT_EQ(permission_grant_save(&db, &grant), 0);

	ASSERT_EQ(tool_context_reload_persistent_grants(tctx), 0);
	EXPECT_EQ(check_command(tctx, "echo ok", nullptr), 0);
	EXPECT_EQ(check_command(tctx, "printf ok", nullptr), 0);
	ASSERT_EQ(permission_grant_delete_subject(
		&db, tctx->grant_project_root, "echo", &deleted), 0);
	EXPECT_EQ(deleted, 1);
	ASSERT_EQ(tool_context_reload_persistent_grants(tctx), 0);
	EXPECT_EQ(check_command(tctx, "echo ok", nullptr), -EPERM);
	EXPECT_EQ(check_command(tctx, "printf ok", nullptr), 0);

	tool_context_destroy(tctx);
	db_close(&db);
	std::remove(db_path);
}

TEST_F(ToolContextTest, CreateWithTilde) {
	struct tool_context *tctx = tool_context_create("~/test", "~/.morph/output");
	ASSERT_NE(tctx, nullptr);
	EXPECT_NE(tool_context_output_dir(tctx)[0], '~');
	EXPECT_NE(tool_context_workdir(tctx)[0], '~');
	tool_context_destroy(tctx);
}

TEST_F(ToolContextTest, CommandNameSkipsEnvironmentAssignments)
{
	char name[TOOL_CONTEXT_CLI_NAME_MAX];
	char direct[PATH_MAX];
	char prefixed[PATH_MAX];

	ASSERT_EQ(tool_context_command_name(
		"LARKSUITE_CLI_NO_UPDATE_NOTIFIER=1 "
		"PROFILE='user profile' /usr/local/bin/lark-cli auth status",
		name, sizeof(name)), 0);
	EXPECT_STREQ(name, "lark-cli");
	ASSERT_EQ(tool_context_command_principal(
		"echo ok", direct, sizeof(direct)), 0);
	ASSERT_EQ(tool_context_command_principal(
		"NOTICE=1 echo ok", prefixed, sizeof(prefixed)), 0);
	EXPECT_STREQ(prefixed, direct);
}

TEST_F(ToolContextTest, ParsesComplexEnvironmentPrefix)
{
	char name[TOOL_CONTEXT_CLI_NAME_MAX];

	ASSERT_EQ(tool_context_command_name(
		"PROFILE=\"a\\\" b\" lark-cli auth status",
		name, sizeof(name)), 0);
	EXPECT_STREQ(name, "lark-cli");
}

TEST_F(ToolContextTest, SynthesizesCliDirectoriesFromCommandName)
{
	char home_template[] = "/tmp/morph_cli_home_XXXXXX";
	char *home = mkdtemp(home_template);
	char config[PATH_MAX];
	char cache[PATH_MAX];
	char state[PATH_MAX];
	char expected[PATH_MAX];
	ScopedEnvVar home_env("HOME");
	ScopedEnvVar config_env("XDG_CONFIG_HOME");
	ScopedEnvVar cache_env("XDG_CACHE_HOME");
	ScopedEnvVar state_env("XDG_STATE_HOME");
	struct tool_directory_capability
		directories[TOOL_CONTEXT_CLI_DIR_MAX] = {};

	ASSERT_NE(home, nullptr);
	ASSERT_EQ(file_path_join(
		config, sizeof(config), home, "xdg-config"), 0);
	ASSERT_EQ(file_path_join(
		cache, sizeof(cache), home, "xdg-cache"), 0);
	ASSERT_EQ(file_path_join(
		state, sizeof(state), home, "xdg-state"), 0);
	home_env.Set(home);
	config_env.Set(config);
	cache_env.Set(cache);
	state_env.Set(state);
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	int count = tool_context_discover_cli_dirs(
		tctx, "NOTICE=1 /usr/local/bin/demo-cli --version",
		directories,
		TOOL_CONTEXT_CLI_DIR_MAX);
#ifdef __APPLE__
	ASSERT_EQ(count, 6);
#else
	ASSERT_EQ(count, 4);
#endif
	ASSERT_EQ(file_path_join(
		expected, sizeof(expected), home, ".demo-cli"), 0);
	EXPECT_TRUE(has_cli_directory(directories, count, expected));
	ASSERT_EQ(file_path_join(
		expected, sizeof(expected), config, "demo-cli"), 0);
	EXPECT_TRUE(has_cli_directory(directories, count, expected));
	ASSERT_EQ(file_path_join(
		expected, sizeof(expected), cache, "demo-cli"), 0);
	EXPECT_TRUE(has_cli_directory(directories, count, expected));
	ASSERT_EQ(file_path_join(
		expected, sizeof(expected), state, "demo-cli"), 0);
	EXPECT_TRUE(has_cli_directory(directories, count, expected));
	for (int i = 0; i < count; i++) {
		EXPECT_EQ(directories[i].create, 1);
		EXPECT_NE(access(directories[i].path, F_OK), 0);
	}

	tool_context_destroy(tctx);
	rmdir(home);
}

TEST_F(ToolContextTest, CliDirectoryGrantFollowsApprovalLifetime)
{
	char dir_template[] = "/tmp/morph_cli_grant_XXXXXX";
	char *base = mkdtemp(dir_template);
	char once_dir[PATH_MAX];
	char session_dir[PATH_MAX];
	const char *paths[4] = {};
	char resolved[PATH_MAX];

	ASSERT_NE(base, nullptr);
	ASSERT_EQ(file_path_join(
		once_dir, sizeof(once_dir), base, "once"), 0);
	ASSERT_EQ(file_path_join(
		session_dir, sizeof(session_dir), base, "session"), 0);
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(tool_context_grant_write_access(
		tctx, "/usr/bin/demo", once_dir, 1, TOOL_OP_ALLOW,
		resolved, sizeof(resolved)), 0);
	EXPECT_EQ(access(once_dir, F_OK), 0);
	EXPECT_EQ(tool_context_collect_write_grants(
		tctx, "/usr/bin/demo", paths, 4), 0);
	ASSERT_EQ(tool_context_grant_write_access(
		tctx, "/usr/bin/demo", session_dir, 1, TOOL_OP_SESSION,
		resolved, sizeof(resolved)), 0);
	ASSERT_EQ(tool_context_collect_write_grants(
		tctx, "/usr/bin/demo", paths, 4), 1);
	char *expected = file_resolve_path(session_dir);
	ASSERT_NE(expected, nullptr);
	EXPECT_STREQ(paths[0], expected);
	free(expected);

	tool_context_destroy(tctx);
	rmdir(session_dir);
	rmdir(once_dir);
	rmdir(base);
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

TEST_F(ToolContextTest, CheckWritePathApprovalAlwaysPersists) {
	op_always_calls = 0;
	struct tool_context *tctx = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(tctx, nullptr);
	tool_context_set_operation_approval(tctx, op_always, NULL);
	EXPECT_EQ(check_write_path(tctx, "/var/test_file.txt"), 0);
	EXPECT_EQ(op_always_calls, 1);
	EXPECT_EQ(tctx->write_allowed_dirs_count, 0);
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

TEST_F(ToolContextTest, AuthorizeReadListWithinOutputDir) {
	const char *work = "/tmp/morph_tctx_work";
	const char *out = "/tmp/morph_tctx_out";
	const char *artifact = "/tmp/morph_tctx_out/artifact.txt";
	file_ensure_dir(work);
	file_ensure_dir(out);
	file_write_all(artifact, "artifact", 8);
	struct tool_context *tctx = tool_context_create(work, out);
	ASSERT_NE(tctx, nullptr);
	EXPECT_EQ(tctx->read_allowed_dirs_count, 1);
	tool_context_set_operation_approval(tctx, op_deny, NULL);
	EXPECT_EQ(check_read_path(tctx, artifact), 0);
	EXPECT_EQ(check_list_path(tctx, out), 0);
	tool_context_destroy(tctx);
	std::remove(artifact);
	rmdir(out);
	rmdir(work);
}

TEST_F(ToolContextTest, AuthorizeReadApprovalAlwaysPersists) {
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
	EXPECT_EQ(tctx->allowed_commands_count, 0);
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
	EXPECT_EQ(tctx->exec_allowed_dirs_count, 0);
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

TEST_F(ToolContextTest, PersistentCommandGrantLoadsInNewContext)
{
	const char *db_path = "/tmp/morph_tctx_grants.db";
	struct db db = {};
	struct tool_context *first;
	struct tool_context *second;

	std::remove(db_path);
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	first = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(tool_context_set_grant_store(first, &db, "/tmp"), 0);
	tool_context_set_operation_approval(first, op_always, NULL);
	op_always_calls = 0;
	EXPECT_EQ(check_command(first, "lark-cli auth status", NULL), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(first);

	second = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(second, nullptr);
	ASSERT_EQ(tool_context_set_grant_store(second, &db, "/tmp"), 0);
	EXPECT_EQ(check_command(second, "lark-cli auth status", NULL), 0);
	tool_context_destroy(second);
	db_close(&db);
	std::remove(db_path);
}

TEST_F(ToolContextTest, PersistentScopedWriteGrantLoadsInNewContext)
{
	const char *db_path = "/tmp/morph_tctx_write_grants.db";
	const char *state_dir = "/var/tmp/morph_lark_state";
	struct db db = {};
	struct tool_context *first;
	struct tool_context *second;
	char resolved[PATH_MAX];
	const char *paths[2] = {};

	std::remove(db_path);
	file_ensure_dir(state_dir);
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	first = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(tool_context_set_grant_store(first, &db, "/tmp"), 0);
	tool_context_set_operation_approval(first, op_always, NULL);
	op_always_calls = 0;
	EXPECT_EQ(tool_context_request_write_access(
		first, "lark-cli", "lark-cli auth status", state_dir,
		resolved, sizeof(resolved)), 0);
	EXPECT_EQ(op_always_calls, 1);
	tool_context_destroy(first);

	second = tool_context_create("/tmp", "/tmp");
	ASSERT_NE(second, nullptr);
	ASSERT_EQ(tool_context_set_grant_store(second, &db, "/tmp"), 0);
	tool_context_set_operation_approval(second, op_deny, NULL);
	EXPECT_EQ(tool_context_request_write_access(
		second, "lark-cli", "lark-cli auth status", state_dir,
		resolved, sizeof(resolved)), 0);
	EXPECT_EQ(tool_context_collect_write_grants(
		second, "lark-cli", paths, 2), 1);
	char *expected = file_resolve_path(state_dir);
	ASSERT_NE(expected, nullptr);
	EXPECT_STREQ(paths[0], expected);
	free(expected);
	tool_context_destroy(second);
	db_close(&db);
	std::remove(db_path);
	rmdir(state_dir);
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
