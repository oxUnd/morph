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
std::string field(const struct json *row, const char *key)
{
	const auto *item = json_member(row, key);
	return json_is_string_value(item) ? json_string_value(item) : "";
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
		struct json *root = json_object(), *map = json_add_object(root, "hooks");
		struct json *groups = json_add_array(map, event), *group = json_object();
		json_array_add(groups, group);
		json_add_string(group, "matcher", matcher);
		struct json *handlers = json_add_array(group, "hooks"), *handler = json_object();
		json_array_add(handlers, handler);
		json_add_string(handler, "type", "command");
		json_add_string(handler, "command", command.c_str());
		json_add_number(handler, "timeout", timeout);
		char *text = json_stringify(root);
		Write(text);
		free(text);
		json_free(root);
	}
	void TrustAll()
	{
		struct json *rows = hook_manager_list(&hooks);
		ASSERT_NE(rows, nullptr);
		std::vector<std::pair<std::string, std::string>> identities;
		const struct json *row;
		json_foreach (row, rows)
			identities.emplace_back(field(row, "id"), field(row, "digest"));
		json_free(rows);
		for (auto &identity : identities)
			ASSERT_EQ(hook_manager_action(&hooks, "trust", identity.first.c_str(),
						      identity.second.c_str()),
				  0);
	}
	struct json *Dispatch(const char *event, const char *json = "{}")
	{
		struct json *input = json_parse_prefix(json, NULL, NULL), *output = nullptr;
		EXPECT_EQ(hook_manager_dispatch(&hooks, event, input, &output), 0);
		json_free(input);
		return output;
	}
};

TEST_F(HooksTest, BatchTrustAndRejectionPersistAcrossRestart)
{
	Define("PreToolUse", "touch should-not-run");
	Init();
	struct json *rows = hook_manager_list(&hooks);
	auto digest = field(json_at(rows, 0), "review_digest");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "deny-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	Init();
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "disabled");
	digest = field(json_at(rows, 0), "review_digest");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	Init();
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "trusted");
	json_free(rows);
	EXPECT_FALSE(std::filesystem::exists(directory + "/should-not-run"));
}

TEST_F(HooksTest, UserTrustSurvivesDatabaseReopenAndWorkspaceChange)
{
	Define("PreToolUse", "printf '{}'");
	db_close(&database);
	ASSERT_EQ(db_open(NULL, &database, (directory + "/trust.db").c_str()), 0);
	Init();
	TrustAll();
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	json_free(rows);
	hook_manager_cleanup(&hooks);
	db_close(&database);
	ASSERT_EQ(db_open(NULL, &database, (directory + "/trust.db").c_str()), 0);
	std::filesystem::create_directory(directory + "/another-workspace");
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/another-workspace").c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "id"), id);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "trusted");
	json_free(rows);
}

TEST_F(HooksTest, BatchTrustRejectsChangedSnapshotWithoutPartialApproval)
{
	Define("Stop", "printf '{}'");
	Init();
	struct json *rows = hook_manager_list(&hooks);
	auto digest = field(json_at(rows, 0), "review_digest");
	json_free(rows);
	Define("Stop", "touch changed-script");
	EXPECT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), -ESTALE);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "needs_review");
	json_free(rows);
	EXPECT_FALSE(std::filesystem::exists(directory + "/changed-script"));
}

TEST_F(HooksTest, BatchTrustIncludesProjectTrustAndRollsBackOnFailure)
{
	std::filesystem::create_directories(directory + "/project/.morph");
	std::ofstream(directory + "/project/.morph/hooks.json") <<
		R"({"hooks":{"PreToolUse":[{"hooks":[{"type":"command","command":"true"}]}]}})";
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/project").c_str()), 0);
	struct json *rows = hook_manager_list(&hooks);
	auto digest = field(json_at(rows, 0), "review_digest");
	json_free(rows);
	ASSERT_EQ(db_exec(&database, "CREATE TRIGGER fail_hook_trust BEFORE INSERT "
		"ON hook_trust BEGIN SELECT RAISE(ABORT,'injected'); END;"), 0);
	EXPECT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), MORPH_ERR_DB);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "project_untrusted");
	json_free(rows);
	ASSERT_EQ(db_exec(&database, "DROP TRIGGER fail_hook_trust;"), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "trusted");
	json_free(rows);
}

