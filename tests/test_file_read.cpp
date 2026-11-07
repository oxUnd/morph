#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "agent/tools/file_read.h"
#include "util/cancel.h"
#include "util/file.h"
#include "util/utf8.h"
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class FileReadTest : public ::testing::Test {
protected:
	char directory[64] = "/tmp/morph-file-read-XXXXXX";
	std::string path;
	struct tool_registry registry = {};
	struct tool_context *context = nullptr;
	struct tool_result result = {};
	struct tool_runtime_context runtime = {};
	int status = 0;

	void SetUp() override
	{
		ASSERT_NE(mkdtemp(directory), nullptr);
		path = std::string(directory) + "/input";
		tool_registry_init(nullptr, &registry);
		tool_result_init(&result);
		context = tool_context_create(nullptr, directory, directory);
		ASSERT_NE(context, nullptr);
		ASSERT_EQ(file_read_init(&registry, context), 0);
	}

	void TearDown() override
	{
		tool_result_cleanup(&result);
		tool_registry_cleanup(&registry);
		tool_context_destroy(context);
		unlink(path.c_str());
		rmdir(directory);
	}

	void Write(const std::string &content)
	{
		ASSERT_EQ(file_write_all(path.c_str(), content.data(), content.size()), 0);
	}

	struct json *Read(const char *options = "{}")
	{
		struct json *args = json_parse_prefix(options, NULL, NULL);
		EXPECT_NE(args, nullptr);
		if (!args)
			return nullptr;
		json_add_string(args, "file_path", "input");
		char *json = json_stringify(args);
		json_free(args);
		struct json *data = Execute(json);
		free(json);
		return data;
	}

	struct json *Execute(const char *arguments)
	{
		tool_result_cleanup(&result);
		tool_result_init(&result);
		result.runtime = &runtime;
		status = tool_exec(&registry, "file_read", arguments, &result);
		return result.data;
	}

	std::string Text(struct json *data, const char *name = "content")
	{
		const char *value = json_string_value(json_member_ci(data, name));
		EXPECT_NE(value, nullptr) << name;
		return value ? value : "";
	}

	double Number(struct json *data, const char *name)
	{
		struct json *value = json_member_ci(data, name);
		EXPECT_TRUE(json_is_number_value(value)) << name;
		return value ? json_number_value(value) : -1;
	}

	bool Flag(struct json *data, const char *name)
	{
		struct json *value = json_member_ci(data, name);
		EXPECT_TRUE(json_is_bool_value(value)) << name;
		return json_is_true(value);
	}
};

TEST_F(FileReadTest, EmptyAndUnterminatedLineCounts)
{
	for (const std::string content : {"", "hello", "hello\n", "\n", "a\nb", "a\nb\n"}) {
		SCOPED_TRACE(content);
		Write(content);
		struct json *data = Read();
		ASSERT_EQ(status, 0);
		EXPECT_EQ(Text(data), content);
		double count = content.empty() ? 0 : content.find('b') != std::string::npos ? 2 : 1;
		EXPECT_EQ(Number(data, "total_lines"), count);
		EXPECT_EQ(Number(data, "returned_lines"), count);
		EXPECT_FALSE(Flag(data, "truncated"));
		EXPECT_EQ(Number(data, "line_start"), 0);
		EXPECT_EQ(Number(data, "line_end"), count);
	}
}

TEST_F(FileReadTest, PreservesCrLfAndUtf8Exactly)
{
	std::string original = "alpha\r\n中文😀\r\nomega";
	Write(original);
	struct json *data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), original);
	EXPECT_EQ(Number(data, "total_lines"), 3);
}

TEST_F(FileReadTest, LinePaginationAndLegacyAlias)
{
	Write("zero\none\ntwo\nthree");
	struct json *data = Read("{\"offset\":1,\"limit\":9,\"max_lines\":2}");
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), "one\ntwo\n");
	EXPECT_EQ(Number(data, "line_start"), 1);
	EXPECT_EQ(Number(data, "line_end"), 3);
	EXPECT_EQ(Number(data, "next_offset"), 3);
	EXPECT_EQ(Number(data, "next_byte_offset"), 13);
	EXPECT_TRUE(json_is_null_value(json_member_ci(data, "total_lines")));
	EXPECT_TRUE(Flag(data, "truncated"));
	EXPECT_EQ(Text(data, "truncation_reason"), "line_limit");
	data = Read("{\"offset\":3}");
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), "three");
	EXPECT_EQ(Number(data, "total_lines"), 4);
}

