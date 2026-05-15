#include <gtest/gtest.h>
#include "agent/tools/bash_exec.h"
#include "agent/tool.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

class BashExecTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	void SetUp() override {
		tool_registry_init(&reg);
	}
	void TearDown() override {
		tool_registry_cleanup(&reg);
	}
};

static std::string exec_tool(struct tool_registry &reg, const char *args_json,
			     int &rc)
{
	char *result = NULL;
	rc = tool_exec(&reg, "bash_exec", args_json, &result);
	std::string s(result ? result : "");
	free(result);
	return s;
}

static std::string exec_command(struct tool_registry &reg, const char *cmd,
				int &rc)
{
	std::string args = std::string("{\"command\":\"") + cmd + "\"}";
	return exec_tool(reg, args.c_str(), rc);
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
	int rc = bash_exec_init(&reg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "bash_exec");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "bash_exec");
}

TEST_F(BashExecTest, InitNullRegistry)
{
	int rc = bash_exec_init(NULL);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, MissingCommand)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_tool(reg, "{}", rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos ||
		    result.find("error") != std::string::npos);
}

TEST_F(BashExecTest, NullArgs)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_tool(reg, NULL, rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos);
}

TEST_F(BashExecTest, EmptyCommand)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_tool(reg, "{\"command\":\"\"}", rc);
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_TRUE(result.find("missing") != std::string::npos);
}

TEST_F(BashExecTest, MalformedJson)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_tool(reg, "not json", rc);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, NullResultPtr)
{
	bash_exec_init(&reg);
	int rc = tool_exec(&reg, "bash_exec", "{\"command\":\"echo hi\"}", NULL);
	EXPECT_EQ(rc, -EINVAL);
}

TEST_F(BashExecTest, BlockedRm)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_TRUE(result.find("blocked") != std::string::npos);
}

TEST_F(BashExecTest, BlockedRmdir)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "rmdir /tmp/x", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMkfs)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "mkfs.ext4 /dev/sda1", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDd)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "dd if=/dev/zero of=/dev/sda", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedShutdown)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "shutdown -h now", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedReboot)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "reboot", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMv)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "mv a b", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCp)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "cp a b", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChmod)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "chmod 777 /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChown)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "chown root /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCurl)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "curl http://example.com", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWget)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "wget http://example.com", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSsh)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "ssh user@host", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedScp)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "scp file user@host:/tmp", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedKill)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "kill -9 1", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedKillall)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "killall process", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPkill)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "pkill -f process", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedApt)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "apt install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedAptGet)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "apt-get install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedYum)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "yum install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedBrew)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "brew install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPip)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "pip install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedNpm)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "npm install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCargo)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "cargo install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGem)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "gem install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedMount)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "mount /dev/sda1 /mnt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSystemctl)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "systemctl start foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedLaunchctl)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "launchctl load foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPasswd)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "passwd root", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedCrontab)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "crontab -e", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUseradd)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "useradd testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedIptables)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "iptables -L", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSysctl)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "sysctl -a", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithLeadingSpaces)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "   rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithLeadingTabs)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"\\t\\trm -rf /\"}";
	exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedWithPathPrefix)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "/usr/bin/rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInPipe)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "echo hi; rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInAndChain)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "echo hi && rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInOrChain)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "echo hi || rm -rf /", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInNewline)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"echo hi\\nrm -rf /\"}";
	exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInSubshell)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "(rm -rf /)", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, AllowedLs)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "ls /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedEcho)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(result.find("hello") != std::string::npos);
}

TEST_F(BashExecTest, AllowedCat)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "cat /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedPwd)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "pwd", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedWhich)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "which ls", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedTrue)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "true", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, AllowedFalse)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "false", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, AllowedGit)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "git --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedFind)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "find /dev/null -maxdepth 0", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedHead)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo -e 'a\\nb\\nc' | head -1", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedGrep)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello | grep hello", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedSort)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo -e 'c\\na\\nb' | sort", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedWc)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello | wc -l", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedEnv)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "env | head -1", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedDate)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "date", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, EchoStdout)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo test_output", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("test_output") != std::string::npos);
}

TEST_F(BashExecTest, StderrCapture)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo err_msg >&2", rc);
	EXPECT_EQ(rc, 0);
	std::string stderr_val = get_json_field(result, "stderr");
	EXPECT_TRUE(stderr_val.find("err_msg") != std::string::npos);
}

TEST_F(BashExecTest, ExitCode)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "exit 42", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 42);
}

TEST_F(BashExecTest, ExitCodeZero)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "exit 0", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 0);
}

