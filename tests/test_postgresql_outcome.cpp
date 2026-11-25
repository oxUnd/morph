#include <gtest/gtest.h>

extern "C" {
#include "errors.h"
#include "persistence/orm/checkpoint_store.h"
#include "persistence/orm/database.h"
#include "persistence/orm/session.h"
#include <nb/cleanup.h>
}

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

class OutcomeSocket {
public:
	~OutcomeSocket() { Reset(); }
	OutcomeSocket() = default;
	OutcomeSocket(const OutcomeSocket &) = delete;
	OutcomeSocket &operator=(const OutcomeSocket &) = delete;
	int Get() const { return descriptor; }
	int Release()
	{
		const int result = descriptor;
		descriptor = -1;
		return result;
	}
	void Reset(int value = -1)
	{
		if (descriptor >= 0)
			close(descriptor);

		descriptor = value;
		if (descriptor >= 0) {
			const struct timeval timeout = {2, 0};
			setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
			const int enabled = 1;
			setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
		}
	}

private:
	int descriptor = -1;
};

/* A framed PGwire relay drops only an armed COMMIT response, after observing
 * the server's successful CommandComplete. Earlier transactions pass through. */
class OutcomeCommitProxy {
public:
	~OutcomeCommitProxy() { Stop(); }
	bool Start(const std::string &source)
	{
		const size_t scheme = source.find("://");
		if (scheme == std::string::npos)
			return false;

		const size_t authority = scheme + 3;
		const size_t end = source.find_first_of("/?", authority);
		if (end == std::string::npos)
			return false;

		const size_t user = source.rfind('@', end);
		const size_t host_begin = user != std::string::npos && user >= authority
						? user + 1 : authority;
		std::string host = source.substr(host_begin, end - host_begin);
		std::string port = "5432";
		if (!host.empty() && host.front() == '[') {
			const size_t bracket = host.find(']');
			if (bracket == std::string::npos)
				return false;

			if (bracket + 1 < host.size())
				port = host.substr(bracket + 2);

			host = host.substr(1, bracket - 1);
		} else {
			const size_t colon = host.rfind(':');
			if (colon != std::string::npos) {
				port = host.substr(colon + 1);
				host.resize(colon);
			}
		}

		struct addrinfo hints{};
		hints.ai_socktype = SOCK_STREAM;
		struct addrinfo *raw = nullptr;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &raw) != 0)
			return false;

		std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> addresses(raw,
										    freeaddrinfo);
		for (const auto *address = addresses.get(); address; address = address->ai_next) {
			OutcomeSocket candidate;
			candidate.Reset(socket(address->ai_family, address->ai_socktype,
					       address->ai_protocol));
			if (candidate.Get() >= 0 &&
			    connect(candidate.Get(), address->ai_addr, address->ai_addrlen) == 0) {
				server.Reset(candidate.Release());
				break;
			}
		}

		if (server.Get() < 0)
			return false;

		listener.Reset(socket(AF_INET, SOCK_STREAM, 0));
		struct sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (listener.Get() < 0 ||
		    bind(listener.Get(), reinterpret_cast<struct sockaddr *>(&address),
			 sizeof(address)) != 0 || listen(listener.Get(), 1) != 0)
			return false;

		socklen_t length = sizeof(address);
		if (getsockname(listener.Get(), reinterpret_cast<struct sockaddr *>(&address),
				&length) != 0)
			return false;

		url = source.substr(0, host_begin) + "127.0.0.1:" +
		      std::to_string(ntohs(address.sin_port)) + source.substr(end);
		worker = std::thread([this] { Relay(); });
		return true;
	}
	void Stop()
	{
		stopping.store(true);
		if (worker.joinable())
			worker.join();

		listener.Reset();
		server.Reset();
	}
	const std::string &Url() const { return url; }
	void Arm() { armed.store(true); }
	bool ConfirmedCommit() const { return committed.load(); }

