#include <gtest/gtest.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include "runtime/resources.h"
#include <nb/file.h>
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
	nb_buf file_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *file_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	const char *data = "hello world";
	int rc = nb_file_write_all(test_file, data, strlen(data), NULL);
	EXPECT_EQ(rc, 0);
	size_t len = 0;
	char *buf = (nb_file_read_all(test_file, &file_contents_buffer, 0, NULL) == NB_OK &&
				     (file_contents_copy = (char *)malloc(
					      nb_buf_len(&file_contents_buffer) + 1)) != NULL
			     ? (memcpy(file_contents_copy, nb_buf_cstr(&file_contents_buffer),
				       nb_buf_len(&file_contents_buffer) + 1),
				*(&len) = nb_buf_len(&file_contents_buffer),
				nb_charp_steal(&file_contents_copy))
			     : NULL);
	ASSERT_NE(buf, nullptr);
	EXPECT_EQ(len, strlen(data));
	EXPECT_STREQ(buf, data);
	free(buf);
}

TEST_F(FileTest, ReadNonexistent) {
	nb_buf missing_contents_buffer NB_CLEANUP(nb_buf_destroy) = {};
	char *missing_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *buf = (nb_file_read_all("/tmp/nonexistent_file_12345", &missing_contents_buffer, 0,
				      NULL) == NB_OK &&
				     (missing_contents_copy = (char *)malloc(
					      nb_buf_len(&missing_contents_buffer) + 1)) != NULL
			     ? (memcpy(missing_contents_copy, nb_buf_cstr(&missing_contents_buffer),
				       nb_buf_len(&missing_contents_buffer) + 1),
				nb_charp_steal(&missing_contents_copy))
			     : NULL);
	EXPECT_EQ(buf, nullptr);
}

TEST_F(FileTest, EnsureDir) {
	int rc = nb_file_mkdir(test_dir, true, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(nb_file_exists(test_dir), 1);
	std::remove(test_dir);
}

TEST_F(FileTest, EnsureDirNested) {
	const char *nested = "/tmp/ma_test_nested/a/b";
	int rc = nb_file_mkdir(nested, true, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(nb_file_exists(nested), 1);
	std::remove("/tmp/ma_test_nested/a/b");
	std::remove("/tmp/ma_test_nested/a");
	std::remove("/tmp/ma_test_nested");
}

TEST_F(FileTest, Exists) {
	EXPECT_EQ(nb_file_exists("/tmp"), 1);
	EXPECT_EQ(nb_file_exists("/tmp/nonexistent_path_xyz"), 0);
}

TEST_F(FileTest, ExpandPath) {
	nb_buf home_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *expanded = (nb_path_expand(&home_path_buffer, "~/test", NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&home_path_buffer))
				  : NULL);
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(expanded[0], '~');
	EXPECT_NE(expanded[0], '\0');
	free(expanded);
}

TEST_F(FileTest, ExpandPathNotHome) {
	nb_buf absolute_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *expanded = (nb_path_expand(&absolute_path_buffer, "/absolute/path", NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&absolute_path_buffer))
				  : NULL);
	ASSERT_NE(expanded, nullptr);
	EXPECT_STREQ(expanded, "/absolute/path");
	free(expanded);
}

TEST_F(FileTest, PathJoinBasic) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128];
	int rc = ((joined_path_status =
			   nb_path_concat(&joined_path_buffer, "/tmp/morph", "file.txt")) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
}

TEST_F(FileTest, PathJoinAvoidsDoubleSlash) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128];
	int rc = ((joined_path_status =
			   nb_path_concat(&joined_path_buffer, "/tmp/morph/", "/file.txt")) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
}

TEST_F(FileTest, PathJoinTooLong) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[8];
	int rc = ((joined_path_status =
			   nb_path_concat(&joined_path_buffer, "/tmp/morph", "file.txt")) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, NB_ERR_NAME_TOO_LONG);
}

TEST_F(FileTest, PathJoinAlloc) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *path = (nb_path_concat(&joined_path_buffer, "/tmp/morph", "file.txt") == NB_OK
			      ? strdup(nb_buf_cstr(&joined_path_buffer))
			      : NULL);
	ASSERT_NE(path, nullptr);
	EXPECT_STREQ(path, "/tmp/morph/file.txt");
	free(path);
}

