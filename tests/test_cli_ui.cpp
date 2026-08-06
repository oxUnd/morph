#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/ui_event.h"
#include "event/event.h"

int cli_event_callback(const struct morph_event *event, void *user_data);
int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
}

#include <poll.h>
#include <cerrno>
#include <thread>
#include <vector>

class CliUiTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void SetUp() override
	{
		cli_set_color_enabled(0);
		ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
		ctx.presentation_ready = 1;
		ctx.turn_active = 1;
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
		ASSERT_EQ(cli_ui_init(&ctx), 0);
	}

	void TearDown() override
	{
		cli_ui_cleanup(&ctx);
		cli_presentation_cleanup(&ctx);
		cli_set_color_enabled(1);
	}
};

TEST_F(CliUiTest, CrossThreadEventWaitsForOwnerDrain)
{
	cJSON *data = cJSON_CreateObject();
	ASSERT_NE(data, nullptr);
	cJSON_AddStringToObject(data, "detail", "owned copy");
	struct morph_event event{
		MORPH_EVENT_ERROR,
		"background.error",
		"end",
		"queued error",
		data,
		"turn-ui",
	};
	int callback_rc = -1;

	testing::internal::CaptureStdout();
	std::thread worker([&] {
		callback_rc = cli_event_callback(&event, &ctx);
	});
	worker.join();
	EXPECT_EQ(callback_rc, 0);
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
	cJSON_Delete(data);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("queued error"), std::string::npos);
}

TEST_F(CliUiTest, WakePipeSignalsQueuedWorkAndDrains)
{
	struct notification notification{};
	notification.id = 42;
	std::snprintf(notification.title, sizeof(notification.title),
		      "nightly task");
	notification.body = const_cast<char *>("finished safely");
	int callback_rc = -1;

	std::thread worker([&] {
		callback_rc = cli_ui_post_notification(&ctx, &notification);
	});
	worker.join();
	ASSERT_EQ(callback_rc, 0);

	struct pollfd fd{};
	fd.fd = cli_ui_wake_fd(&ctx);
	fd.events = POLLIN;
	ASSERT_EQ(poll(&fd, 1, 0), 1);
	EXPECT_NE(fd.revents & POLLIN, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("nightly task"), std::string::npos);
	EXPECT_NE(output.find("finished safely"), std::string::npos);
	EXPECT_NE(output.find("#42"), std::string::npos);

	fd.revents = 0;
	EXPECT_EQ(poll(&fd, 1, 0), 0);
}

TEST_F(CliUiTest, OwnerThreadEventsRemainImmediate)
{
	struct morph_event event{
		MORPH_EVENT_ERROR,
		"owner.error",
		"end",
		"immediate error",
		nullptr,
		"turn-ui",
	};

	ASSERT_TRUE(cli_ui_is_owner(&ctx));
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_event_callback(&event, &ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("immediate error"), std::string::npos);
}

TEST_F(CliUiTest, QueuedEventsRenderBeforeFollowingOwnerEvent)
{
	struct morph_event queued_event{
		MORPH_EVENT_ERROR,
		"background.error",
		"end",
		"first queued error",
		nullptr,
		"turn-ui",
	};
	struct morph_event owner_event{
		MORPH_EVENT_ERROR,
		"owner.error",
		"end",
		"second owner error",
		nullptr,
		"turn-ui",
	};
	int callback_rc = -1;

	std::thread worker([&] {
		callback_rc = cli_event_callback(&queued_event, &ctx);
	});
	worker.join();
	ASSERT_EQ(callback_rc, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_event_callback(&owner_event, &ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	size_t queued_offset = output.find("first queued error");
	size_t owner_offset = output.find("second owner error");
	ASSERT_NE(queued_offset, std::string::npos);
	ASSERT_NE(owner_offset, std::string::npos);
	EXPECT_LT(queued_offset, owner_offset);
}

TEST_F(CliUiTest, ConcurrentProducersPreserveEveryNotification)
{
	constexpr int worker_count = 4;
	constexpr int events_per_worker = 12;
	std::vector<std::thread> workers;
	std::vector<int> results(worker_count, 0);

	for (int worker = 0; worker < worker_count; worker++) {
		workers.emplace_back([&, worker] {
			for (int index = 0; index < events_per_worker; index++) {
				struct notification notification{};

				notification.id = worker * events_per_worker + index;
				std::snprintf(notification.title,
					      sizeof(notification.title),
					      "queued-item-%d-%d", worker, index);
				int rc = cli_ui_post_notification(&ctx, &notification);
				if (rc != 0)
					results[worker] = rc;
			}
		});
	}
	for (auto &worker : workers)
		worker.join();
	for (int rc : results)
		ASSERT_EQ(rc, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	int found = 0;
	size_t offset = 0;
	while ((offset = output.find("queued-item-", offset)) !=
	       std::string::npos) {
		found++;
		offset += sizeof("queued-item-") - 1;
	}
	EXPECT_EQ(found, worker_count * events_per_worker);
}

TEST_F(CliUiTest, NonOwnerCannotDrainPresentationState)
{
	int drain_rc = 0;
	std::thread worker([&] {
		drain_rc = cli_ui_drain(&ctx);
	});
	worker.join();
	EXPECT_EQ(drain_rc, -EPERM);
}