private:
	static uint32_t Length(const unsigned char *data)
	{
		uint32_t value = 0;
		memcpy(&value, data, sizeof(value));
		return ntohl(value);
	}
	static bool Send(int descriptor, const unsigned char *data, size_t length)
	{
		while (length) {
#ifdef MSG_NOSIGNAL
			const ssize_t sent = send(descriptor, data, length, MSG_NOSIGNAL);
#else
			const ssize_t sent = send(descriptor, data, length, 0);
#endif
			if (sent < 0 && errno == EINTR)
				continue;

			if (sent <= 0)
				return false;

			data += static_cast<size_t>(sent);
			length -= static_cast<size_t>(sent);
		}

		return true;
	}
	static bool IsCommit(const unsigned char *data, size_t length)
	{
		if (length < 6 || (data[0] != 'P' && data[0] != 'Q'))
			return false;

		size_t offset = 5;
		if (data[0] == 'P') {
			while (offset < length && data[offset])
				offset++;

			offset++;
		}

		const size_t start = offset;
		while (offset < length && data[offset])
			offset++;

		return offset < length && offset - start == 6 &&
		       memcmp(data + start, "COMMIT", 6) == 0;
	}
	bool Forward(std::vector<unsigned char> &bytes, bool client, int destination)
	{
		while (!bytes.empty()) {
			if (!client && negotiating) {
				if (bytes.front() != 'N' || !Send(destination, bytes.data(), 1))
					return false;

				bytes.erase(bytes.begin());
				negotiating = false;
				continue;
			}

			const bool startup = client && starting;
			const size_t header = startup ? 4 : 5;
			if (bytes.size() < header)
				return true;

			const size_t length = Length(bytes.data() + (startup ? 0 : 1));
			if (length < 4 || length > 16U * 1024U * 1024U)
				return false;

			const size_t frame = length + (startup ? 0 : 1);
			if (bytes.size() < frame)
				return true;

			if (startup) {
				if (length < 8)
					return false;

				starting = Length(bytes.data() + 4) != UINT32_C(196608);
				negotiating = starting;
			} else if (client && armed.load() && IsCommit(bytes.data(), frame)) {
				dropping = true;
			}

			if (!client && dropping) {
				if (bytes[0] == 'C' && length == 11 &&
				    memcmp(bytes.data() + 5, "COMMIT", 7) == 0) {
					committed.store(true);
					return false;
				}
			} else if (!Send(destination, bytes.data(), frame)) {
				return false;
			}

			bytes.erase(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(frame));
		}

		return true;
	}
	void Relay()
	{
		OutcomeSocket client;
		while (!stopping.load() && client.Get() < 0) {
			struct pollfd descriptor = {listener.Get(), POLLIN, 0};
			if (poll(&descriptor, 1, 100) > 0 && (descriptor.revents & POLLIN))
				client.Reset(accept(listener.Get(), nullptr, nullptr));
		}

		listener.Reset();
		std::vector<unsigned char> incoming[2];
		while (!stopping.load() && client.Get() >= 0) {
			struct pollfd descriptors[] = {{client.Get(), POLLIN, 0},
						       {server.Get(), POLLIN, 0}};
			const int ready = poll(descriptors, 2, 100);
			if (ready < 0 && errno != EINTR)
				return;

			for (size_t i = 0; ready > 0 && i < 2; i++) {
				if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)))
					continue;

				unsigned char buffer[16384];
				const ssize_t length =
					recv(descriptors[i].fd, buffer, sizeof(buffer), 0);
				if (length < 0 && errno == EINTR)
					continue;

				if (length <= 0)
					return;

				incoming[i].insert(incoming[i].end(), buffer, buffer + length);
				if (!Forward(incoming[i], i == 0,
					     i == 0 ? server.Get() : client.Get()))
					return;
			}
		}
	}
	OutcomeSocket listener;
	OutcomeSocket server;
	std::thread worker;
	std::string url;
	std::atomic<bool> stopping{false};
	std::atomic<bool> armed{false};
	std::atomic<bool> committed{false};
	bool starting = true;
	bool negotiating = false;
	bool dropping = false;
};

class PostgresqlOutcomeTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		const char *source = getenv("MORPH_TEST_POSTGRESQL_URL");
		ASSERT_NE(source, nullptr);
		ASSERT_TRUE(proxy.Start(source));
		const char *previous = getenv("MORPH_OUTCOME_TEST_URL");
		had_environment = previous != nullptr;
		if (previous)
			previous_environment = previous;

		ASSERT_EQ(setenv("MORPH_OUTCOME_TEST_URL", proxy.Url().c_str(), 1), 0);
		environment_changed = true;
		char pattern[] = "/tmp/morph-postgresql-outcome-XXXXXX";
		const char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		const auto options = Options("MORPH_OUTCOME_TEST_URL");
		ASSERT_EQ(db_open_options(nullptr, &database, &options), NB_OK);
		ASSERT_EQ(db_init_schema(&database), NB_OK);
		ASSERT_EQ(session_create(&database, directory.c_str(), "outcome-test", &session),
			  NB_OK);
	}
	void TearDown() override
	{
		db_close(&database);
		proxy.Stop();
		struct db direct NB_CLEANUP(db_close) = {};
		const auto options = Options("MORPH_TEST_POSTGRESQL_URL");
		if (session.id && db_open_options(nullptr, &direct, &options) == NB_OK)
			EXPECT_EQ(session_delete(&direct, session.id), NB_OK);

		if (environment_changed) {
			if (had_environment)
				EXPECT_EQ(setenv("MORPH_OUTCOME_TEST_URL",
						 previous_environment.c_str(), 1),
					  0);
			else
				EXPECT_EQ(unsetenv("MORPH_OUTCOME_TEST_URL"), 0);
		}

		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}
	static struct db_options Options(const char *environment)
	{
		struct db_options options{};
		options.backend = DB_BACKEND_POSTGRESQL;
		options.url_env = environment;
		options.tls_mode = DB_TLS_DISABLE;
		options.query_timeout_ms = 10000;
		return options;
	}
	bool Write(const std::string &text)
	{
		std::ofstream file(directory + "/file.txt");
		file << text;
		return file.good();
	}
	static std::string Read(const std::string &path)
	{
		std::ifstream file(path);
		return std::string(std::istreambuf_iterator<char>(file),
				   std::istreambuf_iterator<char>());
	}
	OutcomeCommitProxy proxy;
	struct db database{};
	struct session session{};
	std::string directory;
	std::string previous_environment;
	bool had_environment = false;
	bool environment_changed = false;
};

TEST_F(PostgresqlOutcomeTest, RestoreRetainsRecoveryWhenCommittedResponseIsLost)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "before checkpoint", 1), NB_OK);
	ASSERT_TRUE(Write("before"));
	int64_t checkpoint = 0;
	ASSERT_EQ(checkpoint_store_create(&database, session.id, directory.c_str(), "outcome-turn",
					  "edit", &checkpoint), NB_OK);
	ASSERT_EQ(checkpoint_store_capture(&database, checkpoint, "file.txt"), NB_OK);
	ASSERT_TRUE(Write("after"));
	ASSERT_EQ(checkpoint_store_seal(&database, checkpoint), NB_OK);
	ASSERT_EQ(message_add(&database, session.id, "assistant", "after checkpoint", 1), NB_OK);
	ASSERT_EQ(message_count(&database, session.id), 2);

	proxy.Arm();
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_EQ(checkpoint_store_restore(&database, session.id, checkpoint,
					   CHECKPOINT_RESTORE_BOTH, &report),
		  MORPH_ERR_DB_OUTCOME_UNKNOWN);
	ASSERT_TRUE(proxy.ConfirmedCommit());
	ASSERT_NE(report, nullptr);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "database_outcome_unknown")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "code_restored")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "conversation_restored")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "compensated")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(report, "database_rollback_failed")));
	ASSERT_TRUE(cJSON_IsNumber(cJSON_GetObjectItem(report, "error_code")));
	EXPECT_EQ(cJSON_GetObjectItem(report, "error_code")->valueint,
		  MORPH_ERR_DB_OUTCOME_UNKNOWN);
	EXPECT_EQ(Read(directory + "/file.txt"), "before");
	cJSON *effects = cJSON_GetObjectItem(report, "effects");
	ASSERT_EQ(cJSON_GetArraySize(effects), 1);
	cJSON *effect = cJSON_GetArrayItem(effects, 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "applied")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "compensated")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(effect, "recovery_available")));
	cJSON *recovery = cJSON_GetObjectItem(effect, "recovery_path");
	ASSERT_TRUE(cJSON_IsString(recovery));
	const auto recovery_path = std::filesystem::path(directory) / recovery->valuestring;
	EXPECT_TRUE(std::filesystem::is_regular_file(recovery_path));
	EXPECT_EQ(Read(recovery_path.string()), "after");

	struct db direct NB_CLEANUP(db_close) = {};
	const auto options = Options("MORPH_TEST_POSTGRESQL_URL");
	ASSERT_EQ(db_open_options(nullptr, &direct, &options), NB_OK);
	EXPECT_EQ(message_count(&direct, session.id), 1);
	cJSON *info NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(checkpoint_store_info_copy(&direct, session.id, checkpoint, &info), NB_OK);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(info, "code_active")));
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(info, "conversation_restorable")));
}
