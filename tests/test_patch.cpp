#include <gtest/gtest.h>

extern "C" {
#include "agent/patch.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tools/apply_patch.h"
#include "util/file.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class PatchTest : public ::testing::Test {
protected:
	char dir[PATH_MAX]{};

	void SetUp() override
	{
		std::strcpy(dir, "/tmp/morph-patch-test-XXXXXX");
		ASSERT_NE(mkdtemp(dir), nullptr);
	}

	void TearDown() override
	{
		std::error_code error;

		std::filesystem::remove_all(dir, error);
	}

	std::string path(const char *name) const
	{
		return std::string(dir) + "/" + name;
	}

	void write_file(const char *name, const char *content)
	{
		ASSERT_EQ(file_write_all(path(name).c_str(), content,
			std::strlen(content)), 0);
	}

	std::string read_file(const char *name)
	{
		char *content = file_read_all(path(name).c_str(), nullptr);
		EXPECT_NE(content, nullptr);
		std::string value = content ? content : "";
		std::free(content);
		return value;
	}

	int apply(const char *input, struct patch_result *result,
		  char *error, size_t error_size)
	{
		return patch_apply(dir, input, result, error, error_size);
	}
};

TEST_F(PatchTest, AddsUpdatesAndDeletesFiles)
{
	write_file("update.txt", "alpha\nbeta\n");
	write_file("delete.txt", "gone\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Add File: add.txt\n"
		"+created\n"
		"*** Update File: update.txt\n"
		"@@\n"
		" alpha\n"
		"-beta\n"
		"+gamma\n"
		"*** Delete File: delete.txt\n"
		"*** End Patch\n";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(result.changes.nelts, 3U);
	EXPECT_EQ(read_file("add.txt"), "created\n");
	EXPECT_EQ(read_file("update.txt"), "alpha\ngamma\n");
	EXPECT_EQ(access(path("delete.txt").c_str(), F_OK), -1);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, AppliesMultipleOrderedHunks)
{
	write_file("sample.txt", "one\ntwo\nthree\nfour\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: sample.txt\n"
		"@@\n"
		" one\n"
		"-two\n"
		"+second\n"
		"@@\n"
		" three\n"
		"-four\n"
		"+fourth\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("sample.txt"), "one\nsecond\nthree\nfourth\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, UsesSemanticHunkContextLikeCodex)
{
	write_file("numbered.txt", "one\ntwo\nthree\n");
	const char *valid =
		"*** Begin Patch\n"
		"*** Update File: numbered.txt\n"
		"@@ one\n"
		"-two\n"
		"+second\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(valid, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("numbered.txt"), "one\nsecond\nthree\n");
	patch_result_cleanup(&result);

	const char *invalid =
		"*** Begin Patch\n"
		"*** Update File: numbered.txt\n"
		"@@ absent anchor\n"
		"-missing\n"
		"+other\n"
		"*** End Patch";
	EXPECT_EQ(apply(invalid, &result, error, sizeof(error)), -EINVAL);
	EXPECT_NE(std::strstr(error, "expected lines"), nullptr);
}

TEST_F(PatchTest, MissingFileErrorIncludesResolvedWorkspacePath)
{
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: missing.txt\n"
		"@@ -1,1 +1,1 @@\n"
		"-old\n"
		"+new\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -ENOENT);
	EXPECT_NE(std::strstr(error, dir), nullptr);
	EXPECT_NE(std::strstr(error, "missing.txt"), nullptr);
}

TEST_F(PatchTest, AppliesSingleUpdateHunkWithoutHeader)
{
	write_file("todo.c",
		"    unsigned int max_id = 0;\n"
		"    Todo t;\n"
		"    long pos = ftell(f);\n"
		"    while (read_record(f, &t) == 1) {\n"
		"        if (t.id > max_id) max_id = t.id;\n"
		"    }\n"
		"    fseek(f, pos, SEEK_SET); /* 回到记录起点, 追加 */\n"
		"    t.id = max_id + 1;\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: todo.c\n"
		"    unsigned int max_id = 0;\n"
		"    Todo t;\n"
		"    long pos = ftell(f);\n"
		"    while (read_record(f, &t) == 1) {\n"
		"        if (t.id > max_id) max_id = t.id;\n"
		"    }\n"
		"-    fseek(f, pos, SEEK_SET); /* 回到记录起点, 追加 */\n"
		"+    fseek(f, 0, SEEK_END); /* 追加到文件末尾 */\n"
		"    t.id = max_id + 1;\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_NE(read_file("todo.c").find("fseek(f, 0, SEEK_END)"),
		  std::string::npos);
	EXPECT_EQ(result.changes.nelts, 1U);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, FailureAfterSuccessKeepsEarlierChangesLikeCodex)
{
	write_file("first.txt", "before\n");
	write_file("second.txt", "actual\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: first.txt\n"
		"@@\n"
		"-before\n"
		"+after\n"
		"*** Update File: second.txt\n"
		"@@\n"
		"-missing\n"
		"+changed\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -EINVAL);
	EXPECT_NE(std::strstr(error, "expected lines"), nullptr);
	EXPECT_EQ(read_file("first.txt"), "after\n");
	EXPECT_EQ(read_file("second.txt"), "actual\n");
}

TEST_F(PatchTest, AppliesFirstMatchingContextLikeCodex)
{
	write_file("repeat.txt", "same\nsame\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: repeat.txt\n"
		"@@\n"
		"-same\n"
		"+other\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("repeat.txt"), "other\nsame\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, MatchesIgnoringSurroundingWhitespaceLikeCodex)
{
	write_file("space.txt", "    old value   \nnext\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: space.txt\n"
		"@@\n"
		"-old value\n"
		"+new value\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("space.txt"), "new value\nnext\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, RejectsTraversalAndAbsolutePaths)
{
	const char *traversal =
		"*** Begin Patch\n*** Add File: ../escape.txt\n+x\n*** End Patch";
	const char *absolute =
		"*** Begin Patch\n*** Add File: /tmp/escape.txt\n+x\n*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(traversal, &result, error, sizeof(error)), -EINVAL);
	EXPECT_EQ(apply(absolute, &result, error, sizeof(error)), -EINVAL);
}

TEST_F(PatchTest, RejectsSymbolicLinkTargets)
{
	write_file("real.txt", "value\n");
	ASSERT_EQ(symlink(path("real.txt").c_str(), path("link.txt").c_str()), 0);
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: link.txt\n"
		"@@\n"
		"-value\n"
		"+changed\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -ELOOP);
	EXPECT_EQ(read_file("real.txt"), "value\n");
}

TEST_F(PatchTest, PreservesUpdatedFileMode)
{
	write_file("script.sh", "old\n");
	ASSERT_EQ(chmod(path("script.sh").c_str(), 0751), 0);
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: script.sh\n"
		"@@\n"
		"-old\n"
		"+new\n"
		"*** End Patch";
	struct patch_result result{};
	struct stat st{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	ASSERT_EQ(stat(path("script.sh").c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0751);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, ToolAcceptsRawTextAndPublishesGrammar)
{
	struct tool_registry registry{};
	struct tool_context *context = tool_context_create(dir, dir);
	struct tool_result result{};
	const char *input =
		"*** Begin Patch\n"
		"*** Add File: tool.txt\n"
		"+raw input\n"
		"*** End Patch";

	ASSERT_NE(context, nullptr);
	tool_registry_init(&registry);
	ASSERT_EQ(apply_patch_init(&registry, context), 0);
	struct tool_entry *entry = tool_lookup(&registry, "apply_patch");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->desc.input_kind, TOOL_INPUT_TEXT);
	EXPECT_NE(strstr(entry->desc.description, "below 4 KiB"), nullptr);
	EXPECT_NE(strstr(entry->desc.description, "continuation marker"),
		nullptr);
	EXPECT_NE(strstr(entry->desc.description, "Codex patch"), nullptr);
	EXPECT_NE(strstr(entry->desc.description, dir), nullptr);
	EXPECT_NE(strstr(entry->desc.description, "copied verbatim"), nullptr);
	cJSON *format = cJSON_Parse(entry->desc.input_format);
	ASSERT_NE(format, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(format, "syntax")),
		"lark");
	cJSON_Delete(format);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "apply_patch", input, &result), 0);
	EXPECT_EQ(read_file("tool.txt"), "raw input\n");
	ASSERT_NE(result.envelope, nullptr);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok")));
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}
