#include <gtest/gtest.h>
#include "util/cleanup.h"

#include "db/database.h"
#include "agent/memory.h"
#include "persistence/memory_snapshot.h"
#include "persistence/memory_job_store.h"
#include "persistence/memory_repository.h"
#include "persistence/memory_search.h"
#include "persistence/memory_summary_store.h"
#include "agent/memory_internal.h"
#include "util/json.h"
#include "cJSON.h"
#include "session.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <chrono>
#include "models/llm.h"
#include "util/utf8.h"

class MemoryTest : public ::testing::Test
{
      protected:
	struct db db;
	char db_path[256];
	struct memory_options opts;

	void SetUp() override
	{
		std::snprintf(db_path, sizeof(db_path), "/tmp/ma_test_memory_%d.db", getpid());
		std::remove(db_path);
		std::memset(&db, 0, sizeof(db));
		ASSERT_EQ(db_open(NULL, &db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		std::memset(&opts, 0, sizeof(opts));
		opts.worker = memory_worker_create(NULL);
		ASSERT_NE(opts.worker, nullptr);
		opts.enabled = 1;
		opts.capture_input_enabled = 1;
		opts.save_tasks_enabled = 1;
		opts.max_facts = 6;
		opts.max_past_tasks = 4;
		opts.max_tips = 4;
		opts.max_reply_chars = 4096;
	}

	void TearDown() override
	{
		memory_worker_destroy(opts.worker);
		db_close(&db);
		std::remove(db_path);
	}
};

static int explicit_preference(struct db *database, int64_t session_id, const char *value,
			       const char *source, const char *event)
{
	int rc = memory_store_prepare_session(database, session_id);
	char token[64] = {};

	if (rc != 0)
		return rc;
	rc = memory_store_event_token(database, session_id, event, token, sizeof(token));
	if (rc < 0)
		return rc;
	return preference_set(database, session_id, "personal", "response.language", value, source,
			      rc > 0 ? token : event);
}

static int save_accepted_turn(struct db *database, int64_t session_id, const char *input,
			      const char *answer, const struct react_step *steps, int success,
			      const struct memory_options *options)
{
	int rc = memory_accept_input(database, session_id, input, nullptr, options);
	if (rc != 0)
		return rc;
	return memory_save_turn(database, session_id, input, answer, steps, success, options);
}

TEST_F(MemoryTest, PreferencesHaveOneAuthorityAndNoDuplicateRules)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "memory_session", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id,
				      "Please always reply in Chinese and be concise.", "input1",
				      &opts),
		  0);
	char *initial MORPH_CLEANUP(morph_charp_cleanup) =
		preference_render_copy(&db, session.id, 0);
	EXPECT_STREQ(initial, "");
	ASSERT_EQ(preference_set(&db, session.id, "personal", "language", "Chinese", "explicit",
				 "language"),
		  0);
	ASSERT_EQ(preference_set(&db, session.id, "personal", "response.detail", "concise",
				 "explicit", "detail"),
		  0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "always", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_NE(strstr(context, "response.detail: concise"), nullptr);
	EXPECT_EQ(strstr(context, "Unapplied rules"), nullptr);
}

TEST_F(MemoryTest, UpdatedFactSupersedesOlderValue)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "memory_temporal", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "English", "以后都用英文回答", "input1"), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文回答", "input2"), 0);
	char *effective = preference_render_copy(&db, session.id, 0);
	char *history = preference_render_copy(&db, session.id, 1);
	ASSERT_NE(effective, nullptr);
	ASSERT_NE(history, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	EXPECT_EQ(strstr(effective, "English"), nullptr);
	EXPECT_NE(strstr(history, "English"), nullptr);
	EXPECT_NE(strstr(history, "Chinese"), nullptr);
	free(effective);
	free(history);
}

TEST_F(MemoryTest, DirectoryProvidesRoutesAndChangesAreReadOnDemand)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "directory", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "London", "source",
					   "context", .5),
		  0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "Tokyo", "source",
					   "context", .5),
		  0);
	ASSERT_EQ(memory_save_turn(&db, session.id, "Check project", "Verified file_read output",
				   nullptr, 1, &opts),
		  0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "上次是怎么做的", &opts, &context), 0);
	EXPECT_NE(strstr(context, "Memory directory"), nullptr);
	EXPECT_NE(strstr(context, "past_tasks#"), nullptr);
	EXPECT_EQ(strstr(context, "Recent changes"), nullptr);
	struct memory_query_options query = {};
	query.type = MEMORY_QUERY_CHANGES;
	char *history MORPH_CLEANUP(morph_charp_cleanup) =
		memory_query_text_create(&db, session.id, &query);
	ASSERT_NE(history, nullptr);
	EXPECT_NE(strstr(history, "London -> Tokyo"), nullptr);
}

TEST_F(MemoryTest, RenderSessionShowsStoredSections)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_render", "gpt-4o", &s), 0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "Please always reply in Chinese and be concise.",
				     "好的，我之后会用中文简洁回答。", &action, 1, &opts),
		  0);

	ASSERT_EQ(explicit_preference(&db, s.id, "Chinese", "explicit", "pref"), 0);
	ASSERT_EQ(preference_set(&db, s.id, "personal", "response.detail", "concise", "explicit",
				 "detail"),
		  0);
	char *rendered = memory_session_text_create(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_NE(std::strstr(rendered, "Effective preferences"), nullptr);
	EXPECT_NE(std::strstr(rendered, "response.language"), nullptr);
	EXPECT_NE(std::strstr(rendered, "response.detail"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Past tasks"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearFactsKeepsPastTasks)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_clear_facts", "gpt-4o", &s), 0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "Please reply in English.",
				     "Sure, I will reply in English.", &action, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_FACTS), 0);

	char *rendered = memory_session_text_create(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_EQ(std::strstr(rendered, "Current facts"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Past tasks"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearAllRemovesEverything)
{
	struct session s;

	ASSERT_EQ(session_create(&db, "memory_clear_all", "gpt-4o", &s), 0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "这个会话用中文回答，默认简洁。",
				     "好的，我之后会用中文简洁回答。", nullptr, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_ALL), 0);

	char *rendered = memory_session_text_create(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_STREQ(rendered, "No long-term memory stored for this session.");
	free(rendered);
}

static int memory_count_fact(struct db *db, int64_t session_id, const char *key_name)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql = "SELECT COUNT(*) FROM memory_facts "
			  "WHERE session_id=? AND key_name=? AND is_current=1";
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

TEST_F(MemoryTest, InputPreparationDoesNotInferFactsOrPreferencesInAnyLanguage)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "model-input", "mock", &session), 0);
	for (const char *input :
	     {"I am in Tokyo.", "我想知道天气如何。", "我叫了一辆出租车", "以后默认用中文回答",
	      "À l’avenir, réponds en français.", "今後は日本語で回答してください。"}) {
		ASSERT_EQ(memory_accept_input(&db, session.id, input, input, &opts), 0);
		EXPECT_EQ(memory_count_fact(&db, session.id, "user_name"), 0);
		EXPECT_EQ(memory_count_fact(&db, session.id, "location"), 0);
	}
	char *preferences MORPH_CLEANUP(morph_charp_cleanup) =
		preference_render_copy(&db, session.id, 0);
	EXPECT_STREQ(preferences, "");
}

TEST_F(MemoryTest, EmptyMemoryProvidesToolGuidance)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "empty-directory", "mock", &session), 0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "anything", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "memory_update"), nullptr);
	EXPECT_NE(strstr(context, "any language"), nullptr);
}

