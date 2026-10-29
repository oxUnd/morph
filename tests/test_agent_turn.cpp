#include <gtest/gtest.h>

#include "agent/react.h"
#include "agent/history.h"
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
#include <string>
#include "util/utf8.h"

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
		ASSERT_EQ(db_open(NULL, &db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(session_create(&db, "agent_turn", "gpt-test",
					 &sess), 0);
		tok = tokenizer_create(NULL, "gpt-test", 4096);
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

static int agent_turn_summary(const char *, void *, char **out)
{
	*out = strdup("old work checkpoint");
	return *out ? 0 : -ENOMEM;
}

TEST_F(AgentTurnTest, PersistsExecutionOutcomeWithoutInventingTaskCompletion)
{
	const enum react_outcome outcomes[] = {
		REACT_OUTCOME_SUCCESS, REACT_OUTCOME_CANCELLED,
		REACT_OUTCOME_LLM_ERROR, REACT_OUTCOME_TIMEOUT,
		REACT_OUTCOME_MAX_ITERATIONS,
	};
	for (auto outcome : outcomes) {
		struct agent_session_runtime runtime = {};
		runtime.db = &db;
		runtime.session_id = sess.id;
		runtime.react = react;
		runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
		struct agent_turn_input input = {};
		std::string id = react_outcome_name(outcome);
		input.model_input = "Launch the app";
		input.turn_id = id.c_str();
		struct agent_turn turn;
		ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
		react->outcome = outcome;
		react->last_error_code = outcome == REACT_OUTCOME_SUCCESS ? 0 : -EIO;
		react->state = outcome == REACT_OUTCOME_SUCCESS ?
			REACT_STATE_DONE : REACT_STATE_ABORT;
		react->final_answer = strdup("Execution result");
		ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
		int count = 0;
		auto *items = model_history_list(&db, sess.id, 0, &count);
		const struct model_history_item *request = nullptr;
		for (auto *item = items; item; item = item->next) {
			if (item->turn_id && id == item->turn_id &&
			    strcmp(item->kind, "user_message") == 0)
				request = item;
		}
		ASSERT_NE(request, nullptr);
		cJSON *payload = cJSON_Parse(request->payload_json);
		ASSERT_NE(payload, nullptr);
		cJSON *record = cJSON_GetObjectItem(payload, "turn_outcome");
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(record, "status")),
			id.c_str());
		EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(record, "finished")));
		EXPECT_EQ(cJSON_GetObjectItem(record, "error_code")->valueint,
			outcome == REACT_OUTCOME_SUCCESS ? 0 : -EIO);
		EXPECT_EQ(cJSON_GetObjectItem(record, "task_completed"), nullptr);
		cJSON_Delete(payload);
		model_history_free_list(items);
	}
}

TEST_F(AgentTurnTest, OutcomeWriteFailureRollsBackFinalAnswer)
{
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn_input input = {};
	input.model_input = "Launch the app";
	input.turn_id = "rollback_outcome";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	ASSERT_EQ(db_exec(&db, "CREATE TRIGGER fail_outcome BEFORE UPDATE OF "
		"payload_json ON model_history_items BEGIN "
		"SELECT RAISE(ABORT, 'injected'); END;"), 0);
	react->state = REACT_STATE_DONE;
	react->outcome = REACT_OUTCOME_SUCCESS;
	react->final_answer = strdup("Opened");
	EXPECT_NE(agent_turn_finish(&turn, nullptr), 0);
	/* The pre-model user checkpoint survives; the final answer rolls back. */
	EXPECT_EQ(message_count(&db, sess.id), 1);
	int count = 0;
	auto *items = model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(items->kind, "user_message");
	EXPECT_EQ(items->payload_json, nullptr);
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, AbortedTurnWithoutOutcomeIsInterruptedRatherThanSuccessful)
{
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn_input input = {};
	input.model_input = "Verify the fix";
	input.turn_id = "unfinished";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_NONE;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, sess.id, 0, &count);
	ASSERT_EQ(count, 1);
	ASSERT_NE(items->payload_json, nullptr);
	EXPECT_NE(strstr(items->payload_json, "interrupted"), nullptr);
	EXPECT_EQ(strstr(items->payload_json, "success"), nullptr);
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, NonRetryableFailureKeepsRequestAndBlockerForResume)
{
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn_input input = {};
	input.model_input = "Bring Calculator forward";
	input.turn_id = "locked";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_TOOL_ERROR;
	react->terminal_tool_error = -EIO;
	react->last_error_code = -EIO;
	react->final_answer = strdup("The screen is locked. Unlock before continuing.");
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
	int count = 0;
	auto *items = model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 2);
	EXPECT_STREQ(items->kind, "user_message");
	ASSERT_NE(items->payload_json, nullptr);
	EXPECT_NE(strstr(items->payload_json, "tool_error"), nullptr);
	EXPECT_EQ(strstr(items->payload_json, "success"), nullptr);
	ASSERT_NE(items->next, nullptr);
	EXPECT_STREQ(items->next->content,
		"The screen is locked. Unlock before continuing.");
	model_history_free_list(items);
}

