#include <gtest/gtest.h>
#include "runtime/hooks.h"
#include "runtime/runtime.h"
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
		config_set_defaults(NULL, cfg.get());
		ASSERT_EQ(db_open(NULL, &database, ":memory:"), 0);
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

TEST_F(HooksTest, BatchTrustAndRejectionPersistAcrossRestart)
{
	Define("PreToolUse", "touch should-not-run");
	Init();
	cJSON *rows = hook_manager_list(&hooks);
	auto digest = field(cJSON_GetArrayItem(rows, 0), "review_digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "deny-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	Init();
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "disabled");
	digest = field(cJSON_GetArrayItem(rows, 0), "review_digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	Init();
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "trusted");
	cJSON_Delete(rows);
	EXPECT_FALSE(std::filesystem::exists(directory + "/should-not-run"));
}

TEST_F(HooksTest, UserTrustSurvivesDatabaseReopenAndWorkspaceChange)
{
	Define("PreToolUse", "printf '{}'");
	db_close(&database);
	ASSERT_EQ(db_open(NULL, &database, (directory + "/trust.db").c_str()), 0);
	Init();
	TrustAll();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	cJSON_Delete(rows);
	hook_manager_cleanup(&hooks);
	db_close(&database);
	ASSERT_EQ(db_open(NULL, &database, (directory + "/trust.db").c_str()), 0);
	std::filesystem::create_directory(directory + "/another-workspace");
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/another-workspace").c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "id"), id);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "trusted");
	cJSON_Delete(rows);
}

TEST_F(HooksTest, BatchTrustRejectsChangedSnapshotWithoutPartialApproval)
{
	Define("Stop", "printf '{}'");
	Init();
	cJSON *rows = hook_manager_list(&hooks);
	auto digest = field(cJSON_GetArrayItem(rows, 0), "review_digest");
	cJSON_Delete(rows);
	Define("Stop", "touch changed-script");
	EXPECT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), -ESTALE);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "needs_review");
	cJSON_Delete(rows);
	EXPECT_FALSE(std::filesystem::exists(directory + "/changed-script"));
}

TEST_F(HooksTest, BatchTrustIncludesProjectTrustAndRollsBackOnFailure)
{
	std::filesystem::create_directories(directory + "/project/.morph");
	std::ofstream(directory + "/project/.morph/hooks.json") <<
		R"({"hooks":{"PreToolUse":[{"hooks":[{"type":"command","command":"true"}]}]}})";
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/project").c_str()), 0);
	cJSON *rows = hook_manager_list(&hooks);
	auto digest = field(cJSON_GetArrayItem(rows, 0), "review_digest");
	cJSON_Delete(rows);
	ASSERT_EQ(db_exec(&database, "CREATE TRIGGER fail_hook_trust BEFORE INSERT "
		"ON hook_trust BEGIN SELECT RAISE(ABORT,'injected'); END;"), 0);
	EXPECT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), MORPH_ERR_DB);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "project_untrusted");
	cJSON_Delete(rows);
	ASSERT_EQ(db_exec(&database, "DROP TRIGGER fail_hook_trust;"), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "trusted");
	cJSON_Delete(rows);
}

TEST_F(HooksTest, TrustActionReenablesDisabledHook)
{
	Define("PreToolUse", "true");
	Init();
	TrustAll();
	cJSON *rows = hook_manager_list(&hooks);
	auto id = field(cJSON_GetArrayItem(rows, 0), "id");
	auto digest = field(cJSON_GetArrayItem(rows, 0), "digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "disable", id.c_str(), digest.c_str()), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "trusted");
	cJSON_Delete(rows);
}

TEST_F(HooksTest, ProjectBatchTrustDoesNotTransferToAnotherWorkspace)
{
	for (const char *name : {"first", "second"}) {
		std::filesystem::create_directories(directory + "/" + name + "/.morph");
		std::ofstream(directory + "/" + name + "/.morph/hooks.json") <<
			R"({"hooks":{"PreToolUse":[{"hooks":[{"type":"command","command":"true"}]}]}})";
	}
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/first").c_str()), 0);
	cJSON *rows = hook_manager_list(&hooks);
	auto digest = field(cJSON_GetArrayItem(rows, 0), "review_digest");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/second").c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "project_untrusted");
	cJSON_Delete(rows);
}

static std::string hook_test_hash(const std::string &text)
{
	struct sha256_ctx ctx;
	unsigned char hash[32];
	char hex[65];
	sha256_init(&ctx);
	sha256_update(&ctx, reinterpret_cast<const unsigned char *>(text.data()), text.size());
	sha256_final(&ctx, hash);
	for (size_t i = 0; i < sizeof(hash); i++)
		std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
	return hex;
}

TEST_F(HooksTest, LegacyTrustMigratesOnceAndCannotResurrectAfterRevoke)
{
	Define("Stop", "true");
	Init();
	cJSON *rows = hook_manager_list(&hooks);
	auto *row = cJSON_GetArrayItem(rows, 0);
	auto source = field(row, "source"), definition = field(row, "definition");
	auto id = field(row, "id"), digest = field(row, "digest");
	cJSON_Delete(rows);
	auto legacy_id = hook_test_hash(std::string(hooks.workdir) + "\n" + source + "\nStop\n0\n0");
	auto legacy_digest = hook_test_hash(legacy_id + "\n" + definition);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(database.handle,
		"INSERT INTO hook_trust(id,digest,definition) VALUES(?,?,?)", -1, &stmt, nullptr),
		SQLITE_OK);
	sqlite3_bind_text(stmt, 1, legacy_id.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, legacy_digest.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, definition.c_str(), -1, SQLITE_TRANSIENT);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "trusted");
	cJSON_Delete(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "revoke", id.c_str(), digest.c_str()), 0);
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(cJSON_GetArrayItem(rows, 0), "state"), "needs_review");
	cJSON_Delete(rows);
}

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
	ASSERT_EQ(config_load(NULL, cfg.get(), path.c_str()), 0);
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

