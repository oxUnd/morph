#include "model_metadata_test_support.hpp"

extern "C" {
#include "runtime/recap.h"
#include "sapi/cli/cli.h"
#include "sapi/cli/recap.h"
#include "sapi/cli/terminal.h"
int cli_presentation_event(struct cli_context *ctx, const struct morph_event *event);
}

class RecapTest : public RuntimeMetadataTest {
protected:
	void Conversation()
	{
		answer = R"({"summary":"Implemented the fix; installation remains unverified.","next_action":null})";
		Start();
		int64_t id = instance->context.current_session.id;
		ASSERT_EQ(message_add(&instance->context.database, id, "user", "Fix the issue", 3), 0);
		ASSERT_EQ(message_add(&instance->context.database, id, "assistant", "Fixed and tested", 3), 0);
	}

	cJSON *WaitEnd()
	{
		std::unique_lock<std::mutex> guard(lock);
		bool ready = condition.wait_for(guard, std::chrono::seconds(4), [this] {
			return recap_events.size() >= 2;
		});
		if (!ready)
			return nullptr;
		cJSON *event = cJSON_Parse(recap_events.back().c_str());
		cJSON *data = cJSON_Duplicate(cJSON_GetObjectItem(event, "data"), 1);
		cJSON_Delete(event);
		return data;
	}

	std::string Render(cJSON *data, struct cli_context *ctx)
	{
		struct morph_event event{MORPH_EVENT_BACKGROUND, "session.recap", "end",
			"Conversation recap finished", data, nullptr};
		testing::internal::CaptureStdout();
		cli_recap_event(ctx, &event);
		return testing::internal::GetCapturedStdout();
	}
};

TEST(RecapParser, RequiresBoundedStructuredOutput)
{
	for (const char *text : {"plain text", "{}", "{\"summary\":\"done\"}",
		"{\"summary\":\" \",\"next_action\":null}",
		"{\"summary\":\"done\",\"next_action\":1}",
		"{\"summary\":\"done\",\"next_action\":null,\"extra\":true}"}) {
		cJSON *result = runtime_recap_parse(text);
		EXPECT_EQ(result, nullptr);
		cJSON_Delete(result);
	}
	std::string text = "{\"summary\":\"" + std::string(701, 'x') +
		"\",\"next_action\":null}";
	EXPECT_EQ(runtime_recap_parse(text.c_str()), nullptr);
	cJSON *result = runtime_recap_parse(
		R"({"summary":"  已发布，但尚未安装验证。  ","next_action":null})");
	ASSERT_NE(result, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "summary")),
		"已发布，但尚未安装验证。");
	cJSON_Delete(result);
}

TEST(RecapScheduling, RequiresUnfocusedIdleTimeAndNewProgress)
{
	struct cli_recap_state state{};
	state.progress.session_id = 1;
	state.progress.history_id = 2;
	state.unfocused_since = 1000;
	state.last_finished = 2000;
	state.progress.completed_turns = 3;
	EXPECT_FALSE(cli_recap_due(&state, 1000 + CLI_RECAP_DELAY_MS));
	EXPECT_TRUE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
	state.progress.completed_turns = 2;
	EXPECT_FALSE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
	state.progress.completed_turns = 3;
	state.pending = 1;
	EXPECT_FALSE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
	state.pending = 0;
	state.has_recap = 1;
	state.last_recapped_turns = 3;
	state.progress.completed_turns = 4;
	EXPECT_FALSE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
	state.progress.completed_turns = 5;
	EXPECT_TRUE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
	state.unfocused_since = 0;
	EXPECT_FALSE(cli_recap_due(&state, 2000 + CLI_RECAP_DELAY_MS));
}

TEST(RecapScheduling, MissingSessionOrHistoryNeverBecomesDueEvenForRetry)
{
	struct cli_recap_state state{};
	state.unfocused_since = 1;
	state.last_finished = 1;
	state.progress.completed_turns = 3;
	state.retry_at = 1;
	EXPECT_FALSE(cli_recap_due(&state, 2 * CLI_RECAP_DELAY_MS));
	state.progress.session_id = 1;
	EXPECT_FALSE(cli_recap_due(&state, 2 * CLI_RECAP_DELAY_MS));
	state.progress.history_id = 2;
	EXPECT_TRUE(cli_recap_due(&state, 2 * CLI_RECAP_DELAY_MS));
	state.progress.session_id = 0;
	EXPECT_FALSE(cli_recap_due(&state, 2 * CLI_RECAP_DELAY_MS));
}