static int agent_turn_failing_summary(const char *, void *, char **out)
{
	*out = nullptr;
	return -EIO;
}

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
	EXPECT_EQ(result.trace_rc, 0);
	EXPECT_TRUE(result.user_saved);
	EXPECT_TRUE(result.assistant_saved);
	EXPECT_EQ(result.user_rc, 0);
	EXPECT_EQ(result.assistant_rc, 0);
	EXPECT_EQ(result.message_persistence_rc, 0);
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

TEST_F(AgentTurnTest, AssistantFailureKeepsPreModelUserCheckpoint)
{
	ASSERT_EQ(db_exec(&db,
		"CREATE TRIGGER reject_assistant_message "
		"BEFORE INSERT ON messages WHEN NEW.role = 'assistant' "
		"BEGIN SELECT RAISE(ABORT, 'reject assistant'); END;"), 0);

	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "remember this";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	react->final_answer = strdup("answer");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;

	struct agent_turn_result result;
	EXPECT_LT(agent_turn_finish(&turn, &result), 0);
	EXPECT_LT(result.message_persistence_rc, 0);
	EXPECT_FALSE(result.user_saved);
	EXPECT_FALSE(result.assistant_saved);

	int count = 0;
	struct message *messages = message_list(&db, sess.id, &count);
	ASSERT_NE(messages, nullptr);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(messages->role, "user");
	EXPECT_STREQ(messages->content, "remember this");
	message_free_list(messages);
	struct model_history_item *history =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_NE(history, nullptr);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(history->kind, "user_message");
	model_history_free_list(history);
}

TEST_F(AgentTurnTest, FinishDoesNotConsolidateMemoryForAbortedTurn)
{
	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "failing prompt";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);

	struct react_step *step =
		static_cast<struct react_step *>(calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_OBSERVATION;
	step->content = strdup("tool error");
	ASSERT_NE(step->content, nullptr);
	react->steps = step;
	react->final_answer = strdup("tool error");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_MAX_ITERATIONS;

	struct agent_turn_result result;
	ASSERT_EQ(agent_turn_finish(&turn, &result), 0);
	EXPECT_TRUE(result.trace_saved);
	EXPECT_TRUE(result.user_saved);
	EXPECT_TRUE(result.assistant_saved);
	EXPECT_FALSE(result.memory_queued);
	EXPECT_FALSE(result.memory_ran_inline);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
	EXPECT_EQ(model_history_count(&db, sess.id, 0), 1);

	char *rendered = memory_render_session(&db, sess.id, 0);
	ASSERT_NE(rendered, nullptr);
	EXPECT_NE(strstr(rendered,
			 "No long-term memory stored for this session."),
		  nullptr);
	free(rendered);
}

