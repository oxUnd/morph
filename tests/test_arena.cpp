#include <gtest/gtest.h>
#include "util/arena.h"

class ArenaTest : public ::testing::Test {
protected:
	struct arena *a;
	void SetUp() override { a = arena_create(1024); }
	void TearDown() override { arena_destroy(a); }
};

TEST_F(ArenaTest, CreateDestroy) {
	ASSERT_NE(a, nullptr);
}

TEST_F(ArenaTest, BasicAlloc) {
	void *p = arena_alloc(a, 64);
	ASSERT_NE(p, nullptr);
	memset(p, 0xAA, 64);
}

TEST_F(ArenaTest, MultipleAlloc) {
	void *p1 = arena_alloc(a, 128);
	void *p2 = arena_alloc(a, 256);
	void *p3 = arena_alloc(a, 64);
	ASSERT_NE(p1, nullptr);
	ASSERT_NE(p2, nullptr);
	ASSERT_NE(p3, nullptr);
	EXPECT_NE(p1, p2);
	EXPECT_NE(p2, p3);
}

TEST_F(ArenaTest, ZeroSizeAlloc) {
	void *p = arena_alloc(a, 0);
	EXPECT_EQ(p, nullptr);
}

TEST_F(ArenaTest, LargeAlloc) {
	void *p = arena_alloc(a, 2048);
	ASSERT_NE(p, nullptr);
}

TEST_F(ArenaTest, Strdup) {
	const char *s = "hello world";
	char *dup = arena_strdup(a, s);
	ASSERT_NE(dup, nullptr);
	EXPECT_STREQ(dup, s);
}

TEST_F(ArenaTest, StrdupNull) {
	char *dup = arena_strdup(a, nullptr);
	EXPECT_EQ(dup, nullptr);
}

TEST_F(ArenaTest, Reset) {
	void *p1 = arena_alloc(a, 64);
	ASSERT_NE(p1, nullptr);
	arena_reset(a);
	size_t used_after = a->used;
	void *p2 = arena_alloc(a, 32);
	ASSERT_NE(p2, nullptr);
	EXPECT_LT(used_after, (size_t)64);
}

TEST_F(ArenaTest, NullArena) {
	EXPECT_EQ(arena_alloc(nullptr, 64), nullptr);
	EXPECT_EQ(arena_strdup(nullptr, "test"), nullptr);
}

TEST_F(ArenaTest, AlignedAlloc) {
	void *p = arena_alloc_aligned(a, 64, 16);
	ASSERT_NE(p, nullptr);
	EXPECT_EQ((uintptr_t)p % 16, 0);
}

TEST_F(ArenaTest, ManySmallAllocs) {
	for (int i = 0; i < 100; i++) {
		void *p = arena_alloc(a, 8);
		ASSERT_NE(p, nullptr);
	}
}

TEST_F(ArenaTest, CreateDefault) {
	struct arena *a2 = arena_create(0);
	ASSERT_NE(a2, nullptr);
	arena_destroy(a2);
}

static void cleanup_inc(void *data)
{
	int *value = static_cast<int *>(data);
	(*value)++;
}

TEST_F(ArenaTest, CleanupRunsOnReset) {
	int calls = 0;
	struct arena_cleanup *cleanup = arena_cleanup_add(a, 0);
	ASSERT_NE(cleanup, nullptr);
	cleanup->handler = cleanup_inc;
	cleanup->data = &calls;

	arena_reset(a);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(a->cleanup, nullptr);
}

TEST_F(ArenaTest, CleanupRunsOnDestroy) {
	struct arena *a2 = arena_create(1024);
	ASSERT_NE(a2, nullptr);
	int calls = 0;
	struct arena_cleanup *cleanup = arena_cleanup_add(a2, 0);
	ASSERT_NE(cleanup, nullptr);
	cleanup->handler = cleanup_inc;
	cleanup->data = &calls;

	arena_destroy(a2);
	EXPECT_EQ(calls, 1);
}

TEST_F(ArenaTest, CleanupDataAllocatedFromArena) {
	struct arena_cleanup *cleanup = arena_cleanup_add(a, sizeof(int));
	ASSERT_NE(cleanup, nullptr);
	ASSERT_NE(cleanup->data, nullptr);
	int *value = static_cast<int *>(cleanup->data);
	*value = 41;
	cleanup->handler = cleanup_inc;

	arena_cleanup_run(a);
	EXPECT_EQ(*value, 42);
	EXPECT_EQ(a->cleanup, nullptr);
}

TEST_F(ArenaTest, LargeAllocTrackedSeparately) {
	void *p = arena_alloc(a, 2048);
	ASSERT_NE(p, nullptr);
	ASSERT_NE(a->large, nullptr);
	arena_reset(a);
	EXPECT_EQ(a->large, nullptr);
	EXPECT_EQ(a->used, (size_t)0);
}
