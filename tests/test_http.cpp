#include <gtest/gtest.h>
#include "http/client.h"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

class HttpTest : public ::testing::Test {
protected:
  void SetUp() override
  {
	  http_init(NULL);
  }
	void TearDown() override { http_cleanup(); }
};

TEST_F(HttpTest, InitCleanup) {
	EXPECT_NO_FATAL_FAILURE(http_cleanup());
	EXPECT_NO_FATAL_FAILURE(http_init(NULL));
}

TEST_F(HttpTest, GetInvalid) {
	struct http_response resp;
	int rc = http_get(NULL, "http://127.0.0.1:1/nonexistent", &resp);
	EXPECT_NE(rc, 0);
}

TEST_F(HttpTest, ResponseFree) {
	struct http_response resp = {0};
	EXPECT_NO_FATAL_FAILURE(http_response_free(&resp));
}

TEST_F(HttpTest, ResponseFreeNull) {
	EXPECT_NO_FATAL_FAILURE(http_response_free(nullptr));
}

TEST_F(HttpTest, GetNullUrl) {
	struct http_response resp;
	EXPECT_NE(http_get(NULL, nullptr, &resp), 0);
}

TEST_F(HttpTest, PostNullUrl) {
	struct http_response resp;
	EXPECT_NE(http_post(NULL, nullptr, "", 0, "", &resp), 0);
}

TEST_F(HttpTest, PostTimeoutReturnsTimedOut) {
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(server_fd, 0);
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ(bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr),
		       sizeof(addr)), 0);
	socklen_t addr_len = sizeof(addr);
	ASSERT_EQ(getsockname(server_fd,
			      reinterpret_cast<struct sockaddr *>(&addr),
			      &addr_len), 0);
	ASSERT_EQ(listen(server_fd, 1), 0);

	std::thread server([server_fd]() {
		int client_fd = accept(server_fd, nullptr, nullptr);
		if (client_fd >= 0) {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			close(client_fd);
		}
		close(server_fd);
	});
	char url[128];
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
		 static_cast<unsigned>(ntohs(addr.sin_port)));
	struct http_response resp = {};
	int rc = http_post_ex_timeout(NULL, url, "{}", 2, "application/json", nullptr, 0, 1, &resp);

	EXPECT_EQ(rc, -ETIMEDOUT);
	http_response_free(&resp);
	server.join();
}

TEST_F(HttpTest, DoubleInit) {
	EXPECT_NO_FATAL_FAILURE(http_init(NULL));
	EXPECT_NO_FATAL_FAILURE(http_init(NULL));
}

TEST_F(HttpTest, CancellationInterruptsSilentPeersPromptly) {
	for (int kind = 0; kind < 4; kind++) {
		SCOPED_TRACE(kind);
		int listener = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listener, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address),
			sizeof(address)), 0);
		socklen_t length = sizeof(address);
		ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address),
			&length), 0);
		ASSERT_EQ(listen(listener, 1), 0);
		std::mutex mutex;
		std::condition_variable condition;
		bool finished = false;
		std::thread server([&] {
			int peer = accept(listener, nullptr, nullptr);
			std::unique_lock<std::mutex> lock(mutex);
			condition.wait_for(lock, std::chrono::seconds(3), [&] { return finished; });
			if (peer >= 0) close(peer);
			close(listener);
		});
		morph_cancel_token token{};
		http_control control{};
		control.cancel_token = &token;
		std::thread canceller([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			morph_cancel_token_cancel(&token);
		});
		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
			static_cast<unsigned>(ntohs(address.sin_port)));
		http_response response{};
		http_session session{};
		EXPECT_EQ(http_session_init(nullptr, &session), 0);
		http_multipart_part part{HTTP_MULTIPART_TEXT, "text", "test", nullptr};
		auto start = std::chrono::steady_clock::now();
		int rc;
		if (kind == 0)
			rc = http_post(&control, url, "{}", 2, "application/json", &response);
		else if (kind == 1)
			rc = http_post_sse(&control, url, "{}", 2, "application/json",
				[](const char *, size_t, void *) { return 0; }, nullptr);
		else if (kind == 2)
			rc = http_post_multipart_ex(&control, url, &part, 1,
				nullptr, 0, 3, &response);
		else
			rc = http_session_post(&control, &session, url, "{}", 2,
				"application/json", nullptr, 0, 3);
		auto elapsed = std::chrono::steady_clock::now() - start;
		canceller.join();
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		condition.notify_all();
		server.join();
		http_response_free(&response);
		http_session_cleanup(&session);
		EXPECT_EQ(rc, -ECANCELED);
		EXPECT_LT(elapsed, std::chrono::milliseconds(500));
	}
}