TEST_F(MemoryTest, LanguageMentionsComplaintsAndNegationsDoNotChangePreference)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "language_guard", "mock", &s), 0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "你现在回答为啥过程中都是英文？", "我会检查",
				     nullptr, 1, &opts),
		  0);
	sqlite3_stmt *check = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
				     "SELECT COUNT(*) FROM memory_facts WHERE session_id=? "
				     "AND key_name='preferred_language' AND is_current=1",
				     -1, &check, nullptr),
		  SQLITE_OK);
	sqlite3_bind_int64(check, 1, s.id);
	ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(check, 0), 0);
	sqlite3_finalize(check);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "后续回答我的语言改成中文", "好的", nullptr, 1,
				     &opts),
		  0);
	ASSERT_EQ(explicit_preference(&db, s.id, "Chinese", "explicit request", "language"), 0);
	const char *inputs[] = {
		"你现在回答为啥过程中都是英文？",  "不要用英文回答我。",
		"Please do not reply in English.", "Why do you reply in English?",
		"请把这个词翻译成英文。",	   "今天学习中文和英文。",
		"请解释“用英文回答”这句话。",
	};
	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		ASSERT_EQ(save_accepted_turn(&db, s.id, input, "好的", nullptr, 1, &opts), 0);
		char *effective = preference_render_copy(&db, s.id, 0);
		ASSERT_NE(effective, nullptr);
		EXPECT_NE(strstr(effective, "Chinese"), nullptr);
		EXPECT_EQ(strstr(effective, "English"), nullptr);
		free(effective);
	}
}

TEST_F(MemoryTest, ExplicitLanguageSwitchReplacesConflictingStandingRule)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "language_switch", "mock", &s), 0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "Please always reply in English.", "OK", nullptr, 1,
				     &opts),
		  0);
	ASSERT_EQ(save_accepted_turn(&db, s.id, "以后请用中文回答，不要用英文。", "好的", nullptr,
				     1, &opts),
		  0);
	ASSERT_EQ(explicit_preference(&db, s.id, "Chinese", "explicit correction", "new"), 0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, s.id, "How should you always respond?", &opts,
					   &context),
		  0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "Respond in English"), nullptr);
	morph_charp_cleanup(&context);
}

static std::string effective_preferences(struct db *database, int64_t session_id)
{
	char *text = preference_render_copy(database, session_id, 0);
	std::string result = text ? text : "<error>";
	free(text);
	return result;
}

TEST_F(MemoryTest, PersonalPreferencesCrossSessionsButNeverUsers)
{
	struct session first, second, stranger;
	ASSERT_EQ(session_create(&db, "first", "mock", &first), 0);
	ASSERT_EQ(session_create(&db, "second", "mock", &second), 0);
	ASSERT_EQ(session_create(&db, "stranger", "mock", &stranger), 0);
	ASSERT_EQ(preference_bind(&db, first.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, second.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, stranger.id, "bob", "/project"), 0);
	ASSERT_EQ(explicit_preference(&db, first.id, "Chinese", "以后都用中文", "first"), 0);
	EXPECT_NE(effective_preferences(&db, second.id).find("Chinese"), std::string::npos);
	EXPECT_EQ(effective_preferences(&db, stranger.id), "");
	EXPECT_EQ(preference_bind(&db, first.id, "bob", "/project"), -EPERM);
	ASSERT_EQ(explicit_preference(&db, second.id, "English", "以后都用英文", "second"), 0);
	EXPECT_EQ(effective_preferences(&db, first.id).find("Chinese"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, first.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, ScopePrecedenceAndUnsetRestoreInheritance)
{
	struct session first, same_project, other_project;
	ASSERT_EQ(session_create(&db, "first", "mock", &first), 0);
	ASSERT_EQ(session_create(&db, "same", "mock", &same_project), 0);
	ASSERT_EQ(session_create(&db, "other", "mock", &other_project), 0);
	ASSERT_EQ(preference_bind(&db, first.id, "alice", "/one"), 0);
	ASSERT_EQ(preference_bind(&db, same_project.id, "alice", "/one"), 0);
	ASSERT_EQ(preference_bind(&db, other_project.id, "alice", "/two"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "personal", "language", "Chinese", "a", "1"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "project", "language", "English", "b", "2"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "session", "language", "Japanese", "c", "3"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("Japanese"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, same_project.id).find("English"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, other_project.id).find("Chinese"), std::string::npos);
	ASSERT_EQ(preference_set(&db, first.id, "session", "language", nullptr, "unset", "4"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("English"), std::string::npos);
	ASSERT_EQ(preference_set(&db, first.id, "project", "language", nullptr, "unset", "5"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("Chinese"), std::string::npos);
}

TEST_F(MemoryTest, TemporaryQuestionsAndQuotesNeverOverwriteDurablePreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "temporary", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后默认用中文，回答简洁", "1", &opts), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "explicit", "preference"), 0);
	std::string before = effective_preferences(&db, session.id);
	const char *inputs[] = {
		"这次用英文详细回答",
		"这封邮件用英文",
		"Please reply in English.",
		"以后用英文回答吗？",
		"我希望了解为什么回答总是详细的？",
		"请解释：‘以后用英文’是什么意思？",
		"Translate always use English into Chinese.",
		"以后不要用英文",
		"Why do you always use English?",
	};
	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		ASSERT_EQ(memory_accept_input(&db, session.id, input, input, &opts), 0);
		EXPECT_EQ(effective_preferences(&db, session.id), before);
	}
}

TEST_F(MemoryTest, ReplayCannotResurrectDeletedPreferenceOrOverrideNewValue)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "replay", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "English", "以后都用英文", "old"), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "new"), 0);
	std::string current = effective_preferences(&db, session.id);
	ASSERT_EQ(explicit_preference(&db, session.id, "English", "以后都用英文", "old"), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), current);
	ASSERT_EQ(preference_set(&db, session.id, "personal", "language", nullptr, "forget",
				 "delete"),
		  0);
	ASSERT_EQ(explicit_preference(&db, session.id, "English", "以后都用英文", "old"), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
	ASSERT_EQ(memory_save_turn(&db, session.id, "以后都用英文", "OK", nullptr, 1, &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
}

TEST_F(MemoryTest, ClearedMemoryRejectsOlderBackgroundMemoryVersion)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "generation", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	struct memory_options old_job = opts;
	old_job.capture_input_enabled = 0;
	old_job.check_version = 1;
	old_job.memory_version = 0;
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	EXPECT_EQ(memory_save_turn(&db, session.id, "old work", "done", nullptr, 1, &old_job),
		  -ESTALE);
	char *render = memory_session_text_create(&db, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_STREQ(render, "No long-term memory stored for this session.");
	free(render);
}

TEST_F(MemoryTest, OldProfilesAndRulesCannotCompeteWithEffectivePreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "legacy", "mock", &session), 0);
	ASSERT_EQ(
		db_exec(&db,
			"INSERT INTO memory_profiles VALUES(1,'Respond in English',1);"
			"INSERT INTO "
			"memory_procedures(session_id,rule_text,trigger_text,updated_at) "
			"VALUES(1,'Always speak English','complaint',1);"
			"INSERT INTO "
			"memory_facts(session_id,key_name,value_text,source_text,category,"
			"valid_from,created_at,updated_at) VALUES(1,'preferred_language','English',"
			"'你现在回答为啥过程中都是英文？','preference',1,1,1);"),
		0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "new"), 0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "always reply", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "English"), nullptr);
	EXPECT_EQ(strstr(context, "preferred_language"), nullptr);
	morph_charp_cleanup(&context);
	EXPECT_EQ(effective_preferences(&db, session.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, LegacyMigrationIsIdempotentAndCannotOverrideNewExplicitSetting)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "legacy_good", "mock", &session), 0);
	ASSERT_EQ(
		db_exec(&db,
			"INSERT INTO "
			"memory_facts(session_id,key_name,value_text,source_text,category,"
			"valid_from,created_at,updated_at) VALUES(1,'preferred_language','English',"
			"'以后都用英文','preference',1,1,1);"),
		0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "first", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "second"), 0);
	EXPECT_NE(effective_preferences(&db, session.id).find("Chinese [personal"),
		  std::string::npos);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "third", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, DurableJobsRecoverAndDoNotDuplicatePastTasks)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "recovery", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	ASSERT_EQ(db_exec(&db, "INSERT INTO memory_jobs(session_id,generation,payload) VALUES(1,0,"
			       "'{\"input\":\"old task\",\"answer\":\"done\",\"success\":true,"
			       "\"episodes\":true,\"llm\":false,\"tools\":[\"file_read\"]}');"),
		  0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	char *jobs = memory_background_text_create(&db, session.id);
	ASSERT_NE(jobs, nullptr);
	EXPECT_NE(strstr(jobs, "completed"), nullptr);
	free(jobs);
	/* Simulate a crash after episode commit but before the completion marker. */
	ASSERT_EQ(db_exec(&db, "UPDATE memory_jobs SET state='queued'"), 0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM memory_episodes", -1, &stmt,
				     nullptr),
		  SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
	sqlite3_finalize(stmt);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	char *render = memory_session_text_create(&db, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_STREQ(render, "No long-term memory stored for this session.");
	free(render);
}

struct DelayedMemoryModel {
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool released = false;
};

static int delayed_memory_chat(struct model *self, const char *, const char **, int,
			       const struct model_chat_options *, sse_callback cb, void *data)
{
	auto *state = static_cast<DelayedMemoryModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->entered = true;
		state->cv.notify_all();
		state->cv.wait_for(lock, std::chrono::seconds(5), [&] { return state->released; });
	}
	return cb("{\"facts\":[{\"key\":\"language\",\"value\":\"English\",\"category\":"
		  "\"preference\"}],"
		  "\"rules\":[{\"rule_text\":\"Always speak English\"}]}",
		  data);
}