TEST_F(AgentTurnTest, DisabledMemoryDoesNotQueueOrEmitBackgroundStatus)
{
	for (bool configured : {false, true}) {
		struct memory_options options = {};
		struct agent_session_runtime runtime = {};
		int events = 0;

		runtime.db = &db;
		runtime.session_id = sess.id;
		runtime.react = react;
		runtime.memory_options = configured ? &options : nullptr;
		runtime.flags = AGENT_TURN_DEFAULT_FLAGS;
		runtime.background_user_data = &events;
		runtime.background_cb = [](void *data, const char *, const char *,
			const char *, const char *, int, int) {
			++*static_cast<int *>(data);
			return 0;
		};
		struct agent_turn_input input = {};
		input.model_input = "ordinary question";
		struct agent_turn turn = {};
		ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
		react->state = REACT_STATE_DONE;
		react->outcome = REACT_OUTCOME_SUCCESS;
		free(react->final_answer);
		react->final_answer = strdup("answer");
		ASSERT_NE(react->final_answer, nullptr);
		struct agent_turn_result result = {};
		ASSERT_EQ(agent_turn_finish(&turn, &result), 0);
		EXPECT_EQ(events, 0);
		EXPECT_FALSE(result.memory_queued);
		EXPECT_FALSE(result.memory_ran_inline);
	}
}

TEST_F(AgentTurnTest, FailedTurnKeepsUserWithoutPoisoningNextTurn)
{
	struct agent_session_runtime runtime = {};
	struct agent_turn_input failed_input = {};
	struct agent_turn failed_turn;
	struct tool_call call = {};
	int count = 0;

	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	failed_input.model_input = "run malformed tool";
	failed_input.turn_id = "turn_failed";
	ASSERT_EQ(agent_turn_begin(&failed_turn, &runtime, &failed_input), 0);
	std::strcpy(call.id, "provider_failed");
	std::strcpy(call.tool_call_id, "local_failed");
	std::strcpy(call.name, "exec");
	call.arguments = const_cast<char *>("{not-json");
	ASSERT_EQ(agent_history_record_tool_calls(react, nullptr, nullptr,
		&call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "local_failed",
		"provider_failed", "exec", "invalid arguments", -EINVAL), 0);
	react->final_answer = strdup(
		"LLM repeatedly returned an empty response.");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_LLM_ERROR;
	ASSERT_EQ(agent_turn_finish(&failed_turn, nullptr), 0);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
	EXPECT_EQ(model_history_count(&db, sess.id, 0), 3);

	struct message *transcript = message_list(&db, sess.id, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(transcript, nullptr);
	EXPECT_STREQ(transcript->role, "user");
	ASSERT_NE(transcript->next, nullptr);
	EXPECT_STREQ(transcript->next->role, "assistant");
	EXPECT_NE(std::strstr(transcript->next->content, "empty response"),
		nullptr);
	message_free_list(transcript);

	struct agent_turn_input next_input = {};
	struct agent_turn next_turn;
	next_input.model_input = "next request";
	next_input.turn_id = "turn_next";
	ASSERT_EQ(agent_turn_begin(&next_turn, &runtime, &next_input), 0);
	struct model_history_item *active =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->turn_id, "turn_failed");
	EXPECT_STREQ(active->kind, "user_message");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->turn_id, "turn_next");
	EXPECT_STREQ(active->next->kind, "user_message");
	model_history_free_list(active);
	react->final_answer = strdup("recovered");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;
	react->outcome = REACT_OUTCOME_SUCCESS;
	ASSERT_EQ(agent_turn_finish(&next_turn, nullptr), 0);
	active = model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->turn_id, "turn_failed");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->turn_id, "turn_next");
	ASSERT_NE(active->next->next, nullptr);
	EXPECT_STREQ(active->next->next->kind, "assistant_message");
	EXPECT_STREQ(active->next->next->content, "recovered");
	model_history_free_list(active);
}

