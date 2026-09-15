#include <gtest/gtest.h>

extern "C" {
#include "credits.h"
#include "agent/system_prompt.h"
#include "runtime/bootstrap.h"
#include "runtime/context.h"
#include "runtime/scheduler.h"
#include "runtime/turn_scope.h"
#include "runtime/usage.h"
#include "models/llm.h"
#include "util/file.h"
}

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

class PromptLoadTest : public ::testing::Test {
protected:
	char dir[PATH_MAX] = "/tmp/morph-prompt-XXXXXX";
	char *text = nullptr;

	void SetUp() override {
		ASSERT_NE(mkdtemp(dir), nullptr);
	}

	void TearDown() override {
		std::free(text);
		std::filesystem::remove_all(dir);
	}

	std::string write(const char *name, const std::string &content) {
		std::string path = std::string(dir) + "/" + name;
		EXPECT_EQ(file_write_all(path.c_str(), content.data(), content.size()), 0);
		return path;
	}
};

TEST_F(PromptLoadTest, NoSourceDiffersFromExplicitEmptyFile)
{
	ASSERT_EQ(morph_prompt_load(nullptr, "", &text), 0);
	EXPECT_EQ(text, nullptr);
	auto path = write("empty.txt", " \t\r\n");
	ASSERT_EQ(morph_prompt_load(path.c_str(), nullptr, &text), 0);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "");
	EXPECT_EQ(morph_prompt_load(nullptr, nullptr, nullptr), -EINVAL);
}

TEST_F(PromptLoadTest, FilePrecedesSortedDirectoryFragments)
{
	std::string fragments = std::string(dir) + "/fragments";
	ASSERT_TRUE(std::filesystem::create_directory(fragments));
	write("fragments/20-b.txt", "second\n");
	write("fragments/10-a.md", "first\r\n");
	write("fragments/15-empty.txt", "\n");
	write("fragments/.hidden", "ignored");
	auto path = write("base.txt", "base %s %d\n");
	ASSERT_EQ(morph_prompt_load(path.c_str(), fragments.c_str(), &text), 0);
	EXPECT_STREQ(text, "base %s %d\n\nfirst\n\nsecond");
	std::string snapshot = text;
	std::free(text);
	text = nullptr;
	ASSERT_EQ(morph_prompt_load(path.c_str(), fragments.c_str(), &text), 0);
	EXPECT_EQ(snapshot, text);
}

TEST_F(PromptLoadTest, LargeUtf8FileIsNotTruncated)
{
	std::string content(3 * BUFSIZ, 'x');
	content += "\xe4\xb8\xad\xe6\x96\x87";
	auto path = write("large.txt", content);
	ASSERT_EQ(morph_prompt_load(path.c_str(), "", &text), 0);
	EXPECT_EQ(content, text);
}

TEST_F(PromptLoadTest, RejectsInvalidEncodingAndEmbeddedNul)
{
	auto path = write("invalid.txt", std::string("a\0b", 3));
	EXPECT_EQ(morph_prompt_load(path.c_str(), "", &text), -EILSEQ);
	EXPECT_EQ(text, nullptr);
	write("invalid.txt", "\xff");
	EXPECT_EQ(morph_prompt_load(path.c_str(), "", &text), -EILSEQ);
	EXPECT_EQ(text, nullptr);
}

TEST_F(PromptLoadTest, MissingSourcesDoNotReturnPartialContent)
{
	auto base = write("base.txt", "valid");
	std::string missing = std::string(dir) + "/missing";
	EXPECT_EQ(morph_prompt_load(missing.c_str(), "", &text), -ENOENT);
	EXPECT_EQ(text, nullptr);
	EXPECT_LT(morph_prompt_load(base.c_str(), missing.c_str(), &text), 0);
	EXPECT_EQ(text, nullptr);
	EXPECT_EQ(morph_prompt_load(dir, "", &text), -EINVAL);
	EXPECT_EQ(text, nullptr);
}

TEST_F(PromptLoadTest, InvalidFragmentRejectsWholePrompt)
{
	write("10-good.txt", "valid");
	write("20-bad.txt", "\xff");
	EXPECT_EQ(morph_prompt_load("", dir, &text), -EILSEQ);
	EXPECT_EQ(text, nullptr);
}

TEST_F(PromptLoadTest, BootstrapLoadsReplacementSnapshot)
{
	struct config config{};
	struct tool_registry tools{};
	struct runtime_models models{};
	struct runtime_bootstrap_profile profile{};
	config_set_defaults(&config);
	auto path = write("agent.txt", "Custom behavior");
	std::strncpy(config.prompt.mode, "replace", sizeof(config.prompt.mode) - 1);
	std::strncpy(config.prompt.system_prompt_file, path.c_str(),
		sizeof(config.prompt.system_prompt_file) - 1);
	profile.config = &config;
	profile.tools = &tools;
	profile.models = &models;
	profile.process_replica = 1;
	EXPECT_EQ(runtime_bootstrap_models(&profile), 0);
	if (models.react) {
		EXPECT_EQ(models.react->system_prompt_replace, 1);
		EXPECT_STREQ(models.react->system_prompt, "Custom behavior");
		write("agent.txt", "Changed on disk");
		EXPECT_STREQ(models.react->system_prompt, "Custom behavior");
	} else {
		ADD_FAILURE() << "Missing react context";
	}
	runtime_bootstrap_cleanup_models(&models);
	tool_registry_cleanup(&tools);
}

