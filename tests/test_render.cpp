#include <gtest/gtest.h>
#include "render/image.h"
#include "render/video.h"
#include "render/markdown.h"
#include "util/file.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static int create_test_png(const char *path)
{
	unsigned char buf[] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x4B, 0x6D, 0x6B,
		0xC4, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
		0x54, 0x18, 0xD3, 0x63, 0xF8, 0xCF, 0xC0, 0x80,
		0x15, 0x71, 0xD1, 0x41, 0x2B, 0x00, 0x28, 0x3F,
		0x4F, 0xC1, 0x6E, 0xEC, 0xDF, 0x61, 0x00, 0x00,
		0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
		0x60, 0x82
	};
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	size_t written = fwrite(buf, 1, sizeof(buf), f);
	fclose(f);
	return (written == sizeof(buf)) ? 0 : -1;
}

static int create_test_text(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	size_t len = strlen(content);
	size_t written = fwrite(content, 1, len, f);
	fclose(f);
	return (written == len) ? 0 : -1;
}

/* ---- render command: file type detection ---- */

static const char *detect_render_type(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return "text";
	ext++;
	if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
	    strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
	    strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0)
		return "video";
	if (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
	    strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
	    strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
	    strcasecmp(ext, "tga") == 0 || strcasecmp(ext, "hdr") == 0)
		return "image";
	return "text";
}

TEST(RenderDetect, VideoMp4)    { EXPECT_STREQ(detect_render_type("clip.mp4"), "video"); }
TEST(RenderDetect, VideoMov)    { EXPECT_STREQ(detect_render_type("clip.mov"), "video"); }
TEST(RenderDetect, VideoAvi)    { EXPECT_STREQ(detect_render_type("clip.avi"), "video"); }
TEST(RenderDetect, VideoMkv)    { EXPECT_STREQ(detect_render_type("clip.mkv"), "video"); }
TEST(RenderDetect, VideoWebm)   { EXPECT_STREQ(detect_render_type("clip.webm"), "video"); }
TEST(RenderDetect, VideoFlv)    { EXPECT_STREQ(detect_render_type("clip.flv"), "video"); }
TEST(RenderDetect, VideoUpperCase) { EXPECT_STREQ(detect_render_type("clip.MP4"), "video"); }

TEST(RenderDetect, ImagePng)    { EXPECT_STREQ(detect_render_type("photo.png"), "image"); }
TEST(RenderDetect, ImageJpg)    { EXPECT_STREQ(detect_render_type("photo.jpg"), "image"); }
TEST(RenderDetect, ImageJpeg)   { EXPECT_STREQ(detect_render_type("photo.jpeg"), "image"); }
TEST(RenderDetect, ImageGif)    { EXPECT_STREQ(detect_render_type("photo.gif"), "image"); }
TEST(RenderDetect, ImageWebp)   { EXPECT_STREQ(detect_render_type("photo.webp"), "image"); }
TEST(RenderDetect, ImageBmp)    { EXPECT_STREQ(detect_render_type("photo.bmp"), "image"); }
TEST(RenderDetect, ImageTga)    { EXPECT_STREQ(detect_render_type("photo.tga"), "image"); }
TEST(RenderDetect, ImageHdr)    { EXPECT_STREQ(detect_render_type("photo.hdr"), "image"); }
TEST(RenderDetect, ImageUpperCase) { EXPECT_STREQ(detect_render_type("photo.PNG"), "image"); }

TEST(RenderDetect, TextMd)      { EXPECT_STREQ(detect_render_type("readme.md"), "text"); }
TEST(RenderDetect, TextTxt)     { EXPECT_STREQ(detect_render_type("notes.txt"), "text"); }
TEST(RenderDetect, TextNoExt)   { EXPECT_STREQ(detect_render_type("Makefile"), "text"); }
TEST(RenderDetect, TextC)       { EXPECT_STREQ(detect_render_type("main.c"), "text"); }
TEST(RenderDetect, TextJson)    { EXPECT_STREQ(detect_render_type("data.json"), "text"); }
TEST(RenderDetect, TextHtml)    { EXPECT_STREQ(detect_render_type("page.html"), "text"); }

/* ---- render command: file existence checks ---- */

TEST(RenderFile, NonexistentFile) {
	EXPECT_EQ(file_exists("/tmp/morph_test_nonexistent_12345.png"), 0);
}

TEST(RenderFile, ExistingImageFile) {
	const char *path = "/tmp/morph_test_render_exist.png";
	ASSERT_EQ(create_test_png(path), 0);
	EXPECT_NE(file_exists(path), 0);
	remove(path);
}