TEST_F(MemoryTest, InFlightExtractionCannotOverrideOrResurrectPreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "async_old", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "English", "以后都用英文", "old"), 0);
	DelayedMemoryModel state;
	struct model model = {};
	model.handle = &state;
	model.chat = delayed_memory_chat;
	std::strcpy(model.api_key, "mock");
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(memory_save_turn_async(&db, session.id, "以后都用英文", "OK", nullptr, 1, &opts),
		  0);
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		EXPECT_TRUE(state.cv.wait_for(lock, std::chrono::seconds(5),
					      [&] { return state.entered; }));
	}
	EXPECT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "new"), 0);
	EXPECT_EQ(preference_set(&db, session.id, "personal", "language", nullptr, "forget",
				 "delete"),
		  0);
	EXPECT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.released = true;
		state.cv.notify_all();
	}
	memory_async_shutdown(opts.worker);
	opts.llm = nullptr;
	EXPECT_EQ(effective_preferences(&db, session.id), "");
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "上次 always", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(strstr(context, "English"), nullptr);
	morph_charp_cleanup(&context);
}

TEST_F(MemoryTest, IndependentWritersResolveOneValuePerCanonicalKey)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "writers", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	std::atomic<int> failures{0};
	auto write = [&](const char *key, const char *value) {
		struct db connection = {};
		if (db_open(NULL, &connection, db_path) != 0) {
			failures++;
			return;
		}
		for (int i = 0; i < 20; i++) {
			if (preference_set(&connection, session.id, "personal", key, value,
					   "writer", nullptr))
				failures++;
		}
		db_close(&connection);
	};
	std::thread first(write, "preferred_language", "Chinese");
	std::thread second(write, "response.language", "English");
	first.join();
	second.join();
	EXPECT_EQ(failures, 0);
	std::string effective = effective_preferences(&db, session.id);
	EXPECT_NE(effective.find("response.language:"), std::string::npos);
	EXPECT_EQ(effective.find("response.language:", effective.find("response.language:") + 1),
		  std::string::npos);
}

TEST_F(MemoryTest, SourceMessageOrderWinsEvenWhenOlderInputCommitsLater)
{
	struct session older, newer;
	ASSERT_EQ(session_create(&db, "older", "mock", &older), 0);
	ASSERT_EQ(session_create(&db, "newer", "mock", &newer), 0);
	ASSERT_EQ(preference_bind(&db, older.id, "local", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, newer.id, "local", "/project"), 0);
	struct model_history_insert input = {};
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.session_id = older.id;
	input.turn_id = "older-input";
	input.content = "以后都用英文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	input.session_id = newer.id;
	input.turn_id = "newer-input";
	input.content = "以后都用中文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	ASSERT_EQ(explicit_preference(&db, newer.id, "Chinese", input.content, input.turn_id), 0);
	ASSERT_EQ(explicit_preference(&db, older.id, "English", "以后都用英文", "older-input"), 0);
	EXPECT_NE(effective_preferences(&db, older.id).find("Chinese"), std::string::npos);
	EXPECT_EQ(effective_preferences(&db, newer.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, PreferenceReadFailureIsNotTreatedAsEmptyMemory)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "read-failure", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "input"), 0);
	ASSERT_EQ(db_exec(&db, "DROP TABLE memory_preferences"), 0);
	char *context = nullptr;
	EXPECT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context),
		  MORPH_ERR_DB);
	EXPECT_EQ(context, nullptr);
}

TEST_F(MemoryTest, EssentialLanguageSurvivesSmallGeneralMemoryBudget)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "budget", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "以后都用中文", "input"), 0);
	opts.max_reply_chars = 32;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_LE(strlen(context), 512u);
	morph_charp_cleanup(&context);
}

TEST_F(MemoryTest, UnicodeDirectoryPreviewsStayBoundedAndFullFactsRemainReadable)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "unicode-directory", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	std::string value;
	for (int i = 0; i < 150; i++)
		value += "界";
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "complete", value.c_str(), "source",
					   "context", .8),
		  0);
	opts.max_reply_chars = 1400;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "facts#"), nullptr);
	EXPECT_EQ(strstr(context, value.c_str()), nullptr);
	EXPECT_LE(utf8len(context), 1400u);
	EXPECT_EQ(utf8valid(context), nullptr);
	struct memory_query_options query = {};
	query.type = MEMORY_QUERY_FACTS;
	query.record_id = 1;
	char *full MORPH_CLEANUP(morph_charp_cleanup) =
		memory_query_text_create(&db, session.id, &query);
	ASSERT_NE(full, nullptr);
	EXPECT_NE(strstr(full, value.c_str()), nullptr);
}

TEST_F(MemoryTest, LargeOptionalPreferenceCannotDisplaceEssentialLanguage)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "essential-first", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "Always reply in Chinese.",
				      "input"),
		  0);
	std::string value(900, 'x');
	ASSERT_EQ(preference_set(&db, session.id, "personal", "custom.large", value.c_str(),
				 "Explicit request", "second"), 0);
	opts.max_reply_chars = 32;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "custom.large"), nullptr);
	EXPECT_NE(strstr(context, "omitted records"), nullptr);
	EXPECT_LE(utf8len(context), 512u);
}

TEST_F(MemoryTest, ContextOmitsWholeOversizedRecordAndProvidesRecoverableNotice)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "complete-budget", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	std::string value = "LONG_VALUE_START_" + std::string(600, 'x') + "_LONG_VALUE_END";
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "large", value.c_str(), "source",
					  "context", 0.8), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "small", "Complete small value",
	"source",
					  "context", 0.8), 0);
	opts.max_reply_chars = 1400;
	opts.max_facts = 1;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(strstr(context, "LONG_VALUE_START_"), nullptr);
	EXPECT_NE(strstr(context, "Complete small value"), nullptr);
	EXPECT_NE(strstr(context, "omitted records"), nullptr);
	EXPECT_NE(strstr(context, "scope=session"), nullptr);
	EXPECT_LE(utf8len(context), 1400u);
	struct memory_query_options query = {};
	query.type = MEMORY_QUERY_FACTS;
	char *recovered MORPH_CLEANUP(morph_charp_cleanup) =
		memory_query_text_create(&db, session.id, &query);
	ASSERT_NE(recovered, nullptr);
	EXPECT_NE(strstr(recovered, value.c_str()), nullptr);
}

