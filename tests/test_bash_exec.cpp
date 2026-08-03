#include <gtest/gtest.h>
#include "agent/tools/bash_exec.h"
#include "agent/tools/request_permissions.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/file.h"
#include "cJSON.h"
#include <filesystem>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

class BashExecTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	struct tool_context *tctx;
	std::string test_home;
	std::string old_home;
	std::string old_xdg_config;
	std::string old_xdg_cache;
	std::string old_xdg_state;
	bool had_home;
	bool had_xdg_config;
	bool had_xdg_cache;
	bool had_xdg_state;

	static std::string env_value(const char *name, bool *existed)
	{
		const char *value = getenv(name);

		*existed = value != nullptr;
		return value ? value : "";
	}

	static void restore_env(const char *name, bool existed,
				const std::string &value)
	{
		if (existed)
			(void)setenv(name, value.c_str(), 1);
		else
			(void)unsetenv(name);
	}

	void SetUp() override {
		char home_template[] = "/tmp/morph_bash_home_XXXXXX";
		char *home;

		old_home = env_value("HOME", &had_home);
		old_xdg_config =
			env_value("XDG_CONFIG_HOME", &had_xdg_config);
		old_xdg_cache =
			env_value("XDG_CACHE_HOME", &had_xdg_cache);
		old_xdg_state =
			env_value("XDG_STATE_HOME", &had_xdg_state);
		home = mkdtemp(home_template);
		ASSERT_NE(home, nullptr);
		test_home = home;
		ASSERT_EQ(setenv("HOME", test_home.c_str(), 1), 0);
		ASSERT_EQ(unsetenv("XDG_CONFIG_HOME"), 0);
		ASSERT_EQ(unsetenv("XDG_CACHE_HOME"), 0);
		ASSERT_EQ(unsetenv("XDG_STATE_HOME"), 0);
		tool_registry_init(&reg);
		tctx = tool_context_create("/tmp", "/tmp");
	}
	void TearDown() override {
		tool_context_destroy(tctx);
		tool_registry_cleanup(&reg);
		restore_env("HOME", had_home, old_home);
		restore_env("XDG_CONFIG_HOME", had_xdg_config,
			    old_xdg_config);
		restore_env("XDG_CACHE_HOME", had_xdg_cache,
			    old_xdg_cache);
		restore_env("XDG_STATE_HOME", had_xdg_state,
			    old_xdg_state);
		std::error_code error;
		(void)std::filesystem::remove_all(test_home, error);
	}
};

static std::string exec_raw(struct tool_registry &reg, const char *args_json,
			    int &rc)
{
	struct tool_result result;
	tool_result_init(&result);
	rc = tool_exec(&reg, "bash_exec", args_json, &result);
	std::string s;
	if (rc == 0) {
		s = result.text.data ? result.text.data : "";
	} else if (result.data) {
		cJSON *text = cJSON_GetObjectItem(result.data, "text");
		cJSON *stderr_item = cJSON_GetObjectItem(result.data, "stderr");
		cJSON *stdout_item = cJSON_GetObjectItem(result.data, "stdout");
		if (cJSON_IsString(text) && text->valuestring)
			s = text->valuestring;
		else if (cJSON_IsString(stderr_item) && stderr_item->valuestring)
			s = stderr_item->valuestring;
		else if (cJSON_IsString(stdout_item) && stdout_item->valuestring)
			s = stdout_item->valuestring;
	}
	if (s.empty() && result.envelope) {
		cJSON *error = cJSON_GetObjectItem(result.envelope, "error");
		cJSON *message = cJSON_GetObjectItem(error, "message");
		cJSON *details = cJSON_GetObjectItem(error, "details");
		char *details_json = details ? cJSON_PrintUnformatted(details) :
			nullptr;
		if (cJSON_IsString(message) && message->valuestring)
			s = message->valuestring;
		if (details_json) {
			s += " ";
			s += details_json;
			free(details_json);
		}
	}
	if (s.empty())
		s = result.text.data ? result.text.data : "";
	tool_result_cleanup(&result);
	return s;
}

static std::string exec_tool(struct tool_registry &reg,
			     struct tool_context *tctx, const char *args_json,
			     int &rc)
{
	cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
	if (root) {
		cJSON *command = cJSON_GetObjectItem(root, "command");
		cJSON *cwd = cJSON_GetObjectItem(root, "cwd");
		if (cJSON_IsString(command) && command->valuestring)
			tool_context_allow_command_pattern(tctx, command->valuestring);
		if (cJSON_IsString(cwd) && cwd->valuestring)
			tool_context_allow_command_scope(tctx, cwd->valuestring);
		cJSON_Delete(root);
	}
	return exec_raw(reg, args_json, rc);
}

static std::string exec_command(struct tool_registry &reg,
				struct tool_context *tctx, const char *cmd,
				int &rc)
{
	std::string args = std::string("{\"command\":\"") + cmd + "\"}";
	return exec_tool(reg, tctx, args.c_str(), rc);
}

static std::string get_json_field(const std::string &json, const char *field)
{
	cJSON *root = cJSON_Parse(json.c_str());
	if (!root)
		return "";
	cJSON *scope = root;
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (cJSON_IsObject(data))
		scope = data;
	else {
		cJSON *error = cJSON_GetObjectItem(root, "error");
		cJSON *details = cJSON_GetObjectItem(error, "details");
		if (cJSON_IsObject(details))
			scope = details;
	}
	cJSON *item = cJSON_GetObjectItem(scope, field);
	std::string val;
	if (cJSON_IsString(item) && item->valuestring)
		val = item->valuestring;
	else if (cJSON_IsNumber(item))
		val = std::to_string(item->valueint);
	else if (cJSON_IsBool(item))
		val = cJSON_IsTrue(item) ? "true" : "false";
	cJSON_Delete(root);
	return val;
}