TEST_F(PromptLoadTest, BootstrapPromptFailureReleasesPartialModels)
{
	struct config config{};
	struct tool_registry tools{};
	struct runtime_models models{};
	struct runtime_bootstrap_profile profile{};
	config_set_defaults(&config);
	std::string missing = std::string(dir) + "/missing.txt";
	std::strncpy(config.prompt.system_prompt_file, missing.c_str(),
		sizeof(config.prompt.system_prompt_file) - 1);
	profile.config = &config;
	profile.tools = &tools;
	profile.models = &models;
	profile.process_replica = 1;
	EXPECT_EQ(runtime_bootstrap_models(&profile), -ENOENT);
	EXPECT_EQ(models.react, nullptr);
	EXPECT_EQ(models.tokenizer, nullptr);
	runtime_bootstrap_cleanup_models(&models);
	tool_registry_cleanup(&tools);
}

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

static int runtime_test_session_visible(const char *, const char *, void *)
{
	return 1;
}

TEST(RuntimeInternalTest, TurnScopeAppliesPerRequestUserIsolation)
{
	struct db db{};
	struct config config{};
	struct session current{};
	struct react_context react{};
	struct runtime_turn_scope scope{};
	struct runtime_turn_scope_context context{};
	struct runtime_request request{};
	int marker = 7;

	ASSERT_EQ(db_open(&db, ":memory:"), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(session_create(&db, "scope-test", "mock", &current), 0);
	std::strncpy(current.display_id, "display-42",
		    sizeof(current.display_id) - 1);
	context.db = &db;
	context.config = &config;
	context.react = &react;
	context.current_session = &current;
	context.scope = &scope;
	request.session_id = current.id;
	request.user_id = "tenant-user";
	request.restrict_memory_to_user = 1;
	request.memory_visible_fn = runtime_test_session_visible;
	request.memory_visible_user_data = &marker;
	ASSERT_EQ(runtime_turn_scope_begin(&context, &request), 0);
	EXPECT_STREQ(scope.user_id, "tenant-user");
	EXPECT_STREQ(react.tool_runtime.user_id, "tenant-user");
	EXPECT_EQ(react.tool_runtime.restrict_memory_to_user, 1);
	EXPECT_EQ(react.tool_runtime.memory_visible_fn,
		  runtime_test_session_visible);
	EXPECT_EQ(react.tool_runtime.memory_visible_user_data, &marker);
	runtime_turn_scope_finish(&context);
	request.user_id = "another-tenant";
	EXPECT_EQ(runtime_turn_scope_begin(&context, &request), -EPERM);
	db_close(&db);
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

TEST(RuntimeInternalTest, RecordModelUsageAppliesCachedInputPrice)
{
	char db_path[256];
	std::snprintf(db_path, sizeof(db_path),
		      "/tmp/morph_runtime_cached_usage_%d.db", getpid());
	std::remove(db_path);
	struct db db{};
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);

	struct config config{};
	config_set_defaults(&config);
	config.credits.cost_to_credit_coef = 1000.0;
	config.credits.price_count = 1;
	std::strncpy(config.credits.prices[0].provider, "moonshot",
		     sizeof(config.credits.prices[0].provider) - 1);
	std::strncpy(config.credits.prices[0].model, "kimi-k3",
		     sizeof(config.credits.prices[0].model) - 1);
	std::strncpy(config.credits.prices[0].kind, "model_text",
		     sizeof(config.credits.prices[0].kind) - 1);
	config.credits.prices[0].input_per_million = 20.0;
	config.credits.prices[0].cached_input_per_million = 2.0;
	config.credits.prices[0].cached_input_price_configured = 1;

	struct model_usage usage{};
	std::strncpy(usage.provider, "moonshot", sizeof(usage.provider) - 1);
	std::strncpy(usage.model, "kimi-k3", sizeof(usage.model) - 1);
	std::strncpy(usage.kind, "model_text", sizeof(usage.kind) - 1);
	usage.input_tokens = 1000000;
	usage.cached_tokens = 800000;
	ASSERT_EQ(runtime_record_model_usage(&db, &config, "cached-session",
					     &usage), 0);

	struct credit_summary summary{};
	ASSERT_EQ(credit_summary_session(&db, "cached-session", &summary), 0);
	EXPECT_DOUBLE_EQ(summary.estimated_cost, 5.6);
	EXPECT_EQ(summary.credits, 5600);

	db_close(&db);
	std::remove(db_path);
}

TEST(RuntimeInternalTest, RecordModelUsageCanAttributeTenantUser)
{
	char db_path[256];
	std::snprintf(db_path, sizeof(db_path),
		      "/tmp/morph_runtime_tenant_usage_%d.db", getpid());
	std::remove(db_path);
	struct db db{};
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	struct config config{};
	config_set_defaults(&config);
	config.credits.cost_to_credit_coef = 1.0;
	struct model_usage usage{};
	std::strncpy(usage.kind, "model_text", sizeof(usage.kind) - 1);
	usage.input_tokens = 10;
	ASSERT_EQ(runtime_record_model_usage_for_user(
		&db, &config, "tenant-a", "session-a", &usage), 0);
	struct credit_summary tenant{};
	ASSERT_EQ(credit_summary_total(&db, "tenant-a", &tenant), 0);
	EXPECT_EQ(tenant.event_count, 1);
	struct credit_summary local{};
	ASSERT_EQ(credit_summary_total(&db, "local", &local), 0);
	EXPECT_EQ(local.event_count, 0);
	db_close(&db);
	std::remove(db_path);
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