TEST_F(HooksTest, TrustActionReenablesDisabledHook)
{
	Define("PreToolUse", "true");
	Init();
	TrustAll();
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	auto digest = field(json_at(rows, 0), "digest");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "disable", id.c_str(), digest.c_str()), 0);
	ASSERT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "trusted");
	json_free(rows);
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
	struct json *rows = hook_manager_list(&hooks);
	auto digest = field(json_at(rows, 0), "review_digest");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "trust-all", "workspace", digest.c_str()), 0);
	hook_manager_cleanup(&hooks);
	ASSERT_EQ(hook_manager_init(&hooks, &database, cfg.get(), "",
		(directory + "/second").c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "project_untrusted");
	json_free(rows);
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
	struct json *rows = hook_manager_list(&hooks);
	auto *row = json_at(rows, 0);
	auto source = field(row, "source"), definition = field(row, "definition");
	auto id = field(row, "id"), digest = field(row, "digest");
	json_free(rows);
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
	EXPECT_EQ(field(json_at(rows, 0), "state"), "trusted");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "revoke", id.c_str(), digest.c_str()), 0);
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "needs_review");
	json_free(rows);
}

TEST_F(HooksTest, UnreviewedNeverExecutes)
{
	Define("Stop", "touch should-not-exist");
	Init();
	json_free(Dispatch("Stop"));
	EXPECT_FALSE(std::filesystem::exists(directory + "/should-not-exist"));
	struct json *rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "needs_review");
	json_free(rows);
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
	struct json *out = Dispatch("PreToolUse", "{\"tool_name\":\"exec\",\"tool_input\":{}}");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "blocked")));
	EXPECT_NE(field(out, "reason").find("policy denied"), std::string::npos);
	json_free(out);
}

TEST_F(HooksTest, ExitTwoBlocksButCrashDoesNot)
{
	Define("Stop", "echo 'verify tests' >&2; exit 2");
	Init();
	TrustAll();
	struct json *out = Dispatch("Stop");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "blocked")));
	EXPECT_NE(field(out, "reason").find("verify tests"), std::string::npos);
	json_free(out);
	Define("Stop", "exit 1");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	out = Dispatch("Stop");
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
}

TEST_F(HooksTest, ChangedDefinitionRequiresNewReviewAndRejectsStaleTrust)
{
	Define("Stop", "printf '{}'");
	Init();
	TrustAll();
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	auto digest = field(json_at(rows, 0), "digest");
	json_free(rows);
	Define("Stop", "touch changed-script");
	EXPECT_EQ(hook_manager_action(&hooks, "trust", id.c_str(), digest.c_str()), -ESTALE);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "changed / needs_review");
	EXPECT_FALSE(field(json_at(rows, 0), "previous_definition").empty());
	json_free(rows);
	json_free(Dispatch("Stop"));
	EXPECT_FALSE(std::filesystem::exists(directory + "/changed-script"));
}

TEST_F(HooksTest, DisableEnableAndRevokeAreIndependentOfTrust)
{
	Define("Stop", "printf '{}'");
	Init();
	TrustAll();
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	auto digest = field(json_at(rows, 0), "digest");
	json_free(rows);
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
	struct json *rows = hook_manager_list(&hooks);
	for (int i = 0; i < json_count(rows); i++) {
		const struct json *row = json_at(rows, i);
		EXPECT_EQ(hook_manager_action(&hooks, "trust", field(row, "id").c_str(),
					      field(row, "digest").c_str()),
			  -EPERM);
	}
	json_free(rows);
}

TEST_F(HooksTest, WildcardAndIgnoredMatchersFollowEventSemantics)
{
	for (const char *event : {"PreToolUse", "UserPromptSubmit", "Stop", "Interrupt"}) {
		Define(event, "touch matched; printf '{}'",
			strcmp(event, "PreToolUse") == 0 ? "*" : "[");
		if (!hooks.ready)
			Init();
		else
			ASSERT_EQ(hook_manager_reload(&hooks), 0);
		TrustAll();
		struct json *out = Dispatch(event, R"({"tool_name":"file_read"})");
		EXPECT_TRUE(std::filesystem::exists(directory + "/matched")) << event;
		std::filesystem::remove(directory + "/matched");
		json_free(out);
	}
}

TEST_F(HooksTest, CompactionMatcherAndContinueFalseStopWithoutContext)
{
	for (const char *event : {"PreCompact", "PostCompact"}) {
		Define(event, "printf '{\"continue\":false,\"stopReason\":\"Policy stop\"}'",
			"^manual$");
		if (!hooks.ready)
			Init();
		else
			ASSERT_EQ(hook_manager_reload(&hooks), 0);
		TrustAll();
		struct json *out = Dispatch(event, R"({"trigger":"auto"})");
		EXPECT_EQ(out, nullptr);
		json_free(out);
		out = Dispatch(event, R"({"trigger":"manual"})");
		EXPECT_TRUE(json_is_true(json_member_ci(out, "terminate")));
		EXPECT_NE(field(out, "reason").find("Policy stop"), std::string::npos);
		EXPECT_TRUE(field(out, "context").empty());
		json_free(out);
	}
}