static int get_json_int(const std::string &json, const char *field)
{
	cJSON *root = cJSON_Parse(json.c_str());
	if (!root)
		return -999;
	cJSON *scope = root;
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (cJSON_IsObject(data))
		scope = data;
	else {
		cJSON *error = cJSON_GetObjectItem(root, "error");
		cJSON *details = cJSON_GetObjectItem(error, "details");
		if (cJSON_IsObject(details))
			scope = details;
	}
	cJSON *item = cJSON_GetObjectItem(scope, field);
	int val = cJSON_IsNumber(item) ? item->valueint : -999;
	cJSON_Delete(root);
	return val;
}

static bool has_json_field(const std::string &json, const char *field)
{
	cJSON *root = cJSON_Parse(json.c_str());
	if (!root)
		return false;
	cJSON *scope = root;
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (cJSON_IsObject(data))
		scope = data;
	else {
		cJSON *error = cJSON_GetObjectItem(root, "error");
		cJSON *details = cJSON_GetObjectItem(error, "details");
		if (cJSON_IsObject(details))
			scope = details;
	}
	bool found = cJSON_GetObjectItem(scope, field) != NULL;
	cJSON_Delete(root);
	return found;
}

static bool get_json_bool(const std::string &json, const char *field)
{
	cJSON *root = cJSON_Parse(json.c_str());
	if (!root)
		return false;
	cJSON *scope = root;
	cJSON *data = cJSON_GetObjectItem(root, "data");
	if (cJSON_IsObject(data))
		scope = data;
	else {
		cJSON *error = cJSON_GetObjectItem(root, "error");
		cJSON *details = cJSON_GetObjectItem(error, "details");
		if (cJSON_IsObject(details))
			scope = details;
	}
	cJSON *item = cJSON_GetObjectItem(scope, field);
	bool val = cJSON_IsTrue(item);
	cJSON_Delete(root);
	return val;
}

TEST_F(BashExecTest, InitRegister)
{
	int rc = bash_exec_init(&reg, tctx);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "bash_exec");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "bash_exec");
}

TEST_F(BashExecTest, InitNullRegistry)
{
	int rc = bash_exec_init(NULL, tctx);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, MissingCommand)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(reg, tctx, "{}", rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos ||
		    result.find("error") != std::string::npos);
}

TEST_F(BashExecTest, NullArgs)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(reg, tctx, NULL, rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos);
}

TEST_F(BashExecTest, EmptyCommand)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(reg, tctx, "{\"command\":\"\"}", rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos);
}

TEST_F(BashExecTest, MalformedJson)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(reg, tctx, "not json", rc);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, NullResultPtr)
{
	bash_exec_init(&reg, tctx);
	int rc = tool_exec(&reg, "bash_exec", "{\"command\":\"echo hi\"}", NULL);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, PolicyDeniesWithoutRules)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"echo hi\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("not allowed") != std::string::npos ||
		    result.find("interactive approval") != std::string::npos);
}

TEST_F(BashExecTest, PolicyDeniesAdditionalArguments)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "echo hi"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"echo hi extra\"}", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, PolicyDeniesUnconfiguredCwd)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "pwd"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedRm)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("blocked") != std::string::npos);
}

TEST_F(BashExecTest, BlockedRmdir)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "rmdir /tmp/x", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMkfs)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "mkfs.ext4 /dev/sda1", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDd)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "dd if=/dev/zero of=/dev/sda", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedShutdown)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "shutdown -h now", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedReboot)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "reboot", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMv)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "mv a b", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCp)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "cp a b", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChmod)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "chmod 777 /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChown)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "chown root /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSsh)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "ssh user@host", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedScp)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "scp file user@host:/tmp", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedKill)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "kill -9 1", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedKillall)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "killall process", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPkill)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "pkill -f process", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMount)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "mount /dev/sda1 /mnt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSystemctl)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "systemctl start foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedLaunchctl)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "launchctl load foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPasswd)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "passwd root", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCrontab)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "crontab -e", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUseradd)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "useradd testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedIptables)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "iptables -L", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSysctl)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "sysctl -a", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithLeadingSpaces)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "   rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithLeadingTabs)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"\\t\\trm -rf /\"}";
	exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithPathPrefix)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "/usr/bin/rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedAfterEnvironmentAssignment)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"NOTICE=1 rm -f /tmp/x\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_NE(result.find("blocked"), std::string::npos);
}

TEST_F(BashExecTest, QuotedAmpersandInUrlIsNotBlocked)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(
		reg, tctx,
		"{\"command\":\"printf '%s' "
		"\\\"https://example.test/verify?"
		"flow_id=x&user_code=y\\\"\"}",
		rc);

	EXPECT_EQ(rc, 0);
	EXPECT_NE(result.find("flow_id=x&user_code=y"), std::string::npos);
	EXPECT_EQ(result.find("blocked"), std::string::npos);
}

TEST_F(BashExecTest, BlockedInPipe)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "echo hi; rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInAndChain)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "echo hi && rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInOrChain)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "echo hi || rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInNewline)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"echo hi\\nrm -rf /\"}";
	exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInSubshell)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "(rm -rf /)", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, AllowedLs)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "ls /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedEcho)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(result.find("hello") != std::string::npos);
}

