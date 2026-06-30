#include <gtest/gtest.h>

#include "util/arena.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/id.h"
#include "util/queue.h"
#include "util/str.h"
#include "util/strmap.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static bool is_lower_hex_string(const char *s)
{
	if (!s)
		return false;
	for (const char *p = s; *p; p++) {
		if ((*p < '0' || *p > '9') && (*p < 'a' || *p > 'f'))
			return false;
	}
	return true;
}

TEST(UtilId, RandomIdUsesPrefixAndHex)
{
	char id[64];

	ASSERT_EQ(morph_random_id("turn_", id, sizeof(id)), 0);
	EXPECT_EQ(strncmp(id, "turn_", 5), 0);
	EXPECT_EQ(strlen(id), 37u);
	EXPECT_TRUE(is_lower_hex_string(id + 5));
}

TEST(UtilId, RejectsInvalidArgsAndSmallBuffer)
{
	char id[8];

	EXPECT_EQ(morph_random_id(nullptr, id, sizeof(id)), -EINVAL);
	EXPECT_EQ(morph_random_id("turn_", nullptr, 64), -EINVAL);
	EXPECT_EQ(morph_random_id("turn_", id, sizeof(id)), -ENOSPC);
}

TEST(UtilArray, PushGetPopAndClear)
{
	morph_array_t a;
	ASSERT_EQ(morph_array_init(&a, 2, sizeof(int)), 0);

	for (int i = 0; i < 10; i++) {
		int *slot = static_cast<int *>(morph_array_push(&a));
		ASSERT_NE(slot, nullptr);
		*slot = i * 3;
	}

	ASSERT_GE(a.cap, 10u);
	ASSERT_EQ(a.nelts, 10u);
	for (int i = 0; i < 10; i++) {
		int *slot = static_cast<int *>(morph_array_get(&a, (size_t)i));
		ASSERT_NE(slot, nullptr);
		EXPECT_EQ(*slot, i * 3);
	}
	EXPECT_EQ(morph_array_get(&a, 10), nullptr);

	morph_array_pop(&a);
	EXPECT_EQ(a.nelts, 9u);
	morph_array_clear(&a);
	EXPECT_EQ(a.nelts, 0u);

	morph_array_cleanup(&a);
	EXPECT_EQ(a.elts, nullptr);
}

TEST(UtilArray, PushNReturnsContiguousSlots)
{
	morph_array_t a;
	ASSERT_EQ(morph_array_init(&a, 1, sizeof(int)), 0);

	int *slots = static_cast<int *>(morph_array_push_n(&a, 4));
	ASSERT_NE(slots, nullptr);
	for (int i = 0; i < 4; i++)
		slots[i] = i + 10;

	EXPECT_EQ(a.nelts, 4u);
	EXPECT_EQ(*static_cast<int *>(morph_array_get(&a, 3)), 13);
	morph_array_cleanup(&a);
}

TEST(UtilArray, ArenaArrayCannotGrow)
{
	struct arena *arena = arena_create(128);
	ASSERT_NE(arena, nullptr);

	morph_array_t a;
	ASSERT_EQ(morph_array_init_arena(&a, arena, 1, sizeof(int)), 0);
	ASSERT_NE(morph_array_push(&a), nullptr);
	EXPECT_EQ(morph_array_push(&a), nullptr);

	morph_array_cleanup(&a);
	arena_destroy(arena);
}

TEST(UtilArray, RejectsOverflowCapacities)
{
	morph_array_t a;
	EXPECT_EQ(morph_array_init(&a, SIZE_MAX / sizeof(int) + 1,
	                          sizeof(int)), -EOVERFLOW);

	ASSERT_EQ(morph_array_init(&a, 1, sizeof(int)), 0);
	EXPECT_EQ(morph_array_reserve(&a, SIZE_MAX / sizeof(int) + 1),
	          -EOVERFLOW);
	a.nelts = SIZE_MAX - 1;
	EXPECT_EQ(morph_array_push_n(&a, 2), nullptr);
	morph_array_cleanup(&a);
}

