#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "agent/tools/file_list.h"
#include "util/cancel.h"
#include "util/file.h"
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

class FileListTest : public ::testing::Test {
protected:
	char directory[64] = "/tmp/morph-file-list-XXXXXX";
	struct tool_registry registry = {};
	struct tool_context *context = nullptr;
	struct tool_result result = {};
	struct tool_runtime_context runtime = {};
	int status = 0;

	void SetUp() override
	{
		ASSERT_NE(mkdtemp(directory), nullptr);
		tool_registry_init(nullptr, &registry);
		tool_result_init(&result);
		context = tool_context_create(nullptr, directory, directory);
		ASSERT_NE(context, nullptr);
		ASSERT_EQ(file_list_init(&registry, context), 0);
	}

	void TearDown() override
	{
		tool_result_cleanup(&result);
		tool_registry_cleanup(&registry);
		tool_context_destroy(context);
		std::filesystem::remove_all(directory);
	}

	std::string Path(const std::string &name) { return std::string(directory) + "/" + name; }

	void File(const std::string &name)
	{
		ASSERT_EQ(file_write_all(Path(name).c_str(), "", 0), 0);
	}

	void Directory(const std::string &name)
	{
		ASSERT_EQ(mkdir(Path(name).c_str(), 0700), 0);
	}

	cJSON *List(const char *options = "{}", const char *path = ".")
	{
		cJSON *args = cJSON_Parse(options);
		EXPECT_NE(args, nullptr);
		if (!args)
			return nullptr;
		cJSON_AddStringToObject(args, "dir_path", path);
		char *json = cJSON_PrintUnformatted(args);
		cJSON_Delete(args);
		tool_result_cleanup(&result);
		tool_result_init(&result);
		result.runtime = &runtime;
		status = tool_exec(&registry, "file_list", json, &result);
		free(json);
		return result.data;
	}

	std::vector<std::pair<std::string, std::string>> Entries(cJSON *data)
	{
		std::vector<std::pair<std::string, std::string>> values;
		cJSON *entries = cJSON_GetObjectItem(data, "entries");
		EXPECT_TRUE(cJSON_IsArray(entries));
		cJSON *entry;
		cJSON_ArrayForEach(entry, entries) {
			const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "name"));
			const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "type"));
			EXPECT_NE(name, nullptr);
			EXPECT_NE(type, nullptr);
			if (name && type)
				values.emplace_back(name, type);
		}
		return values;
	}
};

TEST_F(FileListTest, EmptyDirectory)
{
	cJSON *data = List();
	ASSERT_EQ(status, 0);
	EXPECT_TRUE(Entries(data).empty());
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "total_entries")), 0);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
	EXPECT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(data, "next_after")));
}

TEST_F(FileListTest, DirectoriesFirstAndHiddenEntriesRetained)
{
	File("z-file");
	Directory("z-dir");
	File(".hidden");
	Directory("a-dir");
	File("a-file");
	File("z-dir/nested");
	cJSON *data = List();
	ASSERT_EQ(status, 0);
	std::vector<std::pair<std::string, std::string>> expected = {
		{"a-dir", "dir"}, {"z-dir", "dir"}, {".hidden", "file"},
		{"a-file", "file"}, {"z-file", "file"}};
	EXPECT_EQ(Entries(data), expected);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "total_entries")), 5);
}

TEST_F(FileListTest, SymlinksAndSpecialFilesHaveTheirOwnTypes)
{
	ASSERT_EQ(symlink("/", Path("outside").c_str()), 0);
	ASSERT_EQ(symlink("missing-target", Path("broken").c_str()), 0);
	ASSERT_EQ(mkfifo(Path("pipe").c_str(), 0600), 0);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_GE(fd, 0);
	struct sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	std::string path = Path("socket");
	ASSERT_LT(path.size(), sizeof(address.sun_path));
	memcpy(address.sun_path, path.c_str(), path.size() + 1);
	int rc = bind(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address));
	close(fd);
	ASSERT_EQ(rc, 0);
	cJSON *data = List();
	ASSERT_EQ(status, 0);
	std::vector<std::pair<std::string, std::string>> expected = {
		{"broken", "symlink"}, {"outside", "symlink"}, {"pipe", "fifo"}, {"socket", "socket"}};
	EXPECT_EQ(Entries(data), expected);
}

