#include <gtest/gtest.h>
#include "runtime/hooks.h"
#include "agent/react.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "util/arena.h"
#include "util/file.h"
#include "util/sha256.h"
#include "util/error.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace
{
std::string field(const cJSON *row, const char *key)
{
	const auto *item = cJSON_GetObjectItemCaseSensitive(row, key);
	return cJSON_IsString(item) ? item->valuestring : "";
}

class HooksTest : public ::testing::Test
{
      protected:
	std::string directory, old_home;
	bool had_home = false;
	std::unique_ptr<struct config> cfg;
	struct db database{};
	struct hook_manager hooks{};

	void SetUp() override
	{
		char path[] = "/tmp/morph-hooks-XXXXXX";
		ASSERT_NE(mkdtemp(path), nullptr);
		directory = path;
		const char *home = getenv("HOME");
		had_home = home != nullptr;
		old_home = home ? home : "";
		setenv("HOME", directory.c_str(), 1);
		std::filesystem::create_directory(directory + "/.morph");
		cfg = std::make_unique<struct config>();
		config_set_defaults(cfg.get());
		ASSERT_EQ(db_open(&database, ":memory:"), 0);
	}
	void TearDown() override
	{
		hook_manager_cleanup(&hooks);
		db_close(&database);
		if (had_home)
			setenv("HOME", old_home.c_str(), 1);
		else
			unsetenv("HOME");
		std::filesystem::remove_all(directory);
	}
	void Init(const char *config_path = "")
	{
		ASSERT_EQ(
		    hook_manager_init(&hooks, &database, cfg.get(), config_path, directory.c_str()),
		    0);
	}
	void Write(const std::string &text, const char *path = ".morph/hooks.json")
	{
		std::ofstream out(directory + "/" + path);
		out << text;
		ASSERT_TRUE(out.good());
	}
	void Define(const char *event, const std::string &command, const char *matcher = "",
		    int timeout = 3)
	{
		cJSON *root = cJSON_CreateObject(), *map = cJSON_AddObjectToObject(root, "hooks");
		cJSON *groups = cJSON_AddArrayToObject(map, event), *group = cJSON_CreateObject();
		cJSON_AddItemToArray(groups, group);
		cJSON_AddStringToObject(group, "matcher", matcher);
		cJSON *handlers = cJSON_AddArrayToObject(group, "hooks"),
		      *handler = cJSON_CreateObject();
		cJSON_AddItemToArray(handlers, handler);
		cJSON_AddStringToObject(handler, "type", "command");
		cJSON_AddStringToObject(handler, "command", command.c_str());
		cJSON_AddNumberToObject(handler, "timeout", timeout);
		char *text = cJSON_PrintUnformatted(root);
		Write(text);
		free(text);
		cJSON_Delete(root);
	}
	void TrustAll()
	{
		cJSON *rows = hook_manager_list(&hooks);
		ASSERT_NE(rows, nullptr);
		std::vector<std::pair<std::string, std::string>> identities;
		const cJSON *row;
		cJSON_ArrayForEach(row, rows)
		    identities.emplace_back(field(row, "id"), field(row, "digest"));
		cJSON_Delete(rows);
		for (auto &identity : identities)
			ASSERT_EQ(hook_manager_action(&hooks, "trust", identity.first.c_str(),
						      identity.second.c_str()),
				  0);
	}
	cJSON *Dispatch(const char *event, const char *json = "{}")
	{
		cJSON *input = cJSON_Parse(json), *output = nullptr;
		EXPECT_EQ(hook_manager_dispatch(&hooks, event, input, &output), 0);
		cJSON_Delete(input);
		return output;
	}
};

TEST_F(HooksTest, UnreviewedNeverExecutes)
{
	Define("Stop", "touch should-not-exist");
	Init();
	cJSON_Delete(Dispatch("Stop"));
	EXPECT_FALSE(std::filesystem::exists(directory + "/should-not-exist"));
	cJSON *rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "needs_review");
	cJSON_Delete(rows);
}