TEST(UtilStrmap, SetGetUpdateAndContains)
{
	morph_strmap_t m;
	int one = 1;
	int two = 2;
	int three = 3;

	ASSERT_EQ(morph_strmap_init(&m, 2), 0);
	EXPECT_EQ(morph_strmap_len(&m), 0u);

	ASSERT_EQ(morph_strmap_set(&m, "alpha", &one), 0);
	ASSERT_EQ(morph_strmap_set(&m, "beta", &two), 0);
	EXPECT_EQ(morph_strmap_len(&m), 2u);
	EXPECT_EQ(morph_strmap_get(&m, "alpha"), &one);
	EXPECT_EQ(morph_strmap_get(&m, "beta"), &two);
	EXPECT_EQ(morph_strmap_get(&m, "missing"), nullptr);
	EXPECT_TRUE(morph_strmap_contains(&m, "alpha"));
	EXPECT_FALSE(morph_strmap_contains(&m, "missing"));

	ASSERT_EQ(morph_strmap_set(&m, "alpha", &three), 0);
	EXPECT_EQ(morph_strmap_len(&m), 2u);
	EXPECT_EQ(morph_strmap_get(&m, "alpha"), &three);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, OwnsCopiedKeys)
{
	morph_strmap_t m;
	int value = 7;
	char key[] = "mutable";

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set(&m, key, &value), 0);
	strcpy(key, "changed");

	EXPECT_EQ(morph_strmap_get(&m, "mutable"), &value);
	EXPECT_EQ(morph_strmap_get(&m, "changed"), nullptr);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, RemoveAndReuseDeletedSlots)
{
	morph_strmap_t m;
	int values[32];

	ASSERT_EQ(morph_strmap_init(&m, 4), 0);
	for (int i = 0; i < 16; i++) {
		char key[16];
		snprintf(key, sizeof(key), "k%d", i);
		values[i] = i;
		ASSERT_EQ(morph_strmap_set(&m, key, &values[i]), 0);
	}
	EXPECT_EQ(morph_strmap_len(&m), 16u);

	EXPECT_EQ(morph_strmap_remove(&m, "k3"), 1);
	EXPECT_EQ(morph_strmap_remove(&m, "k7"), 1);
	EXPECT_EQ(morph_strmap_remove(&m, "missing"), 0);
	EXPECT_FALSE(morph_strmap_contains(&m, "k3"));
	EXPECT_EQ(morph_strmap_len(&m), 14u);

	values[20] = 20;
	ASSERT_EQ(morph_strmap_set(&m, "new", &values[20]), 0);
	EXPECT_EQ(morph_strmap_get(&m, "new"), &values[20]);
	EXPECT_EQ(morph_strmap_len(&m), 15u);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, NullValueIsStillContained)
{
	morph_strmap_t m;

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set(&m, "nil", nullptr), 0);

	EXPECT_EQ(morph_strmap_get(&m, "nil"), nullptr);
	EXPECT_TRUE(morph_strmap_contains(&m, "nil"));

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, ClearKeepsMapReusable)
{
	morph_strmap_t m;
	int value = 42;

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set(&m, "a", &value), 0);
	ASSERT_EQ(morph_strmap_set(&m, "b", &value), 0);
	morph_strmap_clear(&m);

	EXPECT_EQ(morph_strmap_len(&m), 0u);
	EXPECT_FALSE(morph_strmap_contains(&m, "a"));
	ASSERT_EQ(morph_strmap_set(&m, "c", &value), 0);
	EXPECT_EQ(morph_strmap_get(&m, "c"), &value);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, RejectsInvalidArguments)
{
	morph_strmap_t m;
	int value = 1;

	EXPECT_EQ(morph_strmap_init(nullptr, 0), -EINVAL);
	ASSERT_EQ(morph_strmap_init(&m, 0), 0);

	EXPECT_EQ(morph_strmap_set(nullptr, "a", &value), -EINVAL);
	EXPECT_EQ(morph_strmap_set(&m, nullptr, &value), -EINVAL);
	EXPECT_EQ(morph_strmap_get(nullptr, "a"), nullptr);
	EXPECT_EQ(morph_strmap_get(&m, nullptr), nullptr);
	EXPECT_FALSE(morph_strmap_contains(nullptr, "a"));
	EXPECT_EQ(morph_strmap_remove(nullptr, "a"), 0);

	morph_strmap_cleanup(&m);
}