TEST_F(RuntimeFacadeTest, DeferredSessionSkipsRecapPollingAndClearsStaleRetry)
{
	struct runtime_options options{};
	options.create_new_session = 1;
	Open(&options);
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.recap.progress = {99, 100, 1, 3};
	ctx.recap.has_recap = 1;
	ctx.recap.last_finished = 1;
	ctx.recap.retry_at = 1;
	ctx.recap.retried = 1;
	cli_recap_focus(&ctx, 0, 1);
	/* The empty-session path must skip progress queries even while execution is locked. */
	pthread_mutex_lock(&instance->context.execution_lock);
	testing::internal::CaptureStderr();
	for (int i = 1; i <= 3; i++)
		cli_recap_tick(&ctx, i * CLI_RECAP_DELAY_MS);
	std::string diagnostics = testing::internal::GetCapturedStderr();
	pthread_mutex_unlock(&instance->context.execution_lock);
	EXPECT_TRUE(diagnostics.empty()) << diagnostics;
	EXPECT_EQ(ctx.recap.progress.session_id, 0);
	EXPECT_EQ(ctx.recap.retry_at, 0);
	EXPECT_EQ(ctx.recap.retried, 0);
	EXPECT_FALSE(ctx.recap.has_recap);
	EXPECT_FALSE(ctx.recap.pending);
	EXPECT_EQ(instance->recap_job, nullptr);
}

TEST_F(RecapTest, BackgroundRequestUsesOnlyVisibleBoundedHistory)
{
	Conversation();
	int64_t id = instance->context.current_session.id;
	for (int i = 0; i < 10; i++) {
		ASSERT_EQ(message_add(&instance->context.database, id, "user",
			("request-" + std::to_string(i)).c_str(), 1), 0);
		ASSERT_EQ(message_add(&instance->context.database, id, "assistant",
			("answer-" + std::to_string(i)).c_str(), 1), 0);
	}
	ASSERT_EQ(message_add(&instance->context.database, id, "tool", "SECRET_TOOL_OUTPUT", 1), 0);
	ASSERT_EQ(message_add(&instance->context.database, id, "reasoning", "HIDDEN_REASONING", 1), 0);
	std::string huge;
	for (int i = 0; i < 40000; i++)
		huge += "猫";
	ASSERT_EQ(message_add(&instance->context.database, id, "user", huge.c_str(), 1), 0);
	int before = message_count(&instance->context.database, id);
	struct runtime_recap_progress progress{};
	auto started = TitleClock::now();
	ASSERT_EQ(runtime_recap_request(instance, 0, &progress), 0);
	EXPECT_LT(TitleClock::now() - started, std::chrono::seconds(1));
	ASSERT_TRUE(WaitRequest());
	EXPECT_EQ(runtime_recap_request(instance, 0, nullptr), -EALREADY);
	cJSON *body = cJSON_Parse(request_body.c_str());
	ASSERT_NE(body, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(body, "tools"), nullptr);
	const cJSON *messages = cJSON_GetObjectItem(body, "messages");
	size_t bytes = 0;
	for (int i = 0; i < cJSON_GetArraySize(messages); i++) {
		const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(
			cJSON_GetArrayItem(messages, i), "content"));
		ASSERT_NE(content, nullptr);
		EXPECT_EQ(utf8valid(content), nullptr);
		bytes += strlen(content);
		EXPECT_EQ(strstr(content, "SECRET_TOOL_OUTPUT"), nullptr);
		EXPECT_EQ(strstr(content, "HIDDEN_REASONING"), nullptr);
		EXPECT_EQ(strstr(content, "request-0"), nullptr);
	}
	EXPECT_LE(bytes, RECAP_PROMPT_MAX_BYTES);
	cJSON_Delete(body);
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "error_code")), 0);
	EXPECT_TRUE(runtime_recap_is_current(instance, &progress));
	EXPECT_EQ(message_count(&instance->context.database, id), before);
	EXPECT_EQ(model_history_compaction_count(&instance->context.database, id), 0);
	cJSON_Delete(data);
}

