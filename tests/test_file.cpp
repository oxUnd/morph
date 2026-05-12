#include <gtest/gtest.h>
#include "util/file.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

class FileTest : public ::testing::Test {
protected:
	const char *test_dir = "/tmp/ma_test_dir";
	const char *test_file = "/tmp/ma_test_file.txt";
	void SetUp() override {
		std::remove(test_file);
	}
	void TearDown() override {
		std::remove(test_file);
	}
};

TEST_F(FileTest, WriteAndRead) {
	const char *data = "hello world";
	int rc = file_write_all(test_file, data, strlen(data));
	EXPECT_EQ(rc, 0);
	size_t len = 0;
	char *buf = file_read_all(test_file, &len);
	ASSERT_NE(buf, nullptr);
	EXPECT_EQ(len, strlen(data));
	EXPECT_STREQ(buf, data);
	free(buf);
}

TEST_F(FileTest, ReadNonexistent) {
	char *buf = file_read_all("/tmp/nonexistent_file_12345", nullptr);
	EXPECT_EQ(buf, nullptr);
}

TEST_F(FileTest, EnsureDir) {
	int rc = file_ensure_dir(test_dir);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(file_exists(test_dir), 1);
	std::remove(test_dir);
}

TEST_F(FileTest, EnsureDirNested) {
	const char *nested = "/tmp/ma_test_nested/a/b";
	int rc = file_ensure_dir(nested);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(file_exists(nested), 1);
	std::remove("/tmp/ma_test_nested/a/b");
	std::remove("/tmp/ma_test_nested/a");
	std::remove("/tmp/ma_test_nested");
}

TEST_F(FileTest, Exists) {
	EXPECT_EQ(file_exists("/tmp"), 1);
	EXPECT_EQ(file_exists("/tmp/nonexistent_path_xyz"), 0);
}

TEST_F(FileTest, ExpandPath) {
	char *expanded = file_expand_path("~/test");
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(expanded[0], '~');
	EXPECT_NE(expanded[0], '\0');
	free(expanded);
}

TEST_F(FileTest, ExpandPathNotHome) {
	char *expanded = file_expand_path("/absolute/path");
	ASSERT_NE(expanded, nullptr);
	EXPECT_STREQ(expanded, "/absolute/path");
	free(expanded);
}

TEST_F(FileTest, WriteEmpty) {
	int rc = file_write_all(test_file, "", 0);
	EXPECT_EQ(rc, 0);
}