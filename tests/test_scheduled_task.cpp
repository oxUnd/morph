#include <gtest/gtest.h>
#include "db/database.h"
#include "db/scheduled_task.h"
#include "session.h"
#include "agent/tool.h"
#include "agent/tools/scheduled_tasks.h"
#include "event/event.h"
#include "cJSON.h"
#include <climits>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

static int successful_runner(const struct scheduled_task *task,
			     struct scheduled_task_action_result *result,
			     void *user_data)
{
	(void)task;
	(void)user_data;
	result->completed = 1;
	result->body = strdup("{\"status\":\"completed\"}");
	return 0;
}

static int failing_runner(const struct scheduled_task *task,
			  struct scheduled_task_action_result *result,
			  void *user_data)
{
	(void)task;
	(void)user_data;
	result->error = strdup("runner failed");
	return -EIO;
}

static int watch_runner(const struct scheduled_task *task,
			struct scheduled_task_action_result *result,
			void *user_data)
{
	int *count = (int *)user_data;

	(void)task;
	(*count)++;
	if (*count >= 2) {
		result->completed = 1;
		result->body = strdup("{\"ready\":true}");
	} else {
		result->completed = 0;
		result->body = strdup("{\"ready\":false}");
		result->retry_after_seconds = 5;
	}
	return 0;
}

static cJSON *event_at(struct morph_event_recorder *rec, size_t index)
{
	const char *json = morph_event_recorder_get(rec, index);

	if (!json)
		return nullptr;
	return cJSON_Parse(json);
}

static void expect_event_name(struct morph_event_recorder *rec, size_t index,
			      const char *name)
{
	cJSON *root = event_at(rec, index);

	ASSERT_NE(root, nullptr);
	ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(root, "name")));
	EXPECT_STREQ(name, cJSON_GetObjectItem(root, "name")->valuestring);
	cJSON_Delete(root);
}

class ScheduledTaskTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[PATH_MAX];

	void SetUp() override {
		memset(&db, 0, sizeof(db));
		snprintf(db_path, sizeof(db_path), "/tmp/morph_task_test_%d.db",
			 getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
	}

	void TearDown() override {
		db_close(&db);
		std::remove(db_path);
	}
};

struct inbox_reader_args {
	struct db *db;
	std::atomic<int> *failed;
};

static void *inbox_reader_thread(void *arg)
{
	struct inbox_reader_args *args =
		static_cast<struct inbox_reader_args *>(arg);

	for (int i = 0; i < 100; i++) {
		struct notification *notifications = nullptr;
		int count = 0;
		int rc = notification_list_unread(args->db, 50,
						  &notifications, &count);
		if (rc != 0)
			args->failed->store(1);
		notification_free_list(notifications, count);
	}
	return nullptr;
}

TEST_F(ScheduledTaskTest, ConcurrentNotificationInboxAccess) {
	const int thread_count = 4;
	pthread_t threads[thread_count];
	std::atomic<int> failed{0};
	struct inbox_reader_args args = {&db, &failed};

	for (int i = 0; i < thread_count; i++) {
		ASSERT_EQ(pthread_create(&threads[i], nullptr,
					 inbox_reader_thread, &args), 0);
	}

	for (int i = 0; i < 100; i++) {
		int rc = notification_create(&db, 0, "info", "title", "body",
					     "inbox", nullptr);
		if (rc != 0)
			failed.store(1);
	}

	for (int i = 0; i < thread_count; i++)
		ASSERT_EQ(pthread_join(threads[i], nullptr), 0);

	struct notification *notifications = nullptr;
	int count = 0;

	ASSERT_EQ(notification_list_unread(&db, 200, &notifications, &count),
		  0);
	EXPECT_GT(count, 0);
	notification_free_list(notifications, count);
	EXPECT_EQ(failed.load(), 0);
}