TEST_F(AgentTurnTest, FailedCompactionKeepsExecutedWorkAndCheckpointOnResume)
{
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_LOAD_HISTORY | AGENT_TURN_SAVE_MESSAGES;
	struct agent_turn_input input = {};
	input.model_input = "finish the video";
	input.turn_id = "failed-compaction";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	struct tool_call call = {};
	std::strcpy(call.id, "provider-write");
	std::strcpy(call.tool_call_id, "local-write");
	std::strcpy(call.name, "exec");
	call.arguments = const_cast<char *>("{\"command\":\"render video\"}");
	ASSERT_EQ(agent_history_record_tool_calls(react, nullptr, nullptr, &call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "local-write", "provider-write",
		"exec", "Video rendered and verified at /tmp/video.mp4", 0), 0);
	struct model_history_insert checkpoint = {};
	checkpoint.session_id = sess.id;
	checkpoint.turn_id = input.turn_id;
	checkpoint.kind = "compaction_summary";
	checkpoint.role = "system";
	checkpoint.content = "Video rendered; final visual review pending.";
	checkpoint.active = 1;
	ASSERT_EQ(model_history_add(&db, &checkpoint, nullptr), 0);
	react->in_turn_compaction_count = 2;
	react->last_error_code = MORPH_ERR_PROTOCOL;
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_LLM_ERROR;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 4);

	struct agent_turn_input continuation = {};
	continuation.model_input = "continue";
	continuation.turn_id = "resume-compaction";
	struct agent_turn next;
	ASSERT_EQ(agent_turn_begin(&next, &runtime, &continuation), 0);
	bool found_result = false;
	bool found_checkpoint = false;
	for (auto *item = react->history_items; item; item = item->next) {
		if (std::strcmp(item->kind, "tool_result") == 0) {
			found_result = true;
			EXPECT_NE(std::strstr(item->content, "/tmp/video.mp4"), nullptr);
		}
		if (std::strcmp(item->kind, "compaction_summary") == 0)
			found_checkpoint = true;
	}
	EXPECT_TRUE(found_result);
	EXPECT_TRUE(found_checkpoint);
	react->state = REACT_STATE_DONE;
	react->outcome = REACT_OUTCOME_SUCCESS;
	ASSERT_EQ(agent_turn_finish(&next, nullptr), 0);
}

TEST_F(AgentTurnTest, MissingConfigurationKeepsCompleteToolHistory)
{
	struct agent_session_runtime runtime = {};
	struct agent_turn_input input = {};
	struct agent_turn turn;
	struct tool_call call = {};
	int count = 0;

	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	input.model_input = "generate an image";
	input.turn_id = "turn_missing_key";
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	std::strcpy(call.id, "provider_missing_key");
	std::strcpy(call.tool_call_id, "local_missing_key");
	std::strcpy(call.name, "img_gen");
	call.arguments = const_cast<char *>("{\"prompt\":\"sunrise\"}");
	ASSERT_EQ(agent_history_record_tool_calls(react, nullptr, nullptr,
		&call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "local_missing_key",
		"provider_missing_key", "img_gen", "missing api key",
		MORPH_ERR_NOT_CONFIGURED), 0);
	struct react_step *step = static_cast<struct react_step *>(
		calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_OBSERVATION;
	step->content = strdup("missing api key");
	ASSERT_NE(step->content, nullptr);
	step->error_code = MORPH_ERR_NOT_CONFIGURED;
	react->steps = step;
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_LLM_ERROR;
	react->last_error_code = MORPH_ERR_LLM;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);

	struct model_history_item *active = model_history_list(
		&db, sess.id, 1, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->kind, "user_message");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->kind, "assistant_tool_calls");
	ASSERT_NE(active->next->next, nullptr);
	EXPECT_STREQ(active->next->next->kind, "tool_result");
	EXPECT_STREQ(active->next->next->tool_name, "img_gen");
	model_history_free_list(active);
}

TEST_F(AgentTurnTest, BeginCompactsBeforePersistingCurrentUser)
{
	struct model_history_insert old = {};
	old.session_id = sess.id;
	old.turn_id = "turn_old";
	old.kind = "user_message";
	old.role = "user";
	old.content = "old large context";
	old.token_count = 10;
	old.active = 1;
	ASSERT_EQ(model_history_add(&db, &old, nullptr), 0);
	react->compress.max_context_tokens = 2;
	react->compress.summarize_threshold_ratio = 0.5;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 0;
	react->compress.compaction_summary_max_tokens = 10;
	react->compress.summarize = agent_turn_summary;

	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	struct agent_turn_input input = {};
	input.model_input = "current exact request";
	input.turn_id = "turn_current";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);

	int count = 0;
	struct model_history_item *items =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->kind, "compaction_summary");
	ASSERT_NE(items->next, nullptr);
	EXPECT_STREQ(items->next->kind, "user_message");
	EXPECT_STREQ(items->next->turn_id, "turn_current");
	EXPECT_STREQ(items->next->content, "current exact request");
	model_history_free_list(items);
	react->state = REACT_STATE_ABORT;
	EXPECT_EQ(agent_turn_finish(&turn, nullptr), 0);
}