TEST_F(FileTest, PathFullKeepsAbsolute) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128];
	int rc = ((joined_path_status =
			   nb_path_join(&joined_path_buffer, "/tmp/base", "/var/file.txt")) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/var/file.txt");
}

TEST_F(FileTest, PathFullJoinsRelative) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128];
	int rc = ((joined_path_status =
			   nb_path_join(&joined_path_buffer, "/tmp/base", "file.txt")) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/base/file.txt");
}

TEST_F(FileTest, PathAppendSuffix) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128] = "/tmp/morph/session";
	int rc = ((joined_path_status = (nb_buf_clear(&joined_path_buffer),
					 nb_buf_printf(&joined_path_buffer, "%s%s", path, ".json")
						 ? NB_OK
						 : NB_ERR_NOMEM)) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/session.json");
}

TEST_F(FileTest, PathJoinAppendSuffix) {
	nb_buf joined_path_buffer NB_CLEANUP(nb_buf_destroy) = {};
	int joined_path_status = NB_OK;

	char path[128];
	int rc = ((joined_path_status =
			   (nb_path_concat(&joined_path_buffer, "/tmp/morph", "session") != NB_OK ||
					    !nb_buf_append_cstr(&joined_path_buffer, ".json")
				    ? NB_ERR_NOMEM
				    : NB_OK)) != NB_OK
			  ? joined_path_status
		  : (joined_path_status = nb_str_copy_to(nb_buf_str(&joined_path_buffer), path,
							 sizeof(path))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : joined_path_status);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(path, "/tmp/morph/session.json");
}

TEST_F(FileTest, PathAppendAlloc) {
	nb_buf suffixed_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *path = (nb_buf_clear(&suffixed_path_buffer),
		      nb_buf_printf(&suffixed_path_buffer, "%s%s", "/tmp/morph/session", ".json")
			      ? strdup(nb_buf_cstr(&suffixed_path_buffer))
			      : NULL);
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
	EXPECT_EQ(morph_data_find(resolved, sizeof(resolved), "../bin/morph"), NB_ERR_INVAL);
}

TEST_F(FileTest, ExecutableFindLocatesJavaScriptRunner) {
	char resolved[PATH_MAX];

	EXPECT_EQ(morph_executable_find(resolved, sizeof(resolved),
					"morph-js-runner"), 0);
	EXPECT_EQ(access(resolved, X_OK), 0);
}

TEST_F(FileTest, WriteEmpty) {
	int rc = nb_file_write_all(test_file, "", 0, NULL);
	EXPECT_EQ(rc, 0);
}

TEST_F(FileTest, ListFiles) {
	int file_scan_status = NB_OK;

	const char *d = "/tmp/ma_test_list";
	nb_file_mkdir(d, true, NULL);
	nb_file_write_all("/tmp/ma_test_list/z_last.txt", "z", 1, NULL);
	nb_file_write_all("/tmp/ma_test_list/a_first.txt", "a", 1, NULL);
	nb_file_write_all("/tmp/ma_test_list/m_mid.txt", "m", 1, NULL);
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {};
	int n = 0;
	int rc = ((file_scan_status = nb_dir_scan(
			   d, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
			   &names, NULL)) != NB_OK
			  ? file_scan_status
		  : names.names.len > INT_MAX ? NB_ERR_RANGE
					      : (n = (int)names.names.len, NB_OK));
	EXPECT_EQ(rc, 0);
	ASSERT_EQ(n, 3);
	EXPECT_STREQ(nb_dir_listing_name(&names, (size_t)(0)), "a_first.txt");
	EXPECT_STREQ(nb_dir_listing_name(&names, (size_t)(1)), "m_mid.txt");
	EXPECT_STREQ(nb_dir_listing_name(&names, (size_t)(2)), "z_last.txt");
	nb_dir_listing_destroy(&names);
	std::remove("/tmp/ma_test_list/z_last.txt");
	std::remove("/tmp/ma_test_list/a_first.txt");
	std::remove("/tmp/ma_test_list/m_mid.txt");
	std::remove(d);
}

TEST_F(FileTest, ListFilesEmptyDir) {
	int file_scan_status = NB_OK;

	const char *d = "/tmp/ma_test_empty";
	nb_file_mkdir(d, true, NULL);
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {};
	int n = 999;
	int rc = ((file_scan_status = nb_dir_scan(
			   d, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
			   &names, NULL)) != NB_OK
			  ? file_scan_status
		  : names.names.len > INT_MAX ? NB_ERR_RANGE
					      : (n = (int)names.names.len, NB_OK));
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(n, 0);
	nb_dir_listing_destroy(&names);
	std::remove(d);
}

TEST_F(FileTest, ListFilesNoDir) {
	int file_scan_status = NB_OK;

	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {};
	int n = 0;
	int rc = ((file_scan_status = nb_dir_scan("/tmp/nonexistent_xyz_12345",
						  NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
							  NB_DIR_SCAN_FOLLOW_LINKS,
						  &names, NULL)) != NB_OK
			  ? file_scan_status
		  : names.names.len > INT_MAX ? NB_ERR_RANGE
					      : (n = (int)names.names.len, NB_OK));
	EXPECT_NE(rc, 0);
}

TEST_F(FileTest, ListFilesSkipsDirs) {
	int file_scan_status = NB_OK;

	const char *d = "/tmp/ma_test_skip";
	nb_file_mkdir(d, true, NULL);
	nb_file_write_all("/tmp/ma_test_skip/file.txt", "f", 1, NULL);
	nb_file_mkdir("/tmp/ma_test_skip/subdir", true, NULL);
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {};
	int n = 0;
	int rc = ((file_scan_status = nb_dir_scan(
			   d, NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT | NB_DIR_SCAN_FOLLOW_LINKS,
			   &names, NULL)) != NB_OK
			  ? file_scan_status
		  : names.names.len > INT_MAX ? NB_ERR_RANGE
					      : (n = (int)names.names.len, NB_OK));
	EXPECT_EQ(rc, 0);
	ASSERT_EQ(n, 1);
	EXPECT_STREQ(nb_dir_listing_name(&names, (size_t)(0)), "file.txt");
	nb_dir_listing_destroy(&names);
	std::remove("/tmp/ma_test_skip/file.txt");
	std::remove("/tmp/ma_test_skip/subdir");
	std::remove(d);
}

TEST_F(FileTest, ResolvePathAbsolute) {
	nb_buf canonical_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *resolved =
		(nb_path_expand(&canonical_path_buffer, "/tmp", NULL) == NB_OK &&
				 nb_path_canonical_missing(&canonical_path_buffer,
							   nb_buf_cstr(&canonical_path_buffer),
							   NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&canonical_path_buffer))
			 : NULL);
	ASSERT_NE(resolved, nullptr);
	EXPECT_NE(resolved[0], '\0');
	free(resolved);
}

