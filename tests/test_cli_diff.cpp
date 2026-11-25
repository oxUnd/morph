#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/services/git_diff.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/process.h>
}

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

class CliGitDiffTest : public testing::Test {
protected:
	char directory[128] = "/tmp/morph-git-diff-XXXXXX";
	void SetUp() override { ASSERT_NE(mkdtemp(directory), nullptr); }
	void TearDown() override { std::filesystem::remove_all(directory); }

	int Git(std::initializer_list<const char *> arguments)
	{
		std::vector<const char *> argv{"git", "-c", "commit.gpgsign=false", "-c",
			"core.hooksPath=/dev/null"};
		argv.insert(argv.end(), arguments);
		argv.push_back(nullptr);
		nb_process_options options{};
		options.argv = argv.data();
		options.cwd = directory;
		options.search_path = true;
		options.close_fds = true;
		nb_process *child NB_CLEANUP(nb_process_cleanup) = nullptr;
		int rc = nb_process_spawn(&options, &child, nullptr);
		if (rc)
			return rc;

		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {};
		nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {};
		nb_process_exchange_options exchange{};
		exchange.timeout_ms = 5000;
		exchange.max_output = 65536;
		int code = 0;
		rc = nb_process_exchange(child, nullptr, 0, &exchange, &output, &errors, &code, nullptr);
		EXPECT_EQ(rc, 0) << nb_buf_cstr(&errors);
		EXPECT_EQ(code, 0) << nb_buf_cstr(&errors);
		return rc ? rc : code;
	}

	void Write(const std::string &path, const std::string &text)
	{
		std::ofstream(std::string(directory) + "/" + path, std::ios::binary) << text;
	}

	struct cli_git_diff_result *Read(const char *cwd, const struct cli_git_diff_entry *entry = nullptr)
	{
		struct cli_git_diff_job *job NB_CLEANUP(cli_git_diff_stop) = nullptr;
		EXPECT_EQ(cli_git_diff_start(cwd, entry, &job), 0);
		struct cli_git_diff_result *result = nullptr;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		int rc = NB_ERR_AGAIN;
		while (job && rc == NB_ERR_AGAIN && std::chrono::steady_clock::now() < deadline) {
			rc = cli_git_diff_collect(&job, &result);
			if (rc == NB_ERR_AGAIN)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		EXPECT_EQ(rc, 0);
		return result;
	}
};

TEST_F(CliGitDiffTest, SeparatesIndexWorktreeRenamesBinaryAndLiteralPathsFromSubdirectories)
{
	ASSERT_EQ(Git({"init", "-q"}), 0);
	ASSERT_EQ(Git({"config", "user.name", "Diff Test"}), 0);
	ASSERT_EQ(Git({"config", "user.email", "diff@example.invalid"}), 0);
	Write("both.txt", "BASE\n");
	Write("deleted.txt", "DELETED\n");
	Write("old.txt", "RENAMED\n");
	Write("binary.dat", std::string("a\0b", 3));
	const std::string odd = ":literal[中] ?\n.txt";
	Write(odd, "OLD_LITERAL\n");
	ASSERT_EQ(Git({"add", "."}), 0);
	ASSERT_EQ(Git({"commit", "-qm", "base"}), 0);
	Write("both.txt", "INDEX\n");
	ASSERT_EQ(Git({"add", "both.txt"}), 0);
	Write("both.txt", "WORKTREE\n");
	Write(odd, "NEW_LITERAL\n");
	Write("binary.dat", std::string("x\0y", 3));
	std::filesystem::remove(std::string(directory) + "/deleted.txt");
	ASSERT_EQ(Git({"mv", "old.txt", "new.txt"}), 0);
	Write("untracked.txt", "NEW_FILE\n");
	std::filesystem::create_directory(std::string(directory) + "/nested");
	const std::string nested = std::string(directory) + "/nested";
	struct cli_git_diff_result *result NB_CLEANUP(cli_git_diff_result_cleanup) = Read(nested.c_str());
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->status, 0) << nb_buf_cstr(&result->error);
	ASSERT_EQ(result->entries.len, 7u);
	int both = 0, renamed = 0, binary = 0, literal = 0, added = 0, deleted = 0;
	for (size_t i = 0; i < result->entries.len; i++) {
		auto *entry = static_cast<struct cli_git_diff_entry *>(nb_vec_at(&result->entries, i));
		struct cli_git_diff_result *patch NB_CLEANUP(cli_git_diff_result_cleanup) =
			Read(nb_buf_cstr(&result->root), entry);
		ASSERT_NE(patch, nullptr);
		ASSERT_EQ(patch->status, 0) << nb_buf_cstr(&patch->error);
		const std::string text = nb_buf_cstr(&patch->patch);
		if (!strcmp(entry->path, "both.txt")) {
			both++;
			EXPECT_EQ(entry->added, 1);
			EXPECT_EQ(entry->removed, 1);
			EXPECT_NE(text.find(entry->kind == CLI_GIT_DIFF_STAGED ? "+INDEX" : "+WORKTREE"), std::string::npos);
		} else if (!strcmp(entry->path, "new.txt")) {
			renamed++;
			ASSERT_NE(entry->original, nullptr);
			EXPECT_STREQ(entry->original, "old.txt");
			EXPECT_NE(text.find("rename from old.txt"), std::string::npos);
		} else if (!strcmp(entry->path, "binary.dat")) {
			binary++;
			EXPECT_TRUE(entry->binary);
			EXPECT_NE(text.find("Binary files"), std::string::npos);
		} else if (entry->path == odd) {
			literal++;
			EXPECT_NE(text.find("+NEW_LITERAL"), std::string::npos);
		} else if (!strcmp(entry->path, "untracked.txt")) {
			added++;
			EXPECT_EQ(entry->kind, CLI_GIT_DIFF_UNTRACKED);
			EXPECT_NE(text.find("+NEW_FILE"), std::string::npos);
		} else if (!strcmp(entry->path, "deleted.txt")) {
			deleted++;
			EXPECT_NE(text.find("-DELETED"), std::string::npos);
		}
	}