TEST_F(MemoryTest, FactCountLimitReportsOmissionsWithoutDeletingStoredValues)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "count-budget", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	for (int i = 0; i < 3; i++) {
		std::string key = "fact-" + std::to_string(i);
		ASSERT_EQ(memory_store_upsert_fact(&db, session.id, key.c_str(), "Complete value",
						  "source", "context", 0.8), 0);
	}
	opts.max_facts = 1;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "omitted records"), nullptr);
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {};
	struct memory_snapshot_options read = {};
	read.facts_limit = -1;
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
	EXPECT_EQ(snapshot.facts.nelts, 3u);
}

TEST_F(MemoryTest, StoredFactsPreserveLongUnicodeValues)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "long-auto-fact", "mock", &session), 0);
	std::string goal;
	for (int i = 0; i < 180; i++)
		goal += "界";
	goal += "END_MARKER";
	std::string input = "My goal is " + goal + ".";
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "goal", goal.c_str(), input.c_str(),
					   "goal", .7),
		  0);
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {};
	struct memory_snapshot_options read = {};
	read.facts_limit = -1;
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
	ASSERT_EQ(snapshot.facts.nelts, 1u);
	auto *fact = static_cast<struct memory_fact_record *>(morph_array_borrow(&snapshot.facts,
	0));
	EXPECT_STREQ(fact->key, "goal");
	EXPECT_STREQ(fact->value, goal.c_str());
	EXPECT_EQ(utf8valid(fact->value), nullptr);
}

TEST_F(MemoryTest, IndependentWorkersDrainOnlyTheirOwnJobs)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "isolated_workers", "mock", &session), 0);
	DelayedMemoryModel state;
	struct model model{};
	model.handle = &state;
	model.chat = delayed_memory_chat;
	std::strcpy(model.api_key, "mock");
	std::unique_ptr<struct memory_worker, decltype(&memory_worker_destroy)> other(
		memory_worker_create(NULL), memory_worker_destroy);
	ASSERT_NE(other, nullptr);
	struct memory_options second = opts;
	second.worker = other.get();
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(memory_save_turn_async(&db, session.id, "first task", "first answer", nullptr, 1,
					 &opts),
		  0);
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		EXPECT_TRUE(state.cv.wait_for(lock, std::chrono::seconds(5),
					      [&] { return state.entered; }));
	}
	EXPECT_NE(memory_async_pending(opts.worker), 0);
	EXPECT_EQ(memory_async_pending(other.get()), 0);
	EXPECT_EQ(memory_save_turn_async(&db, session.id, "second task", "second answer", nullptr,
					 1, &second),
		  0);
	memory_async_shutdown(other.get());
	EXPECT_EQ(memory_async_pending(other.get()), 0);
	EXPECT_NE(memory_async_pending(opts.worker), 0);
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.released = true;
		state.cv.notify_all();
	}
	memory_async_shutdown(opts.worker);
	opts.llm = nullptr;
	EXPECT_EQ(memory_async_pending(opts.worker), 0);
	char *jobs = memory_background_text_create(&db, session.id);
	ASSERT_NE(jobs, nullptr);
	EXPECT_EQ(strstr(jobs, "queued"), nullptr);
	EXPECT_EQ(strstr(jobs, "running"), nullptr);
	free(jobs);
}

TEST_F(MemoryTest, SnapshotOwnsFactValuesAcrossLaterDatabaseWrites)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "memory_snapshot", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "London", "first",
					   "context", .5),
		  0);
	struct memory_snapshot_options read{};
	read.facts_limit = -1;
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
	ASSERT_GT(snapshot.facts.nelts, 0u);
	auto *fact =
		static_cast<struct memory_fact_record *>(morph_array_borrow(&snapshot.facts, 0));
	ASSERT_STREQ(fact->key, "location");
	EXPECT_STREQ(fact->value, "London");
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "Paris", "second",
					   "context", .5),
		  0);
	EXPECT_STREQ(fact->value, "London");
	char *current MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "current location", &opts, &current),
		  0);
	ASSERT_NE(current, nullptr);
	EXPECT_NE(strstr(current, "location: Paris"), nullptr);
	morph_charp_cleanup(&current);
	memory_snapshot_cleanup(&snapshot);
	EXPECT_EQ(snapshot.facts.elts, nullptr);
}

TEST_F(MemoryTest, SnapshotReadFailureReleasesPreviouslyCopiedSections)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "memory_snapshot_failure", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "London", "first",
					   "context", .5),
		  0);
	ASSERT_EQ(db_exec(&db, "DROP TABLE memory_episodes"), 0);
	struct memory_snapshot_options read{};
	read.facts_limit = -1;
	read.tasks_limit = -1;
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {0};
	EXPECT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), MORPH_ERR_DB);
	EXPECT_EQ(snapshot.facts.elts, nullptr);
	EXPECT_EQ(snapshot.facts.elts, nullptr);
	EXPECT_EQ(snapshot.tasks.elts, nullptr);
	memory_snapshot_cleanup(&snapshot);
}

TEST_F(MemoryTest, JobSnapshotsOwnPayloadAndStateChangesRequireTheExpectedState)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "job-store", "mock", &session), 0);
	int64_t id = 0;
	ASSERT_EQ(memory_job_store_add(&db, session.id, 7, "original payload", &id), 0);
	ASSERT_GT(id, 0);
	struct memory_job_records records MORPH_CLEANUP(memory_job_records_cleanup) = {0};
	ASSERT_EQ(memory_job_store_recoverable(&db, &records), 0);
	ASSERT_EQ(records.rows.nelts, 1u);
	auto *record =
		static_cast<struct memory_job_record *>(morph_array_borrow(&records.rows, 0));
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(record->id, id);
	EXPECT_EQ(record->memory_version, 7);
	ASSERT_EQ(db_exec(&db, "UPDATE memory_jobs SET payload='replacement'"), 0);
	EXPECT_STREQ(record->payload_json, "original payload");
	EXPECT_STREQ(record->state, "queued");
	EXPECT_EQ(memory_job_store_claim(&db, id, getpid()), 1);
	EXPECT_EQ(memory_job_store_claim(&db, id, getpid()), 0);
	EXPECT_EQ(memory_job_store_complete(&db, id, -ESTALE), 0);
	EXPECT_EQ(memory_job_store_requeue(&db, id, "running"), 0);
	memory_job_records_cleanup(&records);
	ASSERT_EQ(memory_job_store_recent(&db, session.id, 20, &records), 0);
	ASSERT_EQ(records.rows.nelts, 1u);
	record = static_cast<struct memory_job_record *>(morph_array_borrow(&records.rows, 0));
	EXPECT_STREQ(record->state, "cancelled");
	EXPECT_EQ(record->error_code, -ESTALE);
	EXPECT_EQ(record->attempts, 1);
	EXPECT_EQ(record->payload_json, nullptr);
	memory_job_records_cleanup(&records);
	ASSERT_EQ(memory_job_store_recent(&db, 0, 20, &records), 0);
	EXPECT_EQ(records.rows.nelts, 0u);
	memory_job_records_cleanup(&records);
}

