#include <gtest/gtest.h>
#include "db/database.h"
#include "agent/history.h"
#include "agent/react.h"
#include "agent/tokenizer.h"
#include "session.h"
#include "util/arena.h"
#include "util/array.h"
#include "util/utf8.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

class SessionTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];
	void SetUp() override {
		snprintf(db_path, sizeof(db_path), "/tmp/ma_test_session_%d.db", getpid());
		std::remove(db_path);
		int rc = db_open(NULL, &db, db_path);
		ASSERT_EQ(rc, 0);
		rc = db_init_schema(&db);
		ASSERT_EQ(rc, 0);
	}
	void TearDown() override {
		db_close(&db);
		std::remove(db_path);
	}
};

static int history_summary_failure(const char *, void *, char **out)
{
	*out = nullptr;
	return -EIO;
}

TEST_F(SessionTest, CreateSession) {
	struct session s;
	int rc = session_create(&db, "test_session", "gpt-4o", &s);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(s.name, "test_session");
	EXPECT_STREQ(s.model, "gpt-4o");
	EXPECT_GT(s.id, 0);
	EXPECT_STRNE(s.display_id, "");
	EXPECT_EQ(strlen(s.display_id), 8);
	// hex format
	for (int i = 0; s.display_id[i]; i++)
		EXPECT_TRUE(isxdigit(s.display_id[i]));
}

TEST_F(SessionTest, CreateDuplicate) {
	struct session s1, s2;
	int rc = session_create(&db, "dup_session", "gpt-4o", &s1);
	EXPECT_EQ(rc, 0);
	rc = session_create(&db, "dup_session", "gpt-4o", &s2);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, GetByName) {
	struct session s;
	session_create(&db, "findme", "gpt-4o", &s);
	struct session found;
	int rc = session_get_by_name(&db, "findme", &found);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(found.name, "findme");
}

