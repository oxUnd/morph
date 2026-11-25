#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/models/files.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/services/file_preview.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/terminal_text.h"
#include <nb/err.h>
}

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

TEST_F(CliFilesTest, RecordedSessionPatchesSurviveReplayAndModelTransferWithoutReadingFiles)
{
	struct cli_transcript_tool tool{};
	tool.id = const_cast<char *>("patch-1");
	tool.name = const_cast<char *>("apply_patch");
	tool.state = 1;
	tool.historical = 1;
	tool.result = const_cast<char *>(R"({"ok":true,"data":{"files":[]},"ui":{"patch_diff":
		"*** Update File: never-existed.c\n*** Move to: renamed.c\n@@ -2,1 +2,1 @@\n-old\n+new\n*** Delete File: gone.c\n@@ -1,1 +0,0 @@\n-removed\n"}})");
	ASSERT_EQ(cli_files_tool(&model, directory, &tool), 0);
	ASSERT_EQ(model.changes.len, 2u);
	ASSERT_EQ(cli_files_tool(&model, directory, &tool), 0);
	EXPECT_EQ(model.changes.len, 2u);
	auto *first = static_cast<struct cli_files_change *>(nb_vec_at(&model.changes, 0));
	EXPECT_NE(std::string(first->patch).find("*** Move to: renamed.c"), std::string::npos);
	EXPECT_EQ(std::string(first->patch).find("*** Delete File"), std::string::npos);
	auto *second = static_cast<struct cli_files_change *>(nb_vec_at(&model.changes, 1));
	EXPECT_EQ(second->action, 'D');
	struct cli_files_model moved NB_CLEANUP(cli_files_cleanup) = {};
	cli_files_init(&moved);
	cli_files_move(&moved, &model);
	EXPECT_EQ(model.changes.len, 0u);
	EXPECT_EQ(model.patch_bytes, 0u);
	ASSERT_EQ(moved.changes.len, 2u);
	EXPECT_GT(moved.patch_bytes, 0u);
	tool.id = const_cast<char *>("failed-patch");
	tool.state = -1;
	ASSERT_EQ(cli_files_tool(&moved, directory, &tool), 0);
	EXPECT_EQ(moved.changes.len, 2u);
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

TEST_F(CliFilesTest, SourcePreviewsCacheSanitizedHighlightingWithoutChangingContent)
{
	const std::string file = std::string(directory) + "/main.c";
	const std::string source = "\n\n/* 中文 comment\n * continued */\n"
		"\tint answer = 42;\n\n";
	const std::string untrusted = "\033[2J\033]8;;https://example.com\033\\" + source;
	std::ofstream(file) << untrusted;
	std::unique_ptr<cli_file_preview, decltype(&cli_file_preview_free)>
		preview(Read(file), cli_file_preview_free);
	ASSERT_NE(preview, nullptr);
	ASSERT_EQ(preview->status, 0);
	EXPECT_FALSE(preview->markdown);
	EXPECT_STREQ(nb_buf_cstr(&preview->content), untrusted.c_str());
	ASSERT_NE(preview->highlighted, nullptr);
	const std::string highlighted = preview->highlighted;
	EXPECT_NE(highlighted.find("\033[36mint"), std::string::npos);
	EXPECT_NE(highlighted.find("\n\033[2;36m * continued"), std::string::npos);
	EXPECT_EQ(highlighted.find("\033[2J"), std::string::npos);
	EXPECT_EQ(highlighted.find("\033]8;"), std::string::npos);
	char *plain = cli_terminal_text_strip_ansi_dup(highlighted.data(),
		highlighted.size(), nullptr);
	ASSERT_NE(plain, nullptr);
	EXPECT_STREQ(plain, source.c_str());
	free(plain);
}

TEST_F(CliFilesTest, MarkdownAndUnknownFilesKeepTheirExistingPreviewPaths)
{
	const std::string source = "#!/usr/bin/env python3\n\n# Heading\n\n"
		"**bold** [link](main.c:3)\n\n```c\nint value = 42;\n```\n";
	for (const char *name : {"notes.md", "notes.MD", "notes.markdown", "notes.MARKDOWN"}) {
		const std::string file = std::string(directory) + "/" + name;
		std::ofstream(file) << source;
		std::unique_ptr<cli_file_preview, decltype(&cli_file_preview_free)>
			preview(Read(file), cli_file_preview_free);
		ASSERT_NE(preview, nullptr);
		ASSERT_EQ(preview->status, 0);
		EXPECT_TRUE(preview->markdown) << name;
		EXPECT_EQ(preview->highlighted, nullptr) << name;
		EXPECT_STREQ(nb_buf_cstr(&preview->content), source.c_str());
	}

	const std::string file = std::string(directory) + "/notes.txt";
	std::ofstream(file) << "\nplain notes\n\n";
	std::unique_ptr<cli_file_preview, decltype(&cli_file_preview_free)>
		preview(Read(file), cli_file_preview_free);
	ASSERT_NE(preview, nullptr);
	EXPECT_EQ(preview->status, 0);
	EXPECT_EQ(preview->highlighted, nullptr);
	EXPECT_STREQ(nb_buf_cstr(&preview->content), "\nplain notes\n\n");
}

TEST_F(CliFilesTest, ExtensionlessScriptsAndMakefilesAreHighlighted)
{
	for (const auto &entry : {
		std::pair{"script", "#!/usr/bin/env -S python3 -u\ndef answer(): return 42\n"},
		std::pair{"Makefile", "ifdef DEBUG\nCC = clang\nendif\n"}}) {
		const std::string file = std::string(directory) + "/" + entry.first;
		std::ofstream(file) << entry.second;
		std::unique_ptr<cli_file_preview, decltype(&cli_file_preview_free)>
			preview(Read(file), cli_file_preview_free);
		ASSERT_NE(preview, nullptr);
		ASSERT_EQ(preview->status, 0);
		ASSERT_NE(preview->highlighted, nullptr);
		EXPECT_NE(std::string(preview->highlighted).find("\033[1;33m"), std::string::npos);
	}
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
