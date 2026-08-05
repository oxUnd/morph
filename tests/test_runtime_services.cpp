#include "runtime_test_support.hpp"

extern "C" {
#include "agent/tool.h"
#include "db/scheduled_task.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

static int facade_test_tool(const char *, struct tool_result *result, void *)
{
	return tool_result_success_text(result, "ok");
}

static int facade_task_runner(const struct scheduled_task *,
			      struct scheduled_task_action_result *result, void *)
{
	result->completed = 1;
	result->body = strdup("task completed");
	return result->body ? 0 : -ENOMEM;
}

static void facade_notification_counter(const struct notification *, void *user)
{
	(*static_cast<int *>(user))++;
}

TEST_F(RuntimeFacadeTest, SessionCrudAndLookupAreConsistent)
{
	struct session first{};
	struct session second{};
	struct session found{};
	struct session selected{};
	int created = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &first), 0);
	ASSERT_GT(first.id, 0);
	ASSERT_EQ(runtime_session_select(instance, "alpha", &second, &created), 0);
	EXPECT_EQ(created, 1);
	EXPECT_STREQ(runtime_session_current_name(instance), "alpha");
	EXPECT_EQ(runtime_session_find_ref(instance, second.display_id, &found), 0);
	EXPECT_EQ(found.id, second.id);
	EXPECT_EQ(runtime_session_find_ref(instance,
		std::to_string(second.id).c_str(), &found), 0);
	EXPECT_EQ(found.id, second.id);
	ASSERT_EQ(runtime_session_rename_and_update(instance, second.id, "renamed"), 0);
	EXPECT_STREQ(runtime_session_current_name(instance), "renamed");
	ASSERT_EQ(runtime_session_select_existing(instance, first.id, &selected), 0);
	EXPECT_EQ(selected.id, first.id);
	EXPECT_EQ(runtime_session_delete_and_update(instance, second.id), 0);
	EXPECT_EQ(runtime_session_find_ref(instance, "renamed", &found), -ENOENT);
}

TEST_F(RuntimeFacadeTest, DetachedSessionDoesNotChangeCurrentSession)
{
	struct session current{};
	struct session detached{};
	int64_t current_id = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	ASSERT_EQ(runtime_session_create_detached(instance, "background", &detached), 0);
	ASSERT_NE(detached.id, current.id);
	ASSERT_EQ(runtime_session_current_id(instance, &current_id), 0);
	EXPECT_EQ(current_id, current.id);
}

TEST_F(RuntimeFacadeTest, SessionListFilterModelAndStatsUseRuntimeDatabase)
{
	struct session created{};
	struct session *sessions = nullptr;
	struct session current{};
	int count = 0;
	int messages = -1;
	int tokens = -1;
	int limit = -1;

	Open();
	ASSERT_EQ(runtime_session_create_and_select(instance, "needle-session", &created), 0);
	ASSERT_EQ(runtime_session_list_query(instance, &sessions, &count, 20,
		"needle"), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(sessions[0].id, created.id);
	runtime_session_list_free(sessions);
	EXPECT_GE(runtime_session_count_all(instance), 2);
	ASSERT_EQ(runtime_session_set_model(instance, "test-model"), 0);
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_STREQ(current.model, "test-model");
	EXPECT_EQ(runtime_session_context_stats(instance, &messages, &tokens, &limit), 0);
	EXPECT_EQ(messages, 0);
	EXPECT_EQ(tokens, 0);
	EXPECT_GT(limit, 0);
}

TEST_F(RuntimeFacadeTest, ToolRegistrationIsVisibleThroughFacade)
{
	struct tool_spec spec{};
	struct tool_desc desc{};
	enum tool_origin origin;
	int enabled;
	int before;

	Open();
	before = runtime_tool_count(instance);
	spec.origin = TOOL_ORIGIN_EXT;
	spec.name = "facade_test";
	spec.description = "Runtime facade test tool";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = facade_test_tool;
	ASSERT_EQ(runtime_register_tool(instance, &spec), 0);
	EXPECT_EQ(runtime_tool_count(instance), before + 1);
	ASSERT_EQ(runtime_tool_find(instance, "facade_test", &desc), 0);
	EXPECT_STREQ(desc.name, "facade_test");
	ASSERT_EQ(runtime_tool_enabled(instance, before, &enabled), 0);
	EXPECT_EQ(enabled, 1);
	ASSERT_EQ(runtime_tool_origin(instance, before, &origin), 0);
	EXPECT_EQ(origin, TOOL_ORIGIN_EXT);
	EXPECT_NE(runtime_register_tool(instance, &spec), 0);
	EXPECT_EQ(runtime_tool_find(instance, "missing", &desc), -ENOENT);
}

TEST_F(RuntimeFacadeTest, McpRegistryReturnsCopiedStatusWithoutConnecting)
{
	struct mcp_server_config server{};
	struct runtime_mcp_status status{};

	Open();
	std::strncpy(server.name, "offline", sizeof(server.name) - 1);
	server.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
	std::strncpy(server.http_url, "http://127.0.0.1:1/mcp",
		sizeof(server.http_url) - 1);
	ASSERT_EQ(runtime_add_mcp_server(instance, &server), 0);
	EXPECT_EQ(runtime_mcp_count(instance), 1);
	ASSERT_EQ(runtime_mcp_find(instance, "offline", &status), 0);
	EXPECT_STREQ(status.config.name, "offline");
	EXPECT_EQ(status.connected, 0);
	EXPECT_EQ(runtime_mcp_info(instance, 1, &status), -EINVAL);
	EXPECT_EQ(runtime_mcp_find(instance, "missing", &status), -ENOENT);
}

TEST_F(RuntimeFacadeTest, MediaCreditsAreAttributedToCurrentSession)
{
	struct credit_summary before{};
	struct credit_summary after{};

	Open();
	ASSERT_EQ(runtime_credit_summary_current_get(instance, &before), 0);
	ASSERT_EQ(runtime_credit_record_media(instance, "image_output", 2, 0,
		"test", "image-model", "{\"test\":true}"), 0);
	ASSERT_EQ(runtime_credit_summary_current_get(instance, &after), 0);
	EXPECT_EQ(after.event_count, before.event_count + 1);
	EXPECT_GE(after.credits, before.credits);
}

TEST_F(RuntimeFacadeTest, TaskCrudRunsEntirelyThroughFacade)
{
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task created{};
	struct scheduled_task loaded{};
	struct scheduled_task updated{};
	struct scheduled_task *items = nullptr;
	int count = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	input.source_session_id = current.id;
	input.title = "runtime task";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) + 3600;
	input.max_attempts = 1;
	input.action_type = "notify";
	input.payload_json = "{}";
	input.policy_json = "{}";
	input.notify_json = "{}";
	ASSERT_EQ(runtime_task_create(instance, &input, &created), 0);
	ASSERT_GT(created.id, 0);
	ASSERT_EQ(runtime_task_get(instance, created.id, &loaded), 0);
	EXPECT_STREQ(loaded.title, "runtime task");
	input.title = "updated task";
	ASSERT_EQ(runtime_task_update(instance, created.id, &input, &updated), 0);
	EXPECT_STREQ(updated.title, "updated task");
	ASSERT_EQ(runtime_task_list(instance, nullptr, 20, &items, &count), 0);
	EXPECT_EQ(count, 1);
	scheduled_task_free_list(items, count);
	ASSERT_EQ(runtime_task_cancel(instance, created.id), 0);
	ASSERT_EQ(runtime_task_get(instance, created.id, &loaded), 0);
	EXPECT_STREQ(loaded.status, "cancelled");
	scheduled_task_cleanup(&created);
	scheduled_task_cleanup(&loaded);
	scheduled_task_cleanup(&updated);
}