TEST_F(BashExecTest, AllowedCat)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "cat /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedPwd)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "pwd", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedWhich)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "which ls", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedTrue)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "true", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, AllowedFalse)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "false", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(get_json_int(result, "exit_code"), 0);
	EXPECT_NE(result.find("\"ok\":false"), std::string::npos);
}

TEST_F(BashExecTest, AllowedGit)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "git --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedFind)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "find /dev/null -maxdepth 0", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedHead)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo -e 'a\\nb\\nc' | head -1", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedGrep)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello | grep hello", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedSort)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo -e 'c\\na\\nb' | sort", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedWc)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello | wc -l", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedEnv)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "env | head -1", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, SensitiveEnvIsNotInherited)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(setenv("MORPH_BASH_SECRET", "do-not-expose", 1), 0);
	int rc;
	std::string result = exec_tool(
		reg, tctx,
		"{\"command\":\"printf '%s' \\\"$MORPH_BASH_SECRET\\\"\"}",
		rc);
	unsetenv("MORPH_BASH_SECRET");
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(get_json_field(result, "stdout").empty());
}

TEST_F(BashExecTest, AllowedDate)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "date", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, EchoStdout)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo test_output", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("test_output") != std::string::npos);
}

TEST_F(BashExecTest, StderrCapture)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo err_msg >&2", rc);
	EXPECT_EQ(rc, 0);
	std::string stderr_val = get_json_field(result, "stderr");
	if (get_json_int(result, "exit_code") == 126)
		EXPECT_TRUE(stderr_val.find("sandbox initialization failed") !=
			    std::string::npos);
	else
		EXPECT_TRUE(stderr_val.find("err_msg") != std::string::npos);
}

TEST_F(BashExecTest, ExitCode)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "exit 42", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 42);
}

TEST_F(BashExecTest, ExitCodeZero)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "exit 0", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, ResultJsonFormat)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo out && echo err >&2", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(has_json_field(result, "command"));
	EXPECT_TRUE(has_json_field(result, "exit_code"));
	EXPECT_TRUE(has_json_field(result, "stdout"));
	EXPECT_TRUE(has_json_field(result, "stderr"));
	EXPECT_TRUE(has_json_field(result, "timed_out"));
}

TEST_F(BashExecTest, TimedOutField)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hi", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_FALSE(get_json_bool(result, "timed_out"));
}

TEST_F(BashExecTest, TruncatedField)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hi", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_FALSE(get_json_bool(result, "stdout_truncated"));
	EXPECT_FALSE(get_json_bool(result, "stderr_truncated"));
}

TEST_F(BashExecTest, CwdOption)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}";
	std::string result = exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("/tmp") != std::string::npos);
}

TEST_F(BashExecTest, CwdFieldInResult)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}";
	std::string result = exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "cwd"), "/tmp");
}

TEST_F(BashExecTest, CwdInvalid)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/nonexistent_dir_xyz\"}";
	std::string result = exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("not allowed") != std::string::npos ||
		    result.find("interactive approval") != std::string::npos);
}

TEST_F(BashExecTest, TimeoutOption)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"echo fast\",\"timeout_seconds\":5}";
	std::string result = exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("fast") != std::string::npos);
}

TEST_F(BashExecTest, TimeoutTriggers)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string args = "{\"command\":\"sleep 10\",\"timeout_seconds\":1}";
	std::string result = exec_tool(reg, tctx, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(get_json_bool(result, "timed_out"));
}

TEST_F(BashExecTest, MultilineOutput)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo -e 'line1\\nline2\\nline3'", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("line1") != std::string::npos);
	EXPECT_TRUE(stdout_val.find("line2") != std::string::npos);
	EXPECT_TRUE(stdout_val.find("line3") != std::string::npos);
}

TEST_F(BashExecTest, LargeOutput)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "seq 1 1000", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_GT(stdout_val.size(), 100u);
}

TEST_F(BashExecTest, SpecialCharsInCommand)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo 'hello world'", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("hello world") != std::string::npos);
}

TEST_F(BashExecTest, UnicodeOutput)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo '\xe4\xbd\xa0\xe5\xa5\xbd'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, PipeAllowed)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello | wc -c", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, RedirectOutsideCwdDenied)
{
#ifndef __linux__
	GTEST_SKIP() << "sandbox not implemented on this platform";
#endif
	bash_exec_init(&reg, tctx);
	remove("/var/tmp/morph_test_bash_exec_redirect");
	int rc;
	std::string result = exec_tool(
		reg, tctx,
		"{\"command\":\"echo hi > /var/tmp/morph_test_bash_exec_redirect\","
		"\"cwd\":\"/tmp\"}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(get_json_int(result, "exit_code"), 0);
	EXPECT_NE(access("/var/tmp/morph_test_bash_exec_redirect", F_OK), 0);
	remove("/var/tmp/morph_test_bash_exec_redirect");
}

TEST_F(BashExecTest, RedirectWithinCwdAllowed)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_tool(
		reg, tctx,
		"{\"command\":\"echo hi > morph_test_bash_exec_redirect && "
		"cat morph_test_bash_exec_redirect\",\"cwd\":\"/tmp\"}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
	EXPECT_TRUE(get_json_field(result, "stdout").find("hi") !=
		    std::string::npos);
	remove("/tmp/morph_test_bash_exec_redirect");
}

TEST_F(BashExecTest, CommandFieldInResult)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo test", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "command"), "echo test");
}

