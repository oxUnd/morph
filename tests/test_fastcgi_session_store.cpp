#include <gtest/gtest.h>

extern "C" {
#include "sapi/fastcgi/storage/quota.h"
#include "sapi/fastcgi/storage/artifacts.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "sapi/fastcgi/storage/events.h"

#include "sapi/fastcgi/session_store.h"
}

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class FastcgiSessionStoreTest : public ::testing::Test
{
      protected:
	struct session_store *store = nullptr;

	void SetUp() override
	{
		store = session_store_open(nullptr, ":memory:");
		ASSERT_NE(store, nullptr);
		ASSERT_EQ(sqlite3_exec(store->db.handle,
				       "INSERT INTO fcgi_user_quota(user_id,profile) "
				       "VALUES('user','admin');"
				       "UPDATE fcgi_quota_profiles SET max_concurrent_turns=-1 "
				       "WHERE name='admin'",
				       nullptr, nullptr, nullptr),
			  SQLITE_OK);
	}

	void TearDown() override
	{
		session_store_close(store);
	}

	int register_artifact()
	{
		return store_artifact_register(store, "user", "session", "image", "image/png",
					       "test.png", "test.png", 10, nullptr);
	}

	int64_t query(const char *sql)
	{
		sqlite3_stmt *statement = nullptr;
		int rc = sqlite3_prepare_v2(store->db.handle, sql, -1, &statement, nullptr);

		EXPECT_EQ(rc, SQLITE_OK);
		if (rc != SQLITE_OK)
			return -1;
		rc = sqlite3_step(statement);
		EXPECT_EQ(rc, SQLITE_ROW);
		int64_t value = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;

		sqlite3_finalize(statement);
		return value;
	}

	template <typename Function> void concurrently(int count, Function function)
	{
		std::mutex mutex;
		std::condition_variable condition;
		int ready = 0;
		bool start = false;
		std::vector<std::thread> threads;

		for (int i = 0; i < count; i++) {
			threads.emplace_back([&, i] {
				{
					std::unique_lock<std::mutex> lock(mutex);

					ready++;
					condition.notify_all();
					condition.wait(lock, [&] { return start; });
				}
				function(i);
			});
		}
		{
			std::unique_lock<std::mutex> lock(mutex);

			condition.wait(lock, [&] { return ready == count; });
			start = true;
		}
		condition.notify_all();
		for (auto &thread : threads)
			thread.join();
	}
};

TEST_F(FastcgiSessionStoreTest, ConcurrentArtifactsPreserveAllUsage)
{
	std::atomic<int> failures{0};

	concurrently(8, [&](int) {
		for (int i = 0; i < 100; i++) {
			if (register_artifact() != 0)
				failures++;
		}
	});
	EXPECT_EQ(failures.load(), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 800);
	EXPECT_EQ(query("SELECT used_bytes FROM fcgi_usage_storage "
			"WHERE user_id='user'"),
		  8000);
	EXPECT_EQ(query("SELECT images FROM fcgi_usage_daily "
			"WHERE user_id='user' AND day=date('now')"),
		  800);
}

TEST_F(FastcgiSessionStoreTest, ConcurrentArtifactsRespectImageAndStorageQuotas)
{
	for (const char *limit : {"daily_images=7", "storage_bytes=75"}) {
		std::string sql = "DELETE FROM fcgi_artifacts;"
				  "DELETE FROM fcgi_usage_daily;DELETE FROM fcgi_usage_storage;"
				  "UPDATE fcgi_quota_profiles SET daily_images=-1,"
				  "storage_bytes=-1 WHERE name='admin';"
				  "UPDATE fcgi_quota_profiles SET " +
				  std::string(limit) + " WHERE name='admin'";
		ASSERT_EQ(sqlite3_exec(store->db.handle, sql.c_str(), nullptr, nullptr, nullptr),
			  SQLITE_OK);
		std::atomic<int> successes{0}, rejected{0}, failures{0};

		concurrently(8, [&](int) {
			for (int i = 0; i < 10; i++) {
				int rc = register_artifact();

				if (rc == 0)
					successes++;
				else if (rc == -EDQUOT)
					rejected++;
				else
					failures++;
			}
		});
		EXPECT_EQ(successes.load(), 7) << limit;
		EXPECT_EQ(rejected.load(), 73) << limit;
		EXPECT_EQ(failures.load(), 0) << limit;
		EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 7);
		EXPECT_EQ(query("SELECT used_bytes FROM fcgi_usage_storage "
				"WHERE user_id='user'"),
			  70);
		EXPECT_EQ(query("SELECT images FROM fcgi_usage_daily "
				"WHERE user_id='user' AND day=date('now')"),
			  7);
	}
}

