#include <gtest/gtest.h>

extern "C" {
#include "agent/patch.h"
#include "util/file.h"
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct SnapshotEntry {
	bool directory;
	std::string content;

	bool operator==(const SnapshotEntry &other) const
	{
		return directory == other.directory && content == other.content;
	}
};

using Snapshot = std::map<fs::path, SnapshotEntry>;

std::string read_text(const fs::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>());
}

Snapshot snapshot(const fs::path &root)
{
	Snapshot result;

	if (!fs::is_directory(root))
		return result;
	for (const auto &entry : fs::recursive_directory_iterator(root)) {
		fs::path relative = fs::relative(entry.path(), root);

		if (entry.is_directory())
			result.emplace(relative, SnapshotEntry{true, {}});
		else if (entry.is_regular_file())
			result.emplace(relative,
				SnapshotEntry{false, read_text(entry.path())});
	}
	return result;
}

std::vector<fs::path> scenario_directories()
{
	const fs::path root = fs::path(MORPH_TEST_SOURCE_DIR) / "tests" /
		"fixtures" / "apply_patch_codex";
	std::vector<fs::path> result;

	for (const auto &entry : fs::directory_iterator(root)) {
		if (entry.is_directory())
			result.push_back(entry.path());
	}
	std::sort(result.begin(), result.end());
	return result;
}

class CodexApplyPatchScenarioTest :
	public ::testing::TestWithParam<fs::path> {
};

TEST_P(CodexApplyPatchScenarioTest, MatchesExpectedFilesystemState)
{
	const fs::path scenario = GetParam();
	char workdir[] = "/tmp/morph-codex-patch-XXXXXX";
	struct patch_result result{};
	char error[BUFSIZ]{};

	ASSERT_NE(mkdtemp(workdir), nullptr);
	const fs::path input = scenario / "input";
	if (fs::is_directory(input))
		fs::copy(input, workdir, fs::copy_options::recursive |
			fs::copy_options::overwrite_existing);
	const std::string patch = read_text(scenario / "patch.txt");
	(void)patch_apply(workdir, patch.c_str(), &result, error, sizeof(error));
	patch_result_cleanup(&result);

	EXPECT_EQ(snapshot(workdir), snapshot(scenario / "expected"))
		<< "scenario: " << scenario.filename() << "\nerror: " << error;
	std::error_code cleanup_error;
	fs::remove_all(workdir, cleanup_error);
}

std::string scenario_name(
	const ::testing::TestParamInfo<fs::path> &parameter)
{
	std::string name = parameter.param.filename().string();

	for (char &character : name) {
		if (!std::isalnum(static_cast<unsigned char>(character)))
			character = '_';
	}
	return name;
}

INSTANTIATE_TEST_SUITE_P(CodexApplyPatchScenarios,
	CodexApplyPatchScenarioTest,
	::testing::ValuesIn(scenario_directories()), scenario_name);

class CodexFocusedPatchTest : public ::testing::Test {
protected:
	fs::path directory;

	void SetUp() override
	{
		char value[] = "/tmp/morph-codex-focused-XXXXXX";

		ASSERT_NE(mkdtemp(value), nullptr);
		directory = value;
	}

	void TearDown() override
	{
		std::error_code error;

		fs::remove_all(directory, error);
	}

	void write(const char *name, const std::string &content)
	{
		fs::path target = directory / name;

		fs::create_directories(target.parent_path());
		std::ofstream stream(target, std::ios::binary);
		stream << content;
		ASSERT_TRUE(stream.good());
	}

	int apply(const std::string &patch, std::string *message = nullptr)
	{
		struct patch_result result{};
		char error[BUFSIZ]{};
		int rc = patch_apply(directory.c_str(), patch.c_str(), &result,
			error, sizeof(error));

		patch_result_cleanup(&result);
		if (message)
			*message = error;
		return rc;
	}
};

TEST_F(CodexFocusedPatchTest, AcceptsLenientHeredocWrapper)
{
	const std::string patch =
		"<<'EOF'\n"
		"*** Begin Patch\n"
		"*** Add File: wrapped.txt\n"
		"+content\n"
		"*** End Patch\n"
		"EOF\n";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "wrapped.txt"), "content\n");
}

TEST_F(CodexFocusedPatchTest, AcceptsCrLfPatchLines)
{
	write("line.txt", "old\n");
	const std::string patch =
		"*** Begin Patch\r\n"
		"*** Update File: line.txt\r\n"
		"@@\r\n"
		"-old\r\n"
		"+new\r\n"
		"*** End Patch\r\n";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "line.txt"), "new\n");
}