TEST_F(ScheduledTaskTest, CreateAndGetTask) {
	struct scheduled_task_input input = {};
	struct scheduled_task task = {};

	input.title = "Check coffee";
	input.kind = "watch";
	input.trigger_type = "interval";
	input.next_run_at = 100;
	input.interval_seconds = 120;
	input.timeout_at = 1000;
	input.max_attempts = 5;
	input.action_type = "tool_call";
	input.payload_json = "{\"order_id\":\"coffee-1\"}";
	input.policy_json = "{\"stop\":\"ready\"}";
	input.notify_json = "{\"targets\":[\"inbox\"]}";

	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);
	EXPECT_GT(task.id, 0);
	EXPECT_STREQ(task.title, "Check coffee");
	EXPECT_STREQ(task.kind, "watch");
	EXPECT_STREQ(task.status, "pending");
	EXPECT_STREQ(task.trigger_type, "interval");
	EXPECT_EQ(task.next_run_at, 100);
	EXPECT_EQ(task.interval_seconds, 120);
	EXPECT_EQ(task.timeout_at, 1000);
	EXPECT_EQ(task.attempts, 0);
	EXPECT_EQ(task.max_attempts, 5);
	EXPECT_STREQ(task.action_type, "tool_call");
	ASSERT_NE(task.payload_json, nullptr);
	EXPECT_STREQ(task.payload_json, "{\"order_id\":\"coffee-1\"}");

	scheduled_task_cleanup(&task);
}

TEST_F(ScheduledTaskTest, TaskAndNotificationTrackSessions) {
	struct scheduled_task_input input = {};
	struct scheduled_task task = {};
	struct scheduled_task fetched = {};
	struct notification notification = {};
	struct session source = {};
	struct session run = {};

	ASSERT_EQ(session_create(&db, "source", "gpt-4o", &source), 0);
	ASSERT_EQ(session_create(&db, "run", "gpt-4o", &run), 0);

	input.source_session_id = source.id;
	input.title = "Check coffee";
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = 100;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"check coffee\"}";

	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);
	EXPECT_EQ(task.source_session_id, source.id);
	EXPECT_EQ(task.latest_session_id, 0);

	ASSERT_EQ(scheduled_task_update_run(&db, task.id, "completed", 0, 1,
					    run.id, nullptr), 0);
	ASSERT_EQ(scheduled_task_get(&db, task.id, &fetched), 0);
	EXPECT_EQ(fetched.source_session_id, source.id);
	EXPECT_EQ(fetched.latest_session_id, run.id);

	ASSERT_EQ(notification_create_for_session(&db, task.id, run.id, "info",
						  "Coffee", "Ready", "inbox",
						  &notification), 0);
	EXPECT_EQ(notification.task_id, task.id);
	EXPECT_EQ(notification.session_id, run.id);

	notification_cleanup(&notification);
	scheduled_task_cleanup(&fetched);
	scheduled_task_cleanup(&task);
}

TEST_F(ScheduledTaskTest, CreateUpdateCancelEmitTaskEvents) {
	struct morph_event_recorder rec;
	struct scheduled_task_event_sink events;
	struct scheduled_task_input input = {};
	struct scheduled_task task = {};
	cJSON *root;
	cJSON *data;
	int64_t id;

	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	events.cb = morph_event_recorder_cb;
	events.user_data = &rec;

	input.title = "Check coffee";
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = 100;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"check coffee\"}";
	ASSERT_EQ(scheduled_task_create_with_events(&db, &input, &task,
						    &events), 0);
	id = task.id;
	scheduled_task_cleanup(&task);

	input.title = "Check tea";
	input.next_run_at = 200;
	input.payload_json = "{\"prompt\":\"check tea\"}";
	ASSERT_EQ(scheduled_task_update_with_events(&db, id, &input, &task,
						    &events), 0);
	scheduled_task_cleanup(&task);
	ASSERT_EQ(scheduled_task_cancel_with_events(&db, id, &events), 0);

	ASSERT_EQ(morph_event_recorder_count(&rec), 3u);
	expect_event_name(&rec, 0, "task.created");
	expect_event_name(&rec, 1, "task.updated");
	expect_event_name(&rec, 2, "task.cancelled");

	root = event_at(&rec, 2);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ("task", cJSON_GetObjectItem(root, "type")->valuestring);
	EXPECT_STREQ("cancelled",
		     cJSON_GetObjectItem(root, "phase")->valuestring);
	data = cJSON_GetObjectItem(root, "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	EXPECT_EQ(id, (int64_t)cJSON_GetObjectItem(data, "task_id")->valuedouble);
	EXPECT_STREQ("cancelled",
		     cJSON_GetObjectItem(data, "status")->valuestring);
	EXPECT_EQ(-ECANCELED, cJSON_GetObjectItem(data, "error_code")->valueint);
	EXPECT_STREQ("cancelled",
		     cJSON_GetObjectItem(data, "reason")->valuestring);
	cJSON_Delete(root);
	morph_event_recorder_cleanup(&rec);
}

TEST_F(ScheduledTaskTest, RejectsInvalidTaskFields) {
	struct scheduled_task_input input = {};

	input.title = "Bad kind";
	input.kind = "unknown";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	EXPECT_EQ(scheduled_task_create(&db, &input, nullptr), -EINVAL);

	input.kind = "reminder";
	input.trigger_type = "interval";
	input.interval_seconds = 0;
	EXPECT_EQ(scheduled_task_create(&db, &input, nullptr), -EINVAL);

	input.kind = "action";
	input.trigger_type = "once";
	input.interval_seconds = 0;
	input.payload_json = nullptr;
	EXPECT_EQ(scheduled_task_create(&db, &input, nullptr), -EINVAL);
}

TEST_F(ScheduledTaskTest, UpdateTaskFields) {
	struct scheduled_task_input input = {};
	struct scheduled_task_input update = {};
	struct scheduled_task task = {};

	input.title = "Original";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);

	update.title = "Updated";
	update.kind = "reminder";
	update.trigger_type = "interval";
	update.next_run_at = 20;
	update.interval_seconds = 30;
	update.action_type = "reminder";
	update.payload_json = "{\"message\":\"updated\"}";
	ASSERT_EQ(scheduled_task_update(&db, task.id, &update, &task), 0);
	EXPECT_STREQ(task.title, "Updated");
	EXPECT_STREQ(task.status, "pending");
	EXPECT_STREQ(task.trigger_type, "interval");
	EXPECT_EQ(task.next_run_at, 20);
	EXPECT_EQ(task.interval_seconds, 30);
	ASSERT_NE(task.payload_json, nullptr);
	EXPECT_STREQ(task.payload_json, "{\"message\":\"updated\"}");
	scheduled_task_cleanup(&task);
}

