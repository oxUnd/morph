#include "runtime_test_support.hpp"

extern "C" {
#include "sapi/cli/commands/registry.h"
#include "util/arena.h"
#include "util/error.h"
}

class HistoryInspectionTest : public RuntimeFacadeTest {
protected:
	struct db db{};
	struct session session{};

	void SetUp() override
	{
		RuntimeFacadeTest::SetUp();
		WriteConfig("[memory]\nenabled=false\n[features]\nhooks=false\n");
		Open();
		ASSERT_EQ(runtime_session_create_and_select(instance, "history-test", &session), 0);
		ASSERT_EQ(db_open(&db, database.c_str()), 0);
	}

	void TearDown() override
	{
		db_close(&db);
		RuntimeFacadeTest::TearDown();
	}

	int64_t Add(const char *kind, const char *content, bool active = true, bool trimmed = false)
	{
		struct model_history_insert item{};
		int64_t id = 0;
		item.session_id = session.id;
		item.kind = kind;
		item.content = content;
		item.role = "assistant";
		item.active = active;
		item.truncated = trimmed;
		item.token_count = 7;
		EXPECT_EQ(model_history_add(&db, &item, &id), 0);
		return id;
	}

	static std::string Field(const cJSON *json, const char *key)
	{
		const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
		return value ? value : "";
	}
};

TEST_F(HistoryInspectionTest, DistinguishesExactSummaryAncestryAndInactiveRecords)
{
	int64_t old = Add("user_message", "old source", false);
	Add("assistant_message", "cancelled content", false);
	int64_t first = Add("compaction_summary", "first summary", false);
	int64_t current = Add("compaction_summary", "active summary");
	Add("tool_result", "shortened tool output", true, true);
	Add("audit_metadata", "never replayed");
	std::string sql = "INSERT INTO history_summary_sources VALUES(" + std::to_string(first) +
		"," + std::to_string(old) + "),(" + std::to_string(current) + "," +
		std::to_string(first) + ")";
	ASSERT_EQ(db_exec(&db, sql.c_str()), 0);
	cJSON *report = nullptr;
	ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
	cJSON *entries = cJSON_GetObjectItem(report, "entries");
	ASSERT_EQ(cJSON_GetArraySize(entries), 6);
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 0), "status"), "VIA SUMMARY");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 1), "status"), "OUT");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 2), "status"), "VIA SUMMARY");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 3), "status"), "IN");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 4), "status"), "IN / TRIMMED");
	EXPECT_NE(Field(cJSON_GetArrayItem(entries, 4), "reason").find("original is unavailable"),
		std::string::npos);
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 5), "status"), "OUT");
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(report, "active_items")), 2);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(report, "history_tokens")), 14);
	cJSON_Delete(report);
	EXPECT_EQ(model_history_count(&db, session.id, 0), 6);
}

TEST_F(HistoryInspectionTest, EntryInputUsesRealSummaryWrapperAndDoesNotReplayInactiveText)
{
	Add("assistant_message", "inactive text", false);
	Add("compaction_summary", "summary facts");
	cJSON *input = nullptr;
	EXPECT_EQ(runtime_session_history_entry_input(instance, 1, &input), -ENODATA);
	ASSERT_EQ(runtime_session_history_entry_input(instance, 2, &input), 0);
	char *text = cJSON_PrintUnformatted(input);
	ASSERT_NE(text, nullptr);
	EXPECT_NE(std::string(text).find("Snapshot through history sequence 1"), std::string::npos);
	EXPECT_NE(std::string(text).find("summary facts"), std::string::npos);
	free(text);
	cJSON_Delete(input);
}

TEST_F(HistoryInspectionTest, InternalMemoryReceiptsStayOutEvenAfterImportOrSummaryCoverage)
{
	int64_t receipt = Add("background_receipt",
		"[Background receipt] memory_consolidation: memory consolidation queued");
	int64_t summary = Add("compaction_summary", "retained summary");
	std::string sql = "INSERT INTO history_summary_sources VALUES(" +
		std::to_string(summary) + "," + std::to_string(receipt) + ")";
	ASSERT_EQ(db_exec(&db, sql.c_str()), 0);
	for (bool active : {true, false}) {
		ASSERT_EQ(db_exec(&db, ("UPDATE model_history_items SET active=" +
			std::string(active ? "1" : "0") + " WHERE id=" +
			std::to_string(receipt)).c_str()), 0);
		cJSON *report = nullptr;
		ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
		cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItem(report, "entries"), 0);
		EXPECT_EQ(Field(entry, "status"), "OUT");
		EXPECT_NE(Field(entry, "reason").find("Internal memory-maintenance"), std::string::npos);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(report, "active_items")), 1);
		EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(report, "history_tokens")), 7);
		cJSON_Delete(report);
		cJSON *input = nullptr;
		EXPECT_EQ(runtime_session_history_entry_input(instance, 1, &input), -ENODATA);
	}
}

