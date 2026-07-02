#include <gtest/gtest.h>

#include "agent/react.h"
#include "agent/tokenizer.h"
#include "agent/turn.h"
#include "db/database.h"
#include "session.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <unistd.h>

class AgentTurnTest : public ::testing::Test {
protected:
	struct db db;
	struct session sess;
	struct tokenizer *tok;
	struct react_context *react;
	char db_path[PATH_MAX];

	void SetUp() override
	{
		snprintf(db_path, sizeof(db_path), "/tmp/ma_test_agent_turn_%d.db",
			 getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(session_create(&db, "agent_turn", "gpt-test",
					 &sess), 0);
		tok = tokenizer_create("gpt-test", 4096);
		ASSERT_NE(tok, nullptr);
		react = react_context_create(nullptr, tok, nullptr, nullptr);
		ASSERT_NE(react, nullptr);
	}

	void TearDown() override
	{
		react_context_destroy(react);
		tokenizer_destroy(tok);
		db_close(&db);
		std::remove(db_path);
	}
};

TEST_F(AgentTurnTest, BeginLoadsHistoryAndFinishPersistsTurn)
{
	ASSERT_EQ(message_add(&db, sess.id, "user", "old question", 2), 0);
	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "full model prompt";
	input.stored_user_input = "display prompt";
	input.turn_id = "turn_test";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	ASSERT_TRUE(turn.begun);
	ASSERT_NE(react->messages, nullptr);
	EXPECT_STREQ(react->messages->content, "old question");

	struct react_step *step =
		static_cast<struct react_step *>(calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_FINAL;
	step->content = strdup("done");
	ASSERT_NE(step->content, nullptr);
	react->steps = step;
	react->final_answer = strdup("hello from assistant");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;

	struct agent_turn_result result;
	ASSERT_EQ(agent_turn_finish(&turn, &result), 0);
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), -EALREADY);
	EXPECT_TRUE(result.trace_saved);
	EXPECT_TRUE(result.user_saved);
	EXPECT_TRUE(result.assistant_saved);
	EXPECT_GT(result.user_tokens, 0);
	EXPECT_GT(result.assistant_tokens, 0);

	int count = 0;
	struct message *msgs = message_list(&db, sess.id, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(msgs, nullptr);
	EXPECT_STREQ(msgs->role, "user");
	EXPECT_STREQ(msgs->content, "old question");
	ASSERT_NE(msgs->next, nullptr);
	EXPECT_STREQ(msgs->next->role, "user");
	EXPECT_STREQ(msgs->next->content, "display prompt");
	EXPECT_STREQ(msgs->next->turn_id, "turn_test");
	ASSERT_NE(msgs->next->next, nullptr);
	EXPECT_STREQ(msgs->next->next->role, "assistant");
	EXPECT_STREQ(msgs->next->next->content, "hello from assistant");
	EXPECT_STREQ(msgs->next->next->turn_id, "turn_test");
	message_free_list(msgs);

	struct session loaded;
	ASSERT_EQ(session_get_by_id(&db, sess.id, &loaded), 0);
	EXPECT_EQ(loaded.token_used,
		  result.user_tokens + result.assistant_tokens);

	int round_no = 0;
	int aborted = 0;
	char *trace = trace_load_latest(&db, sess.id, &round_no, &aborted);
	ASSERT_NE(trace, nullptr);
	EXPECT_EQ(round_no, 1);
	EXPECT_EQ(aborted, 0);
	EXPECT_NE(strstr(trace, "\"type\":\"Final\""), nullptr);
	free(trace);
}
