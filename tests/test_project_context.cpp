#include "agent/project_context.h"
#include "nb/err.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

class ProjectContextTest : public ::testing::Test {
protected:
	std::filesystem::path base;

	void SetUp() override {
		char pattern[] = "/tmp/morph-project-context-XXXXXX";
		char *path = mkdtemp(pattern);
		ASSERT_NE(path, nullptr);
		base = path;

		std::filesystem::create_directories(base / "repo" / "child");
		std::filesystem::create_directory(base / "global");
	}
	void TearDown() override {

		std::filesystem::remove_all(base);
	}
	void write(const std::filesystem::path &path, const std::string &text) {
		std::ofstream(path, std::ios::binary) << text;
	}
	std::string build(bool git = true) {
		std::string cwd = (base / "repo" / "child").string();
		std::string root = (base / "repo").string();
		std::string global = (base / "global").string();
		environment_context env{};
		env.cwd = cwd.c_str();
		env.git_root = git ? root.c_str() : nullptr;
		char *out NB_CLEANUP(nb_charp_cleanup) = nullptr;
		EXPECT_EQ(project_context_create(NULL, &env, global.c_str(), &out), 0);
		return out ? out : "";
	}
};

TEST_F(ProjectContextTest, LoadsGlobalThenRootThenScopedOverride)
{
	write(base / "global" / "AGENTS.md", "GLOBAL_MARKER");
	write(base / "repo" / "AGENTS.md", "ROOT_MARKER");
	write(base / "repo" / "child" / "AGENTS.md", "SHADOWED_MARKER");
	write(base / "repo" / "child" / "AGENTS.override.md", "CHILD_MARKER");
	write(base / "AGENTS.md", "OUTSIDE_MARKER");
	std::string result = build();
	EXPECT_LT(result.find("GLOBAL_MARKER"), result.find("ROOT_MARKER"));
	EXPECT_LT(result.find("ROOT_MARKER"), result.find("CHILD_MARKER"));
	EXPECT_EQ(result.find("SHADOWED_MARKER"), std::string::npos);
	EXPECT_EQ(result.find("OUTSIDE_MARKER"), std::string::npos);
	EXPECT_NE(result.find((base / "repo" / "child").string()), std::string::npos);
}

TEST_F(ProjectContextTest, NoGitLoadsOnlyCurrentDirectoryAndEmptyOverrideFallsBack)
{
	write(base / "repo" / "AGENTS.md", "PARENT_MARKER");
	write(base / "repo" / "child" / "AGENTS.override.md", " \n\t");
	write(base / "repo" / "child" / "AGENTS.md", "LOCAL_MARKER");
	std::string result = build(false);
	EXPECT_EQ(result.find("PARENT_MARKER"), std::string::npos);
	EXPECT_NE(result.find("LOCAL_MARKER"), std::string::npos);
}

TEST_F(ProjectContextTest, LimitsCombinedBodiesWithoutPartialInstructions)
{
	write(base / "repo" / "AGENTS.md", std::string(PROJECT_CONTEXT_MAX_BYTES - 4, 'x'));
	write(base / "repo" / "child" / "AGENTS.md", "CHILD_LONG_MARKER");
	std::string result = build();
	EXPECT_EQ(result.find("CHILD_LONG_MARKER"), std::string::npos);
	EXPECT_NE(result.find("omitted due to budget"), std::string::npos);
}

TEST_F(ProjectContextTest, RejectsOversizedInvalidAndSpecialFiles)
{
	auto path = base / "repo" / "child" / "AGENTS.override.md";
	write(path, std::string(PROJECT_CONTEXT_MAX_BYTES + 1, 'x'));
	EXPECT_NE(build().find("unavailable"), std::string::npos);
	write(path, std::string("bad\0data", 8));
	EXPECT_NE(build().find("unavailable"), std::string::npos);
	write(path, std::string("\xff", 1));
	EXPECT_NE(build().find("unavailable"), std::string::npos);
	std::filesystem::remove(path);
	ASSERT_EQ(mkfifo(path.c_str(), 0600), 0);
	EXPECT_NE(build().find("unavailable"), std::string::npos);
}

TEST_F(ProjectContextTest, OccupiedOutputPreservesExistingSnapshot)
{
	struct environment_context environment{};
	char *snapshot = strdup("retained guidance");
	ASSERT_NE(snapshot, nullptr);
	char *owner = snapshot;
	EXPECT_EQ(project_context_create(nullptr, &environment, nullptr, &snapshot), NB_ERR_STATE);
	EXPECT_EQ(snapshot, owner);
	EXPECT_STREQ(snapshot, "retained guidance");
	free(snapshot);
}

TEST_F(ProjectContextTest, ConfiguredBudgetLoadsLargerFilesWithoutPartialInstructions)
{
	const std::string content = std::string(40000, 'x') + "TAIL_REQUIREMENT";
	write(base / "repo" / "child" / "AGENTS.md", content);
	std::string cwd = (base / "repo" / "child").string();
	struct environment_context environment{};
	environment.cwd = cwd.c_str();
	char *snapshot NB_CLEANUP(nb_charp_cleanup) = nullptr;
	ASSERT_EQ(project_context_create_with_budget(nullptr, &environment, nullptr, 65536,
		&snapshot), 0);
	ASSERT_NE(snapshot, nullptr);
	EXPECT_NE(std::string(snapshot).find(content), std::string::npos);
	nb_charp_cleanup(&snapshot);
	ASSERT_EQ(project_context_create_with_budget(nullptr, &environment, nullptr, 32768,
		&snapshot), 0);
	EXPECT_NE(std::string(snapshot).find("file_read"), std::string::npos);
	EXPECT_EQ(std::string(snapshot).find("TAIL_REQUIREMENT"), std::string::npos);
}