TEST_F(FileReadTest, OffsetAtAndBeyondEnd)
{
	for (const std::string original : {"", "one", "one\n", "one\ntwo"}) {
		Write(original);
		struct json *data = Read("{\"offset\":100}");
		ASSERT_EQ(status, 0);
		EXPECT_EQ(Text(data), "");
		EXPECT_EQ(Number(data, "returned_lines"), 0);
		EXPECT_FALSE(Flag(data, "truncated"));
		EXPECT_EQ(Number(data, "total_lines"), original.empty() ? 0 :
			original.find("two") == std::string::npos ? 1 : 2);
	}
}

TEST_F(FileReadTest, Utf8CharactersAcrossInputBufferBoundaries)
{
	for (size_t padding : {static_cast<size_t>(BUFSIZ), static_cast<size_t>(BUFSIZ + 1),
			       static_cast<size_t>(BUFSIZ + 2), static_cast<size_t>(BUFSIZ + 3)}) {
		std::string original(padding, 'a');
		original += "😀中文\n";
		Write(original);
		struct json *data = Read();
		ASSERT_EQ(status, 0);
		EXPECT_EQ(Text(data), original);
		EXPECT_EQ(utf8valid(Text(data).c_str()), nullptr);
	}
}

TEST_F(FileReadTest, ByteBudgetNeverSplitsUtf8AndContinuationIsLossless)
{
	std::string original = "ab😀中文\r\nend";
	Write(original);
	std::string reconstructed;
	std::string options = "{\"max_bytes\":4}";
	for (int i = 0; i < 20; i++) {
		struct json *data = Read(options.c_str());
		ASSERT_EQ(status, 0);
		std::string chunk = Text(data);
		EXPECT_LE(chunk.size(), 4u);
		EXPECT_EQ(utf8valid(chunk.c_str()), nullptr);
		reconstructed += chunk;
		if (!Flag(data, "truncated"))
			break;
		ASSERT_FALSE(chunk.empty());
		options = "{\"max_bytes\":4,\"byte_offset\":" +
			std::to_string(static_cast<long long>(Number(data, "next_byte_offset"))) +
			",\"expected_version\":\"" + Text(data, "version") + "\"}";
	}
	EXPECT_EQ(reconstructed, original);
}

TEST_F(FileReadTest, LongLineFragmentsDoNotLoseSuffix)
{
	std::string original(16383, 'a');
	original += "中文";
	original += std::string(50000, 'b') + "\ntail";
	Write(original);
	std::string reconstructed;
	std::string options = "{}";
	for (int i = 0; i < 10; i++) {
		struct json *data = Read(options.c_str());
		ASSERT_EQ(status, 0);
		std::string chunk = Text(data);
		EXPECT_EQ(utf8valid(chunk.c_str()), nullptr);
		reconstructed += chunk;
		if (!Flag(data, "truncated"))
			break;
		EXPECT_TRUE(Flag(data, "partial_line"));
		EXPECT_EQ(Text(data, "truncation_reason"), "line_length");
		EXPECT_TRUE(json_is_null_value(json_member_ci(data, "next_offset")));
		options = "{\"byte_offset\":" +
			std::to_string(static_cast<long long>(Number(data, "next_byte_offset"))) + "}";
	}
	EXPECT_EQ(reconstructed, original);
}

TEST_F(FileReadTest, DefaultByteLimitAndExactLimitEof)
{
	std::string original;
	for (int i = 0; i < 100; i++)
		original += std::string(999, 'x') + "\n";
	Write(original);
	struct json *data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data).size(), 51200u);
	EXPECT_TRUE(Flag(data, "truncated"));
	EXPECT_TRUE(Flag(data, "partial_line"));
	EXPECT_EQ(Text(data, "truncation_reason"), "max_bytes");
	Write("abcd");
	data = Read("{\"max_bytes\":4}");
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), "abcd");
	EXPECT_FALSE(Flag(data, "truncated"));
}

TEST_F(FileReadTest, ExactLineLimitDoesNotInventTruncation)
{
	Write("a\nb\n");
	struct json *data = Read("{\"limit\":2}");
	ASSERT_EQ(status, 0);
	EXPECT_FALSE(Flag(data, "truncated"));
	EXPECT_EQ(Number(data, "total_lines"), 2);
	EXPECT_TRUE(json_is_null_value(json_member_ci(data, "next_byte_offset")));
}

