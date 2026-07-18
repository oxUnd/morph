#include <gtest/gtest.h>

extern "C" {
#include "runtime/context.h"
#include "runtime/scheduler.h"
#include "runtime/turn_scope.h"
#include "runtime/usage.h"
#include "models/llm.h"
}

#include <cstdlib>
#include <cstring>

TEST(RuntimeInternalTest, MemoryOptionsMirrorConfiguration)
{
	struct config config{};
	config.memory.enabled = 1;
	config.memory.hot_path_enabled = 1;
	config.memory.cold_path_enabled = 0;
	config.memory.llm_extract_enabled = 1;
	config.memory.max_facts = 12;
	config.memory.max_episodes = 7;
	config.memory.max_procedures = 4;
	config.memory.max_context_chars = 4096;

	struct memory_options options = runtime_memory_options_from_config(&config);
	EXPECT_EQ(options.enabled, 1);
	EXPECT_EQ(options.hot_path_enabled, 1);
	EXPECT_EQ(options.cold_path_enabled, 0);
	EXPECT_EQ(options.llm_extract_enabled, 1);
	EXPECT_EQ(options.max_facts, 12);
	EXPECT_EQ(options.max_episodes, 7);
	EXPECT_EQ(options.max_procedures, 4);
	EXPECT_EQ(options.max_context_chars, 4096);
}

TEST(RuntimeInternalTest, CreditSessionKeyPrefersBoundScopeThenDisplayId)
{
	struct runtime_turn_scope scope{};
	struct session session{};
	char key[64];

	session.id = 42;
	std::strncpy(session.name, "named", sizeof(session.name) - 1);
	std::strncpy(session.display_id, "display-42",
		sizeof(session.display_id) - 1);
	runtime_credit_session_key(nullptr, &session, key, sizeof(key));
	EXPECT_STREQ(key, "display-42");
	scope.bound = 1;
	std::strncpy(scope.credit_session_id, "turn-session",
		sizeof(scope.credit_session_id) - 1);
	runtime_credit_session_key(&scope, &session, key, sizeof(key));
	EXPECT_STREQ(key, "turn-session");
	runtime_credit_session_key(nullptr, nullptr, key, sizeof(key));
	EXPECT_STREQ(key, "");
}

TEST(RuntimeInternalTest, PlanRegistryStateIsIsolatedBySession)
{
	struct runtime_plan_session sessions[2]{};
	struct plan_registry active{};
	int64_t active_id = 0;

	plan_registry_init(&active);
	runtime_plan_session_select(sessions, 2, &active_id, &active, 10);
	EXPECT_EQ(active_id, 10);
	active.count = 3;
	runtime_plan_session_select(sessions, 2, &active_id, &active, 20);
	EXPECT_EQ(active_id, 20);
	EXPECT_EQ(active.count, 0);
	active.count = 1;
	runtime_plan_session_select(sessions, 2, &active_id, &active, 10);
	EXPECT_EQ(active.count, 3);
	runtime_plan_session_forget(sessions, 2, &active_id, &active, 10);
	EXPECT_EQ(active_id, 0);
	EXPECT_EQ(active.count, 0);
}

TEST(RuntimeInternalTest, UsageClassificationAndMetadataAreStable)
{
	struct model_usage usage{};
	EXPECT_EQ(runtime_model_usage_is_billable(nullptr), 0);
	EXPECT_EQ(runtime_model_usage_is_billable(&usage), 0);
	usage.input_tokens = 10;
	usage.output_tokens = 2;
	usage.total_tokens = 12;
	usage.cached_tokens = 3;
	std::strncpy(usage.model, "actual-model", sizeof(usage.model) - 1);
	std::strncpy(usage.response_id, "response-1",
		sizeof(usage.response_id) - 1);
	EXPECT_EQ(runtime_model_usage_is_billable(&usage), 1);
	char *metadata = runtime_model_usage_metadata(&usage);
	ASSERT_NE(metadata, nullptr);
	EXPECT_NE(std::strstr(metadata, "actual-model"), nullptr);
	EXPECT_NE(std::strstr(metadata, "response-1"), nullptr);
	EXPECT_NE(std::strstr(metadata, "cached_tokens"), nullptr);
	std::free(metadata);
}

TEST(RuntimeInternalTest, UsageBindingRestoresPreviousValue)
{
	int first = 1;
	int second = 2;
	model_set_usage_user_data(&first);
	void *previous = runtime_usage_bind(&second);
	EXPECT_EQ(previous, &first);
	EXPECT_EQ(model_get_usage_user_data(), &second);
	runtime_usage_restore(previous);
	EXPECT_EQ(model_get_usage_user_data(), &first);
	model_set_usage_user_data(nullptr);
}

TEST(RuntimeInternalTest, ScheduledTaskPayloadHelpersHandleValidAndInvalidJson)
{
	struct scheduled_task task{};
	std::strncpy(task.title, "Daily report", sizeof(task.title) - 1);
	task.payload_json = const_cast<char *>(
		"{\"prompt\":\"User request: summarize today\\nDetails\","
		"\"turn_id\":\"turn-7\"}");
	char *prompt = runtime_scheduled_task_prompt(&task);
	char *turn_id = runtime_scheduled_task_turn_id(&task);
	char *display = runtime_scheduled_task_display_prompt(&task, prompt);
	ASSERT_NE(prompt, nullptr);
	ASSERT_NE(turn_id, nullptr);
	ASSERT_NE(display, nullptr);
	EXPECT_STREQ(prompt, "User request: summarize today\nDetails");
	EXPECT_STREQ(turn_id, "turn-7");
	EXPECT_STREQ(display, "Daily report: summarize today");
	std::free(prompt);
	std::free(turn_id);
	std::free(display);
	task.payload_json = const_cast<char *>("not-json");
	EXPECT_EQ(runtime_scheduled_task_prompt(&task), nullptr);
	EXPECT_EQ(runtime_scheduled_task_turn_id(&task), nullptr);
}

TEST(RuntimeInternalTest, ErrorAndNotificationTextUseReactSnapshot)
{
	struct react_context react{};
	react.outcome = REACT_OUTCOME_LLM_ERROR;
	std::strncpy(react.outcome_reason, "provider failed",
		sizeof(react.outcome_reason) - 1);
	react.final_answer = const_cast<char *>("partial answer");
	char *error = runtime_react_error_message(&react, -EIO);
	char *notification = runtime_react_notification_body(&react);
	ASSERT_NE(error, nullptr);
	ASSERT_NE(notification, nullptr);
	EXPECT_NE(std::strstr(error, "partial answer"), nullptr);
	EXPECT_NE(std::strstr(error, "provider failed"), nullptr);
	EXPECT_STREQ(notification, "partial answer");
	std::free(error);
	std::free(notification);
}