TEST_F(BashExecTest, ResultJsonFormat)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hi", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_FALSE(get_json_field(result, "command").empty());
	EXPECT_TRUE(get_json_field(result, "exit_code") != "");
	EXPECT_TRUE(get_json_field(result, "stdout") != "");
	EXPECT_TRUE(get_json_field(result, "stderr") != "");
	EXPECT_TRUE(get_json_field(result, "timed_out") != "");
}

TEST_F(BashExecTest, TimedOutField)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hi", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_FALSE(get_json_bool(result, "timed_out"));
}

TEST_F(BashExecTest, TruncatedField)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hi", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_FALSE(get_json_bool(result, "stdout_truncated"));
	EXPECT_FALSE(get_json_bool(result, "stderr_truncated"));
}

TEST_F(BashExecTest, CwdOption)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}";
	std::string result = exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("/tmp") != std::string::npos);
}

TEST_F(BashExecTest, CwdFieldInResult)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/tmp\"}";
	std::string result = exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "cwd"), "/tmp");
}

TEST_F(BashExecTest, CwdInvalid)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"pwd\",\"cwd\":\"/nonexistent_dir_xyz\"}";
	std::string result = exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_int(result, "exit_code"), 126);
}

TEST_F(BashExecTest, TimeoutOption)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"echo fast\",\"timeout_seconds\":5}";
	std::string result = exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("fast") != std::string::npos);
}

TEST_F(BashExecTest, TimeoutTriggers)
{
	bash_exec_init(&reg);
	int rc;
	std::string args = "{\"command\":\"sleep 10\",\"timeout_seconds\":1}";
	std::string result = exec_tool(reg, args.c_str(), rc);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(get_json_bool(result, "timed_out"));
}

TEST_F(BashExecTest, MultilineOutput)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo -e 'line1\\nline2\\nline3'", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("line1") != std::string::npos);
	EXPECT_TRUE(stdout_val.find("line2") != std::string::npos);
	EXPECT_TRUE(stdout_val.find("line3") != std::string::npos);
}

TEST_F(BashExecTest, LargeOutput)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "seq 1 1000", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_GT(stdout_val.size(), 100u);
}

TEST_F(BashExecTest, SpecialCharsInCommand)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo 'hello world'", rc);
	EXPECT_EQ(rc, 0);
	std::string stdout_val = get_json_field(result, "stdout");
	EXPECT_TRUE(stdout_val.find("hello world") != std::string::npos);
}

TEST_F(BashExecTest, UnicodeOutput)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo '\xe4\xbd\xa0\xe5\xa5\xbd'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, PipeAllowed)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello | wc -c", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, RedirectAllowed)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hi > /tmp/morph_test_bash_exec_redirect && cat /tmp/morph_test_bash_exec_redirect", rc);
	EXPECT_EQ(rc, 0);
	remove("/tmp/morph_test_bash_exec_redirect");
}

TEST_F(BashExecTest, CommandFieldInResult)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo test", rc);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(get_json_field(result, "command"), "echo test");
}

TEST_F(BashExecTest, ToolNotFound)
{
	bash_exec_init(&reg);
	char *result = NULL;
	int rc = tool_exec(&reg, "nonexistent_tool", "{}", &result);
	EXPECT_NE(rc, 0);
	free(result);
}

TEST_F(BashExecTest, BlockedDnf)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "dnf install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPip3)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "pip3 install foo", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedSftp)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "sftp user@host", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedRsync)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "rsync -av src/ dst/", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUmount)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "umount /mnt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedFdisk)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "fdisk /dev/sda", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDiskutil)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "diskutil list", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedPoweroff)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "poweroff", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedHalt)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "halt", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedInit)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "init 0", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChgrp)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "chgrp wheel /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedChattr)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "chattr +i /tmp/f", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUserdel)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "userdel testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedUsermod)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "usermod -aG wheel testuser", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGroupadd)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "groupadd testgroup", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedGroupdel)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "groupdel testgroup", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedService)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "service nginx start", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedParted)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "parted /dev/sda print", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedIp6tables)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "ip6tables -L", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, BlockedDefaults)
{
	bash_exec_init(&reg);
	int rc;
	exec_command(reg, "defaults read com.apple.dock", rc);
	EXPECT_EQ(rc, -EPERM);
}

TEST_F(BashExecTest, AllowedMake)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "make --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedCmake)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "cmake --version", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedDiff)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "diff /dev/null /dev/null", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedXargs)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hi | xargs echo", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedAwk)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello | awk '{print $1}'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedSed)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo hello | sed 's/hello/world/'", rc);
	EXPECT_EQ(rc, 0);
}

TEST_F(BashExecTest, AllowedTr)
{
	bash_exec_init(&reg);
	int rc;
	std::string result = exec_command(reg, "echo HELLO | tr A-Z a-z", rc);
	EXPECT_EQ(rc, 0);
}
