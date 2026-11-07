#include <gtest/gtest.h>
#include "util/data.h"
#include "util/file.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/stat.h>
#include <filesystem>
#include <string>

class FileTest : public ::testing::Test {
protected:
  std::string directory;
  std::string directory_path;
  std::string file_path;
  const char *test_dir = nullptr;
  const char *test_file = nullptr;
  void SetUp() override
  {
	  char pattern[] = "/tmp/morph-file-tests-XXXXXX";
	  char *created = mkdtemp(pattern);

	  ASSERT_NE(created, nullptr);
	  directory = created;
	  directory_path = directory + "/directory";
	  file_path = directory + "/file.txt";
	  test_dir = directory_path.c_str();
	  test_file = file_path.c_str();
  }
  void TearDown() override
  {
	  std::error_code ignored;

	  std::filesystem::remove_all(directory, ignored);
  }
};

TEST_F(FileTest, WriteAndRead) {
	const char *data = "hello world";
	int rc = file_write_all(test_file, data, strlen(data));
	EXPECT_EQ(rc, 0);
	size_t len = 0;
	char *buf = file_read_all_copy(test_file, &len);
	ASSERT_NE(buf, nullptr);
	EXPECT_EQ(len, strlen(data));
	EXPECT_STREQ(buf, data);
	free(buf);
}

TEST_F(FileTest, ReadNonexistent) {
	char *buf = file_read_all_copy("/tmp/nonexistent_file_12345", nullptr);
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
	char *expanded = file_expand_path_copy("~/test");
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(expanded[0], '~');
	EXPECT_NE(expanded[0], '\0');
	free(expanded);
}

TEST_F(FileTest, ExpandPathNotHome) {
	char *expanded = file_expand_path_copy("/absolute/path");
	ASSERT_NE(expanded, nullptr);
	EXPECT_STREQ(expanded, "/absolute/path");
	free(expanded);
}

TEST_F(FileTest, PathJoinBasic) {
	char path[128];
	int rc = file_path_join(path, sizeof(path), "/tmp/morph", "file.txt");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
}

TEST_F(FileTest, PathJoinAvoidsDoubleSlash) {
	char path[128];
	int rc = file_path_join(path, sizeof(path), "/tmp/morph/", "/file.txt");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
}

TEST_F(FileTest, PathJoinTooLong) {
	char path[8];
	int rc = file_path_join(path, sizeof(path), "/tmp/morph", "file.txt");
	EXPECT_EQ(rc, -ENAMETOOLONG);
}

TEST_F(FileTest, PathJoinAlloc) {
	char *path = file_path_join_alloc("/tmp/morph", "file.txt");
	ASSERT_NE(path, nullptr);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
	free(path);
}

TEST_F(FileTest, PathFullKeepsAbsolute) {
	char path[128];
	int rc = file_path_full(path, sizeof(path), "/tmp/base", "/var/file.txt");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/var/file.txt");
}

TEST_F(FileTest, PathFullJoinsRelative) {
	char path[128];
	int rc = file_path_full(path, sizeof(path), "/tmp/base", "file.txt");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/base/file.txt");
}

TEST_F(FileTest, PathAppendSuffix) {
	char path[128] = "/tmp/morph/session";
	int rc = file_path_append(path, sizeof(path), ".json");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/session.json");
}

TEST_F(FileTest, PathJoinAppendSuffix) {
	char path[128];
	int rc = file_path_join_append(path, sizeof(path),
				       "/tmp/morph", "session", ".json");
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/session.json");
}

TEST_F(FileTest, PathAppendAlloc) {
	char *path = file_path_append_alloc("/tmp/morph/session", ".json");
	ASSERT_NE(path, nullptr);
	EXPECT_STREQ(path, "/tmp/morph/session.json");
	free(path);
}

TEST_F(FileTest, DataFindUsesExecutableLayout) {
	char resolved[PATH_MAX];

	EXPECT_EQ(morph_data_find(resolved, sizeof(resolved), "skills"), 0);
	EXPECT_NE(strstr(resolved, "/skills"), nullptr);
	EXPECT_EQ(morph_data_find(resolved, sizeof(resolved),
				  "tiktoken/cl100k_base.tiktoken"), 0);
	EXPECT_EQ(morph_data_find(resolved, sizeof(resolved),
				  "fonts/STIXTwoMath-Regular.ttf"), 0);
	EXPECT_EQ(morph_data_find(resolved, sizeof(resolved),
				  "../bin/morph"), -EINVAL);
}

TEST_F(FileTest, ExecutableFindLocatesJavaScriptRunner) {
	char resolved[PATH_MAX];

	EXPECT_EQ(morph_executable_find(resolved, sizeof(resolved),
					"morph-js-runner"), 0);
	EXPECT_EQ(access(resolved, X_OK), 0);
}

TEST_F(FileTest, WriteEmpty) {
	int rc = file_write_all(test_file, "", 0);
	EXPECT_EQ(rc, 0);
}

TEST_F(FileTest, ListFiles) {
	const char *d = "/tmp/ma_test_list";
	file_ensure_dir(d);
	file_write_all("/tmp/ma_test_list/z_last.txt", "z", 1);
	file_write_all("/tmp/ma_test_list/a_first.txt", "a", 1);
	file_write_all("/tmp/ma_test_list/m_mid.txt", "m", 1);
	char **names = nullptr;
	int n = 0;
	int rc = file_list_files_copy(d, &names, &n);
	EXPECT_EQ(rc, 0);
	ASSERT_EQ(n, 3);
	EXPECT_STREQ(names[0], "a_first.txt");
	EXPECT_STREQ(names[1], "m_mid.txt");
	EXPECT_STREQ(names[2], "z_last.txt");
	file_free_list(names, n);
	std::remove("/tmp/ma_test_list/z_last.txt");
	std::remove("/tmp/ma_test_list/a_first.txt");
	std::remove("/tmp/ma_test_list/m_mid.txt");
	std::remove(d);
}