TEST_F(HooksTest, TrustedReceivesJsonAndStdinEof)
{
	Define(
	    "PreToolUse",
	    "python3 -c 'import json,sys; x=json.load(sys.stdin); "
	    "assert x[\"tool_name\"]==\"exec\"; assert x[\"hook_event_name\"]==\"PreToolUse\"; "
	    "print(json.dumps({\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
	    "\"permissionDecision\":\"deny\",\"permissionDecisionReason\":\"policy denied\"}}))'",
	    "^Bash$");
	Init();
	TrustAll();
	cJSON *out = Dispatch("PreToolUse", "{\"tool_name\":\"exec\",\"tool_input\":{}}");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	EXPECT_NE(field(out, "reason").find("policy denied"), std::string::npos);
	cJSON_Delete(out);
}

TEST_F(HooksTest, ExitTwoBlocksButCrashDoesNot)
{
	Define("Stop", "echo 'verify tests' >&2; exit 2");
	Init();
	TrustAll();
	cJSON *out = Dispatch("Stop");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	EXPECT_NE(field(out, "reason").find("verify tests"), std::string::npos);
	cJSON_Delete(out);
	Define("Stop", "exit 1");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	out = Dispatch("Stop");
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	cJSON_Delete(out);
}

TEST_F(HooksTest, ChangedDefinitionRequiresNewReviewAndRejectsStaleTrust)
{
	Define("Stop", "printf '{}'");
	Init();
	TrustAll();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	auto digest = field(cJSON_GetArrayItem(rows, 0), "digest");
	cJSON_Delete(rows);
	Define("Stop", "touch changed-script");
	EXPECT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), -ESTALE);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "changed / needs_review");
	EXPECT_FALSE(field(cJSON_GetArrayItem(rows, 0), "previous_definition").empty());
	cJSON_Delete(rows);
	cJSON_Delete(Dispatch("Stop"));
	EXPECT_FALSE(std::filesystem::exists(directory + "/changed-script"));
}

TEST_F(HooksTest, DisableEnableAndRevokeAreIndependentOfTrust)
{
	Define("Stop", "printf '{}'");
	Init();
	TrustAll();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	auto digest = field(cJSON_GetArrayItem(rows, 0), "digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "disable", id.c_str(), digest.c_str()), 0);
	EXPECT_FALSE(hook_manager_has(&hooks, "Stop"));
	ASSERT_EQ(hook_manager_action(&hooks, "enable", id.c_str(), digest.c_str()), 0);
	EXPECT_TRUE(hook_manager_has(&hooks, "Stop"));
	ASSERT_EQ(hook_manager_action(&hooks, "revoke", id.c_str(), digest.c_str()), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "enable", id.c_str(), digest.c_str()), 0);
	EXPECT_FALSE(hook_manager_has(&hooks, "Stop"));
}

TEST_F(HooksTest, UnsupportedAndInvalidEntriesCannotBeTrusted)
{
	Write(R"({"hooks":{"PermissionRequest":[{"hooks":[{"type":"command","command":"true"}]}],
	"PreToolUse":[{"matcher":"[","hooks":[{"type":"command","command":"true"}]}]}})");
	Init();
	cJSON *rows = hook_manager_list(&hooks);
	for (int i = 0; i < cJSON_GetArraySize(rows); i++) {
		const cJSON *row = cJSON_GetArrayItem(rows, i);
		EXPECT_EQ(hook_manager_action(&hooks, "trust", field(row, "id").c_str(),
					      field(row, "digest").c_str()),
			  -EPERM);
	}
	cJSON_Delete(rows);
}

TEST_F(HooksTest, MatcherAliasDoesNotMatchOtherTools)
{
	Define("PreToolUse", "echo denied >&2; exit 2", "^Bash$");
	Init();
	TrustAll();
	cJSON *out = Dispatch("PreToolUse", "{\"tool_name\":\"file_read\"}");
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	cJSON_Delete(out);
}

TEST_F(HooksTest, CanonicalDigestIgnoresFormattingAndFieldOrder)
{
	Define("Stop", "printf '{}'");
	Init();
	TrustAll();
	Write(R"({"hooks":{"Stop":[{"matcher":"","hooks":[{
	"timeout":3,"command":"printf '{}'","type":"command"}]}]}})");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	EXPECT_TRUE(hook_manager_has(&hooks, "Stop"));
}