TEST_F(AgentTurnTest, AssistantReasoningSurvivesHistoryReloadAndReset)
{
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	ASSERT_EQ(react_set_turn_id(react, "reasoning_turn"), 0);
	react->final_reasoning_content = strdup("Original final reasoning");
	ASSERT_NE(react->final_reasoning_content, nullptr);
	ASSERT_EQ(agent_history_record_assistant(react, "Final answer"), 0);
	ASSERT_EQ(agent_history_reload(react), 0);
	morph_array_t messages{};
	struct arena *arena = arena_create(0);
	ASSERT_NE(arena, nullptr);
	ASSERT_EQ(morph_array_init(&messages, 2, sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, react->history_items, &messages, arena),
		  0);
	ASSERT_EQ(messages.nelts, 1U);
	auto *message = static_cast<struct chat_message *>(morph_array_get(&messages, 0));
	EXPECT_STREQ(message->content, "Final answer");
	EXPECT_STREQ(message->reasoning_content, "Original final reasoning");
	react_reset(react);
	EXPECT_EQ(react->final_reasoning_content, nullptr);
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(AgentTurnTest, ChatHistoryPlacesSummaryBeforePreservedUser)
{
	struct model_history_item user = {};
	struct model_history_item summary = {};
	morph_array_t messages = {};
	struct arena *arena = arena_create(0);

	ASSERT_NE(arena, nullptr);
	std::strcpy(user.kind, "user_message");
	user.content = const_cast<char *>("exact current request");
	user.next = &summary;
	std::strcpy(summary.kind, "compaction_summary");
	summary.content = const_cast<char *>("earlier context summary");
	ASSERT_EQ(morph_array_init(&messages, 2,
		sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(NULL, &user, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 2U);
	struct chat_message *built =
		static_cast<struct chat_message *>(messages.elts);
	EXPECT_STREQ(built[0].role, "assistant");
	EXPECT_TRUE(built[0].is_context_summary);
	EXPECT_NE(strstr(built[0].content, "earlier context summary"), nullptr);
	EXPECT_STREQ(built[1].role, "user");
	EXPECT_STREQ(built[1].content, "exact current request");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(AgentTurnTest, InTurnCompactionPreservesHistoryWhenSummaryFails)
{
	struct model_history_insert old = {};

	old.session_id = sess.id;
	old.turn_id = "turn_fallback";
	old.kind = "user_message";
	old.role = "user";
	old.content = "important active context";
	old.token_count = 10;
	old.active = 1;
	ASSERT_EQ(model_history_add(&db, &old, nullptr), 0);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->compress.max_context_tokens = 100;
	react->compress.max_history_rounds = 2;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 20;
	react->compress.compaction_summary_max_tokens = 20;
	react->compress.summarize = agent_turn_failing_summary;
	ASSERT_EQ(react_set_turn_id(react, "turn_fallback"), 0);
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_EQ(agent_history_compact_with_trigger(react, 1, "in_turn_1"), -EIO);

	int count = 0;
	struct model_history_item *items = model_history_list(&db, sess.id, 1,
		&count);
	bool found_fallback = false;
	for (struct model_history_item *item = items; item;
	     item = item->next) {
		if (std::strcmp(item->kind, "compaction_summary") == 0 &&
		    item->content && std::strstr(item->content,
			"could not be summarized"))
			found_fallback = true;
	}
	EXPECT_FALSE(found_fallback);
	EXPECT_EQ(count, 1);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->content, "important active context");
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, PreferenceIsSavedBeforeModelAndSurvivesCancellation)
{
	struct memory_options options = {};
	options.enabled = 1;
	options.hot_path_enabled = 1;
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.memory_options = &options;
	struct agent_turn_input input = {};
	input.model_input = "以后都用中文回答";
	input.turn_id = "preference-turn";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	ASSERT_NE(react->memory_context, nullptr);
	EXPECT_NE(strstr(react->memory_context, "response.language: Chinese"), nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_CANCELLED;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
	char *effective = preference_render(&db, sess.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	free(effective);
}

TEST_F(AgentTurnTest, CompactionReplayFailureRollsBackDatabaseAndMemory)
{
	struct model_history_insert item = {};
	item.session_id = sess.id;
	item.kind = "assistant_tool_calls";
	item.role = "assistant";
	item.payload_json = "{broken payload";
	item.token_count = 1;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->history_enabled = 1;
	react->compress.max_context_tokens = 10000;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 100;
	react->compress.summarize = agent_turn_summary;
	ASSERT_EQ(agent_history_reload(react), 0);
	auto *original = react->history_items;
	EXPECT_EQ(agent_history_compact(react, 1), MORPH_ERR_PARSE);
	EXPECT_EQ(react->history_items, original);
	EXPECT_EQ(model_history_compaction_count(&db, sess.id), 0);
	EXPECT_EQ(model_history_count(&db, sess.id, 0), 1);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
	EXPECT_TRUE(sqlite3_get_autocommit(db.handle));
}

static int agent_turn_unicode_summary(const char *, void *, char **out)
{
	std::string summary;
	for (int i = 0; i < 300; i++)
		summary += "待验证的构建结果🙂";
	*out = strdup(summary.c_str());
	return *out ? 0 : -ENOMEM;
}

TEST_F(AgentTurnTest, OversizedUnicodeCheckpointIsRejectedWithoutTruncation)
{
	struct model_history_insert item = {};
	item.session_id = sess.id;
	item.kind = "user_message";
	item.content = "objective";
	item.token_count = 1;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->history_enabled = 1;
	react->compress.max_context_tokens = 10000;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_summary_max_tokens = 30;
	react->compress.summarize = agent_turn_unicode_summary;
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_EQ(agent_history_compact(react, 1), MORPH_ERR_PROTOCOL);
	ASSERT_NE(react->history_items, nullptr);
	EXPECT_STREQ(react->history_items->kind, "user_message");
	EXPECT_STREQ(react->history_items->content, "objective");
	EXPECT_EQ(model_history_compaction_count(&db, sess.id), 0);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
}

TEST_F(AgentTurnTest, RestoredAnswerUsesCurrentTokenizerBeforeBudgetSelection)
{
	struct model_history_insert item = {};
	item.session_id = sess.id;
	item.turn_id = "old_answer";
	item.kind = "user_message";
	item.role = "user";
	item.content = "fix";
	item.token_count = 1;
	item.active = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	std::string large(4000, 'x');
	item.kind = "assistant_message";
	item.role = "assistant";
	item.content = large.c_str();
	item.active = 0;
	item.token_count = 1;
	ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->history_enabled = 1;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = 0.1;
	react->compress.compaction_user_message_tokens = 100;
	react->compress.compaction_summary_max_tokens = 20;
	react->compress.summarize = agent_turn_summary;
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	int count = 0;
	auto *items = model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(items->kind, "compaction_summary");
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, NormalCompactionUsesConfiguredRecentRounds)
{
	for (int i = 0; i < 3; i++) {
		std::string turn = "turn_" + std::to_string(i);
		struct model_history_insert item = {};
		item.session_id = sess.id;
		item.turn_id = turn.c_str();
		item.kind = "user_message";
		item.role = "user";
		item.content = "request";
		item.token_count = tokenizer_count(tok, item.content);
		item.active = 1;
		ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
		item.kind = "assistant_message";
		item.role = "assistant";
		item.content = "answered";
		item.token_count = tokenizer_count(tok, item.content);
		ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	}
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->history_enabled = 1;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 0;
	react->compress.max_history_rounds = 2;
	react->compress.summarize = agent_turn_summary;
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	int users = 0;
	for (auto *it = react->history_items; it; it = it->next) {
		if (strcmp(it->kind, "user_message") == 0) {
			EXPECT_STRNE(it->turn_id, "turn_0");
			users++;
		}
	}
	EXPECT_EQ(users, 2);
}

TEST_F(AgentTurnTest, ToolHistoryKeepsOriginalAndDeduplicatesModelContent)
{
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	const char *input = "{\"ok\":true,\"data\":{\"stdout\":\"hello\","
			    "\"exit_code\":0},\"stdout\":\"hello\",\"exit_code\":0}";
	ASSERT_EQ(agent_history_record_tool_result(react, "dedup", nullptr, "exec", input, 0), 0);
	int count = 0;
	auto *items = model_history_list(&db, sess.id, 1, &count);
	ASSERT_NE(items, nullptr);
	cJSON *model = cJSON_Parse(items->content);
	ASSERT_NE(model, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(model, "stdout"), nullptr);
	EXPECT_NE(cJSON_GetObjectItem(cJSON_GetObjectItem(model, "data"), "stdout"), nullptr);
	cJSON *payload = cJSON_Parse(items->payload_json);
	ASSERT_NE(payload, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "original_content")), input);
	cJSON_Delete(payload);
	cJSON_Delete(model);
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, JsonToolTruncationPreservesExecutionControls)
{
	react->history_tool_result_tokens = 100;
	std::string input = "{\"ok\":true,\"data\":{\"stdout\":\"";
	input += std::string(5000, 'x');
	input += "\",\"exit_code\":7,\"session_id\":\"proc_test\"}}";
	char *prepared = nullptr;
	int truncated = 0;
	ASSERT_EQ(agent_history_prepare_tool_content(react, input.c_str(), &prepared, &truncated),
		  0);
	EXPECT_EQ(truncated, 1);
	cJSON *json = cJSON_Parse(prepared);
	ASSERT_NE(json, nullptr);
	cJSON *data = cJSON_GetObjectItem(json, "data");
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "exit_code")), 7);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "session_id")), "proc_test");
	EXPECT_LE(tokenizer_count(react->tokenizer, prepared), 100);
	cJSON_Delete(json);
	free(prepared);
}