TEST(CancelTokenTest, ResetCancelAndQuery) {
	struct morph_cancel_token token;
	morph_cancel_token_reset(&token);
	EXPECT_EQ(morph_cancel_token_is_cancelled(&token), 0);
	morph_cancel_token_cancel(&token);
	EXPECT_EQ(morph_cancel_token_is_cancelled(&token), 1);
	morph_cancel_token_reset(&token);
	EXPECT_EQ(morph_cancel_token_is_cancelled(&token), 0);
}

TEST(CancelTokenTest, RetryWaitStopsWhenCancelled) {
	struct morph_cancel_token token;
	morph_cancel_token_reset(&token);
	struct http_control control{};
	control.cancel_token = &token;
	std::thread canceller([&token]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		morph_cancel_token_cancel(&token);
	});
	auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(http_wait_cancelable(&control, 5000), -ECANCELED);
	auto elapsed = std::chrono::steady_clock::now() - started;
	EXPECT_LT(elapsed, std::chrono::seconds(1));
	canceller.join();
}

/* ----- http_session tests ----- */

class HttpSessionTest : public ::testing::Test {
protected:
	struct http_session session;

	void SetUp() override {
		http_init(NULL);
		memset(&session, 0, sizeof(session));
	}

	void TearDown() override {
		http_session_cleanup(&session);
		http_cleanup();
	}
};

TEST_F(HttpSessionTest, InitCleanup) {
	EXPECT_EQ(http_session_init(NULL, &session), 0);
	EXPECT_NE(session.curl, nullptr);
	EXPECT_EQ(session.initialized, 1);
	http_session_cleanup(&session);
	EXPECT_EQ(session.curl, nullptr);
	EXPECT_EQ(session.initialized, 0);
}

TEST_F(HttpSessionTest, InitNull) {
	EXPECT_NE(http_session_init(NULL, nullptr), 0);
}

TEST_F(HttpSessionTest, CleanupNull) {
	EXPECT_NO_FATAL_FAILURE(http_session_cleanup(nullptr));
}

TEST_F(HttpSessionTest, ResetClearsBuffers) {
	ASSERT_EQ(http_session_init(NULL, &session), 0);
	morph_buf_puts(&session.resp_body, "hello");
	morph_buf_puts(&session.resp_headers, "X-Test: hi\r\n");
	session.status_code = 200;
	http_session_reset(&session);
	EXPECT_EQ(session.resp_body.len, 0);
	EXPECT_EQ(session.resp_headers.len, 0);
	EXPECT_EQ(session.status_code, 0);
}

TEST_F(HttpSessionTest, PostInvalidUrl) {
	ASSERT_EQ(http_session_init(NULL, &session), 0);
	int rc = http_session_post(NULL, &session, "http://127.0.0.1:1/nonexistent", "{}", 2,
				   "application/json", nullptr, 0, 5);
	EXPECT_NE(rc, 0);
}

TEST_F(HttpSessionTest, PostNullUrl) {
	ASSERT_EQ(http_session_init(NULL, &session), 0);
	EXPECT_NE(http_session_post(NULL, &session, nullptr, "{}", 2, "application/json", nullptr,
				    0, 30),
		  0);
}

TEST_F(HttpSessionTest, SessionNullArgs) {
	EXPECT_NE(http_session_post(NULL, nullptr, "http://localhost/", "{}", 2, "application/json",
				    nullptr, 0, 30),
		  0);
}

TEST_F(HttpSessionTest, BodyAndStatus) {
	ASSERT_EQ(http_session_init(NULL, &session), 0);
	const char *body = http_session_body(&session, nullptr);
	ASSERT_NE(body, nullptr);
	EXPECT_EQ(body[0], '\0');
	EXPECT_EQ(http_session_status(&session), 0);
	EXPECT_EQ(http_session_status(nullptr), 0);
}

/* ----- http_response with morph_buf_t ----- */

TEST_F(HttpTest, ResponseFreeWithMorphBuf) {
	struct http_response resp;
	memset(&resp, 0, sizeof(resp));
	EXPECT_EQ(resp.body.data, nullptr);
	EXPECT_EQ(resp.headers.data, nullptr);
	EXPECT_NO_FATAL_FAILURE(http_response_free(&resp));
}