TEST(UtilBuf, AppendPrintfCstrAndTruncate)
{
	morph_buf_t b;
	ASSERT_EQ(morph_buf_init(&b, 4), 0);

	ASSERT_EQ(morph_buf_puts(&b, "abc"), 0);
	ASSERT_EQ(morph_buf_putc(&b, 'd'), 0);
	ASSERT_EQ(morph_buf_printf(&b, "-%d", 42), 0);
	EXPECT_STREQ(morph_buf_cstr(&b), "abcd-42");

	morph_str_t s = morph_buf_str(&b);
	EXPECT_EQ(s.len, 7u);
	EXPECT_EQ(memcmp(s.data, "abcd-42", s.len), 0);

	ASSERT_EQ(morph_buf_truncate(&b, 4), 0);
	EXPECT_STREQ(morph_buf_cstr(&b), "abcd");
	ASSERT_EQ(morph_buf_truncate(&b, 99), 0);
	EXPECT_EQ(b.len, 4u);

	char *detached = morph_buf_detach(&b);
	ASSERT_NE(detached, nullptr);
	EXPECT_STREQ(detached, "abcd");
	free(detached);
	EXPECT_EQ(b.data, nullptr);
}

TEST(UtilBuf, AppendBinaryAndReset)
{
	morph_buf_t b;
	const char bytes[] = {'a', '\0', 'b'};
	ASSERT_EQ(morph_buf_init(&b, 1), 0);

	ASSERT_EQ(morph_buf_append(&b, bytes, sizeof(bytes)), 0);
	EXPECT_EQ(b.len, sizeof(bytes));
	ASSERT_NE(morph_buf_cstr(&b), nullptr);
	EXPECT_EQ(memcmp(b.data, bytes, sizeof(bytes)), 0);

	morph_buf_reset(&b);
	EXPECT_EQ(b.len, 0u);
	EXPECT_STREQ(morph_buf_cstr(&b), "");
	morph_buf_cleanup(&b);
}

TEST(UtilBuf, ArenaBufferGrowsButDoesNotDetach)
{
	struct arena *arena = arena_create(64);
	ASSERT_NE(arena, nullptr);

	morph_buf_t b;
	ASSERT_EQ(morph_buf_init_arena(&b, arena, 2), 0);
	ASSERT_EQ(morph_buf_puts(&b, "abcdef"), 0);
	EXPECT_STREQ(morph_buf_cstr(&b), "abcdef");
	EXPECT_EQ(morph_buf_detach(&b), nullptr);

	morph_buf_cleanup(&b);
	arena_destroy(arena);
}

TEST(UtilBuf, RejectsTooLargeAndOverflow)
{
	morph_buf_t b;
	EXPECT_EQ(morph_buf_init(&b, MORPH_BUF_MAX_CAP + 1), -EFBIG);
	ASSERT_EQ(morph_buf_init(&b, 1), 0);

	b.len = SIZE_MAX - 1;
	EXPECT_EQ(morph_buf_append(&b, "x", 1), -EOVERFLOW);
	EXPECT_EQ(morph_buf_reserve(&b, 2), -EOVERFLOW);
	b.len = 0;
	morph_buf_cleanup(&b);
}

struct QueueNode {
	int value;
	int seq;
	struct morph_queue link;
};

static int queue_node_cmp(const struct morph_queue *a,
                          const struct morph_queue *b)
{
	const QueueNode *na = morph_queue_data(a, QueueNode, link);
	const QueueNode *nb = morph_queue_data(b, QueueNode, link);

	if (na->value == nb->value)
		return 0;
	return na->value < nb->value ? -1 : 1;
}

static std::vector<int> queue_values(struct morph_queue *head)
{
	std::vector<int> values;
	struct morph_queue *q;

	morph_queue_foreach(q, head) {
		QueueNode *node = morph_queue_data(q, QueueNode, link);
		values.push_back(node->value);
	}
	return values;
}

TEST(UtilQueue, InsertRemoveAndData)
{
	struct morph_queue head;
	QueueNode a = {1, 0, {nullptr, nullptr}};
	QueueNode b = {2, 1, {nullptr, nullptr}};

	morph_queue_init(&head);
	EXPECT_TRUE(morph_queue_empty(&head));

	morph_queue_insert_head(&head, &a.link);
	morph_queue_insert_tail(&head, &b.link);
	EXPECT_EQ(morph_queue_head(&head), &a.link);
	EXPECT_EQ(morph_queue_last(&head), &b.link);
	EXPECT_EQ(morph_queue_data(morph_queue_head(&head), QueueNode, link), &a);

	morph_queue_remove(&a.link);
	EXPECT_EQ(a.link.next, nullptr);
	EXPECT_EQ(a.link.prev, nullptr);
	EXPECT_EQ(morph_queue_head(&head), &b.link);
}