TEST_F(RuntimeFacadeTest, DueTaskCreatesAndAcknowledgesNotification)
{
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task created{};
	struct notification *notifications = nullptr;
	int count = 0;
	int delivered = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	input.source_session_id = current.id;
	input.title = "due reminder";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) - 1;
	input.max_attempts = 1;
	input.action_type = "notify";
	input.payload_json = "{\"prompt\":\"remember\"}";
	input.policy_json = "{}";
	input.notify_json = "{}";
	ASSERT_EQ(runtime_task_create(instance, &input, &created), 0);
	ASSERT_GE(runtime_tasks_run_due_for_runtime(instance, 10,
		facade_task_runner, nullptr, facade_notification_counter,
		&delivered), 0);
	ASSERT_EQ(runtime_notification_list(instance, 10, &notifications, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(delivered, 1);
	EXPECT_STREQ(notifications[0].title, "due reminder");
	ASSERT_EQ(runtime_notification_mark_read(instance, notifications[0].id), 0);
	notification_free_list(notifications, count);
	notifications = nullptr;
	ASSERT_EQ(runtime_notification_list(instance, 10, &notifications, &count), 0);
	EXPECT_EQ(count, 0);
	notification_free_list(notifications, count);
	scheduled_task_cleanup(&created);
}

TEST_F(RuntimeFacadeTest, EmptyMemoryCanBeRenderedAndCleared)
{
	Open();
	char *rendered = runtime_memory_render_current(instance, 10);
	ASSERT_NE(rendered, nullptr);
	std::free(rendered);
	EXPECT_EQ(runtime_memory_clear_current(instance, MEMORY_CLEAR_ALL), 0);
}

TEST_F(RuntimeFacadeTest, PersistentPermissionsCanBeListedAndCleared)
{
	struct runtime_permission_grant *grants = nullptr;
	int count = -1;
	int deleted = -1;

	Open();
	ASSERT_EQ(runtime_permission_list(instance, &grants, &count), 0);
	EXPECT_EQ(count, 0);
	runtime_permission_list_free(grants);
	EXPECT_EQ(runtime_permission_revoke_id(instance, 123, &deleted), 0);
	EXPECT_EQ(deleted, 0);
	EXPECT_EQ(runtime_permission_clear(instance, 0, &deleted), 0);
	EXPECT_EQ(deleted, 0);
}

TEST(RuntimeFacadeValidationTest, RejectsInvalidFacadeArguments)
{
	struct session session{};
	struct tool_desc tool{};
	enum tool_origin origin;
	int enabled;
	struct credit_summary credits{};
	struct runtime_turn_status status{};

	EXPECT_EQ(runtime_session_current(nullptr, &session), -EINVAL);
	EXPECT_EQ(runtime_session_current_id(nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_session_find_ref(nullptr, "x", &session), -EINVAL);
	EXPECT_EQ(runtime_tool_info(nullptr, 0, &tool), -EINVAL);
	EXPECT_EQ(runtime_tool_enabled(nullptr, 0, &enabled), -EINVAL);
	EXPECT_EQ(runtime_tool_origin(nullptr, 0, &origin), -EINVAL);
	EXPECT_EQ(runtime_turn_status_get(nullptr, &status), -EINVAL);
	EXPECT_EQ(runtime_credit_summary_today_get(nullptr, &credits), -EINVAL);
	EXPECT_EQ(runtime_task_cancel(nullptr, 1), -EINVAL);
	EXPECT_EQ(runtime_sync_status_instance(nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_permission_list(nullptr, nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_permission_clear(nullptr, 0, nullptr), -EINVAL);
	runtime_turn_status_cleanup(nullptr);
	runtime_session_list_free(nullptr);
	runtime_mcp_list_free(nullptr);
}
