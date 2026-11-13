#include <gtest/gtest.h>
#include "nb/err.h"

extern "C" {
#include "sapi/fastcgi/storage/quota.h"
#include "sapi/fastcgi/storage/artifacts.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/storage/canvas.h"
#include "sapi/fastcgi/storage/tokens.h"
#include "nb/file.h"
#include "nb/cleanup.h"
#include "cJSON.h"

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
#include <csignal>
#include <cstring>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

class FastcgiLoginTokenTest : public ::testing::Test
{
      protected:
	std::vector<std::string> owned;

	std::string create()
	{
		char token[64] = {0};
		int rc = login_token_create("token-user", "alice", "user", 1, token);
		EXPECT_EQ(rc, NB_OK);
		if (rc != NB_OK)
			return {};
		owned.emplace_back(token);
		return token;
	}

	std::string path(const std::string &token)
	{
		return "/tmp/morph-sess/" + token + ".json";
	}

	std::string record(int64_t expires, size_t padding = 0)
	{
		return "{\"padding\":\"" + std::string(padding, 'x') +
		       "\",\"user_id\":\"token-user\",\"username\":\"alice\",\"role\":\"user\","
		       "\"expires_at\":" +
		       std::to_string(expires) + "}";
	}

	void write(const std::string &token, const std::string &content)
	{
		ASSERT_EQ(nb_file_write_all(path(token).c_str(), content.data(), content.size(),
					    nullptr),
			  NB_OK);
	}

	void TearDown() override
	{
		for (const auto &token : owned)
			login_token_revoke(token.c_str());
	}
};

TEST_F(FastcgiLoginTokenTest, PublishesPrivateCompleteCredentialAndRevokesIt)
{
	auto token = create();
	ASSERT_FALSE(token.empty());
	nb_file_info info{};
	ASSERT_EQ(nb_file_stat(path(token).c_str(), false, &info, nullptr), NB_OK);
	EXPECT_EQ(info.mode & 0777u, 0600u);
	ASSERT_EQ(nb_file_stat("/tmp/morph-sess", false, &info, nullptr), NB_OK);
	EXPECT_EQ(info.mode & 0777u, 0700u);
	char user[64] = {0}, name[64] = {0}, role[24] = {0};
	ASSERT_EQ(login_token_verify(token.c_str(), user, name, role, nullptr), 1);
	EXPECT_STREQ(user, "token-user");
	EXPECT_STREQ(name, "alice");
	EXPECT_STREQ(role, "user");
	login_token_revoke(token.c_str());
	EXPECT_FALSE(nb_file_exists(path(token).c_str()));
	EXPECT_EQ(login_token_verify(token.c_str(), user, name, role, nullptr), 0);
	EXPECT_EQ(login_token_verify("../escaped", user, name, role, nullptr), 0);
}

TEST_F(FastcgiLoginTokenTest, ReadsLongCompleteRecordsAndPrunesOnlyParsedExpiredTokens)
{
	auto active = create(), expired = create(), malformed = create();
	ASSERT_FALSE(active.empty());
	ASSERT_FALSE(expired.empty());
	ASSERT_FALSE(malformed.empty());
	const int64_t future = INT64_MAX / 1000 - 1;
	write(active, record(future, 5000));
	write(expired, record(1, 6000));
	write(malformed, "{malformed");
	char user[64] = {0}, name[64] = {0}, role[24] = {0};
	int64_t expires = 0;
	ASSERT_EQ(login_token_verify(active.c_str(), user, name, role, &expires), 1);
	EXPECT_EQ(expires, future);
	EXPECT_STREQ(user, "token-user");
	EXPECT_STREQ(name, "alice");
	EXPECT_STREQ(role, "user");
	ASSERT_EQ(login_token_prune(), NB_OK);
	EXPECT_TRUE(nb_file_exists(path(active).c_str()));
	EXPECT_FALSE(nb_file_exists(path(expired).c_str()));
	EXPECT_TRUE(nb_file_exists(path(malformed).c_str()));
}

TEST_F(FastcgiLoginTokenTest, RejectsOversizeOrTrailingBytesWithoutDeletingInvalidRecords)
{
	auto oversized = create(), trailing = create(), invalid_expiry = create(),
	     rounded_expiry = create();
	ASSERT_FALSE(oversized.empty());
	ASSERT_FALSE(trailing.empty());
	ASSERT_FALSE(invalid_expiry.empty());
	ASSERT_FALSE(rounded_expiry.empty());
	write(oversized, record(1, 65536));
	write(trailing, record(1) + std::string(1, '\0') + "trailing");
	write(invalid_expiry, "{\"expires_at\":1e100}");
	write(rounded_expiry, record(INT64_MAX));
	for (const auto &token : {oversized, trailing, invalid_expiry, rounded_expiry})
		EXPECT_EQ(login_token_verify(token.c_str(), nullptr, nullptr, nullptr, nullptr), 0);
	ASSERT_EQ(login_token_prune(), NB_OK);
	for (const auto &token : {oversized, trailing, invalid_expiry, rounded_expiry})
		EXPECT_TRUE(nb_file_exists(path(token).c_str()));
	char output[64] = "unchanged";
	std::string too_long(65536, 'x');
	EXPECT_EQ(login_token_create("id", too_long.c_str(), "user", 1, output), NB_ERR_TOO_BIG);
	EXPECT_STREQ(output, "unchanged");
}

TEST_F(FastcgiLoginTokenTest, FailedCredentialWriteDoesNotPublishAToken)
{
	auto initial = create();
	ASSERT_FALSE(initial.empty());
	pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0) {
		struct rlimit limit{};
		if (getrlimit(RLIMIT_FSIZE, &limit) != 0)
			_exit(2);
		limit.rlim_cur = 0;
		if (setrlimit(RLIMIT_FSIZE, &limit) != 0 || signal(SIGXFSZ, SIG_IGN) == SIG_ERR)
			_exit(3);
		char token[64] = "unchanged";
		int rc = login_token_create("id", "name", "user", 1, token);
		bool failed = rc < 0 && strcmp(token, "unchanged") == 0;
		if (rc == NB_OK)
			login_token_revoke(token);
		_exit(failed ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
}

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

TEST_F(FastcgiSessionStoreTest, CanvasListsEmptyAndPopulatedSessions)
{
	char *text = nullptr;
	char id[40];

	ASSERT_EQ(canvas_list_json(store, "session", &text), 0);
	cJSON *document = cJSON_Parse(text);
	free(text);
	ASSERT_NE(document, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "nodes")), 0);
	cJSON_Delete(document);
	ASSERT_EQ(canvas_add_node(store, "session", "{\"label\":\"first\"}", id), 0);
	ASSERT_EQ(canvas_add_node(store, "other", "{}", id), 0);
	ASSERT_EQ(canvas_list_json(store, "session", &text), 0);
	document = cJSON_Parse(text);
	free(text);
	ASSERT_NE(document, nullptr);
	cJSON *nodes = cJSON_GetObjectItem(document, "nodes");
	ASSERT_EQ(cJSON_GetArraySize(nodes), 1);
	cJSON *meta = cJSON_GetObjectItem(cJSON_GetArrayItem(nodes, 0), "meta");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(meta, "label")), "first");
	cJSON_Delete(document);
}

