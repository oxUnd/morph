#include <gtest/gtest.h>
#include "fastcgi/security.h"
#include <cstring>

TEST(FastcgiSecurityTest, VerifiesKnownPbkdf2Sha256Vector) {
	const char *encoded =
		"pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
		"eb6c81535592203c092b158f8d3909672362a6f5dbd00d9828044cbaa8b252e9";

	EXPECT_EQ(fcgi_password_verify("password", encoded), 1);
	EXPECT_EQ(fcgi_password_verify("passw0rd", encoded), 0);
}

TEST(FastcgiSecurityTest, HashRoundTripsThroughVerify) {
	char encoded[256];

	ASSERT_EQ(fcgi_password_hash("correct horse battery staple",
				     encoded, sizeof(encoded)), 0);
	EXPECT_NE(strstr(encoded, "pbkdf2-sha256$120000$"), nullptr);
	EXPECT_EQ(fcgi_password_verify("correct horse battery staple",
				       encoded), 1);
	EXPECT_EQ(fcgi_password_verify("wrong", encoded), 0);
}
