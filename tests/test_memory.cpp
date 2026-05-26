#include <gtest/gtest.h>

#include "db/database.h"
#include "agent/memory.h"
#include "session.h"

#include <cstdio>
#include <cstring>

class MemoryTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];
	struct memory_options opts;

	void SetUp() override
	{
		std::snprintf(db_path, sizeof(db_path),
			      "/tmp/ma_test_memory_%d.db", getpid());
		std::remove(db_path);
		std::memset(&db, 0, sizeof(db));
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		std::memset(&opts, 0, sizeof(opts));
		opts.enabled = 1;
		opts.hot_path_enabled = 1;
		opts.cold_path_enabled = 1;
		opts.max_facts = 6;
		opts.max_episodes = 4;
		opts.max_procedures = 4;
		opts.max_context_chars = 4096;
	}

	void TearDown() override
	{
		db_close(&db);
		std::remove(db_path);
	}
};

TEST_F(MemoryTest, ConsolidateCreatesFactsAndProcedures)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_session", "gpt-4o", &s), 0);

	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  nullptr, 1, &opts),
		  0);

	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(
			  db.handle,
			  "SELECT value_text FROM memory_facts "
			  "WHERE session_id=? AND key_name='preferred_language' "
			  "AND is_current=1",
			  -1, &stmt, nullptr),
		  SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, s.id);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
		     "Chinese");
	sqlite3_finalize(stmt);

	ASSERT_EQ(sqlite3_prepare_v2(
			  db.handle,
			  "SELECT COUNT(*) FROM memory_procedures WHERE session_id=?",
			  -1, &stmt, nullptr),
		  SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, s.id);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_GE(sqlite3_column_int(stmt, 0), 2);
	sqlite3_finalize(stmt);
}

TEST_F(MemoryTest, UpdatedFactSupersedesOlderValue)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_temporal", "gpt-4o", &s), 0);

	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please reply in English.",
			  "Sure, I will reply in English.",
			  nullptr, 1, &opts),
		  0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "以后都用中文回答我。",
			  "好的，之后默认使用中文。",
			  nullptr, 1, &opts),
		  0);

	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(
			  db.handle,
			  "SELECT value_text, is_current FROM memory_facts "
			  "WHERE session_id=? AND key_name='preferred_language' "
			  "ORDER BY id ASC",
			  -1, &stmt, nullptr),
		  SQLITE_OK);
	sqlite3_bind_int64(stmt, 1, s.id);

	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
		     "English");
	EXPECT_EQ(sqlite3_column_int(stmt, 1), 0);

	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
		     "Chinese");
	EXPECT_EQ(sqlite3_column_int(stmt, 1), 1);
	sqlite3_finalize(stmt);
}

TEST_F(MemoryTest, BuildContextIncludesEpisodesAndChanges)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_context", "gpt-4o", &s), 0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please reply in English.",
			  "Sure, I will reply in English.",
			  &action, 1, &opts),
		  0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "以后都用中文回答我。",
			  "好的，之后默认使用中文。",
			  &action, 1, &opts),
		  0);

	char *temporal = memory_build_context(&db, s.id,
					      "现在还是英文吗，什么时候改的？",
					      &opts);
	ASSERT_NE(temporal, nullptr);
	EXPECT_NE(std::strstr(temporal, "Recent changes"), nullptr);
	EXPECT_NE(std::strstr(temporal, "preferred_language"), nullptr);
	free(temporal);

	char *episodic = memory_build_context(&db, s.id,
					      "上次你是怎么做的？",
					      &opts);
	ASSERT_NE(episodic, nullptr);
	EXPECT_NE(std::strstr(episodic, "Relevant episodes"), nullptr);
	EXPECT_NE(std::strstr(episodic, "file_read"), nullptr);
	free(episodic);
}

TEST_F(MemoryTest, RenderSessionShowsStoredSections)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_render", "gpt-4o", &s), 0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  &action, 1, &opts),
		  0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_NE(std::strstr(rendered, "Profile"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Current facts"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Standing rules"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Recent episodes"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearFactsKeepsEpisodes)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_clear_facts", "gpt-4o", &s), 0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please reply in English.",
			  "Sure, I will reply in English.",
			  &action, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_FACTS), 0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_EQ(std::strstr(rendered, "Current facts"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Recent episodes"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearAllRemovesEverything)
{
	struct session s;

	ASSERT_EQ(session_create(&db, "memory_clear_all", "gpt-4o", &s), 0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  nullptr, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_ALL), 0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_STREQ(rendered, "No long-term memory stored for this session.");
	free(rendered);
}

static int memory_count_fact(struct db *db, int64_t session_id,
			     const char *key_name)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT COUNT(*) FROM memory_facts "
		"WHERE session_id=? AND key_name=? AND is_current=1";
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

TEST_F(MemoryTest, AmbiguousAnchorsDoNotPolluteFacts)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_precision", "gpt-4o", &s), 0);

	/* "I am in Tokyo" used to also land as user_name="in Tokyo".
	 * It should now only set location. */
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id, "I am in Tokyo.",
			  "Got it.", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "user_name"), 0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "location"), 1);

	/* "我想知道..." should not be misread as a goal anchor. */
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id, "我想知道天气如何。",
			  "天气晴朗。", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "goal"), 0);

	/* "我在想..." should not be misread as a location anchor. */
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id, "我在想这个问题。",
			  "嗯。", nullptr, 1, &opts),
		  0);
	/* Still only one location fact, from the Tokyo turn above. */
	EXPECT_EQ(memory_count_fact(&db, s.id, "location"), 1);

	/* "我叫了一辆出租车" should not be misread as user_name. */
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id, "我叫了一辆出租车。",
			  "好的。", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "user_name"), 0);

	/* Sanity check: a clean anchor still works. */
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id, "Please call me Alice.",
			  "Sure, Alice.", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "preferred_name"), 1);
}

TEST_F(MemoryTest, BuildContextReturnsNullWhenEmpty)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_empty_ctx", "gpt-4o", &s), 0);

	/* No turns consolidated; build_context should return NULL (i.e.
	 * skip the prompt injection entirely) instead of emitting just the
	 * intro line. */
	char *ctx_str = memory_build_context(&db, s.id, "anything", &opts);
	EXPECT_EQ(ctx_str, nullptr);
	free(ctx_str);
}
