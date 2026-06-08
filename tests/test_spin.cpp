#include <gtest/gtest.h>
#include "util/spin.h"
#include "util/utf8.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

TEST(SpinInit, InitDestroy)
{
	struct spin_context ctx;
	spin_init(&ctx, NULL);
	EXPECT_EQ(ctx.running, 0);
	EXPECT_EQ(ctx.active, 0);
	spin_destroy(&ctx);
}

TEST(SpinInit, InitWithDevNull)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	EXPECT_EQ(ctx.running, 0);
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinInit, NullContext)
{
	EXPECT_NO_FATAL_FAILURE(spin_init(NULL, NULL));
	EXPECT_NO_FATAL_FAILURE(spin_destroy(NULL));
}

TEST(SpinLifecycle, StartStop)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	spin_start(&ctx, SPIN_STATE_THINKING, "Test");
	EXPECT_EQ(ctx.running, 1);
	spin_stop(&ctx, SPIN_STATE_COMPLETE, "Done");
	EXPECT_EQ(ctx.running, 0);
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinLifecycle, StartStopNoMessage)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	spin_start(&ctx, SPIN_STATE_EXECUTING, NULL);
	EXPECT_EQ(ctx.running, 1);
	spin_stop(&ctx, SPIN_STATE_COMPLETE, NULL);
	EXPECT_EQ(ctx.running, 0);
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinSetSub, AsciiSubmessage)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	spin_set_sub(&ctx, "hello world");
	EXPECT_STREQ(ctx.submessage, "hello world");
	spin_set_sub(&ctx, NULL);
	EXPECT_EQ(ctx.submessage[0], '\0');
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinSetSub, Utf8TruncationSafe)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);

	std::string long_msg;
	for (int i = 0; i < 200; i++)
		long_msg += "\xe4\xb8\xad";
	spin_set_sub(&ctx, long_msg.c_str());

	size_t len = strlen(ctx.submessage);
	EXPECT_LT(len, sizeof(ctx.submessage));
	EXPECT_GT(len, 0u);

	const char *p = ctx.submessage + len;
	while (p > ctx.submessage && (*p & 0xC0) == 0x80)
		p--;
	if (p > ctx.submessage) {
		unsigned char c = (unsigned char)*p;
		bool valid_start = (c & 0xC0) != 0x80;
		EXPECT_TRUE(valid_start);
	}

	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinSetSub, Utf8MixedContent)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);

	spin_set_sub(&ctx, "Hello \xe4\xb8\x96\xe7\x95\x8c test");
	EXPECT_STREQ(ctx.submessage, "Hello \xe4\xb8\x96\xe7\x95\x8c test");

	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinSetSub, InvalidUtf8IsSanitized)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);

	const char invalid[] = "abc\xe4\xb8""def\xff""ghi";
	spin_set_sub(&ctx, invalid);
	EXPECT_EQ(utf8valid(ctx.submessage), nullptr);
	EXPECT_STREQ(ctx.submessage, "abcdefghi");

	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinUpdate, InvalidUtf8IsSanitized)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);

	spin_start(&ctx, SPIN_STATE_THINKING, "Initial");
	spin_update(&ctx, "hello\xe4\xb8""world");
	EXPECT_EQ(utf8valid(ctx.message), nullptr);
	EXPECT_STREQ(ctx.message, "helloworld");
	spin_stop(&ctx, SPIN_STATE_COMPLETE, "Done");
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinRender, ChineseMarqueeFramesAreValidUtf8)
{
	char *buf = nullptr;
	size_t len = 0;
	FILE *mem = open_memstream(&buf, &len);
	ASSERT_NE(mem, nullptr);

	struct spin_context ctx;
	spin_init(&ctx, mem);
	ctx.running = 1;
	ctx.active = 1;
	ctx.state = SPIN_STATE_THINKING;
	ctx.start_time = time(NULL);
	spin_update(&ctx, "执行工具");
	spin_set_sub(&ctx,
		     "正在读取中文文件内容并进行摘要处理，"
		     "这是一段很长的中英混排 marquee text");

	for (int i = 0; i < 80; i++) {
		ctx.frame = i;
		spin_render(&ctx);
		fflush(mem);
		ASSERT_EQ(utf8valid(buf), nullptr) << "invalid frame " << i;
	}

	ctx.running = 0;
	spin_destroy(&ctx);
	fclose(mem);
	free(buf);
}

TEST(Utf8SkipColumns, DoesNotSplitWideCharacters)
{
	const char *s = "A\xe4\xb8\xad""B";

	EXPECT_EQ(utf8_skip_columns(s, 0), s);
	EXPECT_EQ(utf8_skip_columns(s, 1), s + 1);
	EXPECT_EQ(utf8_skip_columns(s, 2), s + 1);
	EXPECT_EQ(utf8_skip_columns(s, 3), s + 4);
	EXPECT_EQ(utf8_skip_columns(s, 4), s + 5);
}

TEST(SpinSetSub, ExactBufferSize)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);

	char exact[512];
	memset(exact, 'A', sizeof(ctx.submessage) - 1);
	exact[sizeof(ctx.submessage) - 1] = '\0';
	spin_set_sub(&ctx, exact);
	EXPECT_STREQ(ctx.submessage, exact);

	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinSetSub, NullContext)
{
	EXPECT_NO_FATAL_FAILURE(spin_set_sub(NULL, "test"));
}

TEST(SpinUpdate, UpdateMessage)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	spin_start(&ctx, SPIN_STATE_THINKING, "Initial");
	spin_update(&ctx, "Updated");
	EXPECT_STREQ(ctx.message, "Updated");
	spin_stop(&ctx, SPIN_STATE_COMPLETE, "Done");
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinUpdate, NullContext)
{
	EXPECT_NO_FATAL_FAILURE(spin_update(NULL, "test"));
}

TEST(SpinStop, StopNotRunning)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	EXPECT_NO_FATAL_FAILURE(spin_stop(&ctx, SPIN_STATE_COMPLETE, "Done"));
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinClear, ClearNoCrash)
{
	FILE *devnull = fopen("/dev/null", "w");
	ASSERT_NE(devnull, nullptr);
	struct spin_context ctx;
	spin_init(&ctx, devnull);
	EXPECT_NO_FATAL_FAILURE(spin_clear(&ctx));
	spin_destroy(&ctx);
	fclose(devnull);
}

TEST(SpinCancelFlag, SetCancelFlag)
{
	volatile sig_atomic_t flag = 0;
	struct spin_context ctx;
	spin_init(&ctx, NULL);
	spin_set_cancel_flag(&ctx, &flag);
	EXPECT_EQ(ctx.cancel_flag, &flag);
	spin_set_cancel_flag(&ctx, NULL);
	EXPECT_EQ(ctx.cancel_flag, nullptr);
	spin_destroy(&ctx);
}

TEST(SpinCancelFlag, NullContext)
{
	volatile sig_atomic_t flag = 0;
	EXPECT_NO_FATAL_FAILURE(spin_set_cancel_flag(NULL, &flag));
}