TEST_F(BashExecTest, ToolNotFound)
{
	bash_exec_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "nonexistent_tool", "{}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(BashExecTest, BlockedSftp)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "sftp user@host", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedRsync)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "rsync -av src/ dst/", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUmount)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "umount /mnt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedFdisk)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "fdisk /dev/sda", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDiskutil)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "diskutil list", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPoweroff)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "poweroff", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedHalt)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "halt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInit)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "init 0", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChgrp)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "chgrp wheel /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChattr)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "chattr +i /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUserdel)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "userdel testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUsermod)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "usermod -aG wheel testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGroupadd)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "groupadd testgroup", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGroupdel)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "groupdel testgroup", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedService)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "service nginx start", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedParted)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "parted /dev/sda print", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedIp6tables)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "ip6tables -L", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDefaults)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "defaults read com.apple.dock", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, AllowedMake)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "make --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedCmake)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "cmake --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedDiff)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "diff /dev/null /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedXargs)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hi | xargs echo", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedAwk)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello | awk '{print $1}'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedSed)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo hello | sed 's/hello/world/'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedTr)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_command(reg, tctx, "echo HELLO | tr A-Z a-z", rc);
	EXPECT_EQ(rc, 0);
}

/* ---- Pattern-based command allowlist ---- */

TEST_F(BashExecTest, ProgramNamePatternAllowsArgs)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "echo"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"echo hi there friends\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "command"), "echo hi there friends");
}

TEST_F(BashExecTest, ProgramNamePatternRequiresTokenBoundary)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "ech"), 0);
	int rc;
	exec_raw(reg, "{\"command\":\"echo hi\"}", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, PrefixWildcardAllowsSubcommands)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "git status *"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"git status --short\"}", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, PrefixWildcardRejectsOtherSubcommand)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "git status *"), 0);
	int rc;
	exec_raw(reg, "{\"command\":\"git log --oneline\"}", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, WildcardStarAllowsArbitraryCommand)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "*"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"echo wild\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "command"), "echo wild");
}

TEST_F(BashExecTest, WildcardStarStillRespectsBlocklist)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "*"), 0);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"rm -rf /\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("blocked") != std::string::npos);
}

TEST_F(BashExecTest, ExactPatternStillRequiresExactMatch)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "echo hi"), 0);
	int rc;
	exec_raw(reg, "{\"command\":\"echo hi extra\"}", rc);
	EXPECT_EQ(rc, -EPERM);
}

/* ---- Subtree cwd allowlist ---- */

TEST_F(BashExecTest, CwdSubtreeAllowed)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "pwd"), 0);
	ASSERT_EQ(tool_context_allow_command_scope(tctx, "/tmp"), 0);
	mkdir("/tmp/morph_bash_subtree", 0755);
	int rc;
	std::string result = exec_raw(
		reg,
		"{\"command\":\"pwd\",\"cwd\":\"/tmp/morph_bash_subtree\"}",
		rc);
	EXPECT_EQ(rc, 0);
	rmdir("/tmp/morph_bash_subtree");
}

TEST_F(BashExecTest, CwdSubtreeRejectsSiblingPrefix)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "pwd"), 0);
	mkdir("/tmp/morph_bash_root", 0755);
	mkdir("/tmp/morph_bash_root_sibling", 0755);
	ASSERT_EQ(tool_context_allow_command_scope(tctx, "/tmp/morph_bash_root"), 0);
	int rc;
	exec_raw(reg,
		 "{\"command\":\"pwd\",\"cwd\":\"/tmp/morph_bash_root_sibling\"}",
		 rc);
	EXPECT_EQ(rc, -EPERM);
	rmdir("/tmp/morph_bash_root");
	rmdir("/tmp/morph_bash_root_sibling");
}

TEST_F(BashExecTest, CwdWildcardAllowsAny)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "pwd"), 0);
	ASSERT_EQ(tool_context_allow_command_scope(tctx, "*"), 0);
	int rc;
	std::string result = exec_raw(
		reg, "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}", rc);
	EXPECT_EQ(rc, 0);
}

/* ---- Approval callback ---- */

struct ApprovalState {
	int calls;
	enum tool_operation_verdict next;
	std::string last_command;
	std::string last_cwd;
};

static enum tool_operation_verdict approval_stub(
	const struct tool_operation *op, void *user_data)
{
	ApprovalState *s = static_cast<ApprovalState *>(user_data);
	s->calls++;
	EXPECT_EQ(op->kind, TOOL_OP_COMMAND);
	s->last_command = op->action ? op->action : "";
	s->last_cwd = op->scope ? op->scope : "";
	return s->next;
}

struct ScopedApprovalState {
	int command_calls;
	int write_calls;
};

static enum tool_operation_verdict scoped_approval_stub(
	const struct tool_operation *op, void *user_data)
{
	ScopedApprovalState *state =
		static_cast<ScopedApprovalState *>(user_data);

	if (op->kind == TOOL_OP_COMMAND)
		state->command_calls++;
	if (op->kind == TOOL_OP_PATH_WRITE) {
		state->write_calls++;
		EXPECT_NE(op->principal, nullptr);
		if (op->principal)
			EXPECT_NE(std::string(op->principal).find("touch"),
				  std::string::npos);
	}
	return TOOL_OP_SESSION;
}

struct CliDirectoryApprovalState {
	int calls;
	int expected_count;
	std::string expected_path;
};

class BashScopedEnvVar {
public:
	explicit BashScopedEnvVar(const char *name)
		: name_(name), existed_(getenv(name) != nullptr),
		  value_(getenv(name) ? getenv(name) : "")
	{
	}