TEST_F(SessionTest, GetByNameNotFound) {
	struct session found;
	int rc = session_get_by_name(&db, "nonexistent", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, GetById) {
	struct session s;
	session_create(&db, "byid", "gpt-4o", &s);
	struct session found;
	int rc = session_get_by_id(&db, s.id, &found);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(found.id, s.id);
}

TEST_F(SessionTest, GetByIdNotFound) {
	struct session found;
	int rc = session_get_by_id(&db, 99999, &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, ListSessions) {
	struct session s1, s2, s3;
	session_create(&db, "s1", "gpt-4o", &s1);
	session_create(&db, "s2", "claude-3.5", &s2);
	session_create(&db, "s3", "gpt-4o", &s3);
	struct session *list;
	int count = 0;
	int rc = session_list(&db, &list, &count, 0, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(count, 3);
	free(list);
}

TEST_F(SessionTest, RenameSession) {
	struct session s;
	session_create(&db, "original", "gpt-4o", &s);
	int rc = session_rename(&db, s.id, "renamed");
	EXPECT_EQ(rc, 0);
	struct session found;
	rc = session_get_by_name(&db, "renamed", &found);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(found.name, "renamed");
}

TEST_F(SessionTest, DeleteSession) {
	struct session s;
	session_create(&db, "to_delete", "gpt-4o", &s);
	int rc = session_delete(&db, s.id);
	EXPECT_EQ(rc, 0);
	struct session found;
	rc = session_get_by_name(&db, "to_delete", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, UpdateModel) {
	struct session s;
	session_create(&db, "model_test", "gpt-4o", &s);
	int rc = session_update_model(&db, s.id, "claude-3.5");
	EXPECT_EQ(rc, 0);
	struct session found;
	session_get_by_id(&db, s.id, &found);
	EXPECT_STREQ(found.model, "claude-3.5");
}

TEST_F(SessionTest, NullParams) {
	struct session s;
	EXPECT_NE(session_create(nullptr, "test", "gpt-4o", &s), 0);
	EXPECT_NE(session_create(&db, nullptr, "gpt-4o", &s), 0);
}

TEST_F(SessionTest, AddMessage) {
	struct session s;
	session_create(&db, "msg_test", "gpt-4o", &s);
	int rc = message_add(&db, s.id, "user", "hello world", 3);
	EXPECT_EQ(rc, 0);
}

TEST_F(SessionTest, AddMessageWithTurnId) {
	struct session s;
	session_create(&db, "msg_turn_test", "gpt-4o", &s);
	int rc = message_add_with_turn_id(&db, s.id, "user", "hello world",
					  3, "turn_test");
	EXPECT_EQ(rc, 0);
	int count = 0;
	struct message *msgs = message_list(&db, s.id, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(msgs, nullptr);
	EXPECT_STREQ(msgs->turn_id, "turn_test");
	message_free_list(msgs);
}

TEST_F(SessionTest, ListMessages) {
	struct session s;
	session_create(&db, "msg_list", "gpt-4o", &s);
	message_add(&db, s.id, "user", "hello", 1);
	message_add(&db, s.id, "assistant", "hi there", 2);
	int count = 0;
	struct message *msgs = message_list(&db, s.id, &count);
	EXPECT_EQ(count, 2);
	EXPECT_STREQ(msgs->role, "user");
	EXPECT_STREQ(msgs->content, "hello");
	EXPECT_EQ(msgs->turn_id, nullptr);
	message_free_list(msgs);
}

TEST_F(SessionTest, ListMessagesUsesStableOrderForSameTimestamp) {
	struct session s;
	session_create(&db, "msg_list_same_time", "gpt-4o", &s);
	message_add(&db, s.id, "user", "first", 1);
	message_add(&db, s.id, "assistant", "second", 1);
	message_add(&db, s.id, "user", "third", 1);

	ASSERT_EQ(sqlite3_exec(db.handle,
		"UPDATE messages SET created_at=12345", NULL, NULL, NULL),
		SQLITE_OK);

	int count = 0;
	struct message *msgs = message_list(&db, s.id, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(msgs, nullptr);
	ASSERT_NE(msgs->next, nullptr);
	ASSERT_NE(msgs->next->next, nullptr);
	EXPECT_STREQ(msgs->content, "first");
	EXPECT_STREQ(msgs->next->content, "second");
	EXPECT_STREQ(msgs->next->next->content, "third");
	message_free_list(msgs);
}

TEST_F(SessionTest, MessageCount) {
	struct session s;
	session_create(&db, "msg_count", "gpt-4o", &s);
	message_add(&db, s.id, "user", "hello", 1);
	message_add(&db, s.id, "assistant", "hi", 1);
	message_add(&db, s.id, "user", "how are you", 3);
	int count = message_count(&db, s.id);
	EXPECT_EQ(count, 3);
}

TEST_F(SessionTest, MessageFreeListNull) {
	EXPECT_NO_FATAL_FAILURE(message_free_list(nullptr));
}

TEST_F(SessionTest, DisplayIdGetByDisplayId) {
	struct session s;
	session_create(&db, "display_find", "gpt-4o", &s);
	ASSERT_STRNE(s.display_id, "");

	struct session found;
	int rc = session_get_by_display_id(&db, s.display_id, &found);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(found.id, s.id);
	EXPECT_STREQ(found.display_id, s.display_id);
	EXPECT_STREQ(found.name, s.name);

	rc = session_get_by_display_id(&db, "00000000", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, DisplayIdUnique) {
	struct session s1, s2;
	session_create(&db, "unique1", "gpt-4o", &s1);
	session_create(&db, "unique2", "gpt-4o", &s2);
	// extremely unlikely to collide, but still…
	EXPECT_STRNE(s1.display_id, s2.display_id);
	EXPECT_EQ(strlen(s1.display_id), strlen(s2.display_id));
}

TEST_F(SessionTest, DisplayIdPersist) {
	struct session s;
	session_create(&db, "persist_test", "gpt-4o", &s);
	ASSERT_STRNE(s.display_id, "");

	struct session loaded;
	int rc = session_get_by_id(&db, s.id, &loaded);
	ASSERT_EQ(rc, 0);
	EXPECT_STREQ(loaded.display_id, s.display_id);

	rc = session_get_by_name(&db, "persist_test", &loaded);
	ASSERT_EQ(rc, 0);
	EXPECT_STREQ(loaded.display_id, s.display_id);
}

TEST_F(SessionTest, DisplayIdInList) {
	session_create(&db, "list_display", "gpt-4o", nullptr);
	struct session *list;
	int count;
	session_list(&db, &list, &count, 0, NULL);
	ASSERT_GE(count, 1);
	bool found = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(list[i].name, "list_display") == 0) {
			found = true;
			EXPECT_EQ(strlen(list[i].display_id), 8);
			break;
		}
	}
	EXPECT_TRUE(found);
	free(list);
}

TEST_F(SessionTest, AutoNamedUnique) {
	/* Simulate cmd_new: generate unique name with timestamp */
	struct session s1, s2;
	char name1[256], name2[256];
	snprintf(name1, sizeof(name1), "new_%lld", (long long)time(NULL));
	int rc = session_create(&db, name1, "gpt-4o", &s1);
	EXPECT_EQ(rc, 0);

	/* Second call at a different time should not collide */
	sleep(1);
	snprintf(name2, sizeof(name2), "new_%lld", (long long)time(NULL));
	rc = session_create(&db, name2, "gpt-4o", &s2);
	EXPECT_EQ(rc, 0);
	EXPECT_STRNE(s1.name, s2.name);
}

TEST_F(SessionTest, ModelHistoryStoresStructuredItemsInSequence) {
	struct session s;
	struct model_history_insert first = {};
	struct model_history_insert second = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_items", "test", &s), 0);
	first.session_id = s.id;
	first.turn_id = "turn_1";
	first.kind = "assistant_tool_calls";
	first.role = "assistant";
	first.payload_json = "{\"calls\":[]}";
	first.active = 1;
	second.session_id = s.id;
	second.turn_id = "turn_1";
	second.kind = "tool_result";
	second.role = "tool";
	second.content = "done";
	second.tool_call_id = "tool_1";
	second.provider_call_id = "call_1";
	second.tool_name = "exec";
	second.active = 1;
	ASSERT_EQ(model_history_add(&db, &first, nullptr), 0);
	ASSERT_EQ(model_history_add(&db, &second, nullptr), 0);

	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(items->sequence_no, 1);
	EXPECT_STREQ(items->kind, "assistant_tool_calls");
	ASSERT_NE(items->next, nullptr);
	EXPECT_EQ(items->next->sequence_no, 2);
	EXPECT_STREQ(items->next->provider_call_id, "call_1");
	model_history_free_list(items);
}

TEST_F(SessionTest, ModelHistoryDeactivateTurnKeepsOtherTurns) {
	struct session s;
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_deactivate_turn", "test", &s), 0);
	item.session_id = s.id;
	item.turn_id = "turn_failed";
	item.kind = "user_message";
	item.role = "user";
	item.content = "failed request";
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "failure";
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.turn_id = "turn_good";
	item.kind = "user_message";
	item.role = "user";
	item.content = "good request";
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "success";
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);

	ASSERT_EQ(model_history_deactivate_turn(&db, s.id, "turn_failed"), 0);
	EXPECT_EQ(model_history_count(&db, s.id, 0), 4);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->turn_id, "turn_good");
	ASSERT_NE(items->next, nullptr);
	EXPECT_STREQ(items->next->turn_id, "turn_good");
	model_history_free_list(items);
	EXPECT_EQ(model_history_deactivate_turn(&db, s.id, ""), -EINVAL);
}

TEST_F(SessionTest, ModelHistoryMigratesTranscriptOnce) {
	struct session s;
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_migrate", "test", &s), 0);
	ASSERT_EQ(message_add(&db, s.id, "user", "hello", 2), 0);
	ASSERT_EQ(message_add(&db, s.id, "assistant", "hi", 1), 0);
	ASSERT_EQ(model_history_migrate_messages(&db, s.id), 0);
	ASSERT_EQ(model_history_migrate_messages(&db, s.id), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	EXPECT_STREQ(items->kind, "user_message");
	EXPECT_STREQ(items->next->kind, "assistant_message");
	model_history_free_list(items);
}

TEST_F(SessionTest, ModelHistoryCompactionIsPersistentAndKeepsUserBudget) {
	struct session s;
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_compact", "test", &s), 0);
	item.session_id = s.id;
	item.kind = "user_message";
	item.role = "user";
	item.content = "first";
	item.token_count = 3;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "answer";
	item.token_count = 3;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "user_message";
	item.role = "user";
	item.content = "latest";
	item.token_count = 2;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);

	ASSERT_EQ(model_history_compact(&db, s.id, "turn_3", "handoff", 2,
		2, 0, 8, 0, "test", nullptr), 0);
	struct model_history_item *active =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->content, "latest");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->kind, "compaction_summary");
	EXPECT_STREQ(active->next->content, "handoff");
	model_history_free_list(active);
	EXPECT_EQ(model_history_count(&db, s.id, 0), 4);
}

TEST_F(SessionTest, CompactionRestoresArchivedAnswerWithRetainedRequest)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "closed_turn", "test", &s), 0);
	struct model_history_insert item = {};
	item.session_id = s.id;
	item.turn_id = "old_turn";
	item.kind = "user_message";
	item.role = "user";
	item.content = "Fix the placeholder";
	item.token_count = 2;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "Fixed and verified";
	item.token_count = 3;
	item.active = 0;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(model_history_compact(&db, s.id, "now", "checkpoint", 1,
		5, 0, 6, 0, "test", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 3);
	EXPECT_STREQ(items->kind, "user_message");
	EXPECT_STREQ(items->next->content, "Fixed and verified");
	model_history_free_list(items);
}