TEST_F(FastcgiSessionStoreTest, ArtifactFailureRollsBackUsageAndReleasesConnection)
{
	ASSERT_EQ(sqlite3_exec(store->db.handle,
			       "CREATE TRIGGER reject_usage BEFORE INSERT ON fcgi_usage_daily "
			       "BEGIN SELECT RAISE(FAIL,'test usage failure'); END",
			       nullptr, nullptr, nullptr),
		  SQLITE_OK);
	EXPECT_EQ(register_artifact(), -EIO);
	EXPECT_EQ(sqlite3_get_autocommit(store->db.handle), 1);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_usage_storage"), 0);
	ASSERT_EQ(sqlite3_exec(store->db.handle, "DROP TRIGGER reject_usage", nullptr, nullptr,
			       nullptr),
		  SQLITE_OK);
	sqlite3_commit_hook(store->db.handle, [](void *) { return 1; }, nullptr);
	EXPECT_EQ(register_artifact(), -EIO);
	sqlite3_commit_hook(store->db.handle, nullptr, nullptr);
	EXPECT_EQ(sqlite3_get_autocommit(store->db.handle), 1);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_usage_storage"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_usage_daily"), 0);
	concurrently(1, [&](int) { EXPECT_EQ(register_artifact(), 0); });
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 1);
	EXPECT_EQ(query("SELECT used_bytes FROM fcgi_usage_storage"), 10);
}

TEST_F(FastcgiSessionStoreTest, FailedBeginPreservesTheExistingTransaction)
{
	ASSERT_EQ(sqlite3_exec(store->db.handle, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
		  SQLITE_OK);
	EXPECT_EQ(register_artifact(), -EIO);
	EXPECT_EQ(store_quota_begin_turn(store, "user", "session", nullptr), -EIO);
	EXPECT_EQ(store_delete_session(store, "session", "user"), -EIO);
	struct action_record action{};

	EXPECT_EQ(actions_drain_one(store, "actions", &action), 0);
	EXPECT_EQ(sqlite3_get_autocommit(store->db.handle), 0);
	ASSERT_EQ(sqlite3_exec(store->db.handle, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
	concurrently(1, [&](int) { EXPECT_EQ(register_artifact(), 0); });
}

TEST_F(FastcgiSessionStoreTest, ArtifactTransactionsCoexistWithOtherStoreOperations)
{
	std::vector<std::string> sessions;

	for (int i = 0; i < 100; i++) {
		char session_id[64];
		std::string name = "session-" + std::to_string(i);

		ASSERT_EQ(
			store_create_session(store, "user", name.c_str(), "test-model", session_id),
			0);
		sessions.emplace_back(session_id);
		ASSERT_EQ(actions_enqueue(store, "actions", "test", "{}"), 0);
	}
	concurrently(5, [&](int worker) {
		for (int i = 0; i < 100; i++) {
			if (worker == 0) {
				EXPECT_EQ(register_artifact(), 0);
			} else if (worker == 1) {
				char turn_id[64];
				int rc = store_quota_begin_turn(store, "user", "session", turn_id);

				EXPECT_EQ(rc, 0);
				if (rc == 0)
					store_quota_end_turn(store, turn_id);
			} else if (worker == 2) {
				struct action_record action{};

				EXPECT_EQ(actions_drain_one(store, "actions", &action), 1);
				free(action.payload_json);
			} else if (worker == 3) {
				EXPECT_EQ(store_delete_session(
						  store, sessions[static_cast<size_t>(i)].c_str(),
						  "user"),
					  0);
			} else {
				EXPECT_EQ(events_publish(store, "events", "test", "{}"), 0);
			}
		}
	});
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 100);
	EXPECT_EQ(query("SELECT used_bytes FROM fcgi_usage_storage"), 1000);
	EXPECT_EQ(query("SELECT images FROM fcgi_usage_daily"), 100);
	EXPECT_EQ(query("SELECT turns FROM fcgi_usage_daily"), 100);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_running_turns"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_actions WHERE consumed=1"), 100);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_session_owner"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_events"), 100);
}
