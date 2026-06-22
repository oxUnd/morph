#include <gtest/gtest.h>
#include "models/image_gen.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_inpaint.h"
#include "agent/tools/img_compose.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/img_convert.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "render/image.h"
#include "util/file.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static int create_test_png(const char *path) {
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

TEST(ImageGen, InvalidPrompt) {
	struct image_result result;
	int rc = image_gen_create(NULL, NULL, NULL, NULL, NULL, NULL, &result);
	EXPECT_NE(rc, 0);
}

TEST(ImageGen, NullResult) {
	int rc = image_gen_create(NULL, "test", NULL, NULL, NULL, NULL, NULL);
	EXPECT_NE(rc, 0);
}

TEST(ImageGen, ValidateSizeAllowsAliasesAndBounds) {
	EXPECT_EQ(image_gen_validate_size(NULL), 0);
	EXPECT_EQ(image_gen_validate_size("2k"), 0);
	EXPECT_EQ(image_gen_validate_size("3k"), 0);
	EXPECT_EQ(image_gen_validate_size("4k"), 0);
	EXPECT_EQ(image_gen_validate_size("2560x1440"), 0);
	EXPECT_EQ(image_gen_validate_size("2048x2048"), 0);
	EXPECT_EQ(image_gen_validate_size("4096x4096"), 0);
}

TEST(ImageGen, ValidateSizeRejectsOutsidePixelRange) {
	EXPECT_NE(image_gen_validate_size("1920x1080"), 0);
	EXPECT_NE(image_gen_validate_size("4097x4096"), 0);
	EXPECT_NE(image_gen_validate_size("4096x4097"), 0);
	EXPECT_NE(image_gen_validate_size("1024x1024"), 0);
}

TEST(ImageGen, ValidateSizeRejectsMalformedValues) {
	EXPECT_NE(image_gen_validate_size("2560X1440"), 0);
	EXPECT_NE(image_gen_validate_size("2560x1440px"), 0);
	EXPECT_NE(image_gen_validate_size("0x4096"), 0);
	EXPECT_NE(image_gen_validate_size("-1x4096"), 0);
	EXPECT_NE(image_gen_validate_size("5k"), 0);
}

TEST(ImageRender, NullPath) {
	int rc = image_render_terminal(NULL);
	EXPECT_NE(rc, 0);
}

TEST(ImageRender, EmptyPath) {
	int rc = image_render_terminal("");
	EXPECT_NE(rc, 0);
}

TEST(ImageRender, ValidFileFallback) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	int rc = image_render_terminal(path);
	EXPECT_EQ(rc, 0);
	remove(path);
}

TEST(ImageRender, KittyProtocol) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);
	int rc = image_render_terminal(path);
	EXPECT_EQ(rc, 0);
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageRender, Iterm2Protocol) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("ITERM_PROFILE", "Default", 1);
	int rc = image_render_terminal(path);
	EXPECT_EQ(rc, 0);
	unsetenv("ITERM_PROFILE");
	remove(path);
}

TEST(ImageRender, NonexistentFile) {
	const char *path = "/tmp/nonexistent_test_img.png";
	remove(path);
	image_render_terminal(path);
	SUCCEED();  /* fallback to printing path is acceptable */
}

class ImgGenToolTest : public ::testing::Test {
protected:
	struct tool_registry reg;
	void SetUp() override {
		tool_registry_init(&reg);
	}
	void TearDown() override {
		tool_registry_cleanup(&reg);
	}
};

TEST_F(ImgGenToolTest, RegisterTool) {
	int rc = img_gen_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "img_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_gen");
}

TEST_F(ImgGenToolTest, ExecMissingPrompt) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL ||
		    strstr(result.text.data, "missing") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ToolNotFound) {
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "nonexistent", "{}", &result);
	EXPECT_NE(rc, 0);
}

TEST_F(ImgGenToolTest, ExecInvalidSize) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"a cat\",\"size\":\"1920x1080\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "invalid size"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InpaintRegister) {
	int rc = img_inpaint_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_inpaint");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_inpaint");
}

TEST_F(ImgGenToolTest, InpaintExecBadJson) {
	img_inpaint_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_inpaint", "not json", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ComposeRegister) {
	int rc = img_compose_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_compose");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_compose");
}

TEST_F(ImgGenToolTest, ComposeExecBadJson) {
	img_compose_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_compose", "not json", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoRegister) {
	int rc = img_info_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_info");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_info");
}