TEST_F(ScheduledTaskTest, ListDueTasks) {
	struct scheduled_task_input due = {};
	struct scheduled_task_input future = {};
	struct scheduled_task *tasks = nullptr;
	int count = 0;

	due.title = "Due task";
	due.kind = "reminder";
	due.trigger_type = "once";
	due.next_run_at = 10;
	due.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &due, nullptr), 0);

	future.title = "Future task";
	future.kind = "reminder";
	future.trigger_type = "once";
	future.next_run_at = 30;
	future.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &future, nullptr), 0);

	ASSERT_EQ(scheduled_task_list_due(&db, 20, 10, &tasks, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(tasks[0].title, "Due task");
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, ListTasksByStatus) {
	struct scheduled_task_input input = {};
	struct scheduled_task task = {};
	struct scheduled_task *tasks = nullptr;
	int count = 0;

	input.title = "Filter task";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);
	ASSERT_EQ(scheduled_task_cancel(&db, task.id), 0);

	ASSERT_EQ(scheduled_task_list(&db, "cancelled", 10, &tasks, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(tasks[0].status, "cancelled");
	scheduled_task_free_list(tasks, count);
	scheduled_task_cleanup(&task);
}

TEST_F(ScheduledTaskTest, UpdateAndCancelTask) {
	struct scheduled_task_input input = {};
	struct scheduled_task task = {};
	struct scheduled_task fetched = {};

	input.title = "Retry task";
	input.kind = "watch";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 60;
	input.max_attempts = 3;
	input.action_type = "tool_call";
	input.payload_json = "{\"tool\":\"mock\",\"args\":{}}";
	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);

	ASSERT_EQ(scheduled_task_update_run(&db, task.id, "waiting", 70, 1,
					    0, "still preparing"), 0);
	ASSERT_EQ(scheduled_task_get(&db, task.id, &fetched), 0);
	EXPECT_STREQ(fetched.status, "waiting");
	EXPECT_EQ(fetched.next_run_at, 70);
	EXPECT_EQ(fetched.attempts, 1);
	EXPECT_STREQ(fetched.last_error, "still preparing");
	scheduled_task_cleanup(&fetched);

	ASSERT_EQ(scheduled_task_cancel(&db, task.id), 0);
	ASSERT_EQ(scheduled_task_get(&db, task.id, &fetched), 0);
	EXPECT_STREQ(fetched.status, "cancelled");
	EXPECT_EQ(fetched.attempts, 1);

	scheduled_task_cleanup(&fetched);
	scheduled_task_cleanup(&task);
}

TEST_F(ScheduledTaskTest, NotificationInboxUnreadFlow) {
	struct notification notification = {};
	struct notification *notifications = nullptr;
	int count = 0;

	ASSERT_EQ(notification_create(&db, 0, "info", "Coffee ready",
				      "Your coffee is ready.",
				      "inbox", &notification), 0);
	EXPECT_GT(notification.id, 0);
	EXPECT_EQ(notification.task_id, 0);
	EXPECT_STREQ(notification.level, "info");
	EXPECT_STREQ(notification.title, "Coffee ready");
	ASSERT_NE(notification.body, nullptr);
	EXPECT_STREQ(notification.body, "Your coffee is ready.");
	notification_cleanup(&notification);

	ASSERT_EQ(notification_list_unread(&db, 10, &notifications, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(notifications[0].delivery_status, "inbox");
	ASSERT_EQ(notification_mark_read(&db, notifications[0].id, 1234), 0);
	notification_free_list(notifications, count);

	notifications = nullptr;
	count = 0;
	ASSERT_EQ(notification_list_unread(&db, 10, &notifications, &count), 0);
	EXPECT_EQ(count, 0);
	notification_free_list(notifications, count);
}

TEST_F(ScheduledTaskTest, RunDueReminderCreatesInboxNotification) {
	struct scheduled_task_input input = {};
	struct notification *notifications = nullptr;
	int count = 0;
	int ran = 0;

	input.title = "Pick up coffee";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	input.payload_json = "{\"message\":\"Coffee is ready.\"}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due(&db, 20, 10, &ran), 0);
	EXPECT_EQ(ran, 1);

	ASSERT_EQ(notification_list_unread(&db, 10, &notifications, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(notifications[0].title, "Pick up coffee");
	ASSERT_NE(notifications[0].body, nullptr);
	EXPECT_STREQ(notifications[0].body, "Coffee is ready.");
	notification_free_list(notifications, count);
}

TEST_F(ScheduledTaskTest, NotifyJsonControlsDeliveryStatus) {
	struct scheduled_task_input input = {};
	struct notification *notifications = nullptr;
	int count = 0;

	input.title = "Notify session";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	input.notify_json = "{\"targets\":[\"inbox\",\"session\"]}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_collect(&db, 20, 10,
						 &notifications, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(notifications[0].delivery_status, "inbox+session");
	notification_free_list(notifications, count);
}

TEST_F(ScheduledTaskTest, RunDueCollectReturnsCreatedNotification) {
	struct scheduled_task_input input = {};
	struct notification *notifications = nullptr;
	int count = 0;

	input.title = "Say hello";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	input.payload_json = "{\"message\":\"hello\"}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_collect(&db, 20, 10, &notifications,
						 &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(notifications[0].title, "Say hello");
	ASSERT_NE(notifications[0].body, nullptr);
	EXPECT_STREQ(notifications[0].body, "hello");
	notification_free_list(notifications, count);
}

TEST_F(ScheduledTaskTest, RunDueRepeatingReminderReschedules) {
	struct scheduled_task_input input = {};
	struct scheduled_task *tasks = nullptr;
	int count = 0;
	int ran = 0;

	input.title = "Recurring reminder";
	input.kind = "reminder";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 60;
	input.max_attempts = 3;
	input.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due(&db, 20, 10, &ran), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(scheduled_task_list(&db, "waiting", 10, &tasks, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(tasks[0].next_run_at, 80);
	EXPECT_EQ(tasks[0].attempts, 1);
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, RunDueActionUsesRunnerAndCompletes) {
	struct scheduled_task_input input = {};
	struct notification *notifications = nullptr;
	struct scheduled_task *tasks = nullptr;
	int count = 0;

	input.title = "Run action";
	input.kind = "action";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "tool_call";
	input.payload_json = "{\"tool\":\"mock\",\"args\":{}}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_collect_with_runner(
		&db, 20, 10, successful_runner, nullptr, &notifications,
		&count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(notifications[0].level, "info");
	notification_free_list(notifications, count);

	ASSERT_EQ(scheduled_task_list(&db, "completed", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, RunDueActionRetriesThenFails) {
	struct scheduled_task_input input = {};
	struct scheduled_task *tasks = nullptr;
	int ran = 0;
	int count = 0;

	input.title = "Retry action";
	input.kind = "action";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 30;
	input.max_attempts = 2;
	input.action_type = "tool_call";
	input.payload_json = "{\"tool\":\"mock\",\"args\":{}}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_with_runner(&db, 20, 10,
		failing_runner, nullptr, &ran), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(scheduled_task_list(&db, "waiting", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(tasks[0].attempts, 1);
	EXPECT_EQ(tasks[0].next_run_at, 50);
	scheduled_task_free_list(tasks, count);

	ASSERT_EQ(scheduled_task_run_due_with_runner(&db, 60, 10,
		failing_runner, nullptr, &ran), 0);
	ASSERT_EQ(scheduled_task_list(&db, "failed", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(tasks[0].last_error, "runner failed");
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, RunDueWatchReschedulesThenCompletes) {
	struct scheduled_task_input input = {};
	struct scheduled_task *tasks = nullptr;
	int runner_count = 0;
	int ran = 0;
	int count = 0;

	input.title = "Watch action";
	input.kind = "watch";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 30;
	input.max_attempts = 3;
	input.action_type = "tool_call";
	input.payload_json = "{\"tool\":\"mock\",\"args\":{}}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_with_runner(&db, 20, 10,
		watch_runner, &runner_count, &ran), 0);
	ASSERT_EQ(scheduled_task_list(&db, "waiting", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(tasks[0].next_run_at, 25);
	scheduled_task_free_list(tasks, count);

	ASSERT_EQ(scheduled_task_run_due_with_runner(&db, 30, 10,
		watch_runner, &runner_count, &ran), 0);
	ASSERT_EQ(scheduled_task_list(&db, "completed", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, RunDueClaimPreventsDuplicateProcessing) {
	struct scheduled_task_input input = {};
	int ran = 0;

	input.title = "One shot";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.action_type = "reminder";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due(&db, 20, 10, &ran), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(scheduled_task_run_due(&db, 20, 10, &ran), 0);
	EXPECT_EQ(ran, 0);
}

TEST_F(ScheduledTaskTest, ToolCreateDelaySecondsUsesTimeAnchor) {
	struct tool_registry reg;
	struct tool_result result;
	struct scheduled_task task = {};
	cJSON *root;
	cJSON *id_item;
	int64_t id;
	const char *args =
		"{\"op\":\"create\",\"title\":\"anchored reminder\","
		"\"kind\":\"agent\",\"trigger_type\":\"once\","
		"\"delay_seconds\":30,\"prompt\":\"say hi\"}";

	tool_registry_init(&reg);
	tool_result_init(&result);
	ASSERT_EQ(scheduled_tasks_tool_init(&reg, &db), 0);
	ASSERT_EQ(scheduled_tasks_tool_set_time_anchor(&reg, 1000), 0);
	ASSERT_EQ(tool_exec(&reg, "tasks", args, &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	id_item = cJSON_GetObjectItem(root, "id");
	ASSERT_TRUE(cJSON_IsNumber(id_item));
	id = (int64_t)id_item->valuedouble;
	cJSON_Delete(root);
	ASSERT_EQ(scheduled_task_get(&db, id, &task), 0);
	EXPECT_EQ(task.next_run_at, 1030);
	ASSERT_NE(task.payload_json, nullptr);
	EXPECT_NE(strstr(task.payload_json, "\"prompt\":\"say hi\""), nullptr);
	scheduled_task_cleanup(&task);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg);
}

TEST_F(ScheduledTaskTest, ToolCreateEmitsTaskCreatedEvent) {
	struct tool_registry reg;
	struct tool_result result;
	struct morph_event_recorder rec;
	struct scheduled_task_event_sink events;
	const char *args =
		"{\"op\":\"create\",\"title\":\"evented task\","
		"\"kind\":\"agent\",\"trigger_type\":\"once\","
		"\"delay_seconds\":30,\"prompt\":\"say hi\"}";

	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	events.cb = morph_event_recorder_cb;
	events.user_data = &rec;
	tool_registry_init(&reg);
	tool_result_init(&result);
	ASSERT_EQ(scheduled_tasks_tool_init_events(&reg, &db, &events), 0);
	ASSERT_EQ(scheduled_tasks_tool_set_time_anchor(&reg, 1000), 0);
	ASSERT_EQ(tool_exec(&reg, "tasks", args, &result), 0);
	ASSERT_EQ(morph_event_recorder_count(&rec), 1u);
	expect_event_name(&rec, 0, "task.created");
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg);
	morph_event_recorder_cleanup(&rec);
}

TEST_F(ScheduledTaskTest, RunDueAgentUsesRunnerAndReschedules) {
	struct scheduled_task_input input = {};
	struct scheduled_task *tasks = nullptr;
	int count = 0;
	int ran = 0;

	input.title = "Search news";
	input.kind = "agent";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 30;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"search news\"}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_with_runner(&db, 20, 10,
		successful_runner, nullptr, &ran), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(scheduled_task_list(&db, "waiting", 10, &tasks, &count),
		  0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(tasks[0].next_run_at, 50);
	EXPECT_EQ(tasks[0].attempts, 1);
	scheduled_task_free_list(tasks, count);
}

TEST_F(ScheduledTaskTest, RunDueEmitsTaskLifecycleEvents) {
	struct morph_event_recorder rec;
	struct scheduled_task_event_sink events;
	struct scheduled_task_input input = {};
	cJSON *root;
	cJSON *data;
	int ran = 0;

	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	events.cb = morph_event_recorder_cb;
	events.user_data = &rec;

	input.title = "Search news";
	input.kind = "agent";
	input.trigger_type = "interval";
	input.next_run_at = 10;
	input.interval_seconds = 30;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"search news\"}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_with_runner_events(
		&db, 20, 10, successful_runner, nullptr, &ran, &events), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(morph_event_recorder_count(&rec), 4u);
	expect_event_name(&rec, 0, "task.claimed");
	expect_event_name(&rec, 1, "task.started");
	expect_event_name(&rec, 2, "task.notification");
	expect_event_name(&rec, 3, "task.rescheduled");

	root = event_at(&rec, 3);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ("task", cJSON_GetObjectItem(root, "type")->valuestring);
	EXPECT_STREQ("ready", cJSON_GetObjectItem(root, "phase")->valuestring);
	data = cJSON_GetObjectItem(root, "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	EXPECT_STREQ("waiting", cJSON_GetObjectItem(data, "status")->valuestring);
	EXPECT_STREQ("interval", cJSON_GetObjectItem(data, "reason")->valuestring);
	EXPECT_EQ(50, (int64_t)cJSON_GetObjectItem(data, "next_run_at")->valuedouble);
	EXPECT_EQ(1, cJSON_GetObjectItem(data, "attempts")->valueint);
	ASSERT_TRUE(cJSON_IsNumber(cJSON_GetObjectItem(data, "notification_id")));
	cJSON_Delete(root);
	morph_event_recorder_cleanup(&rec);
}

TEST_F(ScheduledTaskTest, RunDueMaxAttemptsEmitsReasonAndErrorCode) {
	struct morph_event_recorder rec;
	struct scheduled_task_event_sink events;
	struct scheduled_task_input input = {};
	cJSON *root;
	cJSON *data;
	int ran = 0;

	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	events.cb = morph_event_recorder_cb;
	events.user_data = &rec;

	input.title = "Fail once";
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = 10;
	input.max_attempts = 1;
	input.action_type = "agent_run";
	input.payload_json = "{\"prompt\":\"fail\"}";
	ASSERT_EQ(scheduled_task_create(&db, &input, nullptr), 0);

	ASSERT_EQ(scheduled_task_run_due_with_runner_events(
		&db, 20, 10, failing_runner, nullptr, &ran, &events), 0);
	EXPECT_EQ(ran, 1);
	ASSERT_EQ(morph_event_recorder_count(&rec), 4u);
	expect_event_name(&rec, 0, "task.claimed");
	expect_event_name(&rec, 1, "task.started");
	expect_event_name(&rec, 2, "task.notification");
	expect_event_name(&rec, 3, "task.max_attempts_reached");

	root = event_at(&rec, 3);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ("failed", cJSON_GetObjectItem(root, "phase")->valuestring);
	data = cJSON_GetObjectItem(root, "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	EXPECT_STREQ("failed", cJSON_GetObjectItem(data, "status")->valuestring);
	EXPECT_STREQ("max_attempts",
		     cJSON_GetObjectItem(data, "reason")->valuestring);
	EXPECT_EQ(-EIO, cJSON_GetObjectItem(data, "error_code")->valueint);
	EXPECT_EQ(1, cJSON_GetObjectItem(data, "attempts")->valueint);
	EXPECT_EQ(1, cJSON_GetObjectItem(data, "max_attempts")->valueint);
	cJSON_Delete(root);
	morph_event_recorder_cleanup(&rec);
}