TEST(UtilQueue, SplitAndAdd)
{
	struct morph_queue head;
	struct morph_queue tail;
	QueueNode nodes[4];

	morph_queue_init(&head);
	for (int i = 0; i < 4; i++) {
		nodes[i].value = i + 1;
		nodes[i].seq = i;
		morph_queue_insert_tail(&head, &nodes[i].link);
	}

	morph_queue_split(&head, &nodes[2].link, &tail);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 2}));
	EXPECT_EQ(queue_values(&tail), std::vector<int>({3, 4}));

	morph_queue_add(&head, &tail);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 2, 3, 4}));
	EXPECT_TRUE(morph_queue_empty(&tail));
}

TEST(UtilQueue, MiddleAndStableSort)
{
	struct morph_queue head;
	QueueNode nodes[6];
	const int values[6] = {4, 1, 3, 1, 2, 3};

	morph_queue_init(&head);
	for (int i = 0; i < 6; i++) {
		nodes[i].value = values[i];
		nodes[i].seq = i;
		morph_queue_insert_tail(&head, &nodes[i].link);
	}

	EXPECT_EQ(morph_queue_data(morph_queue_middle(&head), QueueNode, link),
	          &nodes[2]);

	morph_queue_sort(&head, queue_node_cmp);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 1, 2, 3, 3, 4}));

	struct morph_queue *q = morph_queue_head(&head);
	QueueNode *first_one = morph_queue_data(q, QueueNode, link);
	q = morph_queue_next(q);
	QueueNode *second_one = morph_queue_data(q, QueueNode, link);
	EXPECT_EQ(first_one->seq, 1);
	EXPECT_EQ(second_one->seq, 3);
}

TEST(UtilStr, DupCompareAndToC)
{
	struct arena *arena = arena_create(128);
	ASSERT_NE(arena, nullptr);
	const char raw[] = {'a', '\0', 'b'};

	morph_str_t s = morph_strndup(arena, raw, sizeof(raw));
	ASSERT_TRUE(morph_str_valid(s));
	EXPECT_EQ(s.len, sizeof(raw));
	EXPECT_EQ(memcmp(s.data, raw, sizeof(raw)), 0);
	EXPECT_EQ(s.data[s.len], '\0');

	morph_str_t same = MORPH_STRLIT("a");
	EXPECT_GT(morph_strcmp(s, same), 0);
	EXPECT_EQ(morph_strcasecmp(MORPH_STRLIT("AbC"), MORPH_STRLIT("aBc")), 0);
	EXPECT_EQ(morph_strncmp(MORPH_STRLIT("abc"), "abd", 2), 0);
	EXPECT_LT(morph_strncmp(MORPH_STRLIT("abc"), "abd", 3), 0);

	const char *cstr = morph_str_to_c(arena, s);
	ASSERT_NE(cstr, nullptr);
	EXPECT_EQ(memcmp(cstr, raw, sizeof(raw)), 0);

	arena_destroy(arena);
}

TEST(UtilStr, ChrRchrTrimAndInvalidInputs)
{
	morph_str_t s = MORPH_STRLIT("\t  alpha beta \r\n");
	morph_str_t trimmed = morph_str_trim(s);

	EXPECT_EQ(trimmed.len, strlen("alpha beta"));
	EXPECT_EQ(memcmp(trimmed.data, "alpha beta", trimmed.len), 0);
	EXPECT_EQ(morph_str_chr(trimmed, 'a'), trimmed.data);
	EXPECT_EQ(morph_str_rchr(trimmed, 'a'), trimmed.data + trimmed.len - 1);
	EXPECT_EQ(morph_str_chr(trimmed, 'z'), nullptr);

	morph_str_t spaces = MORPH_STRLIT(" \t\r\n");
	morph_str_t empty = morph_str_trim(spaces);
	EXPECT_EQ(empty.len, 0u);
	ASSERT_NE(empty.data, nullptr);
	EXPECT_STREQ(empty.data, "");

	morph_str_t invalid = {3, nullptr};
	EXPECT_LT(morph_strcmp(invalid, MORPH_STRLIT("abc")), 0);
	EXPECT_LT(morph_strcasecmp(invalid, MORPH_STRLIT("abc")), 0);
	EXPECT_LT(morph_strncmp(invalid, "abc", 3), 0);
	EXPECT_EQ(morph_str_to_c(nullptr, MORPH_STRLIT("x")), nullptr);
}