TEST_F(RecapTest, RecapRendersWithoutInventingNextAndPreservesDraftState)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	ASSERT_EQ(runtime_recap_request(instance, 0, &ctx.recap.request), 0);
	ctx.recap.pending = 1;
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	std::string output = Render(data, &ctx);
	EXPECT_NE(output.find("↳ Recap:"), std::string::npos);
	EXPECT_NE(output.find("installation remains unverified"), std::string::npos);
	EXPECT_EQ(output.find("Next:"), std::string::npos);
	EXPECT_FALSE(ctx.recap.pending);
	EXPECT_TRUE(ctx.recap.has_recap);
	cJSON_Delete(data);
}

TEST_F(RecapTest, StructuredModeSettlesTheRequestWithoutHumanText)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ASSERT_EQ(runtime_recap_request(instance, 0, &ctx.recap.request), 0);
	ctx.recap.pending = 1;
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	struct morph_event event{MORPH_EVENT_BACKGROUND, "session.recap", "end",
		"Conversation recap finished", data, nullptr};
	testing::internal::CaptureStdout();
	EXPECT_EQ(cli_presentation_event(&ctx, &event), 0);
	std::string output = testing::internal::GetCapturedStdout();
	cJSON *serialized = cJSON_Parse(output.c_str());
	EXPECT_NE(serialized, nullptr);
	EXPECT_EQ(output.find("↳ Recap:"), std::string::npos);
	EXPECT_FALSE(ctx.recap.pending);
	EXPECT_EQ(ctx.recap.result_code, 0);
	cJSON_Delete(serialized);
	cJSON_Delete(data);
}

TEST_F(RecapTest, SwitchingAwayAndBackDiscardsCompletedResult)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	int64_t origin = instance->context.current_session.id;
	ASSERT_EQ(runtime_recap_request(instance, 0, &ctx.recap.request), 0);
	ctx.recap.pending = 1;
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	struct session other{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "Other task", &other), 0);
	ASSERT_EQ(runtime_session_select_existing(instance, origin, nullptr), 0);
	EXPECT_TRUE(Render(data, &ctx).empty());
	EXPECT_FALSE(runtime_recap_is_current(instance, &ctx.recap.request));
	cJSON_Delete(data);
}

TEST_F(RecapTest, NewMessagesInvalidateQueuedResult)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	ASSERT_EQ(runtime_recap_request(instance, 0, &ctx.recap.request), 0);
	ctx.recap.pending = 1;
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	ASSERT_EQ(message_add(&instance->context.database, ctx.recap.request.session_id,
		"user", "Actually, change the goal", 3), 0);
	EXPECT_TRUE(Render(data, &ctx).empty());
	cJSON_Delete(data);
}

TEST_F(RecapTest, AutomaticRecapCanBeDisabledButManualStillWorks)
{
	Conversation();
	instance->context.config.general.auto_recap = 0;
	for (int i = 0; i < 2; i++) {
		ASSERT_EQ(message_add(&instance->context.database,
			instance->context.current_session.id, "user", "Continue", 1), 0);
		ASSERT_EQ(message_add(&instance->context.database,
			instance->context.current_session.id, "assistant", "Done", 1), 0);
	}
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.input_status.enabled = 1;
	ctx.recap.unfocused_since = 1;
	ctx.recap.last_finished = 1;
	ASSERT_EQ(runtime_recap_progress(instance, &ctx.recap.progress), 0);
	ASSERT_EQ(ctx.recap.progress.completed_turns, 3);
	cli_recap_tick(&ctx, CLI_RECAP_DELAY_MS + 1);
	EXPECT_FALSE(ctx.recap.pending);
	EXPECT_EQ(instance->recap_job, nullptr);
	ASSERT_EQ(cli_recap_start(&ctx, 0), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	EXPECT_NE(Render(data, &ctx).find("Recap:"), std::string::npos);
	cJSON_Delete(data);
}

TEST_F(RecapTest, IdleUnfocusedProgressAutomaticallyRequestsOneRecap)
{
	Conversation();
	for (int i = 0; i < 2; i++) {
		ASSERT_EQ(message_add(&instance->context.database,
			instance->context.current_session.id, "user", "Continue", 1), 0);
		ASSERT_EQ(message_add(&instance->context.database,
			instance->context.current_session.id, "assistant", "Done", 1), 0);
	}
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.input_status.enabled = 1;
	ASSERT_EQ(runtime_recap_progress(instance, &ctx.recap.progress), 0);
	ctx.recap.unfocused_since = 1;
	ctx.recap.last_finished = 1;
	cli_recap_tick(&ctx, CLI_RECAP_DELAY_MS + 1);
	EXPECT_TRUE(ctx.recap.pending);
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(data, "automatic")));
	EXPECT_NE(Render(data, &ctx).find("Recap:"), std::string::npos);
	EXPECT_EQ(ctx.recap.last_recapped_turns, 3);
	EXPECT_FALSE(cli_recap_due(&ctx.recap, 3 * CLI_RECAP_DELAY_MS));
	cJSON_Delete(data);
}