TEST_F(MemoryTest, MemoryVersionWriteRollbackKeepsFactsAndTheVersionConsistent)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "generation-store", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	int64_t memory_version = 0;
	ASSERT_EQ(memory_store_read_version(&db, session.id, &memory_version), 0);
	ASSERT_EQ(memory_store_begin_versioned_write(&db, session.id, memory_version), 0);
	ASSERT_EQ(memory_store_write_fact(&db, session.id, "project", "temporary", "test",
					  "general", .5),
		  0);
	EXPECT_EQ(memory_store_finish_write(&db, -ECANCELED), -ECANCELED);
	EXPECT_NE(sqlite3_get_autocommit(db.handle), 0);
	EXPECT_EQ(memory_count_fact(&db, session.id, "project"), 0);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	EXPECT_EQ(memory_store_begin_versioned_write(&db, session.id, memory_version), -ESTALE);
	EXPECT_NE(sqlite3_get_autocommit(db.handle), 0);
	ASSERT_EQ(memory_store_begin_write(&db), 0);
	EXPECT_NE(memory_store_begin_versioned_write(&db, session.id, memory_version), 0);
	EXPECT_EQ(sqlite3_get_autocommit(db.handle), 0);
	EXPECT_EQ(memory_store_finish_write(&db, -ECANCELED), -ECANCELED);
	EXPECT_NE(sqlite3_get_autocommit(db.handle), 0);
}

TEST_F(MemoryTest, SnapshotReadRejectsAnOccupiedOwnerWithoutChangingItsRows)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "occupied-snapshot", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "location", "London", "first",
					   "context", .5),
		  0);
	struct memory_snapshot_options read{};
	read.facts_limit = -1;
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {};
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
	ASSERT_GT(snapshot.facts.nelts, 0u);
	void *original = snapshot.facts.elts;
	auto *fact =
		static_cast<struct memory_fact_record *>(morph_array_borrow(&snapshot.facts, 0));
	char *value = fact->value;

	EXPECT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), -EEXIST);
	EXPECT_EQ(snapshot.facts.elts, original);
	EXPECT_EQ(fact->value, value);
	EXPECT_STREQ(fact->value, "London");
	memory_snapshot_cleanup(&snapshot);
	EXPECT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
}

TEST_F(MemoryTest, JobSnapshotReadRejectsAnOccupiedOwnerWithoutChangingItsPayload)
{
	struct session session{};
	ASSERT_EQ(session_create(&db, "occupied-jobs", "mock", &session), 0);
	int64_t id = 0;
	ASSERT_EQ(memory_job_store_add(&db, session.id, 1, "owned payload", &id), 0);
	struct memory_job_records records MORPH_CLEANUP(memory_job_records_cleanup) = {};
	ASSERT_EQ(memory_job_store_recoverable(&db, &records), 0);
	ASSERT_EQ(records.rows.nelts, 1u);
	void *original = records.rows.elts;
	auto *row = static_cast<struct memory_job_record *>(morph_array_borrow(&records.rows, 0));
	char *payload = row->payload_json;

	EXPECT_EQ(memory_job_store_recent(&db, session.id, 20, &records), -EEXIST);
	EXPECT_EQ(records.rows.elts, original);
	EXPECT_EQ(row->payload_json, payload);
	EXPECT_STREQ(row->payload_json, "owned payload");
}

TEST_F(MemoryTest, ContextOutputRejectsOccupiedOwner)
{
	char *context MORPH_CLEANUP(morph_charp_cleanup) = strdup("retained");
	ASSERT_NE(context, nullptr);
	char *owner = context;
	EXPECT_EQ(memory_reply_text_create(&db, 0, "hello", &opts, &context), -EALREADY);
	EXPECT_EQ(context, owner);
	EXPECT_STREQ(context, "retained");
}

TEST_F(MemoryTest, RelevantProjectFactsSurviveRecentNoiseAndRespectOwnerAndProject)
{
	struct session older, current, other_project, other_user;
	ASSERT_EQ(session_create(&db, "recall-older", "mock", &older), 0);
	ASSERT_EQ(session_create(&db, "recall-current", "mock", &current), 0);
	ASSERT_EQ(session_create(&db, "recall-other-project", "mock", &other_project), 0);
	ASSERT_EQ(session_create(&db, "recall-other-user", "mock", &other_user), 0);
	ASSERT_EQ(preference_bind(&db, older.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, current.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, other_project.id, "alice", "/elsewhere"), 0);
	ASSERT_EQ(preference_bind(&db, other_user.id, "bob", "/project"), 0);
	ASSERT_EQ(memory_store_upsert_fact(&db, older.id, "composer_layout",
					   "Use four-edge insets for composer layout",
					   "verified source", "project", 0.8),
		  0);
	for (int i = 0; i < 10; i++) {
		std::string key = "unrelated_" + std::to_string(i);
		ASSERT_EQ(memory_store_upsert_fact(&db, current.id, key.c_str(),
						   "Recent unrelated detail", "source", "project",
						   0.8),
			  0);
	}
	ASSERT_EQ(memory_store_add_note(&db, other_project.id, "project", "composer_secret",
					"OTHER_PROJECT_SECRET", "composer"),
		  0);
	ASSERT_EQ(memory_store_add_note(&db, other_user.id, "project", "composer_secret",
					"OTHER_USER_SECRET", "composer"),
		  0);
	opts.max_facts = 1;
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, current.id, "Fix composer layout", &opts, &context),
		  0);
	ASSERT_NE(context, nullptr);
	struct memory_query_options query = {};
	query.type = MEMORY_QUERY_FACTS;
	query.scope_project = 1;
	query.query = "composer";
	char *found MORPH_CLEANUP(morph_charp_cleanup) =
		memory_query_text_create(&db, current.id, &query);
	ASSERT_NE(found, nullptr);
	EXPECT_NE(strstr(found, "four-edge insets"), nullptr);
	EXPECT_EQ(strstr(found, "Recent unrelated detail"), nullptr);
	EXPECT_EQ(strstr(context, "OTHER_PROJECT_SECRET"), nullptr);
	EXPECT_EQ(strstr(context, "OTHER_USER_SECRET"), nullptr);
}

TEST_F(MemoryTest, RelevantPastTasksAreSearchedWithoutTemporalTriggerWords)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "episode-relevance", "mock", &session), 0);
	ASSERT_EQ(save_accepted_turn(&db, session.id, "Fix composer redraw",
				     "Removed the stale padding after command submission", nullptr,
				     1, &opts),
		  0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "Investigate composer redraw", &opts,
					   &context),
		  0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "past_tasks#"), nullptr);
	EXPECT_NE(strstr(context, "stale padding"), nullptr);
}

struct MemoryEvidenceModel {
	bool saw_observation = false;
};

static int memory_evidence_chat(struct model *model, const char *system, const char **messages, int,
				const struct model_chat_options *, sse_callback callback,
				void *data)
{
	auto *state = static_cast<MemoryEvidenceModel *>(model->handle);
	if (strstr(system, "Organize supplied"))
		return callback("{\"title\":\"\",\"summary\":\"\",\"sources\":[]}", data);
	state->saw_observation = strstr(messages[0], "Build verified: all tests passed") != nullptr;
	return callback(
		"{\"facts\":[{\"key\":\"composer_layout\",\"value\":\"Composer uses four-edge "
		"insets\","
		"\"category\":\"project\"},{\"key\":\"language\",\"value\":\"English\","
		"\"category\":\"preference\"}],\"rules\":[{\"rule_text\":\"Always speak "
		"English\"}],"
		"\"tips\":[{\"key\":\"composer_validation\",\"value\":\"Composer insets were "
		"verified "
		"with the layout tests\",\"evidence\":\"Build verified: all tests passed\"},"
		"{\"key\":\"fabricated\",\"value\":\"Invented fix\",\"evidence\":\"No such "
		"observation\"}],"
		"\"past_task\":{\"key_decisions\":\"Use composer insets; verified layout tests\"}}",
		data);
}