TEST_F(FileListTest, CursorPagesReconstructSortedDirectory)
{
	std::vector<std::pair<std::string, std::string>> expected;
	for (int i = 119; i >= 0; i--) {
		std::string name = std::to_string(i) + "-中文-" + std::string(130, 'x');
		if (i % 5 == 0)
			Directory(name);
		else
			File(name);
		expected.emplace_back(name, i % 5 == 0 ? "dir" : "file");
	}
	std::sort(expected.begin(), expected.end(), [](const auto &a, const auto &b) {
		if ((a.second == "dir") != (b.second == "dir"))
			return a.second == "dir";
		return a.first < b.first;
	});
	std::vector<std::pair<std::string, std::string>> actual;
	std::string options = "{\"limit\":17,\"max_bytes\":2048}";
	for (int page = 0; page < 120; page++) {
		cJSON *data = List(options.c_str());
		ASSERT_EQ(status, 0);
		auto entries = Entries(data);
		ASSERT_FALSE(entries.empty());
		actual.insert(actual.end(), entries.begin(), entries.end());
		char *encoded = cJSON_PrintUnformatted(cJSON_GetObjectItem(data, "entries"));
		ASSERT_NE(encoded, nullptr);
		EXPECT_LE(strlen(encoded), 2048u);
		free(encoded);
		if (!cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")))
			break;
		cJSON *next = cJSON_GetObjectItem(data, "next_after");
		ASSERT_TRUE(cJSON_IsObject(next));
		char *cursor = cJSON_PrintUnformatted(next);
		ASSERT_NE(cursor, nullptr);
		options = "{\"limit\":17,\"max_bytes\":2048,\"after\":" + std::string(cursor) + "}";
		free(cursor);
	}
	EXPECT_EQ(actual, expected);
}

TEST_F(FileListTest, EscapedNamesFitTheActualJsonByteBudget)
{
	for (int i = 0; i < 50; i++)
		File(std::to_string(i) + "-\n\t\"\\-" + std::string(160, '\001'));
	cJSON *data = List("{\"max_bytes\":2048}");
	ASSERT_EQ(status, 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
	char *encoded = cJSON_PrintUnformatted(cJSON_GetObjectItem(data, "entries"));
	ASSERT_NE(encoded, nullptr);
	EXPECT_LE(strlen(encoded), 2048u);
	free(encoded);
	EXPECT_FALSE(Entries(data).empty());
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "truncation_reason")), "max_bytes");
}

TEST_F(FileListTest, LargeDirectoryDefaultPageAndFullExplicitPage)
{
	for (int i = 0; i < 1100; i++)
		File("file-" + std::to_string(i));
	cJSON *data = List();
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Entries(data).size(), 1000u);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "total_entries")), 1100);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(data, "truncation_reason")), "limit");
	data = List("{\"limit\":2000,\"max_bytes\":1048576}");
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Entries(data).size(), 1100u);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
}

TEST_F(FileListTest, CursorPastLastEntryReturnsEmptyPage)
{
	File("first");
	cJSON *data = List("{\"after\":{\"name\":\"zzz\",\"type\":\"file\"}}");
	ASSERT_EQ(status, 0);
	EXPECT_TRUE(Entries(data).empty());
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "total_entries")), 1);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
}

TEST_F(FileListTest, RejectsMalformedOptions)
{
	for (const char *options : {"{\"limit\":0}", "{\"limit\":-1}", "{\"limit\":1.5}",
		"{\"limit\":10001}", "{\"limit\":1e100}", "{\"limit\":null}",
		"{\"max_bytes\":2047}", "{\"max_bytes\":1048577}", "{\"max_bytes\":\"2048\"}",
		"{\"after\":null}", "{\"after\":{}}", "{\"after\":{\"name\":\"x\"}}",
		"{\"after\":{\"name\":\"\",\"type\":\"file\"}}",
		"{\"after\":{\"name\":\"x\",\"type\":\"invalid\"}}"}) {
		SCOPED_TRACE(options);
		List(options);
		EXPECT_EQ(status, -EINVAL);
		EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok")));
	}
	List("{}", "");
	EXPECT_EQ(status, -EINVAL);
}