TEST_F(HooksTest, InlineTomlIsPortableAndStaleReviewIsRejected)
{
	std::string path = directory + "/config.toml";
	Write("[[hooks.Stop]]\n[[hooks.Stop.hooks]]\ntype='command'\ncommand=\"printf "
	      "'{}'\"\ntimeout=3\n",
	      "config.toml");
	ASSERT_EQ(config_load(cfg.get(), path.c_str()), 0);
	EXPECT_NE(strstr(cfg->hooks_json, "Stop"), nullptr);
	Init(path.c_str());
	TrustAll();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	auto digest = field(cJSON_GetArrayItem(rows, 0), "digest");
	cJSON_Delete(rows);
	Write("[[hooks.Stop]]\n[[hooks.Stop.hooks]]\ntype='command'\ncommand='true'\n",
	      "config.toml");
	EXPECT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), -ESTALE);
}

TEST_F(HooksTest, ConcurrentHandlersAllStartEvenWhenOneBlocks)
{
	Write(R"({"hooks":{"Stop":[{"hooks":[
	{"type":"command","command":"touch first; while [ ! -f second ]; do sleep .01; done; echo '{\"decision\":\"block\",\"reason\":\"first\"}'","timeout":2},
	{"type":"command","command":"touch second; while [ ! -f first ]; do sleep .01; done; echo '{\"decision\":\"block\",\"reason\":\"second\"}'","timeout":2}
	]}]}})");
	Init();
	TrustAll();
	cJSON *out = Dispatch("Stop");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	EXPECT_EQ(field(out, "reason"), "first\nsecond\n");
	cJSON_Delete(out);
}

TEST_F(HooksTest, TimeoutAndOutputOverflowAreReportedWithoutHanging)
{
	Define("Stop", "sleep 10", "", 1);
	Init();
	TrustAll();
	auto start = std::chrono::steady_clock::now();
	cJSON_Delete(Dispatch("Stop"));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
	Define("Stop", "python3 -c 'print(\"x\"*100000)'", "", 1);
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	cJSON_Delete(Dispatch("Stop"));
}

TEST_F(HooksTest, CancellationStopsChildren)
{
	Define("Stop", "sleep 10", "", 3);
	Init();
	TrustAll();
	struct morph_cancel_token cancel{};
	hooks.cancel_token = &cancel;
	std::thread thread([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		morph_cancel_token_cancel(&cancel);
	});
	cJSON *out = nullptr;
	EXPECT_EQ(hook_manager_dispatch(&hooks, "Stop", nullptr, &out), -ECANCELED);
	thread.join();
	cJSON_Delete(out);
}

TEST_F(HooksTest, SecretsAreRemovedFromEnvironment)
{
	setenv("MORPH_HOOK_TEST_API_KEY", "never-pass-this", 1);
	Define("Stop", "[ -z \"$MORPH_HOOK_TEST_API_KEY\" ] && "
		       "[ \"$MORPH_HOOK_ACTIVE\" = 1 ] && echo "
		       "'{\"decision\":\"block\",\"reason\":\"clean\"}'");
	Init();
	TrustAll();
	cJSON *out = Dispatch("Stop");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	cJSON_Delete(out);
	unsetenv("MORPH_HOOK_TEST_API_KEY");
}

TEST_F(HooksTest, RewriteUsesCodexOutputShape)
{
	Define(
	    "PreToolUse",
	    "echo '{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
	    "\"permissionDecision\":\"allow\",\"updatedInput\":{\"command\":\"echo checked\"}}}'");
	Init();
	TrustAll();
	cJSON *out = Dispatch("PreToolUse");
	EXPECT_EQ(field(cJSON_GetObjectItem(out, "updatedInput"), "command"), "echo checked");
	cJSON_Delete(out);
}

TEST_F(HooksTest, PlainStopOutputIsAnErrorNotModelContext)
{
	Define("Stop", "echo 'unstructured text'");
	Init();
	TrustAll();
	cJSON *out = Dispatch("Stop");
	EXPECT_EQ(field(out, "context"), "");
	cJSON_Delete(out);
}

TEST(Sha256, KnownDigest)
{
	struct sha256_ctx sha;
	unsigned char digest[32];
	const unsigned char expected[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
					  0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
					  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
					  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	sha256_init(&sha);
	sha256_update(&sha, reinterpret_cast<const unsigned char *>("abc"), 3);
	sha256_final(&sha, digest);
	EXPECT_EQ(memcmp(digest, expected, sizeof(digest)), 0);
}
} /* namespace */