TEST_F(SessionTest, LegacyArchivedAnswerClosesRequestWithoutAssumingSuccess)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "legacy_closure", "test", &s), 0);
	struct model_history_insert item = {};
	item.session_id = s.id;
	item.turn_id = "answered";
	item.kind = "user_message";
	item.role = "user";
	item.content = "Fix the editor";
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "I am blocked on permissions";
	item.active = 0;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.turn_id = "unfinished";
	item.kind = "user_message";
	item.role = "user";
	item.content = "Continue testing";
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	struct tokenizer *tok = tokenizer_create(NULL, "test", 4096);
	ASSERT_NE(tok, nullptr);
	int changed = 0;
	ASSERT_EQ(agent_history_repair(&db, s.id, tok, nullptr, &changed), 0);
	EXPECT_GT(changed, 0);
	int count = 0;
	auto *items = model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items->payload_json, nullptr);
	EXPECT_NE(strstr(items->payload_json, "answered"), nullptr);
	EXPECT_EQ(strstr(items->payload_json, "success"), nullptr);
	EXPECT_EQ(items->next->payload_json, nullptr);
	struct arena *arena = arena_create(0);
	morph_array_t messages = {};
	struct chat_message slot = {};
	ASSERT_EQ(morph_array_init(&messages, 4, sizeof(slot)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, items, &messages, arena), 0);
	auto *request = static_cast<struct chat_message *>(morph_array_get(&messages, 0));
	EXPECT_NE(strstr(request->content, "execution ended: answered"), nullptr);
	EXPECT_EQ(tokenizer_count(tok, request->content), items->token_count);
	request = static_cast<struct chat_message *>(morph_array_get(&messages, 1));
	EXPECT_STREQ(request->content, "Continue testing");
	struct agent_history_diagnostic diagnostic = {};
	ASSERT_EQ(agent_history_diagnose(items, tok, &diagnostic), 0);
	EXPECT_EQ(diagnostic.token_mismatches, 0);
	model_history_free_list(items);
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	ASSERT_EQ(agent_history_repair(&db, s.id, tok, nullptr, &changed), 0);
	EXPECT_EQ(changed, 0);
	tokenizer_destroy(tok);
}

TEST_F(SessionTest, SnapshotBoundaryPrecedesLaterFinalAnswer)
{
	struct model_history_item summary = {};
	struct model_history_item answer = {};
	strcpy(summary.kind, "compaction_summary");
	summary.sequence_no = 10;
	summary.content = const_cast<char *>("Pending work: run the self-test");
	summary.next = &answer;
	strcpy(answer.kind, "assistant_message");
	answer.sequence_no = 11;
	answer.content = const_cast<char *>("Self-test complete: 34 PASS, 0 FAIL");
	struct arena *arena = arena_create(0);
	morph_array_t messages = {};
	struct chat_message slot = {};
	ASSERT_EQ(morph_array_init(&messages, 4, sizeof(slot)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &summary, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 2U);
	auto *message = static_cast<struct chat_message *>(morph_array_get(&messages, 0));
	EXPECT_NE(strstr(message->content, "sequence 9"), nullptr);
	EXPECT_NE(strstr(message->content, "supersede this snapshot"), nullptr);
	EXPECT_TRUE(message->is_context_summary);
	message = static_cast<struct chat_message *>(morph_array_get(&messages, 1));
	EXPECT_STREQ(message->content, "Self-test complete: 34 PASS, 0 FAIL");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(SessionTest, CompactionDoesNotRetainRequestWithoutItsAnswerOnBudgetLimit)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "closure_budget", "test", &s), 0);
	struct model_history_insert item = {};
	item.session_id = s.id;
	item.turn_id = "closed";
	item.kind = "user_message";
	item.role = "user";
	item.content = "Fix it";
	item.token_count = 2;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = "Already done";
	item.token_count = 3;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(model_history_compact(&db, s.id, "now", "checkpoint", 1,
		2, 0, 6, 0, "test", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(items->kind, "compaction_summary");
	model_history_free_list(items);
}

TEST_F(SessionTest, CompactionRetainsRecentRoundsWithinBudget)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "recent_rounds", "test", &s), 0);
	for (int i = 0; i < 3; i++) {
		std::string turn = "turn_" + std::to_string(i);
		struct model_history_insert item = {};
		item.session_id = s.id;
		item.turn_id = turn.c_str();
		item.kind = "user_message";
		item.role = "user";
		item.content = "request";
		item.token_count = 2;
		item.active = 1;
		ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
		item.kind = "assistant_message";
		item.role = "assistant";
		item.content = "done";
		ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	}
	ASSERT_EQ(model_history_compact(&db, s.id, "now", "checkpoint", 1,
		0, 4, 12, 2, "test", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, s.id, 1, &count);
	int tokens = 0;
	for (auto *item = items; item; item = item->next) {
		tokens += item->token_count;
		if (strcmp(item->kind, "compaction_summary") != 0)
			EXPECT_STREQ(item->turn_id, "turn_2");
	}
	EXPECT_LE(tokens, 5);
	EXPECT_EQ(count, 3);
	model_history_free_list(items);
}

TEST_F(SessionTest, ModelHistoryCompactionKeepsLatestCompleteToolExchange) {
	struct session s;
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_recent_tools", "test", &s), 0);
	item.session_id = s.id;
	item.turn_id = "turn_1";
	item.kind = "user_message";
	item.role = "user";
	item.content = "diagnose the failure";
	item.token_count = 2;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.content = "first call";
	item.payload_json = "{\"calls\":[]}";
	item.token_count = 10;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "tool_result";
	item.role = "tool";
	item.content = "old result";
	item.payload_json = nullptr;
	item.tool_call_id = "call_old";
	item.token_count = 10;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.content = "latest call";
	item.payload_json = "{\"calls\":[]}";
	item.tool_call_id = nullptr;
	item.token_count = 10;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "tool_result";
	item.role = "tool";
	item.content = "latest result";
	item.payload_json = nullptr;
	item.tool_call_id = "call_latest";
	item.token_count = 10;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);

	ASSERT_EQ(model_history_compact(&db, s.id, "turn_1", "checkpoint", 2,
		2, 20, 42, 0, "test", nullptr), 0);
	struct model_history_item *active =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 4);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->kind, "user_message");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->kind, "assistant_tool_calls");
	EXPECT_STREQ(active->next->content, "latest call");
	ASSERT_NE(active->next->next, nullptr);
	EXPECT_STREQ(active->next->next->kind, "tool_result");
	EXPECT_STREQ(active->next->next->content, "latest result");
	ASSERT_NE(active->next->next->next, nullptr);
	EXPECT_STREQ(active->next->next->next->kind, "compaction_summary");
	model_history_free_list(active);
}