TEST_F(MemoryTest, ExtractionProducesSearchableFactsAndOnlyEvidenceBackedTips)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "knowledge-loop", "mock", &session), 0);
	ASSERT_EQ(
		explicit_preference(&db, session.id, "Chinese", "Always reply in Chinese.", "pref"),
		0);
	struct react_step observation = {};
	observation.type = REACT_STEP_OBSERVATION;
	observation.tool_name = const_cast<char *>("exec");
	observation.content = const_cast<char *>("Build verified: all tests passed");
	MemoryEvidenceModel state;
	struct model model = {};
	model.handle = &state;
	model.chat = memory_evidence_chat;
	std::strcpy(model.api_key, "mock");
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(memory_save_turn(&db, session.id, "Fix composer insets", "Done", &observation, 1,
				   &opts),
		  0);
	EXPECT_TRUE(state.saw_observation);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "Continue composer work", &opts,
					   &context),
		  0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "Composer uses four-edge insets"), nullptr);
	EXPECT_NE(strstr(context, "Composer insets were verified"), nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "English"), nullptr);
	EXPECT_EQ(strstr(context, "Invented fix"), nullptr);
	cJSON *report MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(memory_report_create(&db, session.id, &opts, "composer", &report), 0);
	EXPECT_GE(cJSON_GetArraySize(cJSON_GetObjectItem(report, "entries")), 3);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.nelts, 0u);
}

TEST_F(MemoryTest, ChineseSearchAndUpdatedFactsPreserveSources)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "chinese-recall", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "project", "leak_fix", "内存泄漏旧方案",
					"old evidence"),
		  0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "project", "leak_fix",
					"内存泄漏通过显式释放解决", "new evidence"),
		  0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.query = "内存泄漏";
	search.project = 1;
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	ASSERT_EQ(matches.records.nelts, 1u);
	auto *record = static_cast<struct memory_record *>(morph_array_borrow(&matches.records, 0));
	EXPECT_STREQ(record->value, "内存泄漏通过显式释放解决");
	EXPECT_STREQ(record->source, "new evidence");
	EXPECT_EQ(memory_search_records(&db, session.id, &search, &matches), -EEXIST);
	memory_search_result_cleanup(&matches);
	search.query = "unrelated absent terms";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.nelts, 0u);
}

static int memory_hide_session(const char *display, const char *, void *hidden)
{
	return strcmp(display, static_cast<const char *>(hidden)) != 0;
}

TEST_F(MemoryTest, VisibilityIsAppliedBeforeFactsRevisionSelection)
{
	struct session visible, hidden;
	ASSERT_EQ(session_create(&db, "visible-recall", "mock", &visible), 0);
	ASSERT_EQ(session_create(&db, "hidden-recall", "mock", &hidden), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &visible), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &hidden), 0);
	ASSERT_EQ(preference_bind(&db, visible.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, hidden.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_store_add_note(&db, visible.id, "project", "build_method",
					"Visible build method", "visible source"),
		  0);
	ASSERT_EQ(memory_store_add_note(&db, hidden.id, "project", "build_method",
					"Hidden build method", "hidden source"),
		  0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.project = 1;
	search.query = "build";
	search.visible_fn = memory_hide_session;
	search.visible_data = hidden.display_id;
	ASSERT_EQ(memory_search_records(&db, visible.id, &search, &matches), 0);
	ASSERT_EQ(matches.records.nelts, 1u);
	auto *record = static_cast<struct memory_record *>(morph_array_borrow(&matches.records, 0));
	EXPECT_STREQ(record->value, "Visible build method");
}

TEST_F(MemoryTest, LargeUserPromptCannotOverflowSearchQueryLimit)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "long-recall-query", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	ASSERT_EQ(
		memory_store_upsert_fact(&db, session.id, "name", "Alice", "input", "identity", .8),
		0);
	std::string prompt(8000, 'x');
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, prompt.c_str(), &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "Alice"), nullptr);
}

TEST_F(MemoryTest, ProposedCommandsCannotBeUsedAsObservedTipEvidence)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "proposed-evidence", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_args = const_cast<char *>("echo Build verified: all tests passed");
	MemoryEvidenceModel state;
	struct model model = {};
	model.handle = &state;
	model.chat = memory_evidence_chat;
	std::strcpy(model.api_key, "mock");
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(
		memory_save_turn(&db, session.id, "Fix composer", "Unverified", &action, 0, &opts),
		0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "tips";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.nelts, 0u);
}

TEST_F(MemoryTest, ScopedClearsRemoveLearnedFactsAndTipsIndependently)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "learned-clear", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "project", "layout", "Insets", "source"),
		  0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "lesson", "verify", "Run checks",
					"evidence"),
		  0);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_FACTS), 0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	ASSERT_EQ(matches.records.nelts, 1u);
	auto *record = static_cast<struct memory_record *>(morph_array_borrow(&matches.records, 0));
	EXPECT_STREQ(record->kind, "tips");
	memory_search_result_cleanup(&matches);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_TIPS), 0);
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.nelts, 0u);
}

TEST_F(MemoryTest, LegacyPreferenceAliasesCannotCompeteWithExplicitSettings)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "legacy-alias-recall", "mock", &session), 0);
	ASSERT_EQ(explicit_preference(&db, session.id, "Chinese", "Always reply in Chinese.",
				      "input"),
		  0);
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "response.language", "English",
					   "legacy source", "context", 0.8),
		  0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "general", "style", "Speak English",
					"old inferred source"),
		  0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "English language style", &opts,
					   &context),
		  0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "English"), nullptr);
}

TEST_F(MemoryTest, OlderLearnedNotesRemainReadable)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "older-note-format", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	struct model model = {};
	std::strcpy(model.api_key, "mock");
	model.chat = [](struct model *, const char *system, const char **, int,
			const struct model_chat_options *, sse_callback callback, void *data) {
		if (strstr(system, "Organize supplied"))
			return callback("{\"title\":\"\",\"summary\":\"\",\"sources\":[]}", data);
		return callback("{\"lessons\":[{\"key\":\"layout_checks\","
				"\"value\":\"Verify the composer layout tests\","
				"\"evidence\":\"All layout tests passed\"}],"
				"\"episode\":{\"key_decisions\":\"Verified composer layout\"}}",
				data);
	};
	struct react_step observation = {};
	observation.type = REACT_STEP_OBSERVATION;
	observation.content = const_cast<char *>("All layout tests passed");
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(memory_save_turn(&db, session.id, "Fix composer", "Done", &observation, 1, &opts),
		  0);
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "tips";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	ASSERT_EQ(matches.records.nelts, 1u);
	auto *record = static_cast<struct memory_record *>(morph_array_borrow(&matches.records, 0));
	EXPECT_STREQ(record->value, "Verify the composer layout tests");
	EXPECT_STREQ(record->source, "All layout tests passed");
}

struct SummaryMemoryModel {
	int extracted = 0;
	int organized = 0;
	bool fail_organization = false;
	bool invalid_source = false;
	bool change_source = false;
	struct db *database = nullptr;
	int64_t session_id = 0;
};