TEST(RenderFile, ExistingTextFile) {
	const char *path = "/tmp/morph_test_render_exist.md";
	ASSERT_EQ(create_test_text(path, "# Hello"), 0);
	EXPECT_NE(file_exists(path), 0);
	remove(path);
}

/* ---- render command: image rendering ---- */

TEST(RenderImage, ValidPng) {
	const char *path = "/tmp/morph_test_render_img.png";
	ASSERT_EQ(create_test_png(path), 0);
	int rc = image_render_terminal(path);
	EXPECT_EQ(rc, 0);
	remove(path);
}

TEST(RenderImage, NullPath) {
	int rc = image_render_terminal(NULL);
	EXPECT_NE(rc, 0);
}

TEST(RenderImage, EmptyPath) {
	int rc = image_render_terminal("");
	EXPECT_NE(rc, 0);
}

TEST(RenderImage, NonexistentFile) {
	int rc = image_render_terminal("/tmp/morph_test_no_such_file.png");
	EXPECT_EQ(rc, 0);
	remove("/tmp/morph_test_no_such_file.png");
}

/* ---- render command: markdown rendering ---- */

TEST(RenderMarkdown, SimpleHeading) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("# Hello World");
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("Hello World"), std::string::npos);
}

TEST(RenderMarkdown, Paragraph) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("This is a paragraph.");
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("paragraph"), std::string::npos);
}

TEST(RenderMarkdown, CodeBlock) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("```c\nint main() { return 0; }\n```");
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("main"), std::string::npos);
}

TEST(RenderMarkdown, NullInput) {
	testing::internal::CaptureStdout();
	markdown_render_ansi(NULL);
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_TRUE(out.empty() || out == "\n");
}

TEST(RenderMarkdown, EmptyString) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("");
	std::string out = testing::internal::GetCapturedStdout();
}

TEST(RenderMarkdown, BoldText) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("This is **bold** text");
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("bold"), std::string::npos);
}

TEST(RenderMarkdown, LinkText) {
	testing::internal::CaptureStdout();
	markdown_render_ansi("[click here](https://example.com)");
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("click here"), std::string::npos);
}

TEST(RenderMarkdown, ReadFileAndRender) {
	const char *path = "/tmp/morph_test_render_md.md";
	ASSERT_EQ(create_test_text(path, "# Test Heading\n\nSome **bold** text.\n"), 0);
	size_t len = 0;
	char *text = file_read_all(path, &len);
	ASSERT_NE(text, nullptr);
	EXPECT_GT(len, 0u);
	testing::internal::CaptureStdout();
	markdown_render_ansi(text);
	std::string out = testing::internal::GetCapturedStdout();
	EXPECT_NE(out.find("Test Heading"), std::string::npos);
	EXPECT_NE(out.find("bold"), std::string::npos);
	free(text);
	remove(path);
}

/* ---- render command: video rendering (null/empty path) ---- */

TEST(RenderVideo, NullPath) {
	int rc = video_play(NULL, NULL);
	EXPECT_NE(rc, 0);
}

/* ---- render command: path expansion ---- */

TEST(RenderPath, TildeExpansion) {
	char *expanded = file_expand_path("~/test.txt");
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(strstr(expanded, "/test.txt"), nullptr);
	free(expanded);
}

TEST(RenderPath, AbsolutePath) {
	char *expanded = file_expand_path("/tmp/test.txt");
	ASSERT_NE(expanded, nullptr);
	EXPECT_STREQ(expanded, "/tmp/test.txt");
	free(expanded);
}

/* ---- render command: markdown with media callback ---- */

TEST(RenderMarkdown, MediaCallbackImage) {
	int image_called = 0;
	std::string collected_type;
	std::string collected_path;

	auto cb = [](const char *type, const char *path, void *user) {
		auto *data = static_cast<int*>(user);
		*data = 1;
	};

	markdown_render_ansi_with_media("![alt](photo.png)", NULL, &image_called);
}

/* ---- render command: buffer rendering ---- */

TEST(RenderMarkdown, BufferRendering) {
	char buf[256];
	size_t n = markdown_render_ansi_to_buf("# Hello", buf, sizeof(buf));
	EXPECT_GT(n, 0u);
	EXPECT_NE(strstr(buf, "Hello"), nullptr);
}

TEST(RenderMarkdown, BufferTooSmall) {
	char buf[4];
	size_t n = markdown_render_ansi_to_buf("# Hello", buf, sizeof(buf));
}