TEST_F(FileListTest, MissingAndNonDirectoryErrorsAreDistinct)
{
	List("{}", "missing");
	EXPECT_EQ(status, -ENOENT);
	File("regular");
	List("{}", "regular");
	EXPECT_EQ(status, -ENOTDIR);
}

TEST_F(FileListTest, PermissionDeniedIsPreserved)
{
	if (geteuid() == 0)
		GTEST_SKIP() << "Root bypasses directory permissions";
	Directory("denied");
	ASSERT_EQ(chmod(Path("denied").c_str(), 0000), 0);
	List("{}", "denied");
	EXPECT_EQ(status, -EACCES);
	ASSERT_EQ(chmod(Path("denied").c_str(), 0700), 0);
}

TEST_F(FileListTest, RejectsInvalidUtf8NamesWithoutChangingTheirIdentity)
{
	std::string path = Path(std::string("bad-\xff", 5));
	int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
	if (fd < 0 && errno == EILSEQ)
		GTEST_SKIP() << "Filesystem requires UTF-8 names";
	ASSERT_GE(fd, 0);
	close(fd);
	List();
	EXPECT_EQ(status, -EILSEQ);
}

TEST_F(FileListTest, CancellationBeforeOpeningDirectory)
{
	struct morph_cancel_token token = {};
	morph_cancel_token_cancel(&token);
	runtime.http.cancel_token = &token;
	List();
	EXPECT_EQ(status, -ECANCELED);
}

TEST_F(FileListTest, CancellationWhileScanning)
{
	for (int i = 0; i < 500; i++)
		File(std::to_string(i));
	int checks = 0;
	runtime.http.interrupt_user_data = &checks;
	runtime.http.interrupt_check = [](void *opaque) {
		int *count = static_cast<int *>(opaque);
		return ++*count >= 5 ? 1 : 0;
	};
	List();
	EXPECT_EQ(status, -ECANCELED);
	EXPECT_EQ(checks, 5);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok")));
}

TEST_F(FileListTest, ListingAnExternalSymlinkDirectoryIsDenied)
{
	ASSERT_EQ(symlink("/", Path("outside").c_str()), 0);
	List("{}", "outside");
	EXPECT_EQ(status, -EPERM);
}

TEST_F(FileListTest, ReaddirFailureDoesNotReturnSuccessfulPartialListing)
{
	char *canonical = file_resolve_path(directory);
	ASSERT_NE(canonical, nullptr);
	struct CloseDirectory {
		const char *path;
		int checks;
		bool closed;
	} state = {canonical, 0, false};
	runtime.http.interrupt_user_data = &state;
	runtime.http.interrupt_check = [](void *opaque) {
		auto *input = static_cast<CloseDirectory *>(opaque);
		if (++input->checks != 2)
			return 0;
		for (int fd = 3; fd < 256; fd++) {
			char target[PATH_MAX];
#ifdef __APPLE__
			if (fcntl(fd, F_GETPATH, target) < 0)
				continue;
#else
			char descriptor[BUFSIZ];
			(void)snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
			ssize_t count = readlink(descriptor, target, sizeof(target) - 1);
			if (count < 0)
				continue;
			target[count] = '\0';
#endif
			if (strcmp(target, input->path) == 0) {
				close(fd);
				input->closed = true;
				break;
			}
		}
		return 0;
	};
	List();
	EXPECT_TRUE(state.closed);
	EXPECT_EQ(status, -EBADF);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok")));
	free(canonical);
}

TEST_F(FileListTest, LongUnicodeNamesAreNotClampedToNameMaxBytes)
{
	std::string name;
	for (int i = 0; i < 100; i++)
		name += "文";
	int fd = open(Path(name).c_str(), O_CREAT | O_WRONLY, 0600);
	if (fd < 0 && errno == ENAMETOOLONG)
		GTEST_SKIP() << "Filesystem limits filename bytes";
	ASSERT_GE(fd, 0);
	close(fd);
	cJSON *data = List("{\"limit\":1}");
	ASSERT_EQ(status, 0);
	auto entries = Entries(data);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0].first, name);
	EXPECT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(data, "truncated")));
}