static int summary_memory_chat(struct model *model, const char *system, const char **messages,
			       int count, const struct model_chat_options *request,
			       sse_callback callback, void *data)
{
	auto *state = static_cast<SummaryMemoryModel *>(model->handle);
	EXPECT_EQ(count, 1);
	EXPECT_NE(request, nullptr);
	if (!strstr(system, "Organize supplied")) {
		state->extracted++;
		return callback(
			"{\"facts\":[{\"key\":\"external_constraint\",\"value\":\"The integration "
			"needs a staging account\",\"category\":\"project\"}],\"past_task\":{"
			"\"key_decisions\":\"Verified the staging workflow\"}}",
			data);
	}
	state->organized++;
	EXPECT_STREQ(request->usage_phase, "memory_summary");
	EXPECT_LE(strlen(messages[0]), 24000u);
	if (state->fail_organization)
		return -ENETUNREACH;
	if (state->change_source) {
		state->change_source = false;
		EXPECT_EQ(memory_note_save(state->database, state->session_id, "New decision",
					   "The staging account changed", "Explicit new decision",
					   "new", "project"),
			  0);
	}
	char kind[64] = {};
	long long id = 0;
	if (std::sscanf(messages[0], "[%63[^#]#%lld]", kind, &id) != 2)
		return MORPH_ERR_FORMAT;
	cJSON *response MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON_AddStringToObject(response, "title", "Staging workflow");
	std::string text = "次回の作業には staging account が必要です。 Read [" +
			   std::string(kind) + "#" + std::to_string(id) +
			   "] for the original evidence.";
	cJSON_AddStringToObject(response, "summary", text.c_str());
	cJSON *sources = cJSON_AddArrayToObject(response, "sources");
	cJSON *source = cJSON_CreateObject();
	cJSON_AddStringToObject(source, "kind", kind);
	cJSON_AddNumberToObject(source, "id",
				state->invalid_source ? 900000 : static_cast<double>(id));
	cJSON_AddItemToArray(sources, source);
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = cJSON_PrintUnformatted(response);
	return callback(json, data);
}

static void configure_summary_model(struct memory_options *options, struct model *model,
				    SummaryMemoryModel *state)
{
	std::strcpy(model->api_key, "unit-test");
	model->chat = summary_memory_chat;
	model->handle = state;
	options->llm = model;
	options->learn_from_chats_enabled = 1;
}

TEST_F(MemoryTest, TwoStageLearningProducesBoundedSummaryAndNoForegroundModelCalls)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-flow", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	SummaryMemoryModel state;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	ASSERT_EQ(memory_save_turn(&db, session.id, "Check staging", "Verified", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(state.extracted, 1);
	EXPECT_EQ(state.organized, 1);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "كيف أكمل العمل السابق؟", &opts,
					   &context),
		  0);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "Project memory summaries"), nullptr);
	EXPECT_NE(strstr(context, "次回の作業"), nullptr);
	EXPECT_NE(strstr(context, "Memory directory"), nullptr);
	EXPECT_NE(strstr(context, "past_tasks#"), nullptr);
	EXPECT_LE(utf8len(context), static_cast<size_t>(opts.max_reply_chars));
	EXPECT_EQ(state.extracted, 1);
	EXPECT_EQ(state.organized, 1);
	struct memory_query_options query = {};
	query.type = MEMORY_QUERY_SUMMARY;
	query.record_id = session.id;
	char *full MORPH_CLEANUP(morph_charp_cleanup) =
		memory_query_text_create(&db, session.id, &query);
	ASSERT_NE(full, nullptr);
	EXPECT_NE(strstr(full, "Details:"), nullptr);
	EXPECT_NE(strstr(full, "Model-organized historical context"), nullptr);
}

TEST_F(MemoryTest, OrganizationRetryDoesNotRepeatCommittedExtractionOrTask)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-recovery", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	SummaryMemoryModel state;
	state.fail_organization = true;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	ASSERT_EQ(memory_save_turn_async(&db, session.id, "Check staging", "Verified", nullptr, 1,
					 &opts),
		  0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.extracted, 1);
	EXPECT_EQ(state.organized, 1);
	struct memory_job_records jobs MORPH_CLEANUP(memory_job_records_cleanup) = {};
	ASSERT_EQ(memory_job_store_recent(&db, session.id, 10, &jobs), 0);
	ASSERT_EQ(jobs.rows.nelts, 1u);
	auto *job = static_cast<struct memory_job_record *>(morph_array_borrow(&jobs.rows, 0));
	EXPECT_STREQ(job->state, "failed");
	EXPECT_STREQ(job->stage, "organize");
	state.fail_organization = false;
	ASSERT_EQ(db_exec(&db, "UPDATE memory_jobs SET updated_at=updated_at-60"), 0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.extracted, 1);
	EXPECT_EQ(state.organized, 2);
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {};
	struct memory_snapshot_options read = {};
	read.tasks_limit = -1;
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &read, &snapshot), 0);
	EXPECT_EQ(snapshot.tasks.nelts, 1u);
}

TEST_F(MemoryTest, ExtractionAndResumeStageCommitTogether)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "atomic-learning", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	SummaryMemoryModel state;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	ASSERT_EQ(db_exec(&db, "CREATE TRIGGER reject_memory_stage BEFORE UPDATE OF stage "
			       "ON memory_jobs WHEN NEW.stage='organize' BEGIN "
			       "SELECT RAISE(ABORT,'injected failure'); END"),
		  0);
	ASSERT_EQ(memory_save_turn_async(&db, session.id, "Check staging", "Verified", nullptr, 1,
					 &opts),
		  0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.extracted, 1);
	EXPECT_EQ(state.organized, 0);
	struct memory_search_result notes MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options read = {};
	read.kind = "learned_facts";
	ASSERT_EQ(memory_search_records(&db, session.id, &read, &notes), 0);
	EXPECT_EQ(notes.records.nelts, 0u);
	struct memory_job_records jobs MORPH_CLEANUP(memory_job_records_cleanup) = {};
	ASSERT_EQ(memory_job_store_recent(&db, session.id, 10, &jobs), 0);
	ASSERT_EQ(jobs.rows.nelts, 1u);
	auto *job = static_cast<struct memory_job_record *>(morph_array_borrow(&jobs.rows, 0));
	EXPECT_STREQ(job->state, "failed");
	EXPECT_STREQ(job->stage, "extract");
	ASSERT_EQ(db_exec(&db, "DROP TRIGGER reject_memory_stage"), 0);
	ASSERT_EQ(db_exec(&db, "UPDATE memory_jobs SET updated_at=updated_at-60"), 0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.extracted, 2);
	EXPECT_EQ(state.organized, 1);
	memory_search_result_cleanup(&notes);
	ASSERT_EQ(memory_search_records(&db, session.id, &read, &notes), 0);
	EXPECT_EQ(notes.records.nelts, 1u);
	struct memory_snapshot snapshot MORPH_CLEANUP(memory_snapshot_cleanup) = {};
	struct memory_snapshot_options task_read = {};
	task_read.tasks_limit = -1;
	ASSERT_EQ(memory_snapshot_read(&db, session.id, &task_read, &snapshot), 0);
	EXPECT_EQ(snapshot.tasks.nelts, 1u);
}

TEST_F(MemoryTest, SourceChangesHideSummaryAndRejectInFlightOldSummary)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-stale", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	SummaryMemoryModel state;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	ASSERT_EQ(memory_save_turn(&db, session.id, "Check staging", "Verified", nullptr, 1, &opts),
		  0);
	ASSERT_EQ(memory_note_save(&db, session.id, "Account changed", "Use the new account",
				   "explicit", "change", "project"),
		  0);
	struct memory_search_result summaries MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "summary";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &summaries), 0);
	EXPECT_EQ(summaries.records.nelts, 0u);
	state.database = &db;
	state.session_id = session.id;
	state.change_source = true;
	opts.organize_only = 1;
	EXPECT_EQ(memory_save_turn(&db, session.id, "", "", nullptr, 1, &opts), -ESTALE);
	memory_search_result_cleanup(&summaries);
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &summaries), 0);
	EXPECT_EQ(summaries.records.nelts, 0u);
	ASSERT_EQ(memory_save_turn(&db, session.id, "", "", nullptr, 1, &opts), 0);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	memory_search_result_cleanup(&summaries);
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &summaries), 0);
	EXPECT_EQ(summaries.records.nelts, 0u);
}