TEST_F(HooksTest, JsonSyntaxDiagnosticSurvivesRuntimeFailure)
{
	Write("{\n  \"hooks\": @\n}\n");
	Write("[memory]\nenabled = false\n", "dev.toml");
	struct runtime_options options = {};
	struct config_file_error error = {};
	struct runtime *instance = nullptr;
	std::string config = directory + "/dev.toml";
	std::string db = directory + "/runtime.db";
	std::string event_json;
	options.config_path = config.c_str();
	options.db_path = db.c_str();
	options.workdir = directory.c_str();
	options.startup_error = &error;
	options.event_cb = [](const struct morph_event *event, void *data) {
		if (strcmp(event->name, "startup.failed") == 0) {
			char *json = morph_event_to_json_string(event);
			if (json) {
				*static_cast<std::string *>(data) = json;
				free(json);
			}
		}
		return 0;
	};
	options.event_user_data = &event_json;
	EXPECT_EQ(runtime_open(&options, &instance), MORPH_ERR_PARSE);
	EXPECT_EQ(instance, nullptr);
	EXPECT_EQ(std::filesystem::canonical(error.file),
		std::filesystem::canonical(directory + "/.morph/hooks.json"));
	EXPECT_EQ(error.detail.line, 2);
	EXPECT_EQ(error.detail.column, 12);
	EXPECT_NE(strstr(error.detail.message, "Invalid JSON"), nullptr);
	cJSON *event = cJSON_Parse(event_json.c_str());
	ASSERT_NE(event, nullptr);
	const cJSON *data = cJSON_GetObjectItem(event, "data");
	ASSERT_NE(data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "file")), error.file);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "line")), 2);
	cJSON_Delete(event);
	Write(R"({"hooks":{}})");
	ASSERT_EQ(runtime_open(&options, &instance), 0);
	EXPECT_EQ(error.file[0], '\0');
	EXPECT_EQ(error.detail.message[0], '\0');
	runtime_close(instance);
}

TEST_F(HooksTest, JsonDiagnosticsCoverTruncationTrailingInputAndRecovery)
{
	Write("{\n  \"hooks\":");
	EXPECT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "", directory.c_str()),
		MORPH_ERR_PARSE);
	EXPECT_EQ(hooks.error.detail.line, 2);
	EXPECT_NE(strstr(hooks.error.detail.message, "end of JSON"), nullptr);
	Write(R"({"hooks":{}} trailing)");
	EXPECT_EQ(hook_manager_reload(&hooks), MORPH_ERR_PARSE);
	EXPECT_EQ(hooks.error.detail.line, 1);
	EXPECT_EQ(hooks.error.detail.column, 14);
	Write(R"({"hooks":[]})");
	EXPECT_EQ(hook_manager_reload(&hooks), MORPH_ERR_CONFIG);
	EXPECT_NE(strstr(hooks.error.detail.message, "hooks structure"), nullptr);
	Write(R"({"hooks":{}})");
	EXPECT_EQ(hook_manager_reload(&hooks), 0);
	EXPECT_EQ(hooks.error.file[0], '\0');
	EXPECT_EQ(hooks.error.detail.line, 0);
}

TEST_F(HooksTest, SecondaryTomlDiagnosticNamesTheProjectFile)
{
	Write("[memory]\nenabled = \"invalid\"\n", ".morph/config.toml");
	EXPECT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "", directory.c_str()),
		MORPH_ERR_CONFIG);
	EXPECT_EQ(std::filesystem::canonical(hooks.error.file),
		std::filesystem::canonical(directory + "/.morph/config.toml"));
	EXPECT_EQ(hooks.error.detail.line, 2);
	EXPECT_STREQ(hooks.error.detail.path, "memory.enabled");
	EXPECT_EQ(hooks.error.detail.code, CONFIG_VALIDATION_TYPE);
	EXPECT_NE(strstr(hooks.error.detail.message, "TOML type"), nullptr);
}
