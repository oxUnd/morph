#include <gtest/gtest.h>
#include "db/database.h"
#include "db/scheduled_task.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

class ScheduledTaskTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];

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
	ASSERT_EQ(scheduled_task_create(&db, &input, &task), 0);

	ASSERT_EQ(scheduled_task_update_run(&db, task.id, "waiting", 70, 1,
					    "still preparing"), 0);
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