	~BashScopedEnvVar()
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

static enum tool_operation_verdict cli_directory_approval_stub(
	const struct tool_operation *op, void *user_data)
{
	CliDirectoryApprovalState *state =
		static_cast<CliDirectoryApprovalState *>(user_data);

	state->calls++;
	EXPECT_EQ(op->kind, TOOL_OP_COMMAND);
	EXPECT_EQ(op->directories_count, state->expected_count);
	for (int i = 0; i < op->directories_count; i++) {
		if (state->expected_path == op->directories[i].path)
			state->expected_path.clear();
		EXPECT_EQ(op->directories[i].create, 1);
	}
	EXPECT_TRUE(state->expected_path.empty());
	return TOOL_OP_ALLOW;
}

TEST_F(BashExecTest, CommandApprovalGrantsSynthesizedCliDirectoriesOnce)
{
	char home_template[] = "/tmp/morph_bash_cli_home_XXXXXX";
	char *home = mkdtemp(home_template);
	char config[PATH_MAX];
	char cache[PATH_MAX];
	char state_dir[PATH_MAX];
	char dot_dir[PATH_MAX];
	char path[PATH_MAX];
	char command[PATH_MAX + 32];
	char args[PATH_MAX + 64];
	BashScopedEnvVar home_env("HOME");
	BashScopedEnvVar config_env("XDG_CONFIG_HOME");
	BashScopedEnvVar cache_env("XDG_CACHE_HOME");
	BashScopedEnvVar state_env("XDG_STATE_HOME");
	const char *grants[4] = {};
	int rc;

	ASSERT_NE(home, nullptr);
	ASSERT_EQ(file_path_join(
		config, sizeof(config), home, "xdg-config"), 0);
	ASSERT_EQ(file_path_join(
		cache, sizeof(cache), home, "xdg-cache"), 0);
	ASSERT_EQ(file_path_join(
		state_dir, sizeof(state_dir), home, "xdg-state"), 0);
	ASSERT_EQ(file_path_join(
		dot_dir, sizeof(dot_dir), home, ".touch"), 0);
	ASSERT_EQ(file_path_join(
		path, sizeof(path), dot_dir, "state.txt"), 0);
	home_env.Set(home);
	config_env.Set(config);
	cache_env.Set(cache);
	state_env.Set(state_dir);
	char *expected = file_resolve_path(dot_dir);
	ASSERT_NE(expected, nullptr);
#ifdef __APPLE__
	CliDirectoryApprovalState state{0, 6, expected};
#else
	CliDirectoryApprovalState state{0, 4, expected};
#endif
	free(expected);
	bash_exec_init(&reg, tctx);
	tool_context_set_operation_approval(
		tctx, cli_directory_approval_stub, &state);
	snprintf(command, sizeof(command), "NOTICE=1 touch %s", path);
	snprintf(args, sizeof(args), "{\"command\":\"%s\"}", command);
	std::string result = exec_raw(reg, args, rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
	EXPECT_EQ(access(path, F_OK), 0);
	EXPECT_EQ(tool_context_collect_write_grants(
		tctx, "/usr/bin/touch", grants, 4), 0);
	std::remove(path);
	rmdir(dot_dir);
	{
		char dir[PATH_MAX];

		ASSERT_EQ(file_path_join(
			dir, sizeof(dir), config, "touch"), 0);
		rmdir(dir);
		rmdir(config);
		ASSERT_EQ(file_path_join(
			dir, sizeof(dir), cache, "touch"), 0);
		rmdir(dir);
		rmdir(cache);
		ASSERT_EQ(file_path_join(
			dir, sizeof(dir), state_dir, "touch"), 0);
		rmdir(dir);
		rmdir(state_dir);
#ifdef __APPLE__
		char library[PATH_MAX];
		char base[PATH_MAX];

		ASSERT_EQ(file_path_join(
			library, sizeof(library), home, "Library"), 0);
		ASSERT_EQ(file_path_join(
			base, sizeof(base), library,
			"Application Support"), 0);
		ASSERT_EQ(file_path_join(
			dir, sizeof(dir), base, "touch"), 0);
		rmdir(dir);
		rmdir(base);
		ASSERT_EQ(file_path_join(
			base, sizeof(base), library, "Caches"), 0);
		ASSERT_EQ(file_path_join(
			dir, sizeof(dir), base, "touch"), 0);
		rmdir(dir);
		rmdir(base);
		rmdir(library);
#endif
	}
	rmdir(home);
}

TEST_F(BashExecTest, ApprovalCallbackAllowsOnce)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALLOW, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"echo hi\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(state.last_command, "echo hi");
	EXPECT_EQ(get_json_field(result, "command"), "echo hi");
}

TEST_F(BashExecTest, ApprovalCallbackDenies)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_DENY, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"echo hi\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(state.calls, 1);
	EXPECT_TRUE(result.find("denied") != std::string::npos);
}

TEST_F(BashExecTest, PackageManagerRequiresApproval)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_raw(
		reg,
		"{\"command\":\"npm --version\",\"timeout_seconds\":5}",
		rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(result.find("blocked"), std::string::npos);
	EXPECT_TRUE(result.find("not allowed") != std::string::npos ||
		    result.find("interactive approval") != std::string::npos);
}