TEST_F(FastcgiSessionStoreTest, CanvasReadFailureDoesNotReturnPartialOutput)
{
	char id[40];
	char *text = nullptr;

	ASSERT_EQ(canvas_add_node(store, "session", "{}", id), 0);
	sqlite3_progress_handler(store->db.handle, 1, [](void *) { return 1; }, nullptr);
	EXPECT_NE(canvas_list_json(store, "session", &text), 0);
	EXPECT_EQ(text, nullptr);
	sqlite3_progress_handler(store->db.handle, 0, nullptr, nullptr);
	EXPECT_EQ(sqlite3_next_stmt(store->db.handle, nullptr), nullptr);
	ASSERT_EQ(canvas_list_json(store, "session", &text), 0);
	free(text);
}

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
				else if (rc == NB_ERR_NO_SPACE)
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
	EXPECT_EQ(register_artifact(), NB_ERR_IO);
	EXPECT_EQ(sqlite3_get_autocommit(store->db.handle), 1);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_artifacts"), 0);
	EXPECT_EQ(query("SELECT count(*) FROM fcgi_usage_storage"), 0);
	ASSERT_EQ(sqlite3_exec(store->db.handle, "DROP TRIGGER reject_usage", nullptr, nullptr,
			       nullptr),
		  SQLITE_OK);
	sqlite3_commit_hook(store->db.handle, [](void *) { return 1; }, nullptr);
	EXPECT_EQ(register_artifact(), NB_ERR_IO);
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
	EXPECT_EQ(register_artifact(), NB_ERR_IO);
	EXPECT_EQ(store_quota_begin_turn(store, "user", "session", nullptr), NB_ERR_IO);
	EXPECT_EQ(store_delete_session(store, "session", "user"), NB_ERR_IO);
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