TEST_F(HistoryInspectionTest, InactiveCallDoesNotHideAnActiveOrphanResult)
{
	struct model_history_insert call{};
	call.session_id = session.id;
	call.kind = "assistant_tool_calls";
	call.role = "assistant";
	call.payload_json = "{\"calls\":[{\"tool_call_id\":\"local\","
		"\"provider_call_id\":\"remote\",\"name\":\"exec\",\"arguments\":\"{}\"}]}";
	call.active = 0;
	ASSERT_EQ(model_history_add(&db, &call, nullptr), 0);
	struct model_history_insert result{};
	result.session_id = session.id;
	result.kind = "tool_result";
	result.role = "tool";
	result.content = "orphan result";
	result.tool_call_id = "local";
	result.provider_call_id = "remote";
	result.active = 1;
	ASSERT_EQ(model_history_add(&db, &result, nullptr), 0);
	cJSON *report = nullptr;
	ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "needs_repair")));
	cJSON_Delete(report);
}

TEST_F(HistoryInspectionTest, LegacyDisplayHistoryIsReadOnlyAndNotClaimedAsConfirmedInput)
{
	ASSERT_EQ(message_add(&db, session.id, "user", "legacy 中文", 7), 0);
	cJSON *report = nullptr;
	ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
	cJSON *entries = cJSON_GetObjectItem(report, "entries");
	ASSERT_EQ(cJSON_GetArraySize(entries), 1);
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 0), "status"), "LEGACY");
	EXPECT_EQ(model_history_count(&db, session.id, 0), 0);
	cJSON_Delete(report);
}

TEST_F(HistoryInspectionTest, CapturedInputIsRedactedReplacedPersistedAndSessionScoped)
{
	struct react_context *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	react->history_secrets[0] = strdup("exact-secret-value");
	ASSERT_NE(react->history_secrets[0], nullptr);
	react->history_secret_count = 1;
	strcpy(react->turn_id, "snapshot-turn");
	struct model model{};
	strcpy(model.adapter, "openai-chat-compatible");
	strcpy(model.model_id, "snapshot-model");
	strcpy(model.provider, "openai");
	struct chat_message message{};
	message.role = const_cast<char *>("user");
	message.content = const_cast<char *>("first input exact-secret-value");
	struct tool_desc tool{};
	tool.name = "inspect";
	tool.description = "Inspection tool schema";
	tool.input_schema = "{\"type\":\"object\",\"properties\":{}}";
	ASSERT_EQ(agent_history_record_request(react, &model, "Instructions and memory", &message,
		1, &tool, 1), 0);
	cJSON *snapshot = nullptr;
	ASSERT_EQ(runtime_session_last_request(instance, &snapshot), 0);
	std::string input = Field(snapshot, "input_json");
	EXPECT_EQ(input.find("exact-secret-value"), std::string::npos);
	EXPECT_NE(input.find("[REDACTED]"), std::string::npos);
	EXPECT_NE(input.find("Instructions and memory"), std::string::npos);
	EXPECT_NE(input.find("Inspection tool schema"), std::string::npos);
	cJSON_Delete(snapshot);
	message.content = const_cast<char *>("replacement input");
	ASSERT_EQ(agent_history_record_request(react, &model, "new instructions", &message,
		1, nullptr, 0), 0);
	react_context_destroy(react);
	runtime_close(instance);
	instance = nullptr;
	Open();
	ASSERT_EQ(runtime_session_select_existing(instance, session.id, nullptr), 0);
	ASSERT_EQ(runtime_session_last_request(instance, &snapshot), 0);
	EXPECT_NE(Field(snapshot, "input_json").find("replacement input"), std::string::npos);
	EXPECT_EQ(Field(snapshot, "input_json").find("first input"), std::string::npos);
	cJSON_Delete(snapshot);
	struct session other{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "other", &other), 0);
	EXPECT_EQ(runtime_session_last_request(instance, &snapshot), -ENODATA);
}

TEST_F(HistoryInspectionTest, CompactionRecordsOnlyActualSummarySources)
{
	Add("user_message", "source facts");
	Add("assistant_message", "cancelled text", false);
	struct react_context *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.summarize = [](const char *, void *, char **out) {
		*out = strdup("condensed facts");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	react_context_destroy(react);
	cJSON *report = nullptr;
	ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
	cJSON *entries = cJSON_GetObjectItem(report, "entries");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 0), "status"), "VIA SUMMARY");
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 1), "status"), "OUT");
	cJSON_Delete(report);
}