TEST_F(BashExecTest, PackageManagerRunsAfterApproval)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALLOW, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	std::string result = exec_raw(
		reg,
		"{\"command\":\"npm --version\",\"timeout_seconds\":5}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(state.last_command, "npm --version");
	EXPECT_EQ(get_json_field(result, "command"), "npm --version");
	EXPECT_NE(get_json_int(result, "exit_code"), -999);
}

TEST_F(BashExecTest, NetworkToolRequiresApproval)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_raw(
		reg,
		"{\"command\":\"curl --version\",\"timeout_seconds\":5}",
		rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(result.find("blocked"), std::string::npos);
	EXPECT_TRUE(result.find("not allowed") != std::string::npos ||
		    result.find("interactive approval") != std::string::npos);
}

TEST_F(BashExecTest, NetworkToolRunsAfterApproval)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALLOW, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	std::string result = exec_raw(
		reg,
		"{\"command\":\"curl --version\",\"timeout_seconds\":5}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	EXPECT_EQ(state.last_command, "curl --version");
	EXPECT_EQ(get_json_field(result, "command"), "curl --version");
	EXPECT_NE(get_json_int(result, "exit_code"), -999);
}

TEST_F(BashExecTest, ApprovalCallbackAlwaysPersistsProgram)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALWAYS, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	exec_raw(reg, "{\"command\":\"NOTICE=1 echo first\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	std::string r2 = exec_raw(
		reg, "{\"command\":\"PROFILE=test echo second arg\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1); /* callback NOT called again */
	EXPECT_EQ(get_json_field(r2, "command"),
		  "PROFILE=test echo second arg");
}

