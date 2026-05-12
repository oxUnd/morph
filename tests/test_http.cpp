#include <gtest/gtest.h>
#include "http/client.h"

class HttpTest : public ::testing::Test {
protected:
	void SetUp() override { http_init(); }
	void TearDown() override { http_cleanup(); }
};

TEST_F(HttpTest, InitCleanup) {
	EXPECT_NO_FATAL_FAILURE(http_cleanup());
	EXPECT_NO_FATAL_FAILURE(http_init());
}

TEST_F(HttpTest, GetInvalid) {
	struct http_response resp;
	int rc = http_get("http://127.0.0.1:1/nonexistent", &resp);
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
	EXPECT_NE(http_get(nullptr, &resp), 0);
}

TEST_F(HttpTest, PostNullUrl) {
	struct http_response resp;
	EXPECT_NE(http_post(nullptr, "", 0, "", &resp), 0);
}

TEST_F(HttpTest, DoubleInit) {
	EXPECT_NO_FATAL_FAILURE(http_init());
	EXPECT_NO_FATAL_FAILURE(http_init());
}