TEST_F(SessionTest, ModelHistoryRepairsInterruptedToolCall) {
	struct session s;
	struct model_history_insert call = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_repair", "test", &s), 0);
	call.session_id = s.id;
	call.turn_id = "turn_interrupted";
	call.kind = "assistant_tool_calls";
	call.role = "assistant";
	call.payload_json =
		"{\"calls\":[{\"tool_call_id\":\"tool_1\","
		"\"provider_call_id\":\"call_1\",\"name\":\"exec\","
		"\"arguments\":\"{}\"}]}";
	call.active = 1;
	ASSERT_EQ(model_history_add(&db, &call, nullptr), 0);
	ASSERT_EQ(agent_history_repair_interrupted(&db, s.id), 0);
	ASSERT_EQ(agent_history_repair_interrupted(&db, s.id), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items->next, nullptr);
	EXPECT_STREQ(items->next->kind, "tool_result");
	EXPECT_STREQ(items->next->provider_call_id, "call_1");
	EXPECT_NE(std::strstr(items->next->content, "interrupted"), nullptr);
	model_history_free_list(items);
}

TEST_F(SessionTest, HistoryRepairUsesLocalIdsForDuplicateProviderIds) {
	struct session s;
	struct model_history_insert call = {};
	struct model_history_insert result = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_duplicate_provider", "test", &s),
		0);
	call.session_id = s.id;
	call.turn_id = "turn_duplicate_provider";
	call.kind = "assistant_tool_calls";
	call.role = "assistant";
	call.payload_json =
		"{\"calls\":[{\"tool_call_id\":\"local_1\","
		"\"provider_call_id\":\"provider_same\",\"name\":\"tool\","
		"\"arguments\":\"{}\"},{\"tool_call_id\":\"local_2\","
		"\"provider_call_id\":\"provider_same\",\"name\":\"tool\","
		"\"arguments\":\"{}\"}]}";
	call.active = 1;
	ASSERT_EQ(model_history_add(&db, &call, nullptr), 0);
	result.session_id = s.id;
	result.turn_id = "turn_duplicate_provider";
	result.kind = "tool_result";
	result.role = "tool";
	result.content = "first result";
	result.tool_call_id = "local_1";
	result.provider_call_id = "provider_same";
	result.tool_name = "tool";
	result.active = 1;
	ASSERT_EQ(model_history_add(&db, &result, nullptr), 0);

	ASSERT_EQ(agent_history_repair_interrupted(&db, s.id), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(items, nullptr);
	struct model_history_item *tail = items;
	while (tail->next)
		tail = tail->next;
	EXPECT_STREQ(tail->kind, "tool_result");
	EXPECT_STREQ(tail->tool_call_id, "local_2");
	EXPECT_NE(std::strstr(tail->content, "interrupted"), nullptr);
	model_history_free_list(items);
}

TEST_F(SessionTest, ModelHistoryTruncatesUtf8ToolResultsAndRedactsSecrets) {
	struct session s;
	struct react_context react = {};
	std::string output =
		"Bearer secret-token api_key=another-secret ";
	int count = 0;

	for (int i = 0; i < 100; i++)
		output += "构建完成🙂";
	ASSERT_EQ(session_create(&db, "history_truncate", "test", &s), 0);
	react.history_db = &db;
	react.history_session_id = s.id;
	react.history_enabled = 1;
	react.history_tool_result_tokens = 64;
	ASSERT_EQ(agent_history_record_tool_result(&react, "tool_1", "call_1",
		"exec", output.c_str(), 0), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(items->truncated, 1);
	EXPECT_NE(std::strstr(items->content, "tool output truncated"), nullptr);
	EXPECT_EQ(std::strstr(items->content, "secret-token"), nullptr);
	EXPECT_EQ(std::strstr(items->content, "another-secret"), nullptr);
	EXPECT_NE(std::strstr(items->content, "Bearer [REDACTED]"), nullptr);
	model_history_free_list(items);
}

TEST_F(SessionTest, ToolResultTokenLimitIncludesRedactionAndOmissionMarker)
{
	auto *tokenizer = tokenizer_create(NULL, "deepseek-flash", 128000);
	ASSERT_NE(tokenizer, nullptr);
	std::string text = "HEAD ";
	for (int i = 0; i < 3000; i++)
		text += "测试🙂!@#$%^&*() short-secret ";
	text += " TAIL";
	for (auto *counter : {tokenizer, static_cast<struct tokenizer *>(nullptr)}) {
		for (int limit : {1, 2, 8, 64, 257, 4000}) {
			struct react_context ctx = {};
			char *prepared = nullptr;
			int truncated = 0;
			ctx.tokenizer = counter;
			ctx.history_tool_result_tokens = limit;
			ctx.history_secrets[0] = strdup("short-secret");
			ctx.history_secret_count = 1;
			ASSERT_EQ(agent_history_prepare_tool_content(&ctx, text.c_str(),
				&prepared, &truncated), 0);
			ASSERT_NE(prepared, nullptr);
			EXPECT_EQ(truncated, 1);
			EXPECT_LE(tokenizer_count(counter, prepared), limit);
			EXPECT_EQ(strstr(prepared, "short-secret"), nullptr);
			EXPECT_EQ(utf8valid(prepared), nullptr);
			if (limit >= 64) {
				EXPECT_NE(strstr(prepared, "HEAD"), nullptr);
				EXPECT_NE(strstr(prepared, "TAIL"), nullptr);
				EXPECT_NE(strstr(prepared, "tool output truncated"), nullptr);
			}
			free(prepared);
			free(ctx.history_secrets[0]);
		}
	}
	tokenizer_destroy(tokenizer);
}

TEST_F(SessionTest, ToolResultLimitPreservesLargeButLowTokenOutput)
{
	auto *tokenizer = tokenizer_create(NULL, "deepseek-flash", 128000);
	ASSERT_NE(tokenizer, nullptr);
	if (!tokenizer->encoder) {
		tokenizer_destroy(tokenizer);
		GTEST_SKIP() << "BPE vocabulary unavailable";
	}
	struct react_context ctx = {};
	std::string text(12000, ' ');
	char *prepared = nullptr;
	int truncated = 1;
	ctx.tokenizer = tokenizer;
	ctx.history_tool_result_tokens = 256;
	ASSERT_LE(tokenizer_count(tokenizer, text.c_str()), 256);
	ASSERT_EQ(agent_history_prepare_tool_content(&ctx, text.c_str(),
		&prepared, &truncated), 0);
	EXPECT_EQ(truncated, 0);
	EXPECT_STREQ(prepared, text.c_str());
	free(prepared);
	tokenizer_destroy(tokenizer);
}

TEST_F(SessionTest, TranscriptAndModelHistoryWritesAreIdempotent) {
	struct session s;
	struct model_history_insert item = {};
	int64_t first_id = 0;
	int64_t second_id = 0;

	ASSERT_EQ(session_create(&db, "history_idempotency", "test", &s), 0);
	ASSERT_EQ(message_add_with_turn_id(&db, s.id, "user", "first", 1,
		"turn_same"), 0);
	ASSERT_EQ(message_add_with_turn_id(&db, s.id, "user", "duplicate", 1,
		"turn_same"), 0);
	EXPECT_EQ(message_count(&db, s.id), 1);

	item.session_id = s.id;
	item.turn_id = "turn_same";
	item.kind = "user_message";
	item.role = "user";
	item.content = "first";
	item.idempotency_key = "turn:turn_same:user";
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, &first_id), 0);
	item.content = "duplicate";
	ASSERT_EQ(model_history_add(&db, &item, &second_id), 0);
	EXPECT_EQ(first_id, second_id);
	EXPECT_EQ(model_history_count(&db, s.id, 0), 1);
}