TEST_F(HistoryInspectionTest, PlainHistoryIncludesToolsAndPreservesCompatibilityCommands)
{
	Add("user_message", "user text");
	Add("tool_result", "tool output");
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
	cli_set_color_enabled(0);


	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&ctx, "/history --all");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_NE(output.find("Tool result"), std::string::npos);
	EXPECT_NE(output.find("[#1 IN]"), std::string::npos);
	EXPECT_NE(output.find("estimated"), std::string::npos);
	EXPECT_EQ(output.find('\033'), std::string::npos);
	EXPECT_EQ(cli_command_dispatch(&ctx, "/history --entry bad"), -EINVAL);
	EXPECT_EQ(cli_command_dispatch(&ctx, "/history --entry"), -EINVAL);

	cli_set_color_enabled(1);
}

TEST_F(HistoryInspectionTest, SourceRecordingFailureRollsBackTheEntireCompaction)
{
	Add("user_message", "source must survive");
	ASSERT_EQ(db_exec(&db, "CREATE TRIGGER fail_summary_sources BEFORE INSERT ON "
		"history_summary_sources BEGIN SELECT RAISE(ABORT,'test failure'); END"), 0);
	struct react_context *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.summarize = [](const char *, void *, char **out) {
		*out = strdup("summary");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_NE(agent_history_compact(react, 1), 1);
	EXPECT_EQ(model_history_count(&db, session.id, 0), 1);
	EXPECT_EQ(model_history_count(&db, session.id, 1), 1);
	EXPECT_EQ(model_history_compaction_count(&db, session.id), 0);
	react_context_destroy(react);
}

TEST_F(HistoryInspectionTest, FailedSummarizerPreservesActiveHistory)
{
	Add("user_message", "old details unavailable in fallback");
	struct react_context *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.summarize = [](const char *, void *, char **) { return -EIO; };
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact_with_trigger(react, 1, "in_turn_1"), -EIO);
	react_context_destroy(react);
	cJSON *report = nullptr;
	ASSERT_EQ(runtime_session_history_inspect(instance, &report), 0);
	cJSON *entries = cJSON_GetObjectItem(report, "entries");
	EXPECT_EQ(cJSON_GetArraySize(entries), 1);
	EXPECT_EQ(Field(cJSON_GetArrayItem(entries, 0), "status"), "IN");
	cJSON_Delete(report);
}

TEST_F(HistoryInspectionTest, TransientSummaryFailureRetriesBeforeCommit)
{
	Add("user_message", "keep the exact objective");
	auto *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	int attempts = 0;
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.summarize_user_data = &attempts;
	react->compress.summarize = [](const char *, void *user, char **out) -> int {
		if (++*static_cast<int *>(user) == 1)
			return MORPH_ERR_NETWORK;
		*out = strdup("Objective and verification checkpoint");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_EQ(agent_history_compact(react, 1), 1);
	EXPECT_EQ(attempts, 2);
	EXPECT_EQ(model_history_compaction_count(&db, session.id), 1);
	react_context_destroy(react);
}

TEST_F(HistoryInspectionTest, OverflowPrunesOnlyCandidateToolGroupsAndKeepsRequirements)
{
	Add("user_message", "EXACT_REQUIREMENT");
	auto *react = react_context_create(nullptr, nullptr, nullptr, nullptr);
	ASSERT_NE(react, nullptr);
	int attempts = 0;
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = session.id;
	ASSERT_EQ(react_set_turn_id(react, "overflow-test"), 0);
	struct tool_call call{};
	strcpy(call.id, "provider-old");
	strcpy(call.tool_call_id, "old");
	strcpy(call.name, "exec");
	call.arguments = const_cast<char *>("{}");
	ASSERT_EQ(agent_history_record_tool_calls(react, "gather", nullptr, &call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "old", "provider-old", "exec",
						   "OLD_BULKY_TOOL_RESULT", 0),
		  0);
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.summarize_user_data = &attempts;
	react->compress.summarize = [](const char *input, void *user, char **out) -> int {
		EXPECT_NE(strstr(input, "EXACT_REQUIREMENT"), nullptr);
		if (++*static_cast<int *>(user) == 1)
			return MORPH_ERR_CONTEXT_LIMIT;
		EXPECT_EQ(strstr(input, "OLD_BULKY_TOOL_RESULT"), nullptr);
		*out = strdup("Continue the exact requirement");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	EXPECT_EQ(attempts, 2);
	EXPECT_GE(model_history_count(&db, session.id, 0), 3);
	react_context_destroy(react);
}
