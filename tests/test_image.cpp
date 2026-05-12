#include <gtest/gtest.h>
#include "models/image_gen.h"
#include "agent/tools/img_gen.h"
#include "agent/tool.h"
#include "render/image.h"
#include <string.h>

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