TEST_F(AgentTurnTest, GenericJsonTruncationReturnsAnExplicitPreview)
{
	react->history_tool_result_tokens = 100;
	std::string input = "[\"" + std::string(5000, 'x') + "\"]";
	char *prepared = nullptr;
	int truncated = 0;
	ASSERT_EQ(agent_history_prepare_tool_content(react, input.c_str(), &prepared, &truncated),
		  0);
	cJSON *json = cJSON_Parse(prepared);
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(truncated, 1);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(json, "history_truncated")));
	EXPECT_EQ(cJSON_GetObjectItem(json, "ok"), nullptr);
	EXPECT_LE(tokenizer_count(react->tokenizer, prepared), 100);
	cJSON_Delete(json);
	free(prepared);
}

TEST_F(AgentTurnTest, RepeatedCompactionSummarizesOldFactsAndKeepsRecentExchanges)
{
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.compaction_summary_max_tokens = 100;
	react->compress.compaction_user_message_tokens = 80;
	ASSERT_EQ(react_set_turn_id(react, "long-running-task"), 0);
	ASSERT_EQ(agent_history_record_user(react, "Render the video and verify it"), 0);
	auto add_group = [&](const char *id, const char *command, const char *result,
		const char *reasoning) {
		struct tool_call call = {};
		std::strcpy(call.id, id);
		std::strcpy(call.tool_call_id, id);
		std::strcpy(call.name, "exec");
		call.arguments = const_cast<char *>(command);
		EXPECT_EQ(agent_history_record_tool_calls(react, "Visible action", reasoning,
			&call, 1), 0);
		EXPECT_EQ(agent_history_record_tool_result(react, id, id, "exec", result, 0), 0);
	};
	std::string bulky = "OLD_RENDER_EVIDENCE /tmp/video.mp4 ";
	for (int i = 0; i < 1000; i++)
		bulky += "frame passed ";
	add_group("old-render", "{\"command\":\"render\"}", bulky.c_str(),
		"PRIVATE_REASONING_MUST_NOT_ENTER_SUMMARY");
	add_group("recent-review", "{\"command\":\"inspect-current-frame\"}",
		"RECENT_EXACT_RESULT", "RECENT_REASONING");
	std::string captured;
	react->compress.summarize_user_data = &captured;
	react->compress.summarize = [](const char *input, void *data, char **out) -> int {
		*static_cast<std::string *>(data) = input;
		*out = strdup("Video rendered at /tmp/video.mp4; review pending; render passed.");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	EXPECT_NE(captured.find("OLD_RENDER_EVIDENCE"), std::string::npos);
	EXPECT_NE(captured.find("Visible action"), std::string::npos);
	EXPECT_EQ(captured.find("PRIVATE_REASONING"), std::string::npos);
	EXPECT_EQ(captured.find("RECENT_EXACT_RESULT"), std::string::npos);
	EXPECT_EQ(captured.find("inspect-current-frame"), std::string::npos);
	bool kept = false;
	for (auto *item = react->history_items; item; item = item->next) {
		if (!std::strcmp(item->kind, "assistant_tool_calls")) {
			kept = true;
			EXPECT_NE(std::strstr(item->payload_json, "RECENT_REASONING"), nullptr);
		}
	}
	EXPECT_TRUE(kept);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
		"SELECT count(*) FROM history_summary_sources s JOIN model_history_items m "
		"ON s.source_item_id=m.id WHERE m.tool_call_id='recent-review'",
		-1, &stmt, nullptr), SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(stmt, 0), 0);
	sqlite3_finalize(stmt);
	add_group("bulky-review", "{\"command\":\"review\"}", bulky.c_str(), "PRIVATE_REASONING_2");
	add_group("new-review", "{\"command\":\"check-latest\"}", "LATEST_EXACT_RESULT", nullptr);
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	EXPECT_NE(captured.find("Video rendered at /tmp/video.mp4"), std::string::npos);
	EXPECT_EQ(captured.find("PRIVATE_REASONING"), std::string::npos);
	EXPECT_EQ(captured.find("LATEST_EXACT_RESULT"), std::string::npos);
	EXPECT_EQ(model_history_compaction_count(&db, sess.id), 2);
}