TEST_F(ImgGenToolTest, InfoMissingPath) {
	img_info_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoInvalidFile) {
	img_info_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info", "{\"file_path\":\"/nonexistent.png\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoDeniesSymlinkEscape) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	const char *link_path = "/tmp/morph_img_work/link.png";
	file_ensure_dir(work);
	ASSERT_EQ(create_test_png(outside), 0);
	std::remove(link_path);
	ASSERT_EQ(symlink(outside, link_path), 0);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_info_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info",
		"{\"file_path\":\"link.png\"}", &result);
	EXPECT_EQ(rc, -EPERM);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(link_path);
	std::remove(outside);
	rmdir(work);
}

TEST_F(ImgGenToolTest, ResizeRegister) {
	int rc = img_resize_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_resize");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_resize");
}

TEST_F(ImgGenToolTest, ResizeMissingArgs) {
	img_resize_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ResizeInvalidFile) {
	img_resize_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize",
		"{\"file_path\":\"/nonexistent.png\",\"width\":10,\"height\":10}",
		&result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ResizeDeniesOutsideInput) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	file_ensure_dir(work);
	ASSERT_EQ(create_test_png(outside), 0);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_resize_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize",
		"{\"file_path\":\"/tmp/morph_img_outside.png\","
		"\"width\":4,\"height\":4}",
		&result);
	EXPECT_EQ(rc, -EPERM);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(outside);
	rmdir(work);
}

TEST_F(ImgGenToolTest, ConvertRegister) {
	int rc = img_convert_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_convert");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_convert");
}

TEST_F(ImgGenToolTest, ConvertMissingArgs) {
	img_convert_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ConvertUnsupportedFormat) {
	img_convert_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert",
		"{\"file_path\":\"/tmp/x.png\",\"format\":\"avif\"}",
		&result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ConvertDeniesOutsideInputBeforeOutput) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	file_ensure_dir(work);
	ASSERT_EQ(create_test_png(outside), 0);
	struct tool_context *tctx = tool_context_create(work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_convert_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert",
		"{\"file_path\":\"/tmp/morph_img_outside.png\","
		"\"format\":\"jpg\"}",
		&result);
	EXPECT_EQ(rc, -EPERM);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_TRUE(strstr(result.text.data, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(outside);
	rmdir(work);
}

TEST(ImageDetectFmt, Png) {
	unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00};
	EXPECT_EQ(image_detect_fmt(png, sizeof(png)), 100);
}

TEST(ImageDetectFmt, Jpeg) {
	unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
	EXPECT_EQ(image_detect_fmt(jpg, sizeof(jpg)), 101);
}

TEST(ImageDetectFmt, Unknown) {
	unsigned char raw[] = {0x00, 0x01, 0x02, 0x03};
	EXPECT_EQ(image_detect_fmt(raw, sizeof(raw)), 0);
}

TEST(ImageDetectFmt, ShortBuffer) {
	unsigned char buf[] = {0x89};
	EXPECT_EQ(image_detect_fmt(buf, 1), 0);
}

static int create_test_jpeg(const char *path) {
	unsigned char buf[] = {
		0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46,
		0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
		0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
		0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
		0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C,
		0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
		0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D,
		0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20,
		0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
		0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27,
		0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34,
		0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
		0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4,
		0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
		0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xDA,
		0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00,
		0x7B, 0x94, 0x11, 0xCD, 0xA5, 0xFF, 0xD9
	};
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	size_t written = fwrite(buf, 1, sizeof(buf), f);
	fclose(f);
	return (written == sizeof(buf)) ? 0 : -1;
}

TEST(ImageRender, KittyJpegProtocol) {
	const char *path = "/tmp/test_kitty_jpeg.jpg";
	ASSERT_EQ(create_test_jpeg(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);

	char captured[4096] = {0};
	fflush(stdout);
	int pipefd[2];
	ASSERT_EQ(pipe(pipefd), 0);
	int old_stdout = dup(STDOUT_FILENO);
	ASSERT_NE(old_stdout, -1);
	ASSERT_EQ(dup2(pipefd[1], STDOUT_FILENO), STDOUT_FILENO);
	close(pipefd[1]);

	int rc = image_render_terminal(path);

	fflush(stdout);
	dup2(old_stdout, STDOUT_FILENO);
	close(old_stdout);
	ssize_t n = read(pipefd[0], captured, sizeof(captured) - 1);
	close(pipefd[0]);
	if (n > 0) captured[n] = '\0';

	EXPECT_EQ(rc, 0);
	/*
	 * Kitty's graphics protocol only supports f=100 (PNG), f=24 (RGB),
	 * and f=32 (RGBA). JPEG input must be transcoded to PNG before
	 * transmission, so we expect f=100 in the output stream.
	 */
	EXPECT_NE(strstr(captured, "f=100"), nullptr)
		<< "JPEG input must be transcoded to PNG (f=100) for kitty, got: " << captured;
	EXPECT_EQ(strstr(captured, "f=101"), nullptr)
		<< "f=101 is not a valid kitty graphics format, got: " << captured;
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageDetectFmt, WebpMagicBytes) {
	unsigned char webp[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};
	EXPECT_EQ(image_detect_fmt(webp, sizeof(webp)), 0)
		<< "WebP not natively detected by image_detect_fmt";
}

TEST(ImageDetectFmt, GifMagicBytes) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x00};
	EXPECT_EQ(image_detect_fmt(gif, sizeof(gif)), 0)
		<< "GIF not natively detected by image_detect_fmt";
}