TEST_F(SessionTest, HistoryBuilderUsesProviderNeutralCallIds) {
	struct model_history_item call = {};
	struct model_history_item result = {};
	struct arena *arena = arena_create(4096);
	morph_array_t messages;

	ASSERT_NE(arena, nullptr);
	ASSERT_EQ(morph_array_init(&messages, 2,
		sizeof(struct chat_message)), 0);
	std::strcpy(call.kind, "assistant_tool_calls");
	call.payload_json = const_cast<char *>(
		"{\"reasoning_content\":\"inspect state\",\"calls\":[{"
		"\"tool_call_id\":\"local_1\","
		"\"provider_call_id\":\"provider_1\",\"name\":\"exec\","
		"\"arguments\":\"{}\"}]}");
	call.active = 1;
	call.next = &result;
	std::strcpy(result.kind, "tool_result");
	result.content = const_cast<char *>("done");
	result.tool_call_id = const_cast<char *>("local_1");
	result.provider_call_id = const_cast<char *>("provider_1");
	result.active = 1;
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &call, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 2U);
	auto *assistant = static_cast<struct chat_message *>(
		morph_array_get(&messages, 0));
	auto *tool = static_cast<struct chat_message *>(
		morph_array_get(&messages, 1));
	ASSERT_NE(assistant, nullptr);
	ASSERT_NE(tool, nullptr);
	ASSERT_EQ(assistant->tool_call_count, 1);
	EXPECT_STREQ(assistant->reasoning_content, "inspect state");
	EXPECT_STREQ(assistant->tool_calls[0].id, "local_1");
	EXPECT_STREQ(tool->tool_call_id, "local_1");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(SessionTest, HistoryBuilderMigratesLegacyBashExecCalls) {
	struct model_history_item item = {};
	struct arena *arena = arena_create(4096);
	morph_array_t messages;

	ASSERT_NE(arena, nullptr);
	ASSERT_EQ(morph_array_init(&messages, 1,
		sizeof(struct chat_message)), 0);
	std::strcpy(item.kind, "assistant_tool_calls");
	item.payload_json = const_cast<char *>(
		"{\"calls\":[{\"tool_call_id\":\"legacy_1\","
		"\"provider_call_id\":\"provider_1\","
		"\"name\":\"bash_exec\",\"arguments\":\"{"
		"\\\"command\\\":\\\"pwd\\\","
		"\\\"timeout_seconds\\\":30,"
		"\\\"cwd\\\":\\\"/tmp\\\","
		"\\\"write_paths\\\":[\\\"/tmp\\\"]}\"}]}");
	item.active = 1;
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &item, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 1U);
	auto *message = static_cast<struct chat_message *>(
		morph_array_get(&messages, 0));
	ASSERT_NE(message, nullptr);
	ASSERT_EQ(message->tool_call_count, 1);
	EXPECT_STREQ(message->tool_calls[0].name, "exec");
	EXPECT_STREQ(message->tool_calls[0].arguments, "{\"command\":\"pwd\"}");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(SessionTest, MigratesLiveLegacyBashExecCall) {
	struct tool_call call = {};

	std::strcpy(call.name, "bash_exec");
	call.input_kind = TOOL_INPUT_JSON;
	call.arguments = strdup(
		"{\"command\":\"make\",\"timeout_seconds\":60}");
	ASSERT_NE(call.arguments, nullptr);
	ASSERT_EQ(agent_history_migrate_legacy_tool_call(&call, nullptr), 0);
	EXPECT_STREQ(call.name, "exec");
	EXPECT_STREQ(call.arguments, "{\"command\":\"make\"}");
	free(call.arguments);
}

