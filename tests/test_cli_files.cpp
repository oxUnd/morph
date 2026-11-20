#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/models/files.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/services/file_preview.h"
#include "sapi/cli/presentation/messages.h"
#include <nb/err.h>
}

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

class CliFilesTest : public testing::Test {
protected:
	struct cli_files_model model{};
	char directory[128] = "/tmp/morph-files-XXXXXX";

	void SetUp() override
	{
		ASSERT_NE(mkdtemp(directory), nullptr);
		cli_files_init(&model);
	}

	void TearDown() override
	{
		cli_files_cleanup(&model);
		std::filesystem::remove_all(directory);
	}

	static struct cli_file_preview *Read(const std::string &path)
	{
		struct cli_file_preview_job *job = nullptr;
		EXPECT_EQ(cli_file_preview_start(path.c_str(), &job), 0);
		if (!job)
			return nullptr;

		struct cli_file_preview *preview = nullptr;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		int rc = NB_ERR_AGAIN;
		while (rc == NB_ERR_AGAIN && std::chrono::steady_clock::now() < deadline) {
			rc = cli_file_preview_collect(&job, &preview);
			if (rc == NB_ERR_AGAIN)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		EXPECT_EQ(rc, 0);
		cli_file_preview_stop(&job);
		return preview;
	}
};

TEST_F(CliFilesTest, LocalReferencesDecodeAndKeepSourceLocations)
{
	for (const char *link : {"src/../hello%20world.c:17:4", "hello%20world.c#L17"}) {
		nb_buf path{};
		int line = 0;
		ASSERT_EQ(cli_files_resolve(directory, link, &path, &line), 0);
		EXPECT_EQ(std::string(nb_buf_cstr(&path)),
			std::string(directory) + "/hello world.c");
		EXPECT_EQ(line, 17);
		nb_buf_cleanup(&path);
	}

	for (const char *link : {"https://example.com/a.c", "file://remote/a.c", "#section",
		"data:text/plain,content", "bad%00name", "bad%0Aname"}) {
		nb_buf path{};
		EXPECT_NE(cli_files_resolve(directory, link, &path, nullptr), 0) << link;
		EXPECT_EQ(path.len, 0u);
		nb_buf_cleanup(&path);
	}
}

TEST_F(CliFilesTest, InventoryUsesMarkdownLinksAndSuccessfulToolReceipts)
{
	ASSERT_EQ(cli_files_markdown(&model, directory,
		"[source](src/main.c:23) [again](./src/main.c) ![image](output.png)\n"
		"[web](https://example.com)\n```\n[not a link](ignored.txt)\n```"), 0);
	ASSERT_EQ(model.entries.len, 2u);
	auto *first = static_cast<cli_files_entry *>(nb_vec_at(&model.entries, 0));
	EXPECT_EQ(first->line, 23);
	EXPECT_EQ(first->kind, CLI_FILES_REFERENCED);

	struct cli_transcript_tool tool{};
	tool.name = const_cast<char *>("apply_patch");
	tool.summary = const_cast<char *>("completed");
	tool.state = 1;
	tool.result = const_cast<char *>(R"({"ok":true,"data":{"files":[
		{"path":"src/main.c","action":"update"},
		{"path":"removed.c","action":"delete"},
		{"path":"new.c","action":"add"}]},
		"artifacts":[{"path":"generated.png","kind":"image"}]})");
	ASSERT_EQ(cli_files_tool(&model, directory, &tool), 0);
	ASSERT_EQ(model.entries.len, 4u);
	first = static_cast<cli_files_entry *>(nb_vec_at(&model.entries, 0));
	EXPECT_EQ(first->kind, CLI_FILES_MODIFIED);
	EXPECT_EQ(static_cast<cli_files_entry *>(nb_vec_at(&model.entries, 3))->kind,
		CLI_FILES_GENERATED);
	tool.state = -1;
	tool.result = const_cast<char *>(R"({"data":{"files":[{"path":"failed.c"}]}})");
	ASSERT_EQ(cli_files_tool(&model, directory, &tool), 0);
	EXPECT_EQ(model.entries.len, 4u);
}

TEST_F(CliFilesTest, PreviewHandlesTextMarkdownMissingAndBinaryFiles)
{
	const std::string text = std::string(directory) + "/notes.md";
	std::ofstream(text) << "# Heading\n\nA document with 中文 text.\n";
	auto *preview = Read(text);
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, 0);
	EXPECT_TRUE(preview->markdown);
	EXPECT_NE(std::string(nb_buf_cstr(&preview->content)).find("中文"), std::string::npos);
	cli_file_preview_free(preview);

	const std::string binary = std::string(directory) + "/binary.dat";
	std::ofstream(binary, std::ios::binary).write("a\0b", 3);
	preview = Read(binary);
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, NB_ERR_FORMAT);
	EXPECT_EQ(preview->content.len, 0u);
	cli_file_preview_free(preview);

	preview = Read(std::string(directory) + "/missing");
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, NB_ERR_NOT_FOUND);
	cli_file_preview_free(preview);
}

TEST_F(CliFilesTest, SpecialFilesDoNotBlockAndLargeTextIsBounded)
{
	const std::string fifo = std::string(directory) + "/fifo";
	ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
	auto *preview = Read(fifo);
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, NB_ERR_UNSUPPORTED);
	cli_file_preview_free(preview);

	const std::string large = std::string(directory) + "/large.txt";
	std::ofstream(large) << std::string(1024 * 1024 - 1, 'a') << "中文";
	preview = Read(large);
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, 0);
	EXPECT_TRUE(preview->truncated);
	EXPECT_LE(preview->content.len, 1024u * 1024u);
	cli_file_preview_free(preview);
}

TEST_F(CliFilesTest, PreviewJobOwnersArePreservedAndCancellationReapsWorkers)
{
	const std::string file = std::string(directory) + "/file.txt";
	std::ofstream(file) << "content";
	struct cli_file_preview_job *job = nullptr;
	ASSERT_EQ(cli_file_preview_start(file.c_str(), &job), 0);
	auto *original = job;
	EXPECT_EQ(cli_file_preview_start(file.c_str(), &job), NB_ERR_EXISTS);
	EXPECT_EQ(job, original);
	cli_file_preview_stop(&job);
	EXPECT_EQ(job, nullptr);
}

TEST_F(CliFilesTest, MarkdownHitRangesFollowWrappedDestinationsAndDistinctOccurrences)
{
	nb_vec links{};
	const char *display = "Before src/demo.c:80, inspect Source (src/\n  demo.c:80).";
	ASSERT_EQ(cli_messages_markdown_links(&links, display,
		"Before src/demo.c:80, inspect [Source](src/demo.c:80)."), 0);
	ASSERT_EQ(links.len, 2u);
	auto *target = static_cast<cli_messages_link *>(nb_vec_at(&links, 0));
	EXPECT_EQ(std::string(target->url), "src/demo.c:80");
	EXPECT_EQ(std::string(display + target->start, target->end - target->start),
		"src/\n  demo.c:80");
	cli_messages_links_cleanup(&links);
	ASSERT_EQ(cli_messages_markdown_links(&links, "A (same.md), B (same.md).",
		"[A](same.md), [B](same.md)."), 0);
	EXPECT_EQ(links.len, 4u);
	cli_messages_links_cleanup(&links);
}