TEST_F(HooksTest, InterruptRunsAfterCancellationAndExhaustedTurnBudget)
{
	Define("Interrupt", "cat > interrupted.json; printf '{\"continue\":false}'");
	Init();
	TrustAll();
	volatile sig_atomic_t cancelled = 1;
	hooks.cancelled = &cancelled;
	hooks.remaining_ms = 0;
	struct json *out = Dispatch("Interrupt");
	EXPECT_TRUE(std::filesystem::exists(directory + "/interrupted.json"));
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	EXPECT_FALSE(json_is_true(json_member_ci(out, "terminate")));
	hooks.cancelled = nullptr;
	json_free(out);
}

TEST_F(HooksTest, InterruptUsesShortTimeoutAndRejectsLongConfiguration)
{
	Write(R"({"hooks":{"Interrupt":[{"hooks":[
		{"type":"command","command":"true"},
		{"type":"command","command":"true","timeout":4}]}]}})");
	Init();
	struct json *rows = hook_manager_list(&hooks);
	EXPECT_EQ(json_int_value(json_member_ci(json_at(rows, 0), "timeout")), 1);
	EXPECT_EQ(field(json_at(rows, 1), "state"), "invalid");
	EXPECT_FALSE(field(json_at(rows, 1), "diagnostic").empty());
	json_free(rows);
}

TEST_F(HooksTest, UnsupportedOutputCannotApplyPartialBlockOrRewrite)
{
	Define("PreToolUse", "printf '{\"suppressOutput\":false,\"hookSpecificOutput\":{"
		"\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\"}}'");
	Init();
	TrustAll();
	struct json *out = Dispatch("PreToolUse");
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
	struct json *rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_member_ci(json_at(rows, 0), "last_run"), "name"), "hook.failed");
	json_free(rows);
}

TEST_F(HooksTest, ToolPlainTextIsIgnoredAndStartupCanStop)
{
	Define("PreToolUse", "printf 'tool checker log'");
	Init();
	TrustAll();
	struct json *out = Dispatch("PreToolUse");
	EXPECT_TRUE(field(out, "context").empty());
	json_free(out);
	struct json *rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_member_ci(json_at(rows, 0), "last_run"), "name"), "hook.completed");
	json_free(rows);
	Define("SessionStart", "printf '{\"continue\":false,\"stopReason\":\"Startup denied\"}'");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	out = Dispatch("SessionStart");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "terminate")));
	json_free(out);
}

TEST_F(HooksTest, SlowHooksReportProgressAndBalancedCompletion)
{
	Define("PreToolUse", "sleep 0.4; printf '{}'", "*");
	Init();
	TrustAll();
	std::vector<std::string> events;
	hooks.event_user = &events;
	hooks.event_cb = [](const struct morph_event *event, void *user) -> int {
		auto *seen = static_cast<std::vector<std::string> *>(user);
		seen->emplace_back(event->name);
		if (strcmp(event->name, "hook.started") == 0)
			EXPECT_STREQ(event->phase, "begin");
		if (strcmp(event->name, "hook.completed") == 0)
			EXPECT_GE(json_number_value(json_member_ci(event->data, "duration_ms")),
				  300);
		return 0;
	};
	struct json *out = Dispatch("PreToolUse");
	ASSERT_EQ(events.size(), 3u);
	EXPECT_EQ(events[0], "hook.started");
	EXPECT_EQ(events[1], "hook.progress");
	EXPECT_EQ(events[2], "hook.completed");
	hooks.event_cb = nullptr;
	hooks.event_user = nullptr;
	json_free(out);
}

TEST_F(HooksTest, MatcherAliasDoesNotMatchOtherTools)
{
	Define("PreToolUse", "echo denied >&2; exit 2", "^Bash$");
	Init();
	TrustAll();
	struct json *out = Dispatch("PreToolUse", "{\"tool_name\":\"file_read\"}");
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
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
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	auto digest = field(json_at(rows, 0), "digest");
	json_free(rows);
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
	struct json *out = Dispatch("Stop");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "blocked")));
	EXPECT_EQ(field(out, "reason"), "first\nsecond\n");
	json_free(out);
}