TEST_F(FileTest, ListFilesEmptyDir) {
	const char *d = "/tmp/ma_test_empty";
	file_ensure_dir(d);
	char **names = nullptr;
	int n = 999;
	int rc = file_list_files_copy(d, &names, &n);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(n, 0);
	file_free_list(names, n);
	std::remove(d);
}

TEST_F(FileTest, ListFilesNoDir) {
	char **names = nullptr;
	int n = 0;
	int rc = file_list_files_copy("/tmp/nonexistent_xyz_12345", &names, &n);
	EXPECT_NE(rc, 0);
}

TEST_F(FileTest, ListFilesSkipsDirs) {
	const char *d = "/tmp/ma_test_skip";
	file_ensure_dir(d);
	file_write_all("/tmp/ma_test_skip/file.txt", "f", 1);
	file_ensure_dir("/tmp/ma_test_skip/subdir");
	char **names = nullptr;
	int n = 0;
	int rc = file_list_files_copy(d, &names, &n);
	EXPECT_EQ(rc, 0);
	ASSERT_EQ(n, 1);
	EXPECT_STREQ(names[0], "file.txt");
	file_free_list(names, n);
	std::remove("/tmp/ma_test_skip/file.txt");
	std::remove("/tmp/ma_test_skip/subdir");
	std::remove(d);
}

TEST_F(FileTest, ResolvePathAbsolute) {
	char *resolved = file_resolve_path_copy("/tmp");
	ASSERT_NE(resolved, nullptr);
	EXPECT_NE(resolved[0], '\0');
	free(resolved);
}

TEST_F(FileTest, ResolvePathHome) {
	char *resolved = file_resolve_path_copy("~/test_resolve");
	ASSERT_NE(resolved, nullptr);
	EXPECT_NE(resolved[0], '~');
	EXPECT_NE(resolved[0], '\0');
	free(resolved);
}

TEST_F(FileTest, PathIsWithinBasic) {
	char *resolved = file_resolve_path_copy("/tmp");
	ASSERT_NE(resolved, nullptr);
	char path_buf[512];
	snprintf(path_buf, sizeof(path_buf), "%s/ma_test_dir/file.txt", resolved);
	free(resolved);
	file_ensure_dir("/tmp/ma_test_dir");
	EXPECT_EQ(path_is_within(path_buf, "/tmp/ma_test_dir"), 1);
	EXPECT_EQ(path_is_within("/var/other/file.txt", "/tmp/ma_test_dir"), 0);
	std::remove("/tmp/ma_test_dir");
}

TEST_F(FileTest, PathIsWithinExactMatch) {
	EXPECT_EQ(path_is_within("/tmp/ma_test_dir", "/tmp/ma_test_dir"), 1);
}

TEST_F(FileTest, PathIsWithinPrefixAttack) {
	EXPECT_EQ(path_is_within("/tmp/ma_test_dir_other/file.txt", "/tmp/ma_test_dir"), 0);
}

TEST_F(FileTest, PathIsWithinNull) {
	EXPECT_EQ(path_is_within(NULL, "/tmp"), 0);
	EXPECT_EQ(path_is_within("/tmp", NULL), 0);
}

TEST_F(FileTest, ResolvePathNull) {
	char *resolved = file_resolve_path_copy(NULL);
	EXPECT_EQ(resolved, nullptr);
}

TEST_F(FileTest, ListsRejectOccupiedOwnersAndPreserveCountsOnFailure)
{
	ASSERT_EQ(file_write_all(test_file, "data", 4), 0);
	for (auto list : {file_list_files_copy, file_list_dirs_copy}) {
		char **names = nullptr;
		int count = 999;
		EXPECT_EQ(list(test_file, &names, &count), -ENOTDIR);
		EXPECT_EQ(names, nullptr);
		EXPECT_EQ(count, 999);
		const std::string missing = directory + "/missing";
		EXPECT_EQ(list(missing.c_str(), &names, &count), -ENOENT);
		EXPECT_EQ(names, nullptr);
		EXPECT_EQ(count, 999);
		ASSERT_EQ(list(directory.c_str(), &names, &count), 0);
		ASSERT_NE(names, nullptr);
		char **owner = names;
		const int owned_count = count;
		count = 999;
		EXPECT_EQ(list(directory.c_str(), &names, &count), -EALREADY);
		EXPECT_EQ(names, owner);
		EXPECT_EQ(count, 999);
		file_free_list(names, owned_count);
	}
}

TEST_F(FileTest, CopiedNamesSurviveDirectoryEntryRemoval)
{
	const std::string file = directory + "/copied.txt";
	const std::string child = directory + "/copied-dir";
	ASSERT_EQ(file_write_all(file.c_str(), "x", 1), 0);
	ASSERT_EQ(file_ensure_dir(child.c_str()), 0);
	char **files = nullptr;
	char **dirs = nullptr;
	int file_count = 999;
	int dir_count = 999;
	ASSERT_EQ(file_list_files_copy(directory.c_str(), &files, &file_count), 0);
	ASSERT_EQ(file_list_dirs_copy(directory.c_str(), &dirs, &dir_count), 0);
	ASSERT_EQ(file_count, 1);
	ASSERT_EQ(dir_count, 1);
	ASSERT_EQ(std::remove(file.c_str()), 0);
	ASSERT_EQ(std::remove(child.c_str()), 0);
	EXPECT_STREQ(files[0], "copied.txt");
	EXPECT_STREQ(dirs[0], "copied-dir");
	file_free_list(files, file_count);
	file_free_list(dirs, dir_count);
}