TEST_F(BashExecTest, RequestedWritePathIsGrantedToSandbox)
{
	const char *dir = "/var/tmp/morph_bash_grant";
	const char *path = "/var/tmp/morph_bash_grant/state.txt";
	ScopedApprovalState state{0, 0};
	int rc;

	file_ensure_dir(dir);
	std::remove(path);
	bash_exec_init(&reg, tctx);
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	std::string result = exec_raw(
		reg,
		"{\"command\":\"touch "
		"/var/tmp/morph_bash_grant/state.txt\","
		"\"write_paths\":[\"/var/tmp/morph_bash_grant\"]}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.command_calls, 1);
	EXPECT_EQ(state.write_calls, 1);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
	EXPECT_EQ(access(path, F_OK), 0);
	std::remove(path);
	rmdir(dir);
}

TEST_F(BashExecTest, SessionWriteGrantAvoidsRepeatedApproval)
{
	const char *dir = "/var/tmp/morph_bash_session_grant";
	ScopedApprovalState state{0, 0};
	int rc;

	file_ensure_dir(dir);
	bash_exec_init(&reg, tctx);
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	exec_raw(
		reg,
		"{\"command\":\"touch "
		"/var/tmp/morph_bash_session_grant/one.txt\","
		"\"write_paths\":[\"/var/tmp/morph_bash_session_grant\"]}",
		rc);
	ASSERT_EQ(rc, 0);
	exec_raw(
		reg,
		"{\"command\":\"touch "
		"/var/tmp/morph_bash_session_grant/two.txt\","
		"\"write_paths\":[\"/var/tmp/morph_bash_session_grant\"]}",
		rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.command_calls, 1);
	EXPECT_EQ(state.write_calls, 1);
	std::remove("/var/tmp/morph_bash_session_grant/one.txt");
	std::remove("/var/tmp/morph_bash_session_grant/two.txt");
	rmdir(dir);
}

TEST_F(BashExecTest, WritePathsRejectCompoundShellCommands)
{
	ScopedApprovalState state{0, 0};
	int rc;

	std::remove("/var/tmp/morph_compound_one");
	std::remove("/var/tmp/morph_compound_two");
	bash_exec_init(&reg, tctx);
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	std::string result = exec_raw(
		reg,
		"{\"command\":\"touch /var/tmp/morph_compound_one; "
		"touch /var/tmp/morph_compound_two\","
		"\"write_paths\":[\"/var/tmp\"]}",
		rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_NE(result.find("single simple command"), std::string::npos);
	EXPECT_EQ(state.command_calls, 0);
	EXPECT_EQ(state.write_calls, 0);
	EXPECT_NE(access("/var/tmp/morph_compound_one", F_OK), 0);
	EXPECT_NE(access("/var/tmp/morph_compound_two", F_OK), 0);
}

TEST_F(BashExecTest, ApprovalCallbackAlwaysPersistsCwd)
{
	bash_exec_init(&reg, tctx);
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "pwd"), 0);
	mkdir("/tmp/morph_bash_persist", 0755);
	mkdir("/tmp/morph_bash_persist/sub", 0755);
	ApprovalState state{0, TOOL_OP_ALWAYS, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	exec_raw(reg,
		 "{\"command\":\"pwd\",\"cwd\":\"/tmp/morph_bash_persist\"}",
		 rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	exec_raw(reg,
		 "{\"command\":\"pwd\",\"cwd\":\"/tmp/morph_bash_persist/sub\"}",
		 rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	rmdir("/tmp/morph_bash_persist/sub");
	rmdir("/tmp/morph_bash_persist");
}

TEST_F(BashExecTest, BlocklistOverridesApprovalCallback)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALWAYS, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"rm -rf /\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(state.calls, 0);
	EXPECT_TRUE(result.find("blocked") != std::string::npos);
}

TEST_F(BashExecTest, NoCallbackKeepsStrictDeny)
{
	bash_exec_init(&reg, tctx);
	int rc;
	std::string result = exec_raw(reg, "{\"command\":\"echo hi\"}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("not allowed") != std::string::npos ||
		    result.find("interactive approval") != std::string::npos);
}

TEST_F(BashExecTest, LocalModeReadsOutsideWorkdir)
{
	int rc;

	tool_context_set_bash_exec_mode(tctx, "local");
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(
		reg, "{\"command\":\"test -r /etc/hosts\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, LocalModeDeletesTmpWithoutApproval)
{
	char path[] = "/tmp/morph_local_delete_XXXXXX";
	char command[PATH_MAX];
	char args[PATH_MAX + 128];
	int fd = mkstemp(path);
	int rc;

	ASSERT_GE(fd, 0);
	close(fd);
	tool_context_set_bash_exec_mode(tctx, "local");
	bash_exec_init(&reg, tctx);
	snprintf(command, sizeof(command), "rm -f %s", path);
	snprintf(args, sizeof(args),
		"{\"command\":\"%s\",\"delete_paths\":[\"/tmp\"]}",
		command);
	std::string result = exec_raw(reg, args, rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
	EXPECT_NE(access(path, F_OK), 0);
	std::remove(path);
}

TEST_F(BashExecTest, LocalModeRequiresApprovalOutsideDefaultRoots)
{
	int rc;

	tool_context_set_bash_exec_mode(tctx, "local");
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"true\",\"write_paths\":[\"/\"]}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_NE(result.find("outside"), std::string::npos);
}

TEST_F(BashExecTest, LocalModeSupportsNestedAdditionalPermissions)
{
	const char *dir = "/var/tmp/morph_nested_permission";
	const char *path = "/var/tmp/morph_nested_permission/state.txt";
	ScopedApprovalState state{0, 0};
	int rc;

	ASSERT_EQ(file_ensure_dir(dir), 0);
	std::remove(path);
	tool_context_set_bash_exec_mode(tctx, "local");
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"touch /var/tmp/morph_nested_permission/state.txt\","
		"\"sandbox_permissions\":\"with_additional_permissions\","
		"\"additional_permissions\":{\"file_system\":{"
		"\"write\":[\"/var/tmp/morph_nested_permission\"]}}}", rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(state.write_calls, 1);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_EQ(access(path, F_OK), 0);
	std::remove(path);
	rmdir(dir);
}

TEST_F(BashExecTest, SandboxDenialOverridesTrailingSuccessfulCommand)
{
	const char *dir = "/var/tmp/morph_denial_detection";
	const char *path = "/var/tmp/morph_denial_detection/state.txt";
	int rc;

	ASSERT_EQ(file_ensure_dir(dir), 0);
	std::remove(path);
	tool_context_set_bash_exec_mode(tctx, "local");
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"touch /var/tmp/morph_denial_detection/state.txt; "
		"echo finished\"}", rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_NE(result.find("sandbox_denied"), std::string::npos) << result;
	EXPECT_NE(access(path, F_OK), 0);
	rmdir(dir);
}

TEST_F(BashExecTest, RequestPermissionsPregrantsCommandSession)
{
	const char *dir = "/var/tmp/morph_requested_permission";
	const char *path = "/var/tmp/morph_requested_permission/state.txt";
	ScopedApprovalState state{0, 0};
	struct tool_result permission_result;
	int rc;

	ASSERT_EQ(file_ensure_dir(dir), 0);
	std::remove(path);
	tool_context_set_bash_exec_mode(tctx, "local");
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	ASSERT_EQ(bash_exec_init(&reg, tctx), 0);
	ASSERT_EQ(request_permissions_init(&reg, tctx), 0);
	tool_result_init(&permission_result);
	rc = tool_exec(&reg, "request_permissions",
		"{\"command\":\"touch /var/tmp/morph_requested_permission/state.txt\","
		"\"scope\":\"session\","
		"\"permissions\":{\"file_system\":{"
		"\"write\":[\"/var/tmp/morph_requested_permission\"]}},"
		"\"justification\":\"test output\"}", &permission_result);
	EXPECT_EQ(rc, 0);
	tool_result_cleanup(&permission_result);
	EXPECT_EQ(state.write_calls, 1);
	std::string result = exec_raw(reg,
		"{\"command\":\"touch /var/tmp/morph_requested_permission/state.txt\"}",
		rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_EQ(state.write_calls, 1);
	EXPECT_EQ(access(path, F_OK), 0);
	std::remove(path);
	rmdir(dir);
}

TEST_F(BashExecTest, TurnPermissionIsClearedBeforeNextTurn)
{
	const char *dir = "/var/tmp/morph_turn_permission";
	const char *paths[4] = { nullptr };
	ScopedApprovalState state{0, 0};
	struct tool_result permission_result;
	char principal[TOOL_CONTEXT_CLI_NAME_MAX];
	int rc;

	ASSERT_EQ(file_ensure_dir(dir), 0);
	tool_context_set_bash_exec_mode(tctx, "local");
	tool_context_set_operation_approval(tctx, scoped_approval_stub, &state);
	ASSERT_EQ(request_permissions_init(&reg, tctx), 0);
	tool_result_init(&permission_result);
	rc = tool_exec(&reg, "request_permissions",
		"{\"command\":\"touch /var/tmp/morph_turn_permission/state.txt\","
		"\"scope\":\"turn\",\"permissions\":{\"file_system\":{"
		"\"write\":[\"/var/tmp/morph_turn_permission\"]}}}",
		&permission_result);
	ASSERT_EQ(rc, 0);
	tool_result_cleanup(&permission_result);
	ASSERT_EQ(tool_context_command_principal(
		"touch /var/tmp/morph_turn_permission/state.txt", principal,
		sizeof(principal)), 0);
	EXPECT_EQ(tool_context_collect_write_grants(tctx, principal, paths, 4), 1);
	tool_context_clear_turn_grants(tctx);
	EXPECT_EQ(tool_context_collect_write_grants(tctx, principal, paths, 4), 0);
	rmdir(dir);
}

TEST_F(BashExecTest, LocalPermissionProfileSeparatesWriteAndDelete)
{
	const char *dir = "/var/tmp/morph_profile_permission";
	int rc;

	ASSERT_EQ(file_ensure_dir(dir), 0);
	tool_context_set_bash_exec_mode(tctx, "local");
	ASSERT_EQ(tool_context_add_bash_exec_profile_path(
		tctx, TOOL_PATH_WRITE, dir), 0);
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"touch /var/tmp/morph_profile_permission/state.txt\"}",
		rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	result = exec_raw(reg,
		"{\"command\":\"rm -f /var/tmp/morph_profile_permission/state.txt\","
		"\"additional_permissions\":{\"file_system\":{"
		"\"delete\":[\"/var/tmp/morph_profile_permission\"]}}}", rc);
	EXPECT_EQ(rc, -EPERM) << result;
	EXPECT_EQ(access("/var/tmp/morph_profile_permission/state.txt", F_OK), 0);
	std::remove("/var/tmp/morph_profile_permission/state.txt");
	rmdir(dir);
}

TEST_F(BashExecTest, LocalModeKeepsProxyEnvButFiltersSecrets)
{
	const char *old_proxy = getenv("https_proxy");
	const char *old_secret = getenv("MORPH_TEST_SECRET");
	std::string saved_proxy = old_proxy ? old_proxy : "";
	std::string saved_secret = old_secret ? old_secret : "";
	int rc;

	setenv("https_proxy", "http://127.0.0.1:18765", 1);
	setenv("MORPH_TEST_SECRET", "must-not-leak", 1);
	tool_context_set_bash_exec_mode(tctx, "local");
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"printf '%s|%s' $https_proxy "
		"$MORPH_TEST_SECRET\"}", rc);
	if (old_proxy)
		setenv("https_proxy", saved_proxy.c_str(), 1);
	else
		unsetenv("https_proxy");
	if (old_secret)
		setenv("MORPH_TEST_SECRET", saved_secret.c_str(), 1);
	else
		unsetenv("MORPH_TEST_SECRET");
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_EQ(get_json_field(result, "stdout"),
		"http://127.0.0.1:18765|");
}

TEST_F(BashExecTest, ServerModeExecutesAllowlistedCommand)
{
	int rc;

	tool_context_set_bash_exec_mode(tctx, "server");
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "echo *"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_path(
		tctx, TOOL_PATH_READ, "@tmp"), 0);
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(
		reg, "{\"command\":\"echo server-ok\"}", rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_NE(get_json_field(result, "stdout").find("server-ok"),
		std::string::npos);
}

TEST_F(BashExecTest, ServerModeKeepsOnlyConfiguredEnvironment)
{
	const char *old_proxy = getenv("https_proxy");
	std::string saved_proxy = old_proxy ? old_proxy : "";
	int rc;

	setenv("https_proxy", "http://127.0.0.1:18766", 1);
	tool_context_set_bash_exec_mode(tctx, "server");
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "printf *"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_path(
		tctx, TOOL_PATH_READ, "@tmp"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_env(
		tctx, "https_proxy"), 0);
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"printf '%s' $https_proxy\"}", rc);
	if (old_proxy)
		setenv("https_proxy", saved_proxy.c_str(), 1);
	else
		unsetenv("https_proxy");
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_EQ(get_json_field(result, "stdout"),
		"http://127.0.0.1:18766");
}

TEST_F(BashExecTest, ServerModeDeniesUnconfiguredDeletePath)
{
	int rc;

	tool_context_set_bash_exec_mode(tctx, "server");
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "rm"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_path(
		tctx, TOOL_PATH_READ, "@tmp"), 0);
	bash_exec_init(&reg, tctx);
	std::string result = exec_raw(reg,
		"{\"command\":\"rm -f /tmp/no-such-file\","
		"\"delete_paths\":[\"/tmp\"]}", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_NE(result.find("configured sandbox policy"), std::string::npos);
}

TEST_F(BashExecTest, ServerModeUsesConfiguredDeleteCapability)
{
	char path[] = "/tmp/morph_server_delete_XXXXXX";
	char command[PATH_MAX];
	char args[PATH_MAX + 128];
	int fd = mkstemp(path);
	int rc;

	ASSERT_GE(fd, 0);
	close(fd);
	tool_context_set_bash_exec_mode(tctx, "server");
	ASSERT_EQ(tool_context_allow_command_pattern(tctx, "rm"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_path(
		tctx, TOOL_PATH_READ, "@tmp"), 0);
	ASSERT_EQ(tool_context_add_bash_exec_server_path(
		tctx, TOOL_PATH_DELETE, "@tmp"), 0);
	bash_exec_init(&reg, tctx);
	snprintf(command, sizeof(command), "rm -f %s", path);
	snprintf(args, sizeof(args),
		"{\"command\":\"%s\",\"delete_paths\":[\"/tmp\"]}",
		command);
	std::string result = exec_raw(reg, args, rc);
	EXPECT_EQ(rc, 0) << result;
	EXPECT_EQ(get_json_int(result, "exit_code"), 0) << result;
	EXPECT_NE(access(path, F_OK), 0);
	std::remove(path);
}