TEST_F(FileTest, ResolvePathHome) {
	nb_buf canonical_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *resolved =
		(nb_path_expand(&canonical_path_buffer, "~/test_resolve", NULL) == NB_OK &&
				 nb_path_canonical_missing(&canonical_path_buffer,
							   nb_buf_cstr(&canonical_path_buffer),
							   NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&canonical_path_buffer))
			 : NULL);
	ASSERT_NE(resolved, nullptr);
	EXPECT_NE(resolved[0], '~');
	EXPECT_NE(resolved[0], '\0');
	free(resolved);
}

TEST_F(FileTest, PathIsWithinBasic) {
	nb_buf canonical_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *resolved =
		(nb_path_expand(&canonical_path_buffer, "/tmp", NULL) == NB_OK &&
				 nb_path_canonical_missing(&canonical_path_buffer,
							   nb_buf_cstr(&canonical_path_buffer),
							   NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&canonical_path_buffer))
			 : NULL);
	ASSERT_NE(resolved, nullptr);
	char path_buf[512];
	snprintf(path_buf, sizeof(path_buf), "%s/ma_test_dir/file.txt", resolved);
	free(resolved);
	nb_file_mkdir("/tmp/ma_test_dir", true, NULL);
	EXPECT_EQ(nb_path_is_within(path_buf, "/tmp/ma_test_dir", true), 1);
	EXPECT_EQ(nb_path_is_within("/var/other/file.txt", "/tmp/ma_test_dir", true), 0);
	std::remove("/tmp/ma_test_dir");
}

TEST_F(FileTest, PathIsWithinExactMatch) {
	EXPECT_EQ(nb_path_is_within("/tmp/ma_test_dir", "/tmp/ma_test_dir", true), 1);
}

