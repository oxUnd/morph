#include <gtest/gtest.h>

extern "C" {
#include "sapi/fastcgi/auth.h"
#include "nb/err.h"
}

#include <map>
#include <memory>
#include <string>
#include <vector>

static int cached_authorization(struct fcgi_auth *auth, const std::string &token,
			       const char *expected_user)
{
	std::string bearer = "Bearer " + token;
	request_t request{};
	request.auth = auth;
	request.auth_hdr = bearer.c_str();
	int accepted = auth_check(&request);
	if (accepted && expected_user)
		EXPECT_STREQ(request.user_id, expected_user);
	return accepted;
}

TEST(FastcgiAuthTest, NativeCachePreservesUpdatesRevocationAndExpiration)
{
	std::unique_ptr<struct fcgi_auth, decltype(&auth_destroy)> auth(auth_create(nullptr),
		auth_destroy);
	ASSERT_NE(auth, nullptr);
	auth_token_created(auth.get(), "audit-cache-token", "first", "alice", "user", INT64_MAX);
	EXPECT_EQ(cached_authorization(auth.get(), "audit-cache-token", "first"), 1);
	auth_token_created(auth.get(), "audit-cache-token", "second", "bob", "admin", INT64_MAX);
	EXPECT_EQ(cached_authorization(auth.get(), "audit-cache-token", "second"), 1);
	auth_token_revoked(auth.get(), "audit-cache-token");
	EXPECT_EQ(cached_authorization(auth.get(), "audit-cache-token", nullptr), 0);
	auth_token_created(auth.get(), "audit-cache-token", "expired", "bob", "user", 1);
	EXPECT_EQ(cached_authorization(auth.get(), "audit-cache-token", nullptr), 0);
}

TEST(FastcgiAuthTest, NativeCachePreservesHistoricalCapacityAndBucketEvictionOrder)
{
	std::unique_ptr<struct fcgi_auth, decltype(&auth_destroy)> auth(auth_create(nullptr),
		auth_destroy);
	ASSERT_NE(auth, nullptr);
	std::map<unsigned, std::vector<std::string>> original_buckets;
	for (unsigned index = 0; index < 4096; index++) {
		std::string token = "audit-cache-capacity-" + std::to_string(index);
		unsigned hash = 5381;
		for (unsigned char byte : token)
			hash = hash * 33u + byte;
		original_buckets[hash % 256].push_back(token);
		auth_token_created(auth.get(), token.c_str(), "owner", "alice", "user", INT64_MAX);
	}
	std::string victim = original_buckets.begin()->second.back();
	auth_token_created(auth.get(), "audit-cache-new", "new", "bob", "user", INT64_MAX);
	EXPECT_EQ(cached_authorization(auth.get(), victim, nullptr), 0);
	EXPECT_EQ(cached_authorization(auth.get(), "audit-cache-new", "new"), 1);
	for (const auto &bucket : original_buckets) {
		for (const auto &token : bucket.second) {
			if (token != victim)
				ASSERT_EQ(cached_authorization(auth.get(), token, "owner"), 1);
		}
	}
}
