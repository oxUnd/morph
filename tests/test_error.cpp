#include <gtest/gtest.h>

#include "util/error.h"

#include <errno.h>
#include <string.h>

static int return_errno_with_zero(void)
{
	errno = 0;
	MORPH_RETURN_ERRNO();
}

TEST(ErrorTest, ClassifiesErrnoAndDomainRanges)
{
	EXPECT_TRUE(morph_err_is_errno(-EINVAL));
	EXPECT_TRUE(morph_err_is_errno(-ENOMEM));
	EXPECT_FALSE(morph_err_is_errno(0));
	EXPECT_FALSE(morph_err_is_errno(MORPH_ERR_PARSE));

	EXPECT_TRUE(morph_err_is_domain(MORPH_ERR_PARSE));
	EXPECT_TRUE(morph_err_is_domain(MORPH_ERR_PROTOCOL));
	EXPECT_FALSE(morph_err_is_domain(-EINVAL));
	EXPECT_FALSE(morph_err_is_domain(0));
}

TEST(ErrorTest, NamesKnownErrors)
{
	EXPECT_STREQ(morph_errname(0), "OK");
	EXPECT_STREQ(morph_errname(-EINVAL), "EINVAL");
	EXPECT_STREQ(morph_errname(-ENOMEM), "ENOMEM");
	EXPECT_STREQ(morph_errname(MORPH_ERR_PARSE), "MORPH_ERR_PARSE");
	EXPECT_STREQ(morph_errname(MORPH_ERR_LLM), "MORPH_ERR_LLM");
	EXPECT_STREQ(morph_errname(-9999), "MORPH_ERR_UNKNOWN");
}

TEST(ErrorTest, StringifiesErrnoAndDomainErrors)
{
	EXPECT_STREQ(morph_strerror(-EINVAL), strerror(EINVAL));
	EXPECT_STREQ(morph_strerror(MORPH_ERR_PARSE), "parse error");
	EXPECT_STREQ(morph_strerror(MORPH_ERR_PROTOCOL), "protocol error");
	EXPECT_STREQ(morph_strerror(123), "unknown error");
}

TEST(ErrorTest, ReturnErrnoNeverReturnsSuccess)
{
	EXPECT_EQ(return_errno_with_zero(), -EIO);
}