TEST_F(HooksTest, DisableUnreviewedDoesNotGrantTrust)
{
	Define("Stop", "printf '{}'");
	Init();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	auto digest = field(cJSON_GetArrayItem(rows, 0), "digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "disable", id.c_str(), digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "disabled");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "enable", id.c_str(), digest.c_str()), 0);
	EXPECT_FALSE(hook_manager_has(&hooks, "Stop"));
}

TEST_F(HooksTest, ProjectTrustIsSeparateFromHookTrust)
{
	std::string workspace = directory + "/workspace";
	std::filesystem::create_directories(workspace + "/.morph");
	std::ofstream(workspace + "/.morph/hooks.json")
	    << R"({"hooks":{"Stop":[{"hooks":[{"type":"command","command":"printf '{}'"}]}]}})";
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "", workspace.c_str()), 0);
	cJSON *rows = hook_manager_list(&hooks);
	auto *row = cJSON_GetArrayItem(rows, 0);
	auto id = field(row, "id"), digest = field(row, "digest");
	auto workspace_digest = field(row, "workspace_digest");
	EXPECT_EQ(field(row, "state"), "project_untrusted");
	cJSON_Delete(rows);
	EXPECT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), -EPERM);
	ASSERT_EQ(
	    hook_manager_action(&hooks, "trust-project", "workspace", workspace_digest.c_str()), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), 0);
	EXPECT_TRUE(hook_manager_has(&hooks, "Stop"));
	ASSERT_EQ(
	    hook_manager_action(&hooks, "revoke-project", "workspace", workspace_digest.c_str()),
	    0);
	EXPECT_FALSE(hook_manager_has(&hooks, "Stop"));
}

TEST_F(HooksTest, LargeContextUsesTokenThresholdAndSpillsFullText)
{
	Write(R"({"hooks":{"SessionStart":[{"hooks":[{"type":"command",
	"command":"printf 'many words of context for the next model request'","additionalContextLimit":1}]}]}})");
	Init();
	TrustAll();
	cJSON *out = Dispatch("SessionStart");
	EXPECT_NE(field(out, "context").find("Full hook context:"), std::string::npos);
	int files = 0;
	for (auto &entry : std::filesystem::directory_iterator(directory + "/.morph/output")) {
		std::ifstream input(entry.path());
		std::string text((std::istreambuf_iterator<char>(input)), {});
		EXPECT_EQ(text, "many words of context for the next model request");
		files++;
	}
	EXPECT_EQ(files, 1);
	cJSON_Delete(out);
}

TEST_F(HooksTest, AskAndStopTerminationHaveDistinctDecisions)
{
	Define("PreToolUse", "echo "
			     "'{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
			     "\"permissionDecision\":\"ask\"}}'");
	Init();
	TrustAll();
	cJSON *out = Dispatch("PreToolUse");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "ask")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	cJSON_Delete(out);
	Define("Stop", "echo '{\"continue\":false,\"stopReason\":\"Stop now\"}'");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	out = Dispatch("Stop");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(out, "terminate")));
	cJSON_Delete(out);
}

TEST_F(HooksTest, MalformedDecisionCannotApplyPartialRewrite)
{
	Define("PreToolUse", "echo '{\"continue\":false,\"hookSpecificOutput\":{"
			     "\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"allow\","
			     "\"updatedInput\":{\"command\":\"must not run\"}}}'");
	Init();
	TrustAll();
	cJSON *out = Dispatch("PreToolUse");
	EXPECT_EQ(cJSON_GetObjectItem(out, "updatedInput"), nullptr);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(out, "blocked")));
	cJSON_Delete(out);
}

TEST_F(HooksTest, HooksBesideSelectedConfigMergeWithInlineDefinitions)
{
	Write("[memory]\nenabled=false\n[[hooks.SessionStart]]\n[[hooks.SessionStart.hooks]]\n"
	      "type='command'\ncommand='true'\n",
	      "config.toml");
	Write(R"({"hooks":{"Stop":[{"hooks":[{"type":"command","command":"true"}]}]}})",
	      "hooks.json");
	std::string config = directory + "/config.toml";
	Init(config.c_str());
	cJSON *rows = hook_manager_list(&hooks);
	EXPECT_EQ(cJSON_GetArraySize(rows), 2);
	cJSON_Delete(rows);
}

TEST_F(HooksTest, NonObjectHandlerIsRejectedWithoutExecution)
{
	Write(R"({"hooks":{"Stop":[{"hooks":[[]]}]}})");
	EXPECT_LT(hook_manager_init(&hooks, &database, cfg.get(), "", directory.c_str()), 0);
}