TEST_F(FileReadTest, NulAfterInitialSampleIsNotSilentlyIgnored)
{
	std::string original(5000, 'x');
	original += std::string("\0hidden", 7);
	Write(original);
	struct json *data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_TRUE(Flag(data, "binary"));
	EXPECT_EQ(json_member_ci(data, "content"), nullptr);
	EXPECT_EQ(Text(data, "preview_format"), "hex");
	EXPECT_NE(Text(data, "preview").find("78"), std::string::npos);
}

TEST_F(FileReadTest, InvalidUtf8AndIncompleteCharactersReturnHex)
{
	for (const std::string original : {std::string("\xff\xfeX", 3),
		std::string("ok\xe4\xb8", 4), std::string("\xc0\xaf", 2),
		std::string("\xed\xa0\x80", 3)}) {
		Write(original);
		struct json *data = Read();
		ASSERT_EQ(status, 0);
		EXPECT_TRUE(Flag(data, "binary"));
		EXPECT_EQ(utf8valid(Text(data, "preview").c_str()), nullptr);
	}
}

TEST_F(FileReadTest, BomHandlingIsExplicit)
{
	Write("\xef\xbb\xbfhello\n");
	struct json *data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), "hello\n");
	EXPECT_EQ(Number(data, "total_lines"), 1);
	data = Read("{\"byte_offset\":3}");
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data), "hello\n");
	EXPECT_FALSE(Flag(data, "starts_mid_line"));
	Write(std::string("\xff\xfeh\0i\0", 6));
	data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_TRUE(Flag(data, "binary"));
	EXPECT_EQ(Text(data, "encoding"), "utf-16");
	Write(std::string("\0\0\xfe\xff\0\0\0h", 8));
	data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_EQ(Text(data, "encoding"), "utf-32");
}

TEST_F(FileReadTest, RejectsInvalidNumericArguments)
{
	Write("text");
	for (const char *options :
	     {"{\"offset\":-1}", "{\"offset\":1.5}", "{\"offset\":1e100}", "{\"offset\":\"1\"}",
	      "{\"limit\":0}", "{\"limit\":-2}", "{\"max_lines\":0}", "{\"max_bytes\":3}",
	      "{\"max_bytes\":262145}", "{\"byte_offset\":-1}", "{\"byte_offset\":0,\"offset\":0}",
	      "{\"expected_version\":1}", "{\"offset\":null}", "{\"limit\":1000001}"}) {
		SCOPED_TRACE(options);
		Read(options);
		EXPECT_EQ(status, -EINVAL);
		EXPECT_FALSE(json_is_true(json_member_ci(result.envelope, "ok")));
	}
	/* Send overflow unchanged; the JSON module rejects it before a tree exists. */
	Execute("{\"file_path\":\"input\",\"offset\":1e999}");
	EXPECT_EQ(status, -EINVAL);
	EXPECT_FALSE(json_is_true(json_member_ci(result.envelope, "ok")));
}

TEST_F(FileReadTest, RejectsCursorInsideUtf8Character)
{
	Write("中text");
	Read("{\"byte_offset\":1}");
	EXPECT_EQ(status, -EINVAL);
	Read("{\"byte_offset\":100}");
	EXPECT_EQ(status, -EINVAL);
	struct json *data = Read("{\"byte_offset\":7}");
	EXPECT_EQ(status, 0);
	EXPECT_EQ(Text(data), "");
}

TEST_F(FileReadTest, DetectsChangedFileOnContinuation)
{
	Write("first\nsecond");
	struct json *data = Read("{\"limit\":1}");
	ASSERT_EQ(status, 0);
	std::string version = Text(data, "version");
	std::string options = "{\"byte_offset\":6,\"expected_version\":\"" + version + "\"}";
	Write("first\nchanged");
	Read(options.c_str());
	EXPECT_EQ(status, -ESTALE);
}

TEST_F(FileReadTest, RejectsNonRegularFileWithoutBlocking)
{
	ASSERT_EQ(mkfifo(path.c_str(), 0600), 0);
	Read();
	EXPECT_EQ(status, -ENOTSUP);
	unlink(path.c_str());
	ASSERT_EQ(mkdir(path.c_str(), 0700), 0);
	Read();
	EXPECT_EQ(status, -EISDIR);
	rmdir(path.c_str());
}

TEST_F(FileReadTest, PreservesMissingFileError)
{
	Read();
	EXPECT_EQ(status, -ENOENT);
}