TEST_F(RecapTest, FastAutomaticFailureKeepsItsBoundedRetryDeadline)
{
	Conversation();
	answer = "invalid output";
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.input_status.enabled = 1;
	ASSERT_EQ(cli_recap_start(&ctx, 1), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	EXPECT_TRUE(Render(data, &ctx).empty());
	int64_t retry_at = ctx.recap.retry_at;
	EXPECT_GT(retry_at, cli_recap_now());
	cli_recap_tick(&ctx, cli_recap_now());
	EXPECT_EQ(ctx.recap.retry_at, retry_at);
	EXPECT_EQ(ctx.recap.retried, 1);
	cJSON_Delete(data);
}

TEST_F(RecapTest, FocusReturnCancelsAutomaticAndShutdownIsBounded)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.input_status.enabled = 1;
	ASSERT_EQ(cli_recap_start(&ctx, 1), 0);
	ASSERT_TRUE(WaitRequest());
	cli_recap_focus(&ctx, 1, cli_recap_now());
	EXPECT_FALSE(runtime_recap_is_current(instance, &ctx.recap.request));
	auto started = TitleClock::now();
	runtime_close(instance);
	instance = nullptr;
	ctx.runtime = nullptr;
	EXPECT_LT(TitleClock::now() - started, std::chrono::seconds(3));
}

TEST_F(RecapTest, ManualRecapSurvivesFocusReturnAndSwitchingRemembersProgress)
{
	Conversation();
	struct cli_context ctx{};
	ctx.runtime = instance;
	ctx.input_status.enabled = 1;
	int64_t origin = instance->context.current_session.id;
	ASSERT_EQ(cli_recap_start(&ctx, 0), 0);
	ASSERT_TRUE(WaitRequest());
	cli_recap_focus(&ctx, 1, cli_recap_now());
	EXPECT_TRUE(runtime_recap_is_current(instance, &ctx.recap.request));
	Release();
	cJSON *data = WaitEnd();
	ASSERT_NE(data, nullptr);
	EXPECT_NE(Render(data, &ctx).find("Recap:"), std::string::npos);
	struct session other{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "Other task", &other), 0);
	cli_recap_tick(&ctx, cli_recap_now() + 2000);
	EXPECT_FALSE(ctx.recap.has_recap);
	ASSERT_EQ(runtime_session_select_existing(instance, origin, nullptr), 0);
	cli_recap_tick(&ctx, cli_recap_now() + 4000);
	EXPECT_TRUE(ctx.recap.has_recap);
	EXPECT_EQ(ctx.recap.last_recapped_turns, 1);
	cJSON_Delete(data);
}

TEST_F(RecapTest, MissingHistoryAndForegroundExecutionHaveExplicitErrors)
{
	Start();
	EXPECT_EQ(runtime_recap_request(instance, 0, nullptr), -ENODATA);
	pthread_mutex_lock(&instance->context.execution_lock);
	EXPECT_EQ(runtime_recap_request(instance, 0, nullptr), -EBUSY);
	pthread_mutex_unlock(&instance->context.execution_lock);
}
