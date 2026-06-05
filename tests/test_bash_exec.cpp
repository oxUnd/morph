#include <gtest/gtest.h>
#include "agent/tools/bash_exec.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

class BashExecTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	struct tool_context *tctx;
	void SetUp() override {
		tool_registry_init(&reg);
		tctx = tool_context_create("/tmp", "/tmp");
	}
	void TearDown() override {
		tool_context_destroy(tctx);
		tool_registry_cleanup(&reg);
	}
};

static std::string exec_raw(struct tool_registry &reg, const char *args_json,
			    int &rc)
{
	char *result = NULL;
	rc = tool_exec(&reg, "bash_exec", args_json, &result);
	std::string s(result ? result : "");
	free(result);
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
	cJSON *item = cJSON_GetObjectItem(root, field);
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
	cJSON *item = cJSON_GetObjectItem(root, field);
	int val = cJSON_IsNumber(item) ? item->valueint : -999;
	cJSON_Delete(root);
	return val;
}

static bool get_json_bool(const std::string &json, const char *field)
{
	cJSON *root = cJSON_Parse(json.c_str());
	if (!root)
		return false;
	cJSON *item = cJSON_GetObjectItem(root, field);
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

TEST_F(BashExecTest, BlockedCurl)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "curl http://example.com", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWget)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "wget http://example.com", rc);
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

TEST_F(BashExecTest, BlockedApt)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "apt install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedAptGet)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "apt-get install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedYum)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "yum install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedBrew)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "brew install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPip)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "pip install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedNpm)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "npm install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCargo)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "cargo install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGem)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "gem install foo", rc);
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
	EXPECT_FALSE(get_json_field(result, "command").empty());
	EXPECT_TRUE(get_json_field(result, "exit_code") != "");
	EXPECT_TRUE(get_json_field(result, "stdout") != "");
	EXPECT_TRUE(get_json_field(result, "stderr") != "");
	EXPECT_TRUE(get_json_field(result, "timed_out") != "");
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
	char *result = NULL;
	int rc = tool_exec(&reg, "nonexistent_tool", "{}", &result);
	EXPECT_NE(rc, 0);
	free(result);
}

TEST_F(BashExecTest, BlockedDnf)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "dnf install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPip3)
{
	bash_exec_init(&reg, tctx);
	int rc;
	exec_command(reg, tctx, "pip3 install foo", rc);
	EXPECT_EQ(rc, -EPERM);
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

TEST_F(BashExecTest, ApprovalCallbackAlwaysPersistsProgram)
{
	bash_exec_init(&reg, tctx);
	ApprovalState state{0, TOOL_OP_ALWAYS, "", ""};
	tool_context_set_operation_approval(tctx, approval_stub, &state);
	int rc;
	exec_raw(reg, "{\"command\":\"echo first\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1);
	std::string r2 = exec_raw(reg, "{\"command\":\"echo second arg\"}", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(state.calls, 1); /* callback NOT called again */
	EXPECT_EQ(get_json_field(r2, "command"), "echo second arg");
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