TEST(ImageDetectFmt, NullInput) {
	EXPECT_EQ(image_detect_fmt(NULL, 0), 0);
}

TEST(ImageRender, KittyPngProtocol) {
	const char *path = "/tmp/test_kitty_png.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);

	char captured[4096] = {0};
	fflush(stdout);
	int pipefd[2];
	ASSERT_EQ(pipe(pipefd), 0);
	int old_stdout = dup(STDOUT_FILENO);
	ASSERT_NE(old_stdout, -1);
	ASSERT_EQ(dup2(pipefd[1], STDOUT_FILENO), STDOUT_FILENO);
	close(pipefd[1]);

	int rc = image_render_terminal(path);

	fflush(stdout);
	dup2(old_stdout, STDOUT_FILENO);
	close(old_stdout);
	ssize_t n = read(pipefd[0], captured, sizeof(captured) - 1);
	close(pipefd[0]);
	if (n > 0) captured[n] = '\0';

	EXPECT_EQ(rc, 0);
	EXPECT_NE(strstr(captured, "f=100"), nullptr)
		<< "PNG data should use f=100 in kitty protocol, got: " << captured;
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageGenExtContentType, Png) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "png");
}

TEST(ImageGenExtContentType, Jpeg) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "jpg");
}

TEST(ImageGenExtContentType, Webp) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "webp");
}

TEST(ImageGenExtContentType, Gif) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "gif");
}

TEST(ImageGenExtContentType, Bmp) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "bmp");
}

TEST(ImageGenExtContentType, CaseInsensitive) {
	const char *headers = "HTTP/1.1 200 OK\r\nCONTENT-TYPE: IMAGE/JPEG\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "jpg");
}

TEST(ImageGenExtContentType, WithCharset) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png; charset=utf-8\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "png");
}

TEST(ImageGenExtContentType, UnknownType) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
	EXPECT_EQ(image_gen_ext_from_content_type(headers), nullptr);
}

TEST(ImageGenExtContentType, NullHeaders) {
	EXPECT_EQ(image_gen_ext_from_content_type(NULL), nullptr);
}

TEST(ImageGenExtContentType, EmptyHeaders) {
	EXPECT_EQ(image_gen_ext_from_content_type(""), nullptr);
}

TEST(ImageGenExtMagic, Png) {
	unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	EXPECT_STREQ(image_gen_ext_from_magic(png, sizeof(png)), "png");
}

TEST(ImageGenExtMagic, Jpeg) {
	unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
	EXPECT_STREQ(image_gen_ext_from_magic(jpg, sizeof(jpg)), "jpg");
}

TEST(ImageGenExtMagic, Webp) {
	unsigned char webp[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};
	EXPECT_STREQ(image_gen_ext_from_magic(webp, sizeof(webp)), "webp");
}

TEST(ImageGenExtMagic, Gif87a) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x37, 0x61};
	EXPECT_STREQ(image_gen_ext_from_magic(gif, sizeof(gif)), "gif");
}

TEST(ImageGenExtMagic, Gif89a) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61};
	EXPECT_STREQ(image_gen_ext_from_magic(gif, sizeof(gif)), "gif");
}

TEST(ImageGenExtMagic, Bmp) {
	unsigned char bmp[] = {0x42, 0x4D, 0x00, 0x00};
	EXPECT_STREQ(image_gen_ext_from_magic(bmp, sizeof(bmp)), "bmp");
}

TEST(ImageGenExtMagic, UnknownDefaultsToPng) {
	unsigned char unknown[] = {0x00, 0x01, 0x02, 0x03};
	EXPECT_STREQ(image_gen_ext_from_magic(unknown, sizeof(unknown)), "png");
}

TEST(ImageGenExtMagic, NullData) {
	EXPECT_STREQ(image_gen_ext_from_magic(NULL, 0), "png");
}

TEST(ImageGenExtMagic, ShortBuffer) {
	unsigned char buf[] = {0x89};
	EXPECT_STREQ(image_gen_ext_from_magic(buf, 1), "png");
}
