#include <gtest/gtest.h>
#include "nb/err.h"

extern "C" {
#include "agent/tool_context.h"
#include "config/config.h"
#include "runtime/services.h"
#include "sapi/fastcgi/agent_bridge.h"
#include "nb/cleanup.h"
}

#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <unistd.h>

extern "C" int fcgi_turn_ask_user(
	const char *, const char *const *, int, const char *, int, int,
	const struct ask_user_options *,
	char ***, int *, void *)
{
	return NB_ERR_UNSUPPORTED;
}

extern "C" enum tool_operation_verdict fcgi_turn_operation_approval(
	const struct tool_operation *, void *)
{
	return TOOL_OP_DENY;
}

TEST(FastcgiRuntimeTest, SharedBootstrapLoadsSkillsMcpAndServerTools)
{
	const std::string root = "/tmp/morph_fastcgi_runtime_" +
		std::to_string(getpid());
	const std::string skills = root + "/skills";
	const std::string exts = root + "/exts";
	const std::string output = root + "/output";
	const std::string config = root + "/config.toml";
	const std::string database = root + "/morph.db";
	const std::string log = root + "/fastcgi.log";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(skills + "/fastcgi-test");
	std::filesystem::create_directories(exts);
	{
		std::ofstream skill(skills + "/fastcgi-test/SKILL.md");
		skill << "---\nname: fastcgi-test\n"
		      << "description: FastCGI runtime integration test.\n"
		      << "---\nUse this test skill.\n";
	}
	{
		std::ofstream cfg(config);
		cfg << "[memory]\nenabled = false\n"
		    << "[skill]\ndir = \"" << skills << "\"\n"
		    << "[ext]\ndir = \"" << exts << "\"\n"
		    << "[[mcp.servers]]\n"
		    << "name = \"fastcgi-disabled\"\n"
		    << "transport = \"http\"\n"
		    << "url = \"http://127.0.0.1:1/mcp\"\n"
		    << "auto_connect = false\n";
	}
	setenv("MORPH_FCGI_CONFIG", config.c_str(), 1);
	setenv("MORPH_FCGI_OUTPUT_DIR", output.c_str(), 1);
	setenv("MORPH_FCGI_LOG_FILE", log.c_str(), 1);
	setenv("MORPH_FCGI_RUNTIME_MIN_WORKERS", "1", 1);
	setenv("MORPH_FCGI_RUNTIME_MAX_WORKERS", "2", 1);
	setenv("MORPH_FCGI_RUNTIME_QUEUE_MAX", "1", 1);
	setenv("MORPH_FCGI_RUNTIME_IDLE_SECONDS", "1", 1);
	setenv("MORPH_FCGI_RUNTIME_WAIT_SECONDS", "1", 1);
	struct fcgi_bridge *bridge = nullptr;
	ASSERT_EQ(fcgi_bridge_init(&bridge, database.c_str()), 0);
	EXPECT_EQ(fcgi_bridge_runtime_count(bridge), 1);
	struct runtime *runtime = fcgi_bridge_runtime_borrow(bridge);
	ASSERT_NE(runtime, nullptr);
	EXPECT_GT(runtime_skill_count(runtime), 0);
	struct skill_entry skill{};
	EXPECT_EQ(runtime_skill_find_borrow(runtime, "fastcgi-test", &skill), 0);
	struct tool_desc tool{};
	EXPECT_EQ(runtime_tool_find_borrow(runtime, "activate_skill", &tool), 0);
	EXPECT_EQ(runtime_tool_find_borrow(runtime, "tasks", &tool), 0);
	EXPECT_EQ(runtime_tool_find_borrow(runtime, "ask_user", &tool), 0);
	EXPECT_EQ(runtime_tool_find_borrow(runtime, "apply_patch", &tool), 0);
	EXPECT_EQ(runtime_mcp_count(runtime), 1);
	struct runtime_mcp_status mcp{};
	EXPECT_EQ(runtime_mcp_find(runtime, "fastcgi-disabled", &mcp), 0);
	EXPECT_EQ(mcp.connected, 0);
	const struct config *loaded = fcgi_bridge_config_borrow(bridge);
	ASSERT_NE(loaded, nullptr);
	EXPECT_STREQ(loaded->dynamic_tools.mode, "server");
	EXPECT_STREQ(fcgi_artifact_output_dir_borrow(bridge), output.c_str());
	struct runtime *first = nullptr;
	struct runtime *second = nullptr;
	ASSERT_EQ(fcgi_bridge_runtime_acquire(bridge, "session-a", &first), 0);
	ASSERT_EQ(fcgi_bridge_runtime_acquire(bridge, "session-b", &second), 0);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_NE(first, second);
	EXPECT_EQ(fcgi_bridge_runtime_count(bridge), 2);
	fcgi_bridge_runtime_release(bridge, second);
	auto waiting = std::async(std::launch::async, [bridge] {
		struct runtime *acquired = nullptr;
		int rc = fcgi_bridge_runtime_acquire(bridge, "session-a", &acquired);

		return std::make_pair(rc, acquired);
	});
	EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(50)),
		  std::future_status::timeout);
	fcgi_bridge_runtime_release(bridge, first);
	ASSERT_EQ(waiting.wait_for(std::chrono::seconds(2)),
		  std::future_status::ready);
	auto waited = waiting.get();
	EXPECT_EQ(waited.first, 0);
	fcgi_bridge_runtime_release(bridge, waited.second);
	for (int i = 0; i < 30 && fcgi_bridge_runtime_count(bridge) != 1; i++)
		usleep(100000);
	EXPECT_EQ(fcgi_bridge_runtime_count(bridge), 1);
	first = nullptr;
	second = nullptr;
	ASSERT_EQ(fcgi_bridge_runtime_acquire(bridge, "queue-a", &first), 0);
	ASSERT_EQ(fcgi_bridge_runtime_acquire(bridge, "queue-b", &second), 0);
	auto queued = std::async(std::launch::async, [bridge] {
		struct runtime *acquired = nullptr;
		int rc = fcgi_bridge_runtime_acquire(bridge, "queue-c", &acquired);

		return std::make_pair(rc, acquired);
	});
	struct fcgi_bridge_pool_status pool{};
	for (int i = 0; i < 20; i++) {
		fcgi_bridge_pool_status(bridge, &pool);
		if (pool.waiting_turns == 1)
			break;
		usleep(10000);
	}
	EXPECT_EQ(pool.waiting_turns, 1);
	struct runtime *rejected = nullptr;
	EXPECT_EQ(fcgi_bridge_runtime_acquire(bridge, "queue-d", &rejected), NB_ERR_AGAIN);
	fcgi_bridge_runtime_release(bridge, first);
	ASSERT_EQ(queued.wait_for(std::chrono::seconds(2)),
		  std::future_status::ready);
	auto queued_result = queued.get();
	EXPECT_EQ(queued_result.first, 0);
	fcgi_bridge_runtime_release(bridge, queued_result.second);
	fcgi_bridge_runtime_release(bridge, second);
	first = nullptr;
	ASSERT_EQ(fcgi_bridge_runtime_acquire(bridge, "ticket-held", &first), 0);
	struct fcgi_turn_ticket *ticket NB_CLEANUP(fcgi_turn_ticket_cleanup) = nullptr;
	ASSERT_EQ(fcgi_bridge_turn_enqueue(bridge, "ticket-held", &ticket, nullptr), 0);
	struct fcgi_turn_ticket *original = ticket;
	struct runtime *timed_out = nullptr;
	EXPECT_EQ(fcgi_bridge_turn_acquire(bridge, ticket, &timed_out), NB_ERR_TIMEOUT);
	EXPECT_EQ(ticket, original);
	EXPECT_EQ(timed_out, nullptr);
	fcgi_bridge_pool_status(bridge, &pool);
	EXPECT_EQ(pool.waiting_turns, 0);
	fcgi_turn_ticket_cleanup(&ticket);
	EXPECT_EQ(ticket, nullptr);
	ASSERT_EQ(fcgi_bridge_turn_enqueue(bridge, "ticket-held", &ticket, nullptr), 0);
	original = ticket;
	auto cancelled = std::async(std::launch::async, [bridge, ticket] {
		struct runtime *acquired = nullptr;
		int rc = fcgi_bridge_turn_acquire(bridge, ticket, &acquired);

		return std::make_pair(rc, acquired);
	});
	EXPECT_EQ(cancelled.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
	fcgi_bridge_turn_cancel(bridge, ticket);
	ASSERT_EQ(cancelled.wait_for(std::chrono::seconds(2)), std::future_status::ready);
	auto cancelled_result = cancelled.get();
	EXPECT_EQ(cancelled_result.first, NB_ERR_CANCELLED);
	EXPECT_EQ(cancelled_result.second, nullptr);
	EXPECT_EQ(ticket, original);
	fcgi_turn_ticket_cleanup(&ticket);
	fcgi_turn_ticket_cleanup(&ticket);
	fcgi_bridge_runtime_release(bridge, first);
	ASSERT_EQ(fcgi_bridge_turn_enqueue(bridge, "ticket-success", &ticket, nullptr), 0);
	first = nullptr;
	ASSERT_EQ(fcgi_bridge_turn_acquire(bridge, ticket, &first), 0);
	fcgi_turn_ticket_cleanup(&ticket);
	fcgi_bridge_pool_status(bridge, &pool);
	EXPECT_EQ(pool.waiting_turns, 0);
	EXPECT_EQ(pool.busy_workers, 1);
	fcgi_bridge_runtime_release(bridge, first);
	const std::string second_output = root + "/second-output";
	const std::string second_database = root + "/second.db";
	setenv("MORPH_FCGI_OUTPUT_DIR", second_output.c_str(), 1);
	struct fcgi_bridge *other = nullptr;
	ASSERT_EQ(fcgi_bridge_init(&other, second_database.c_str()), 0);
	EXPECT_NE(fcgi_bridge_runtime_borrow(bridge), fcgi_bridge_runtime_borrow(other));
	EXPECT_STREQ(fcgi_artifact_output_dir_borrow(bridge), output.c_str());
	EXPECT_STREQ(fcgi_artifact_output_dir_borrow(other), second_output.c_str());
	fcgi_bridge_shutdown(other);
	EXPECT_NE(fcgi_bridge_runtime_borrow(bridge), nullptr);
	ASSERT_EQ(fcgi_bridge_turn_begin(bridge), 0);
	ASSERT_EQ(fcgi_bridge_turn_enqueue(bridge, "shutdown-ticket", &ticket, nullptr), 0);
	first = nullptr;
	ASSERT_EQ(fcgi_bridge_turn_acquire(bridge, ticket, &first), 0);

	auto shutdown = std::async(std::launch::async, [bridge] { fcgi_bridge_shutdown(bridge); });
	EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	fcgi_turn_ticket_cleanup(&ticket);
	fcgi_bridge_turn_end(bridge);
	EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	fcgi_bridge_runtime_release(bridge, first);
	ASSERT_EQ(shutdown.wait_for(std::chrono::seconds(2)), std::future_status::ready);
	shutdown.get();
	unsetenv("MORPH_FCGI_CONFIG");
	unsetenv("MORPH_FCGI_OUTPUT_DIR");
	unsetenv("MORPH_FCGI_LOG_FILE");
	unsetenv("MORPH_FCGI_RUNTIME_MIN_WORKERS");
	unsetenv("MORPH_FCGI_RUNTIME_MAX_WORKERS");
	unsetenv("MORPH_FCGI_RUNTIME_QUEUE_MAX");
	unsetenv("MORPH_FCGI_RUNTIME_IDLE_SECONDS");
	unsetenv("MORPH_FCGI_RUNTIME_WAIT_SECONDS");
	std::filesystem::remove_all(root);
}