TEST_F(HooksTest, TimeoutAndOutputOverflowAreReportedWithoutHanging)
{
	Define("Stop", "sleep 10", "", 1);
	Init();
	TrustAll();
	auto start = std::chrono::steady_clock::now();
	json_free(Dispatch("Stop"));
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
	Define("Stop", "python3 -c 'print(\"x\"*100000)'", "", 1);
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	json_free(Dispatch("Stop"));
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
	struct json *out = nullptr;
	EXPECT_EQ(hook_manager_dispatch(&hooks, "Stop", nullptr, &out), -ECANCELED);
	thread.join();
	json_free(out);
}

TEST_F(HooksTest, SecretsAreRemovedFromEnvironment)
{
	setenv("MORPH_HOOK_TEST_API_KEY", "never-pass-this", 1);
	Define("Stop", "[ -z \"$MORPH_HOOK_TEST_API_KEY\" ] && "
		       "[ \"$MORPH_HOOK_ACTIVE\" = 1 ] && echo "
		       "'{\"decision\":\"block\",\"reason\":\"clean\"}'");
	Init();
	TrustAll();
	struct json *out = Dispatch("Stop");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
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
	struct json *out = Dispatch("PreToolUse");
	EXPECT_EQ(field(json_member_ci(out, "updatedInput"), "command"), "echo checked");
	json_free(out);
}

TEST_F(HooksTest, PlainStopOutputIsAnErrorNotModelContext)
{
	Define("Stop", "echo 'unstructured text'");
	Init();
	TrustAll();
	struct json *out = Dispatch("Stop");
	EXPECT_EQ(field(out, "context"), "");
	json_free(out);
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
	struct json *rows = hook_manager_list(&hooks);
	auto id = field(json_at(rows, 0), "id");
	auto digest = field(json_at(rows, 0), "digest");
	json_free(rows);
	ASSERT_EQ(hook_manager_action(&hooks, "disable", id.c_str(), digest.c_str()), 0);
	rows = hook_manager_list(&hooks);
	EXPECT_EQ(field(json_at(rows, 0), "state"), "disabled");
	json_free(rows);
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
	struct json *rows = hook_manager_list(&hooks);
	auto *row = json_at(rows, 0);
	auto id = field(row, "id"), digest = field(row, "digest");
	auto workspace_digest = field(row, "workspace_digest");
	EXPECT_EQ(field(row, "state"), "project_untrusted");
	json_free(rows);
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
	struct json *out = Dispatch("SessionStart");
	EXPECT_NE(field(out, "context").find("Full hook context:"), std::string::npos);
	int files = 0;
	for (auto &entry : std::filesystem::directory_iterator(directory + "/.morph/output")) {
		std::ifstream input(entry.path());
		std::string text((std::istreambuf_iterator<char>(input)), {});
		EXPECT_EQ(text, "many words of context for the next model request");
		files++;
	}
	EXPECT_EQ(files, 1);
	json_free(out);
}

TEST_F(HooksTest, AskAndStopTerminationHaveDistinctDecisions)
{
	Define("PreToolUse", "echo "
			     "'{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\","
			     "\"permissionDecision\":\"ask\"}}'");
	Init();
	TrustAll();
	struct json *out = Dispatch("PreToolUse");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "ask")));
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
	Define("Stop", "echo '{\"continue\":false,\"stopReason\":\"Stop now\"}'");
	ASSERT_EQ(hook_manager_reload(&hooks), 0);
	TrustAll();
	out = Dispatch("Stop");
	EXPECT_TRUE(json_is_true(json_member_ci(out, "terminate")));
	json_free(out);
}

TEST_F(HooksTest, MalformedDecisionCannotApplyPartialRewrite)
{
	Define("PreToolUse", "echo '{\"continue\":false,\"hookSpecificOutput\":{"
			     "\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"allow\","
			     "\"updatedInput\":{\"command\":\"must not run\"}}}'");
	Init();
	TrustAll();
	struct json *out = Dispatch("PreToolUse");
	EXPECT_EQ(json_member_ci(out, "updatedInput"), nullptr);
	EXPECT_FALSE(json_is_true(json_member_ci(out, "blocked")));
	json_free(out);
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
	struct json *rows = hook_manager_list(&hooks);
	EXPECT_EQ(json_count(rows), 2);
	json_free(rows);
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
	struct json *event = json_parse_prefix(event_json.c_str(), NULL, NULL);
	ASSERT_NE(event, nullptr);
	const struct json *data = json_member_ci(event, "data");
	ASSERT_NE(data, nullptr);
	EXPECT_STREQ(json_string_value(json_member_ci(data, "file")), error.file);
	EXPECT_EQ(json_number_value(json_member_ci(data, "line")), 2);
	json_free(event);
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