	EXPECT_EQ(both, 2);
	EXPECT_EQ(renamed, 1);
	EXPECT_EQ(binary, 1);
	EXPECT_EQ(literal, 1);
	EXPECT_EQ(added, 1);
	EXPECT_EQ(deleted, 1);
}

TEST_F(CliGitDiffTest, NonRepositoriesAndCancellationHaveExplicitOwnedResults)
{
	struct cli_git_diff_result *result NB_CLEANUP(cli_git_diff_result_cleanup) = Read(directory);
	ASSERT_NE(result, nullptr);
	EXPECT_NE(result->status, 0);
	EXPECT_EQ(result->entries.len, 0u);
	struct cli_git_diff_job *job NB_CLEANUP(cli_git_diff_stop) = nullptr;
	ASSERT_EQ(cli_git_diff_start(directory, nullptr, &job), 0);
	EXPECT_EQ(cli_git_diff_start(directory, nullptr, &job), NB_ERR_EXISTS);
	cli_git_diff_stop(&job);
	EXPECT_EQ(job, nullptr);
}

TEST_F(CliGitDiffTest, NewFilesWorkBeforeFirstCommitAndLargeDiffsAreMarkedTruncated)
{
	ASSERT_EQ(Git({"init", "-q"}), 0);
	Write("new.txt", "new\n");
	struct cli_git_diff_result *result NB_CLEANUP(cli_git_diff_result_cleanup) = Read(directory);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->status, 0) << nb_buf_cstr(&result->error);
	ASSERT_EQ(result->entries.len, 1u);
	Write("new.txt", std::string(2u * 1024u * 1024u, 'x'));
	auto *entry = static_cast<struct cli_git_diff_entry *>(nb_vec_at(&result->entries, 0));
	struct cli_git_diff_result *patch NB_CLEANUP(cli_git_diff_result_cleanup) = Read(directory, entry);
	ASSERT_NE(patch, nullptr);
	EXPECT_EQ(patch->status, 0);
	EXPECT_TRUE(patch->truncated);
	EXPECT_LE(patch->patch.len, 1024u * 1024u);
}