TEST_F(FileTest, PathIsWithinPrefixAttack) {
	EXPECT_EQ(nb_path_is_within("/tmp/ma_test_dir_other/file.txt", "/tmp/ma_test_dir", true),
		  0);
}

TEST_F(FileTest, PathIsWithinNull) {
	EXPECT_EQ(nb_path_is_within(NULL, "/tmp", true), 0);
	EXPECT_EQ(nb_path_is_within("/tmp", NULL, true), 0);
}

TEST_F(FileTest, ResolvePathNull) {
	nb_buf canonical_path_buffer NB_CLEANUP(nb_buf_destroy) = {};

	char *resolved =
		(nb_path_expand(&canonical_path_buffer, NULL, NULL) == NB_OK &&
				 nb_path_canonical_missing(&canonical_path_buffer,
							   nb_buf_cstr(&canonical_path_buffer),
							   NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&canonical_path_buffer))
			 : NULL);
	EXPECT_EQ(resolved, nullptr);
}

TEST_F(FileTest, ListsRejectOccupiedOwnersAndPreserveContentsOnFailure)
{
	ASSERT_EQ(nb_file_write_all(test_file, "data", 4, NULL), 0);
	for (unsigned flags :
	     {static_cast<unsigned>(NB_DIR_SCAN_FILES), static_cast<unsigned>(NB_DIR_SCAN_DIRS)}) {
		nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {};
		EXPECT_EQ(nb_dir_scan(test_file, flags, &names, nullptr), NB_ERR_NOT_DIR);
		EXPECT_FALSE(names.initialized);
		const std::string missing = directory + "/missing";
		EXPECT_EQ(nb_dir_scan(missing.c_str(), flags, &names, nullptr), NB_ERR_NOT_FOUND);
		EXPECT_FALSE(names.initialized);
		ASSERT_EQ(nb_dir_scan(directory.c_str(), flags, &names, nullptr), NB_OK);
		ASSERT_TRUE(names.initialized);
		const void *storage = names.names.data;
		const size_t count = names.names.len;
		EXPECT_EQ(nb_dir_scan(directory.c_str(), flags, &names, nullptr), NB_ERR_STATE);
		EXPECT_EQ(names.names.data, storage);
		EXPECT_EQ(names.names.len, count);
	}
}

TEST_F(FileTest, CopiedNamesSurviveDirectoryEntryRemoval)
{
	int file_scan_status = NB_OK;
	int directory_scan_status = NB_OK;

	const std::string file = directory + "/copied.txt";
	const std::string child = directory + "/copied-dir";
	ASSERT_EQ(nb_file_write_all(file.c_str(), "x", 1, NULL), 0);
	ASSERT_EQ(nb_file_mkdir(child.c_str(), true, NULL), 0);
	nb_dir_listing files NB_CLEANUP(nb_dir_listing_destroy) = {};
	nb_dir_listing dirs NB_CLEANUP(nb_dir_listing_destroy) = {};
	int file_count = 999;
	int dir_count = 999;
	ASSERT_EQ(((file_scan_status = nb_dir_scan(directory.c_str(),
						   NB_DIR_SCAN_FILES | NB_DIR_SCAN_SORT |
							   NB_DIR_SCAN_FOLLOW_LINKS,
						   &files, NULL)) != NB_OK
			   ? file_scan_status
		   : files.names.len > INT_MAX ? NB_ERR_RANGE
					       : (file_count = (int)files.names.len, NB_OK)),
		  0);
	ASSERT_EQ(((directory_scan_status = nb_dir_scan(directory.c_str(),
							NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
								NB_DIR_SCAN_FOLLOW_LINKS,
							&dirs, NULL)) != NB_OK
			   ? directory_scan_status
		   : dirs.names.len > INT_MAX ? NB_ERR_RANGE
					      : (dir_count = (int)dirs.names.len, NB_OK)),
		  0);
	ASSERT_EQ(file_count, 1);
	ASSERT_EQ(dir_count, 1);
	ASSERT_EQ(std::remove(file.c_str()), 0);
	ASSERT_EQ(std::remove(child.c_str()), 0);
	EXPECT_STREQ(nb_dir_listing_name(&files, (size_t)(0)), "copied.txt");
	EXPECT_STREQ(nb_dir_listing_name(&dirs, (size_t)(0)), "copied-dir");
	nb_dir_listing_destroy(&files);
	nb_dir_listing_destroy(&dirs);
}