TEST_F(SessionTest, HistoryDiagnoseAndRepairNormalizesInvalidItems) {
	struct session s;
	struct model_history_insert malformed = {};
	struct model_history_insert incomplete = {};
	struct model_history_insert invalid_arguments = {};
	struct model_history_insert invalid_result = {};
	struct model_history_insert orphan = {};
	struct tokenizer *tokenizer = tokenizer_create(NULL, "test", 4096);
	struct agent_history_diagnostic diagnostic = {};
	int changed = 0;
	int count = 0;

	ASSERT_NE(tokenizer, nullptr);
	ASSERT_EQ(session_create(&db, "history_diagnose", "test", &s), 0);
	malformed.session_id = s.id;
	malformed.kind = "assistant_tool_calls";
	malformed.role = "assistant";
	malformed.payload_json = "{not-json";
	malformed.token_count = 99;
	malformed.active = 1;
	ASSERT_EQ(model_history_add(&db, &malformed, nullptr), 0);
	incomplete.session_id = s.id;
	incomplete.kind = "assistant_tool_calls";
	incomplete.role = "assistant";
	incomplete.payload_json =
		"{\"calls\":[{\"tool_call_id\":\"partial\","
		"\"provider_call_id\":\"provider\"}]}";
	incomplete.token_count = 99;
	incomplete.active = 1;
	ASSERT_EQ(model_history_add(&db, &incomplete, nullptr), 0);
	invalid_arguments.session_id = s.id;
	invalid_arguments.kind = "assistant_tool_calls";
	invalid_arguments.role = "assistant";
	invalid_arguments.payload_json =
		"{\"calls\":[{\"tool_call_id\":\"invalid_args\","
		"\"provider_call_id\":\"invalid_provider\","
		"\"name\":\"exec\",\"arguments\":\"{not-json\"}]}";
	invalid_arguments.token_count = 99;
	invalid_arguments.active = 1;
	ASSERT_EQ(model_history_add(&db, &invalid_arguments, nullptr), 0);
	invalid_result.session_id = s.id;
	invalid_result.kind = "tool_result";
	invalid_result.role = "tool";
	invalid_result.content = "invalid arguments";
	invalid_result.tool_call_id = "invalid_args";
	invalid_result.provider_call_id = "invalid_provider";
	invalid_result.token_count = 99;
	invalid_result.active = 1;
	ASSERT_EQ(model_history_add(&db, &invalid_result, nullptr), 0);
	orphan.session_id = s.id;
	orphan.kind = "tool_result";
	orphan.role = "tool";
	orphan.content = "orphan";
	orphan.tool_call_id = "missing";
	orphan.token_count = 99;
	orphan.active = 1;
	ASSERT_EQ(model_history_add(&db, &orphan, nullptr), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(agent_history_diagnose(items, tokenizer, &diagnostic), 0);
	EXPECT_EQ(diagnostic.invalid_payloads, 3);
	EXPECT_EQ(diagnostic.orphan_results, 2);
	EXPECT_EQ(diagnostic.token_mismatches, 5);
	model_history_free_list(items);
	ASSERT_EQ(agent_history_repair(&db, s.id, tokenizer, nullptr,
		&changed), 0);
	EXPECT_GE(changed, 2);
	items = model_history_list(&db, s.id, 1, &count);
	EXPECT_EQ(count, 0);
	model_history_free_list(items);
	tokenizer_destroy(tokenizer);
}

TEST_F(SessionTest, FreeformToolHistoryPreservesRawInput) {
	struct model_history_item item = {};
	morph_array_t messages = {};
	struct arena *arena = arena_create(4096);
	const char *raw =
		"*** Begin Patch\n*** Add File: a.txt\n+x\n*** End Patch";
	std::string payload =
		"{\"calls\":[{\"tool_call_id\":\"local_text\","
		"\"provider_call_id\":\"provider_text\","
		"\"name\":\"apply_patch\",\"input_kind\":\"text\","
		"\"input\":\"*** Begin Patch\\n*** Add File: a.txt\\n+x\\n"
		"*** End Patch\"}]}";

	ASSERT_NE(arena, nullptr);
	std::strcpy(item.kind, "assistant_tool_calls");
	std::strcpy(item.role, "assistant");
	item.payload_json = const_cast<char *>(payload.c_str());
	item.active = 1;
	ASSERT_EQ(morph_array_init(&messages, 2,
		sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &item, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 1U);
	auto *message = static_cast<struct chat_message *>(
		morph_array_get(&messages, 0));
	ASSERT_NE(message, nullptr);
	ASSERT_EQ(message->tool_call_count, 1);
	EXPECT_EQ(message->tool_calls[0].input_kind, TOOL_INPUT_TEXT);
	EXPECT_STREQ(message->tool_calls[0].arguments, raw);
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(SessionTest, ToolCallRedactionPreservesJsonAndSourceTemplates) {
	struct session session;
	struct react_context react = {};
	const char *source =
		"return `export default \"data:${mime};base64,${buf.toString(\"base64\")}\";`;";
	const char *binary = "data:image/png;base64,QUJDRA==";

	ASSERT_EQ(session_create(&db, "redacted_calls", "test", &session), 0);
	react.history_db = &db;
	react.history_session_id = session.id;
	react.history_enabled = 1;
	for (int i = 0; i < 2; i++) {
		struct tool_call call = {};
		cJSON *args = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "source", source);
		cJSON_AddStringToObject(args, "image", binary);
		cJSON_AddStringToObject(args, "key", "Bearer secret-value");
		char *json = cJSON_PrintUnformatted(args);
		cJSON_Delete(args);
		ASSERT_NE(json, nullptr);
		std::snprintf(call.id, sizeof(call.id), "provider_%d", i);
		std::snprintf(call.tool_call_id, sizeof(call.tool_call_id), "local_%d", i);
		std::strcpy(call.name, i == 0 ? "exec" : "apply_patch");
		call.input_kind = i == 0 ? TOOL_INPUT_JSON : TOOL_INPUT_TEXT;
		call.arguments = i == 0 ? json : const_cast<char *>(source);
		ASSERT_EQ(agent_history_record_tool_calls(&react, nullptr, nullptr,
			&call, 1), 0);
		free(json);
	}
	int count = 0;
	struct model_history_item *items = model_history_list(&db, session.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(std::strstr(items->payload_json, "QUJDRA=="), nullptr);
	EXPECT_EQ(std::strstr(items->payload_json, "secret-value"), nullptr);
	EXPECT_EQ(items->token_count, tokenizer_count(nullptr, items->payload_json));
	morph_array_t messages = {};
	struct arena *arena = arena_create(0);
	ASSERT_NE(arena, nullptr);
	ASSERT_EQ(morph_array_init(&messages, 2, sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, items, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 2U);
	auto *message = static_cast<struct chat_message *>(morph_array_get(&messages, 0));
	cJSON *args = cJSON_Parse(message->tool_calls[0].arguments);
	ASSERT_NE(args, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(args, "source")), source);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(args, "image")),
		"[binary data omitted]");
	cJSON_Delete(args);
	message = static_cast<struct chat_message *>(morph_array_get(&messages, 1));
	EXPECT_STREQ(message->tool_calls[0].arguments, source);
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	model_history_free_list(items);
}

TEST_F(SessionTest, ToolResultEnvelopeKeepsArtifactsMetaAndRemovesBinary) {
	struct session s;
	struct react_context react = {};
	struct tool_artifact_list artifacts = {};
	cJSON *meta = cJSON_CreateObject();
	int count = 0;

	ASSERT_NE(meta, nullptr);
	ASSERT_EQ(session_create(&db, "history_envelope", "test", &s), 0);
	react.history_db = &db;
	react.history_session_id = s.id;
	react.history_enabled = 1;
	react.history_tool_result_tokens = 1000;
	react.history_secrets[0] = strdup("runtime-exact-secret");
	react.history_secret_count = 1;
	artifacts.count = 1;
	artifacts.items[0].kind = TOOL_ARTIFACT_FILE;
	std::strcpy(artifacts.items[0].path, "/tmp/report.txt");
	std::strcpy(artifacts.items[0].mime, "text/plain");
	cJSON_AddStringToObject(meta, "history_content", "concise result");
	ASSERT_EQ(agent_history_record_tool_result_ex(&react, "local_1",
		"provider_1", "tool", "data:image/png;base64,QUJDRA== "
		"runtime-exact-secret", 0, &artifacts, meta), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(std::strstr(items->content, "QUJDRA=="), nullptr);
	EXPECT_EQ(std::strstr(items->content, "runtime-exact-secret"), nullptr);
	EXPECT_NE(std::strstr(items->content, "[binary data omitted]"), nullptr);
	ASSERT_NE(items->payload_json, nullptr);
	EXPECT_NE(std::strstr(items->payload_json, "/tmp/report.txt"), nullptr);
	EXPECT_NE(std::strstr(items->payload_json, "history_content"), nullptr);
	model_history_free_list(items);
	free(react.history_secrets[0]);
	cJSON_Delete(meta);
}

TEST_F(SessionTest, CompactionFailureRollsBackWithoutChangingHistory) {
	struct session s;
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_rollback", "test", &s), 0);
	item.session_id = s.id;
	item.kind = "user_message";
	item.role = "user";
	item.content = "keep me";
	item.token_count = 2;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(sqlite3_exec(db.handle,
		"CREATE TRIGGER fail_history_checkpoint BEFORE INSERT ON "
		"history_compactions BEGIN SELECT RAISE(ABORT, 'injected'); END;",
		nullptr, nullptr, nullptr), SQLITE_OK);
	EXPECT_NE(model_history_compact(&db, s.id, "turn_1", "summary", 2,
		0, 0, 2, 0, "manual", nullptr), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->content, "keep me");
	EXPECT_EQ(model_history_count(&db, s.id, 0), 1);
	EXPECT_EQ(model_history_compaction_count(&db, s.id), 0);
	model_history_free_list(items);
}

TEST_F(SessionTest, AutomaticCompactionFailurePreservesHistoryAtEveryBudget)
{
	struct session s;
	struct react_context react = {};
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_fallback", "test", &s), 0);
	item.session_id = s.id;
	item.turn_id = "turn_1";
	item.kind = "user_message";
	item.role = "user";
	item.content = "recoverable input";
	item.token_count = 60;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	react.history_db = &db;
	react.history_session_id = s.id;
	react.history_enabled = 1;
	std::strcpy(react.turn_id, "turn_2");
	react.compress.max_context_tokens = 100;
	react.compress.max_history_rounds = 1;
	react.compress.summarize_threshold_ratio = 0.5;
	react.compress.compress_target_ratio = 0.5;
	react.compress.compaction_user_message_tokens = 20;
	react.compress.compaction_summary_max_tokens = 20;
	react.compress.summarize = history_summary_failure;
	react.history_items = model_history_list(&db, s.id, 1, &count);
	ASSERT_NE(react.history_items, nullptr);
	EXPECT_EQ(agent_history_compact(&react, 0), -EIO);
	EXPECT_EQ(model_history_count(&db, s.id, 1), 1);
	EXPECT_EQ(model_history_compaction_count(&db, s.id), 0);

	ASSERT_EQ(sqlite3_exec(db.handle,
		"UPDATE model_history_items SET token_count=100 WHERE active=1",
		nullptr, nullptr, nullptr), SQLITE_OK);
	model_history_free_list(react.history_items);
	react.history_items = model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(agent_history_compact(&react, 0), -EIO);
	EXPECT_EQ(model_history_compaction_count(&db, s.id), 0);
	bool found_fallback = false;
	for (struct model_history_item *cur = react.history_items; cur;
	     cur = cur->next) {
		if (std::strcmp(cur->kind, "compaction_summary") == 0) {
			found_fallback = true;
			EXPECT_NE(std::strstr(cur->content,
				"could not be summarized"), nullptr);
		}
	}
	EXPECT_FALSE(found_fallback);
	model_history_free_list(react.history_items);
}

TEST_F(SessionTest, ManualCompactionFailureLeavesHistoryUnchanged) {
	struct session s;
	struct react_context react = {};
	struct model_history_insert item = {};
	int count = 0;

	ASSERT_EQ(session_create(&db, "history_manual_failure", "test", &s),
		0);
	item.session_id = s.id;
	item.turn_id = "turn_1";
	item.kind = "user_message";
	item.role = "user";
	item.content = "unchanged";
	item.token_count = 10;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	react.history_db = &db;
	react.history_session_id = s.id;
	react.history_enabled = 1;
	std::strcpy(react.turn_id, "turn_2");
	react.compress.max_context_tokens = 100;
	react.compress.summarize = history_summary_failure;
	react.history_items = model_history_list(&db, s.id, 1, &count);
	EXPECT_EQ(agent_history_compact(&react, 1), -EIO);
	EXPECT_EQ(model_history_count(&db, s.id, 1), 1);
	EXPECT_EQ(model_history_compaction_count(&db, s.id), 0);
	model_history_free_list(react.history_items);
}

TEST_F(SessionTest, BackgroundReceiptIsReplayableAndIdempotent) {
	struct session s;
	struct react_context react = {};
	struct arena *arena = arena_create(4096);
	morph_array_t messages;
	int count = 0;

	ASSERT_NE(arena, nullptr);
	ASSERT_EQ(session_create(&db, "history_receipt", "test", &s), 0);
	react.history_db = &db;
	react.history_session_id = s.id;
	react.history_enabled = 1;
	std::strcpy(react.turn_id, "turn_receipt");
	ASSERT_EQ(agent_history_record_receipt(&react, "build", "end",
		"archive created", "ios_release", 1, 0), 0);
	ASSERT_EQ(agent_history_record_receipt(&react, "build", "end",
		"archive created", "ios_release", 1, 0), 0);
	struct model_history_item *items =
		model_history_list(&db, s.id, 1, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->kind, "background_receipt");
	EXPECT_NE(std::strstr(items->content, "ios_release"), nullptr);
	ASSERT_EQ(morph_array_init(&messages, 1,
		sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, items, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 1U);
	auto *message = static_cast<struct chat_message *>(
		morph_array_get(&messages, 0));
	ASSERT_NE(message, nullptr);
	EXPECT_STREQ(message->role, "system");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
	model_history_free_list(items);
}

TEST_F(SessionTest, MemoryMaintenanceIsNeitherPersistedNorReplayed) {
	struct session session = {};
	struct react_context react = {};
	ASSERT_EQ(session_create(&db, "memory_receipts", "test", &session), 0);
	react.history_db = &db;
	react.history_session_id = session.id;
	react.history_enabled = 1;
	strcpy(react.turn_id, "maintenance-turn");
	for (const char *phase : {"begin", "ready", "progress", "end", "failed"}) {
		ASSERT_EQ(agent_history_record_receipt(&react, "background.progress", phase,
			"memory maintenance status", "memory_consolidation", -1, 0), 0);
	}
	EXPECT_EQ(model_history_count(&db, session.id, 0), 0);
	struct model_history_item legacy = {};
	strcpy(legacy.kind, "background_receipt");
	legacy.content = const_cast<char *>("[Background receipt] memory_consolidation: queued");
	legacy.payload_json = const_cast<char *>("{malformed legacy metadata");
	struct model_history_item metadata = {};
	strcpy(metadata.kind, "background_receipt");
	metadata.content = const_cast<char *>("Different wording for internal maintenance");
	metadata.payload_json = const_cast<char *>("{\"task\":\"memory_consolidation\"}");
	legacy.next = &metadata;
	struct model_history_item meaningful = {};
	strcpy(meaningful.kind, "background_receipt");
	meaningful.content = const_cast<char *>("archive created");
	meaningful.payload_json = const_cast<char *>("{\"task\":\"ios_release\"}");
	metadata.next = &meaningful;
	struct arena *arena = arena_create(4096);
	ASSERT_NE(arena, nullptr);
	morph_array_t messages;
	struct chat_message message = {};
	ASSERT_EQ(morph_array_init(&messages, 4, sizeof(message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &legacy, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 1U);
	auto *actual = static_cast<struct chat_message *>(morph_array_get(&messages, 0));
	EXPECT_STREQ(actual->content, "archive created");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(SessionTest, HistoryReadFailureIsDistinctFromEmptyHistory) {
	int count = 0;
	ASSERT_EQ(sqlite3_exec(db.handle, "DROP TABLE model_history_items",
		nullptr, nullptr, nullptr), SQLITE_OK);
	EXPECT_EQ(model_history_list(&db, 1, 1, &count), nullptr);
	EXPECT_EQ(count, -1);
}

TEST_F(SessionTest, CompactionDropsWholeToolGroupWhenItExceedsBudget) {
	struct session session;
	struct model_history_insert item = {};
	ASSERT_EQ(session_create(&db, "bounded_tool_group", "test", &session), 0);
	item.session_id = session.id;
	item.kind = "assistant_tool_calls";
	item.payload_json = "{\"calls\":[]}";
	item.token_count = 10;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "tool_result";
	item.payload_json = nullptr;
	item.content = "first result";
	item.token_count = 5;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.content = "second result";
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(model_history_compact(&db, session.id, "turn", "checkpoint",
		2, 0, 15, 20, 0, "test", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, session.id, 1, &count);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(count, 1);
	EXPECT_STREQ(items->kind, "compaction_summary");
	model_history_free_list(items);
}

TEST_F(SessionTest, RepeatedManualCompactionCreatesFreshSummary) {
	struct session session;
	ASSERT_EQ(session_create(&db, "repeat_compaction", "test", &session), 0);
	ASSERT_EQ(model_history_compact(&db, session.id, "turn", "first", 2,
		0, 0, 10, 0, "manual", nullptr), 0);
	ASSERT_EQ(model_history_compact(&db, session.id, "turn", "second", 2,
		0, 0, 10, 0, "manual", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, session.id, 1, &count);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(count, 1);
	EXPECT_STREQ(items->content, "second");
	model_history_free_list(items);
}

TEST_F(SessionTest, CompactionCommitFailureClosesTransactionAndRestoresHistory) {
	struct session session;
	struct model_history_insert item = {};
	ASSERT_EQ(session_create(&db, "deferred_commit_failure", "test", &session), 0);
	item.session_id = session.id;
	item.kind = "user_message";
	item.content = "keep original history";
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(sqlite3_exec(db.handle,
		"CREATE TABLE commit_parent(id INTEGER PRIMARY KEY);"
		"CREATE TABLE commit_child(parent_id INTEGER REFERENCES commit_parent(id) "
		"DEFERRABLE INITIALLY DEFERRED);"
		"CREATE TRIGGER fail_commit AFTER INSERT ON history_compactions BEGIN "
		"INSERT INTO commit_child VALUES(123); END;",
		nullptr, nullptr, nullptr), SQLITE_OK);
	EXPECT_NE(model_history_compact(&db, session.id, "turn", "checkpoint", 2,
		0, 0, 10, 0, "manual", nullptr), 0);
	EXPECT_TRUE(sqlite3_get_autocommit(db.handle));
	EXPECT_EQ(model_history_count(&db, session.id, 0), 1);
	EXPECT_EQ(model_history_count(&db, session.id, 1), 1);
	EXPECT_EQ(model_history_compaction_count(&db, session.id), 0);
}

TEST_F(SessionTest, UnusedUserRetentionBudgetKeepsRecentToolExchange)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "shared_compaction_budget", "test", &session), 0);
	struct model_history_insert item = {};
	item.session_id = session.id;
	item.turn_id = "turn_active";
	item.active = 1;
	item.kind = "user_message";
	item.role = "user";
	item.content = "request";
	item.token_count = 2;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.content = "call";
	item.token_count = 8;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	item.kind = "tool_result";
	item.role = "tool";
	item.content = "result";
	item.tool_call_id = "call_1";
	item.token_count = 8;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	ASSERT_EQ(model_history_compact(&db, session.id, "turn_active", "summary", 2,
		20, 0, 18, 0, "test", nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, session.id, 1, &count);
	EXPECT_EQ(count, 4);
	int total = 0;
	for (auto *p = items; p; p = p->next)
		total += p->token_count;
	EXPECT_LE(total, 22);
	model_history_free_list(items);
}

TEST_F(SessionTest, ToolCallHistoryCountsAssistantTextAsWellAsArguments)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "tool_preamble_tokens", "test", &session), 0);
	struct react_context ctx = {};
	ctx.history_enabled = 1;
	ctx.history_db = &db;
	ctx.history_session_id = session.id;
	std::string text(4000, '!');
	struct tool_call call = {};
	std::strcpy(call.id, "provider_1");
	std::strcpy(call.tool_call_id, "local_1");
	std::strcpy(call.name, "exec");
	call.arguments = const_cast<char *>("{}");
	ASSERT_EQ(agent_history_record_tool_calls(&ctx, text.c_str(), nullptr,
		&call, 1), 0);
	int count = 0;
	auto *items = model_history_list(&db, session.id, 1, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_GE(items->token_count, tokenizer_count(nullptr, text.c_str()));
	struct agent_history_diagnostic diagnostic = {};
	EXPECT_EQ(agent_history_diagnose(items, nullptr, &diagnostic), 0);
	EXPECT_EQ(diagnostic.token_mismatches, 0);
	model_history_free_list(items);
}

TEST_F(SessionTest, ToolResultRepairsInvalidUtf8AndPreservesUsefulEnds)
{
	struct react_context ctx = {};
	ctx.history_tool_result_tokens = 128;
	std::string text = "HEAD ";
	for (int i = 0; i < 1500; i++)
		text += "测试🙂 data ";
	text += "\xe4\xb8oops";
	for (int i = 0; i < 1500; i++)
		text += "数据🙂 tail ";
	text += " TAIL";
	char *prepared = nullptr;
	ASSERT_EQ(agent_history_prepare_tool_content(&ctx, text.c_str(),
		&prepared, nullptr), 0);
	ASSERT_NE(prepared, nullptr);
	EXPECT_EQ(utf8valid(prepared), nullptr);
	EXPECT_LE(tokenizer_count(nullptr, prepared), 128);
	EXPECT_NE(strstr(prepared, "HEAD"), nullptr);
	EXPECT_NE(strstr(prepared, "TAIL"), nullptr);
	free(prepared);
}