TEST_F(AgentTurnTest, CompactionRejectsHistoryGrowthAfterSnapshotWithoutLosingNewResults)
{
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	ASSERT_EQ(react_set_turn_id(react, "snapshot-test"), 0);
	ASSERT_EQ(agent_history_record_user(react, "original objective"), 0);
	react->compress.summarize_user_data = react;
	react->compress.summarize = [](const char *, void *data, char **out) -> int {
		auto *ctx = static_cast<react_context *>(data);
		EXPECT_TRUE(sqlite3_get_autocommit(ctx->history_db->handle));
		struct model_history_insert item = {};
		item.session_id = ctx->history_session_id;
		item.kind = "background_receipt";
		item.role = "system";
		item.content = "New tool result arrived during generation";
		item.active = 1;
		EXPECT_EQ(model_history_add(ctx->history_db, &item, nullptr), 0);
		*out = strdup("Earlier objective checkpoint");
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_EQ(agent_history_compact(react, 1), -EAGAIN);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 2);
	EXPECT_EQ(model_history_compaction_count(&db, sess.id), 0);
	EXPECT_TRUE(sqlite3_get_autocommit(db.handle));
}

TEST_F(AgentTurnTest, OversizedCheckpointIsRewrittenWithoutLosingVerification)
{
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->compress.max_context_tokens = 1000;
	react->compress.compress_target_ratio = .5;
	react->compress.compaction_summary_max_tokens = 100;
	ASSERT_EQ(react_set_turn_id(react, "rewrite-test"), 0);
	ASSERT_EQ(agent_history_record_user(react, "original objective"), 0);
	int attempts = 0;
	react->compress.summarize_user_data = &attempts;
	react->compress.summarize = [](const char *input, void *data, char **out) -> int {
		if (++*static_cast<int *>(data) == 1) {
			std::string large;
			for (int i = 0; i < 1000; i++)
				large += "completed work ";
			large += "Verification: build passed; visual review pending";
			*out = strdup(large.c_str());
		} else {
			EXPECT_NE(std::strstr(input, "at most 50 tokens"), nullptr);
			*out = strdup("Completed: rendered. Pending: visual review. Verification: build passed.");
		}
		return *out ? 0 : -ENOMEM;
	};
	ASSERT_EQ(agent_history_reload(react), 0);
	ASSERT_EQ(agent_history_compact(react, 1), 1);
	EXPECT_EQ(attempts, 2);
	ASSERT_NE(react->history_items, nullptr);
	EXPECT_NE(std::strstr(react->history_items->content, "Verification: build passed."), nullptr);
}
