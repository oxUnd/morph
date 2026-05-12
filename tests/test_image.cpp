#include <gtest/gtest.h>
#include "models/image_gen.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_edit.h"
#include "agent/tools/img_info.h"
#include "agent/tool.h"
#include "render/image.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
	int rc = image_gen_create(NULL, NULL, NULL, NULL, &result);
	EXPECT_NE(rc, 0);
}

TEST(ImageGen, NullResult) {
	int rc = image_gen_create(NULL, "test", NULL, NULL, NULL);
	EXPECT_NE(rc, 0);
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
};

TEST_F(ImgGenToolTest, RegisterTool) {
	int rc = img_gen_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_lookup(&reg, "img_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_gen");
}

TEST_F(ImgGenToolTest, ExecMissingPrompt) {
	img_gen_init(&reg, NULL);
	char *result = NULL;
	int rc = tool_exec(&reg, "img_gen", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(strstr(result, "error") != NULL ||
		    strstr(result, "missing") != NULL);
	free(result);
}

TEST_F(ImgGenToolTest, ToolNotFound) {
	char *result = NULL;
	int rc = tool_exec(&reg, "nonexistent", "{}", &result);
	EXPECT_NE(rc, 0);
}

TEST_F(ImgGenToolTest, EditRegister) {
	int rc = img_edit_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_edit");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_edit");
}

TEST_F(ImgGenToolTest, EditMissingArgs) {
	img_edit_init(&reg, NULL);
	char *result = NULL;
	int rc = tool_exec(&reg, "img_edit", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result, nullptr);
	free(result);
}

TEST_F(ImgGenToolTest, InfoRegister) {
	int rc = img_info_init(&reg);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&reg, "img_info");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_info");
}

TEST_F(ImgGenToolTest, InfoMissingPath) {
	img_info_init(&reg);
	char *result = NULL;
	int rc = tool_exec(&reg, "img_info", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result, nullptr);
	free(result);
}

TEST_F(ImgGenToolTest, InfoInvalidFile) {
	img_info_init(&reg);
	char *result = NULL;
	int rc = tool_exec(&reg, "img_info", "{\"file_path\":\"/nonexistent.png\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result, nullptr);
	free(result);
}