#include <gtest/gtest.h>

#include "errors.h"

#include <errno.h>
#include <string.h>

static int return_errno_with_zero(void)
{
	errno = 0;
	return nb_status_from_errno(errno, NULL);
}

TEST(ErrorTest, ClassifiesErrnoAndDomainRanges)
{
	EXPECT_TRUE(((NB_ERR_INVAL) < NB_OK && !NB_ERR_IS_USER(NB_ERR_INVAL)));
	EXPECT_TRUE(((NB_ERR_NOMEM) < NB_OK && !NB_ERR_IS_USER(NB_ERR_NOMEM)));
	EXPECT_FALSE(((0) < NB_OK && !NB_ERR_IS_USER(0)));
	EXPECT_TRUE(((NB_ERR_PARSE) < NB_OK && !NB_ERR_IS_USER(NB_ERR_PARSE)));

	EXPECT_TRUE(NB_ERR_IS_USER(MORPH_ERR_DB));
	EXPECT_TRUE(NB_ERR_IS_USER(MORPH_ERR_LLM));
	EXPECT_FALSE(NB_ERR_IS_USER(NB_ERR_INVAL));
	EXPECT_FALSE(NB_ERR_IS_USER(0));
}

TEST(ErrorTest, NamesKnownErrors)
{
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, 0), "NB_OK");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_INVAL),
		     "NB_ERR_INVAL");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_NOMEM),
		     "NB_ERR_NOMEM");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_PARSE),
		     "NB_ERR_PARSE");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, MORPH_ERR_LLM),
		     "MORPH_ERR_LLM");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT,
				 MORPH_ERR_REACT_MAX_ITERATIONS),
		     "MORPH_ERR_REACT_MAX_ITERATIONS");
	EXPECT_STREQ(nb_err_name(morph_error_descriptions(), MORPH_ERROR_COUNT, -9999), "UNKNOWN");
}

TEST(ErrorTest, StringifiesErrnoAndDomainErrors)
{
	EXPECT_STREQ(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_INVAL),
		     "invalid argument");
	EXPECT_STREQ(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_PARSE),
		     "parse error");
	EXPECT_STREQ(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_PROTOCOL),
		     "protocol error");
	EXPECT_STREQ(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
				MORPH_ERR_REACT_MAX_ITERATIONS),
		     "maximum ReAct iterations reached");
	EXPECT_STREQ(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, 123),
		     "unknown error");
}

TEST(ErrorTest, ReturnErrnoNeverReturnsSuccess)
{
	EXPECT_EQ(return_errno_with_zero(), NB_ERR_IO);
}

TEST(ErrorTest, NativeStatusUserRangeRemainsValidInCpp)
{
	nb_status code = NB_ERR_USER(100000);
	EXPECT_EQ(sizeof(code), sizeof(int));
	EXPECT_EQ(static_cast<int>(code), -101000);
	EXPECT_STREQ(nb_status_str(code), "user error");
	nb_status lowest = static_cast<nb_status>(INT_MIN);
	EXPECT_TRUE(NB_ERR_IS_USER(lowest));
	EXPECT_STREQ(nb_status_str(lowest), "user error");
}