TEST_F(FileReadTest, HugeSparseFileStopsBeforeReadingTail)
{
	Write(std::string(32768, 'x'));
	int fd = open(path.c_str(), O_WRONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ftruncate(fd, 1024LL * 1024 * 1024), 0);
	close(fd);
	struct json *data = Read();
	ASSERT_EQ(status, 0);
	EXPECT_FALSE(Flag(data, "binary"));
	EXPECT_EQ(Text(data), std::string(16384, 'x'));
	EXPECT_EQ(Number(data, "size"), 1024LL * 1024 * 1024);
	EXPECT_EQ(Number(data, "next_byte_offset"), 16384);
	EXPECT_TRUE(json_is_null_value(json_member_ci(data, "total_lines")));
}

TEST_F(FileReadTest, CancellationBeforeReading)
{
	Write("text");
	struct morph_cancel_token token = {};
	morph_cancel_token_cancel(&token);
	runtime.http.cancel_token = &token;
	Read();
	EXPECT_EQ(status, -ECANCELED);
}

TEST_F(FileReadTest, CancellationWhileSkippingLines)
{
	Write(std::string(1024 * 1024, 'x'));
	int checks = 0;
	runtime.http.interrupt_user_data = &checks;
	runtime.http.interrupt_check = [](void *opaque) {
		int *count = static_cast<int *>(opaque);
		return ++*count >= 5 ? 1 : 0;
	};
	Read("{\"offset\":100}");
	EXPECT_EQ(status, -ECANCELED);
	EXPECT_EQ(checks, 5);
}

TEST_F(FileReadTest, DetectsChangesDuringReading)
{
	Write(std::string(32768, 'x'));
	struct Mutation {
		const char *path;
		int checks;
	} mutation = {path.c_str(), 0};
	runtime.http.interrupt_user_data = &mutation;
	runtime.http.interrupt_check = [](void *opaque) {
		auto *state = static_cast<Mutation *>(opaque);
		if (++state->checks == 3) {
			int fd = open(state->path, O_WRONLY | O_APPEND);
			if (fd >= 0) {
				(void)write(fd, "!", 1);
				close(fd);
			}
		}
		return 0;
	};
	Read();
	EXPECT_EQ(status, -ESTALE);
}

TEST_F(FileReadTest, MixedTextPaginationReconstructsEverySourceByte)
{
	std::string original;
	for (int i = 0; i < 120; i++) {
		original += "中文😀\t" + std::string(static_cast<size_t>(i % 19), 'x');
		original += i % 3 == 0 ? "\r\n" : "\n";
	}
	original += "tail without newline";
	Write(original);
	for (int budget : {4, 7, 31, 8193}) {
		SCOPED_TRACE(budget);
		std::string assembled;
		std::string options = "{\"max_bytes\":" + std::to_string(budget) +
			",\"limit\":7}";
		uint64_t previous = 0;
		for (size_t page = 0; page <= original.size(); page++) {
			struct json *data = Read(options.c_str());
			ASSERT_EQ(status, 0);
			std::string chunk = Text(data);
			ASSERT_FALSE(chunk.empty());
			EXPECT_EQ(utf8valid(chunk.c_str()), nullptr);
			EXPECT_LE(chunk.size(), static_cast<size_t>(budget));
			assembled += chunk;
			if (!Flag(data, "truncated"))
				break;
			uint64_t cursor = static_cast<uint64_t>(Number(data, "next_byte_offset"));
			ASSERT_GT(cursor, previous);
			EXPECT_EQ(cursor, assembled.size());
			previous = cursor;
			options = "{\"max_bytes\":" + std::to_string(budget) +
				",\"limit\":7,\"byte_offset\":" + std::to_string(cursor) +
				",\"expected_version\":\"" + Text(data, "version") + "\"}";
		}
		EXPECT_EQ(assembled, original);
	}
}

TEST_F(FileReadTest, PreservesAccessDeniedError)
{
	if (geteuid() == 0)
		GTEST_SKIP() << "Root bypasses file permissions";
	Write("private");
	ASSERT_EQ(chmod(path.c_str(), 0000), 0);
	Read();
	EXPECT_EQ(status, -EACCES);
	ASSERT_EQ(chmod(path.c_str(), 0600), 0);
}

TEST_F(FileReadTest, CancellationWhileReadingText)
{
	Write(std::string(32768, 'x'));
	int checks = 0;
	runtime.http.interrupt_user_data = &checks;
	runtime.http.interrupt_check = [](void *opaque) {
		int *count = static_cast<int *>(opaque);
		return ++*count >= 3 ? 1 : 0;
	};
	Read();
	EXPECT_EQ(status, -ECANCELED);
	EXPECT_EQ(checks, 3);
}