TEST_F(CodexFocusedPatchTest, NormalizesUnicodePunctuationWhenMatching)
{
	write("unicode.txt", "left — right\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: unicode.txt\n"
		"@@\n"
		"-left - right\n"
		"+matched\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "unicode.txt"), "matched\n");
}

TEST_F(CodexFocusedPatchTest, EndOfFileMarkerSelectsLastMatch)
{
	write("tail.txt", "same\nmiddle\nsame\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: tail.txt\n"
		"@@\n"
		"-same\n"
		"+last\n"
		"*** End of File\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "tail.txt"), "same\nmiddle\nlast\n");
}

TEST_F(CodexFocusedPatchTest, PatternLongerThanInputFailsWithoutMutation)
{
	write("short.txt", "one\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: short.txt\n"
		"@@\n"
		"-one\n"
		"-two\n"
		"+changed\n"
		"*** End Patch";

	EXPECT_EQ(apply(patch), -EINVAL);
	EXPECT_EQ(read_text(directory / "short.txt"), "one\n");
}

TEST_F(CodexFocusedPatchTest, RejectsContentAfterEndPatch)
{
	const std::string patch =
		"*** Begin Patch\n"
		"*** Add File: value.txt\n"
		"+value\n"
		"*** End Patch\n"
		"trailing";

	EXPECT_EQ(apply(patch), -EINVAL);
	EXPECT_FALSE(fs::exists(directory / "value.txt"));
}

TEST_F(CodexFocusedPatchTest, AcceptsEnvironmentIdPreamble)
{
	const std::string patch =
		"*** Begin Patch\n"
		"*** Environment ID: local\n"
		"*** Add File: environment.txt\n"
		"+value\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "environment.txt"), "value\n");
}

TEST_F(CodexFocusedPatchTest, PureAdditionBeforeEarlierRemovalUsesOriginalIndexes)
{
	write("ordered.txt", "line1\nline2\nline3\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: ordered.txt\n"
		"@@\n"
		"+after-context\n"
		"+second-line\n"
		"@@\n"
		" line1\n"
		"-line2\n"
		"-line3\n"
		"+line2-replacement\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "ordered.txt"),
		"line1\nline2-replacement\nafter-context\nsecond-line\n");
}

TEST_F(CodexFocusedPatchTest, KeepsSingleIndentedUpdateMarkerAsContext)
{
	write("markers.txt", "old a\n*** Update File: b.txt\nold b\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: markers.txt\n"
		"@@\n"
		"-old a\n"
		"+new a\n"
		" *** Update File: b.txt\n"
		"@@\n"
		"-old b\n"
		"+new b\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "markers.txt"),
		"new a\n*** Update File: b.txt\nnew b\n");
}

TEST_F(CodexFocusedPatchTest, PreservesBareEmptyUpdateLine)
{
	write("blank.txt", "before\n\nafter\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: blank.txt\n"
		"@@\n"
		" before\n"
		"\n"
		"-after\n"
		"+changed\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "blank.txt"), "before\n\nchanged\n");
}

TEST_F(CodexFocusedPatchTest, IgnoresBlankLinesAfterEndOfFileMarker)
{
	write("eof.txt", "one\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: eof.txt\n"
		"@@\n"
		"+two\n"
		"*** End of File\n"
		"\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "eof.txt"), "one\ntwo\n");
}

TEST_F(CodexFocusedPatchTest, FallsBackWhenModelInventsDescriptiveAnchor)
{
	write("todo.c",
		"/**\n"
		" * todo.c - CLI Todo Manager\n"
		" * Compile: gcc -o todo todo.c\n"
		" */\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: todo.c\n"
		"@@ file header comment\n"
		" /**\n"
		"  * todo.c - CLI Todo Manager\n"
		"- * Compile: gcc -o todo todo.c\n"
		"+ * Compile: gcc -o todo todo.c -Wall -Wextra\n"
		"  */\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "todo.c"),
		"/**\n"
		" * todo.c - CLI Todo Manager\n"
		" * Compile: gcc -o todo todo.c -Wall -Wextra\n"
		" */\n");
}

TEST_F(CodexFocusedPatchTest, FallsBackWhenAnchorPointsAfterOldLines)
{
	write("misplaced.txt", "target\nreal anchor\n");
	const std::string patch =
		"*** Begin Patch\n"
		"*** Update File: misplaced.txt\n"
		"@@ real anchor\n"
		"-target\n"
		"+updated\n"
		"*** End Patch";

	ASSERT_EQ(apply(patch), 0);
	EXPECT_EQ(read_text(directory / "misplaced.txt"),
		"updated\nreal anchor\n");
}

} /* namespace */