TEST_F(MemoryTest, UnknownSummarySourcesAreRejected)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-source", "mock", &session), 0);
	ASSERT_EQ(memory_store_prepare_session(&db, session.id), 0);
	SummaryMemoryModel state;
	state.invalid_source = true;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	EXPECT_EQ(memory_save_turn(&db, session.id, "Check staging", "Verified", nullptr, 1, &opts),
		  MORPH_ERR_FORMAT);
	struct memory_search_result summaries MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "summary";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &summaries), 0);
	EXPECT_EQ(summaries.records.nelts, 0u);
}

TEST_F(MemoryTest, ExistingMemoryIsOrganizedAsBoundedBackgroundCatchUp)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-backfill", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, session.id, "Staging", "Use staging", "explicit", "note",
				   "project"),
		  0);
	SummaryMemoryModel state;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	ASSERT_EQ(memory_accept_input(&db, session.id, "Continue", "next", &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.extracted, 0);
	EXPECT_EQ(state.organized, 1);
	ASSERT_EQ(memory_accept_input(&db, session.id, "Continue again", "again", &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.organized, 1);
}

TEST_F(MemoryTest, PrivateSessionNotesAndTheirSummaryNeverEnterAnotherSession)
{
	struct session source, other;
	ASSERT_EQ(session_create(&db, "private-summary", "mock", &source), 0);
	ASSERT_EQ(session_create(&db, "other-summary", "mock", &other), 0);
	ASSERT_EQ(preference_bind(&db, source.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, other.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, source.id, "Private staging", "PRIVATE_STAGING", "explicit",
				   "note", "session"),
		  0);
	SummaryMemoryModel state;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	opts.organize_only = 1;
	ASSERT_EQ(memory_save_turn(&db, source.id, "", "", nullptr, 1, &opts), 0);
	char *context MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, other.id, "staging", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(strstr(context, "PRIVATE_STAGING"), nullptr);
	EXPECT_EQ(strstr(context, "次回の作業"), nullptr);
	morph_charp_cleanup(&context);
	ASSERT_EQ(memory_reply_text_create(&db, source.id, "staging", &opts, &context), 0);
	EXPECT_NE(strstr(context, "PRIVATE_STAGING"), nullptr);
	EXPECT_NE(strstr(context, "次回の作業"), nullptr);
}

TEST_F(MemoryTest, SearchUsesModelChosenPhrasesWithoutALanguageDictionary)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "multilingual-search", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	for (const char *text :
	     {"接続手順を確認", "إعدادات الحساب", "réglage du compte", "内存泄漏"}) {
		ASSERT_EQ(
			memory_note_save(&db, session.id, text, text, "explicit", text, "project"),
			0);
		struct memory_search_result matches MORPH_CLEANUP(
			memory_search_result_cleanup) = {};
		struct memory_search_options search = {};
		search.query = text;
		ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
		ASSERT_GE(matches.records.nelts, 1u);
	}
}

TEST_F(MemoryTest, RestartedExtractionRetainsActualToolObservations)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "observation-recovery", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	cJSON *payload MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON_AddStringToObject(payload, "input", "Fix composer");
	cJSON_AddStringToObject(payload, "answer", "Done");
	cJSON_AddBoolToObject(payload, "success", 1);
	cJSON_AddBoolToObject(payload, "learn_from_chats", 1);
	cJSON_AddBoolToObject(payload, "save_tasks", 1);
	cJSON *steps = cJSON_AddArrayToObject(payload, "steps");
	cJSON *observation = cJSON_CreateObject();
	cJSON_AddNumberToObject(observation, "type", REACT_STEP_OBSERVATION);
	cJSON_AddNumberToObject(observation, "error", 0);
	cJSON_AddStringToObject(observation, "content", "Build verified: all tests passed");
	cJSON_AddStringToObject(observation, "arguments", "");
	cJSON_AddStringToObject(observation, "tool", "exec");
	cJSON_AddItemToArray(steps, observation);
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = cJSON_PrintUnformatted(payload);
	int64_t id = 0;
	ASSERT_EQ(memory_job_store_add(&db, session.id, 0, json, &id), 0);
	MemoryEvidenceModel state;
	struct model model = {};
	model.handle = &state;
	model.chat = memory_evidence_chat;
	std::strcpy(model.api_key, "mock");
	opts.llm = &model;
	opts.learn_from_chats_enabled = 1;
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_TRUE(state.saw_observation);
	struct memory_search_result tips MORPH_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "tips";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &tips), 0);
	ASSERT_EQ(tips.records.nelts, 1u);
}

TEST_F(MemoryTest, FailedSummaryCatchUpDoesNotCreateUnboundedRetryJobs)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "summary-backoff", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, session.id, "Staging", "Use staging", "explicit", "note",
				   "project"),
		  0);
	SummaryMemoryModel state;
	state.fail_organization = true;
	struct model model = {};
	configure_summary_model(&opts, &model, &state);
	for (int i = 0; i < 3; i++) {
		ASSERT_EQ(memory_accept_input(&db, session.id, "Continue", "input", &opts), 0);
		memory_async_shutdown(opts.worker);
	}
	EXPECT_EQ(state.organized, 1);
	struct memory_job_records jobs MORPH_CLEANUP(memory_job_records_cleanup) = {};
	ASSERT_EQ(memory_job_store_recent(&db, session.id, 10, &jobs), 0);
	EXPECT_EQ(jobs.rows.nelts, 1u);
	state.fail_organization = false;
	ASSERT_GE(memory_job_store_retry_session(&db, session.id), 0);
	ASSERT_EQ(memory_async_resume(&db, &opts), 0);
	memory_async_shutdown(opts.worker);
	EXPECT_EQ(state.organized, 2);
	EXPECT_EQ(state.extracted, 0);
}

TEST_F(MemoryTest, ParallelEditsOnOneConnectionHaveOneWinnerAndNoMixedTransaction)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "parallel-edit", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, session.id, "Staging", "Old value", "explicit", "note",
				   "project"),
		  0);
	std::atomic<int> successes{0};
	std::atomic<int> stale{0};
	std::atomic<int> failures{0};
	auto edit = [&](const char *value) {
		int rc = memory_record_edit(&db, session.id, "notes", 1, 0, "Old value", value,
					    "explicit correction");
		if (rc == 0)
			successes++;
		else if (rc == -ESTALE)
			stale++;
		else
			failures++;
	};
	std::thread first(edit, "First new value");
	std::thread second(edit, "Second new value");
	first.join();
	second.join();
	EXPECT_EQ(successes, 1);
	EXPECT_EQ(stale, 1);
	EXPECT_EQ(failures, 0);
	EXPECT_NE(sqlite3_get_autocommit(db.handle), 0);
}

TEST_F(MemoryTest, ExistingProjectBindingCannotMoveSavedMemoryToAnotherProject)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "bound-project", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/one"), 0);
	ASSERT_EQ(memory_note_save(&db, session.id, "Project one", "Private project detail",
				   "explicit", "note", "project"),
		  0);
	EXPECT_EQ(preference_bind(&db, session.id, "alice", "/two"), -EPERM);
	EXPECT_EQ(preference_bind(&db, session.id, "bob", "/one"), -EPERM);
	EXPECT_EQ(preference_bind(&db, session.id, "alice", "/one"), 0);
}

TEST_F(MemoryTest, SearchCasefoldsNonAsciiTextWithoutLanguageSpecificRules)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "casefold-search", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, session.id, "école", "Straße", "source", "note", "project"),
		  0);
	for (const char *query : {"ÉCOLE", "STRASSE", "e\u0301cole"}) {
		struct memory_search_result matches MORPH_CLEANUP(
			memory_search_result_cleanup) = {};
		struct memory_search_options search = {};
		search.query = query;
		ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
		EXPECT_EQ(matches.records.nelts, 1u);
	}
}
