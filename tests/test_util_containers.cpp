#include <gtest/gtest.h>

#include "util/cleanup.h"
#include "util/json.h"
#include "util/base64.h"
#include <unistd.h>
#include "util/array.h"
#include "util/buf.h"
#include "util/id.h"
#include "util/name.h"
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

TEST(UtilId, RandomIdNbytesControlsHexLength)
{
	char id[16];

	ASSERT_EQ(morph_random_id_nbytes("", 4, id, sizeof(id)), 0);
	EXPECT_EQ(strlen(id), 8u);
	EXPECT_TRUE(is_lower_hex_string(id));
}

TEST(UtilId, RejectsInvalidArgsAndSmallBuffer)
{
	char id[8];

	EXPECT_EQ(morph_random_id(nullptr, id, sizeof(id)), -EINVAL);
	EXPECT_EQ(morph_random_id("turn_", nullptr, 64), -EINVAL);
	EXPECT_EQ(morph_random_id("turn_", id, sizeof(id)), -ENOSPC);
	EXPECT_EQ(morph_random_id_nbytes("", 0, id, sizeof(id)), -EINVAL);
	EXPECT_EQ(morph_random_id_nbytes("", 17, id, sizeof(id)), -EINVAL);
}

TEST(UtilArray, PushGetPopAndClear)
{
	morph_array_t a;
	ASSERT_EQ(morph_array_init(&a, 2, sizeof(int)), 0);

	for (int i = 0; i < 10; i++) {
		int *slot = static_cast<int *>(morph_array_push_borrow(&a));
		ASSERT_NE(slot, nullptr);
		*slot = i * 3;
	}

	ASSERT_GE(a.cap, 10u);
	ASSERT_EQ(a.nelts, 10u);
	for (int i = 0; i < 10; i++) {
		int *slot = static_cast<int *>(morph_array_borrow(&a, (size_t)i));
		ASSERT_NE(slot, nullptr);
		EXPECT_EQ(*slot, i * 3);
	}
	EXPECT_EQ(morph_array_borrow(&a, 10), nullptr);

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

	int *slots = static_cast<int *>(morph_array_push_n_borrow(&a, 4));
	ASSERT_NE(slots, nullptr);
	for (int i = 0; i < 4; i++)
		slots[i] = i + 10;

	EXPECT_EQ(a.nelts, 4u);
	EXPECT_EQ(*static_cast<int *>(morph_array_borrow(&a, 3)), 13);
	morph_array_cleanup(&a);
}

TEST(UtilArray, AutomaticCleanupSupportsGrowth)
{
	morph_array_t values MORPH_CLEANUP(morph_array_cleanup) = {};
	ASSERT_EQ(morph_array_init(&values, 1, sizeof(int)), 0);
	for (int i = 0; i < 128; i++) {
		int *slot = static_cast<int *>(morph_array_push_borrow(&values));
		ASSERT_NE(slot, nullptr);
		*slot = i;
	}
	EXPECT_EQ(*static_cast<int *>(morph_array_borrow(&values, 127)), 127);
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
	EXPECT_EQ(morph_array_push_n_borrow(&a, 2), nullptr);
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

	ASSERT_EQ(morph_strmap_set_borrow(&m, "alpha", &one), 0);
	ASSERT_EQ(morph_strmap_set_borrow(&m, "beta", &two), 0);
	EXPECT_EQ(morph_strmap_len(&m), 2u);
	EXPECT_EQ(morph_strmap_borrow(&m, "alpha"), &one);
	EXPECT_EQ(morph_strmap_borrow(&m, "beta"), &two);
	EXPECT_EQ(morph_strmap_borrow(&m, "missing"), nullptr);
	EXPECT_TRUE(morph_strmap_contains(&m, "alpha"));
	EXPECT_FALSE(morph_strmap_contains(&m, "missing"));

	ASSERT_EQ(morph_strmap_set_borrow(&m, "alpha", &three), 0);
	EXPECT_EQ(morph_strmap_len(&m), 2u);
	EXPECT_EQ(morph_strmap_borrow(&m, "alpha"), &three);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, OwnsCopiedKeys)
{
	morph_strmap_t m;
	int value = 7;
	char key[] = "mutable";

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set_borrow(&m, key, &value), 0);
	strcpy(key, "changed");

	EXPECT_EQ(morph_strmap_borrow(&m, "mutable"), &value);
	EXPECT_EQ(morph_strmap_borrow(&m, "changed"), nullptr);

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
		ASSERT_EQ(morph_strmap_set_borrow(&m, key, &values[i]), 0);
	}
	EXPECT_EQ(morph_strmap_len(&m), 16u);

	EXPECT_EQ(morph_strmap_remove(&m, "k3"), 1);
	EXPECT_EQ(morph_strmap_remove(&m, "k7"), 1);
	EXPECT_EQ(morph_strmap_remove(&m, "missing"), 0);
	EXPECT_FALSE(morph_strmap_contains(&m, "k3"));
	EXPECT_EQ(morph_strmap_len(&m), 14u);

	values[20] = 20;
	ASSERT_EQ(morph_strmap_set_borrow(&m, "new", &values[20]), 0);
	EXPECT_EQ(morph_strmap_borrow(&m, "new"), &values[20]);
	EXPECT_EQ(morph_strmap_len(&m), 15u);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, NullValueIsStillContained)
{
	morph_strmap_t m;

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set_borrow(&m, "nil", nullptr), 0);

	EXPECT_EQ(morph_strmap_borrow(&m, "nil"), nullptr);
	EXPECT_TRUE(morph_strmap_contains(&m, "nil"));

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, ClearKeepsMapReusable)
{
	morph_strmap_t m;
	int value = 42;

	ASSERT_EQ(morph_strmap_init(&m, 0), 0);
	ASSERT_EQ(morph_strmap_set_borrow(&m, "a", &value), 0);
	ASSERT_EQ(morph_strmap_set_borrow(&m, "b", &value), 0);
	morph_strmap_clear(&m);

	EXPECT_EQ(morph_strmap_len(&m), 0u);
	EXPECT_FALSE(morph_strmap_contains(&m, "a"));
	ASSERT_EQ(morph_strmap_set_borrow(&m, "c", &value), 0);
	EXPECT_EQ(morph_strmap_borrow(&m, "c"), &value);

	morph_strmap_cleanup(&m);
}

TEST(UtilStrmap, RejectsInvalidArguments)
{
	morph_strmap_t m;
	int value = 1;

	EXPECT_EQ(morph_strmap_init(nullptr, 0), -EINVAL);
	ASSERT_EQ(morph_strmap_init(&m, 0), 0);

	EXPECT_EQ(morph_strmap_set_borrow(nullptr, "a", &value), -EINVAL);
	EXPECT_EQ(morph_strmap_set_borrow(&m, nullptr, &value), -EINVAL);
	EXPECT_EQ(morph_strmap_borrow(nullptr, "a"), nullptr);
	EXPECT_EQ(morph_strmap_borrow(&m, nullptr), nullptr);
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
	EXPECT_STREQ(morph_buf_cstr_borrow(&b), "abcd-42");

	morph_str_t s = morph_buf_str_borrow(&b);
	EXPECT_EQ(s.len, 7u);
	EXPECT_EQ(memcmp(s.data, "abcd-42", s.len), 0);

	ASSERT_EQ(morph_buf_truncate(&b, 4), 0);
	EXPECT_STREQ(morph_buf_cstr_borrow(&b), "abcd");
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
	ASSERT_NE(morph_buf_cstr_borrow(&b), nullptr);
	EXPECT_EQ(memcmp(b.data, bytes, sizeof(bytes)), 0);

	morph_buf_reset(&b);
	EXPECT_EQ(b.len, 0u);
	EXPECT_STREQ(morph_buf_cstr_borrow(&b), "");
	morph_buf_cleanup(&b);
}

TEST(UtilBuf, DetachTransfersOwnershipAndFailurePreservesSource)
{
	morph_buf_t buffer MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&buffer, 2), 0);
	ASSERT_EQ(morph_buf_puts(&buffer, "abcdef"), 0);
	char *original = buffer.data;
	buffer.failed = -ENOMEM;
	EXPECT_EQ(morph_buf_detach(&buffer), nullptr);
	EXPECT_EQ(buffer.data, original);
	buffer.failed = 0;
	char *text MORPH_CLEANUP(morph_charp_cleanup) = morph_buf_detach(&buffer);
	ASSERT_NE(text, nullptr);
	EXPECT_EQ(text, original);
	EXPECT_EQ(buffer.data, nullptr);
	morph_buf_cleanup(&buffer);
	EXPECT_STREQ(text, "abcdef");
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
	const char raw[] = {'a', '\0', 'b'};
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = morph_strndup(raw, sizeof(raw));
	ASSERT_NE(copy, nullptr);
	morph_str_t view = {sizeof(raw), copy};
	EXPECT_EQ(memcmp(view.data, raw, sizeof(raw)), 0);
	EXPECT_EQ(view.data[view.len], '\0');
	EXPECT_GT(morph_strcmp(view, MORPH_STRLIT("a")), 0);
	EXPECT_EQ(morph_strcasecmp(MORPH_STRLIT("AbC"), MORPH_STRLIT("aBc")), 0);
	EXPECT_EQ(morph_strncmp(MORPH_STRLIT("abc"), "abd", 2), 0);
	EXPECT_LT(morph_strncmp(MORPH_STRLIT("abc"), "abd", 3), 0);
	char *cstr MORPH_CLEANUP(morph_charp_cleanup) = morph_str_cstr_copy(view);
	ASSERT_NE(cstr, nullptr);
	EXPECT_NE(cstr, copy);
	EXPECT_EQ(memcmp(cstr, raw, sizeof(raw)), 0);
	EXPECT_EQ(morph_strndup(nullptr, 1), nullptr);
	EXPECT_EQ(morph_strndup("x", SIZE_MAX), nullptr);
}

TEST(UtilStr, ChrRchrTrimAndInvalidInputs)
{
	morph_str_t s = MORPH_STRLIT("\t  alpha beta \r\n");
	morph_str_t trimmed = morph_str_trim_borrow(s);

	EXPECT_EQ(trimmed.len, strlen("alpha beta"));
	EXPECT_EQ(memcmp(trimmed.data, "alpha beta", trimmed.len), 0);
	EXPECT_EQ(morph_str_chr_borrow(trimmed, 'a'), trimmed.data);
	EXPECT_EQ(morph_str_rchr_borrow(trimmed, 'a'), trimmed.data + trimmed.len - 1);
	EXPECT_EQ(morph_str_chr_borrow(trimmed, 'z'), nullptr);

	morph_str_t spaces = MORPH_STRLIT(" \t\r\n");
	morph_str_t empty = morph_str_trim_borrow(spaces);
	EXPECT_EQ(empty.len, 0u);
	ASSERT_NE(empty.data, nullptr);
	EXPECT_STREQ(empty.data, "");

	morph_str_t invalid = {3, nullptr};
	EXPECT_LT(morph_strcmp(invalid, MORPH_STRLIT("abc")), 0);
	EXPECT_LT(morph_strcasecmp(invalid, MORPH_STRLIT("abc")), 0);
	EXPECT_LT(morph_strncmp(invalid, "abc", 3), 0);
	EXPECT_EQ(morph_str_cstr_copy(invalid), nullptr);
}

TEST(UtilJson, SerializedDocumentHasIndependentOwnership)
{
	for (int i = 0; i < 5; i++) {
		cJSON *root MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		std::string payload(1024 * 1024, 'x');
		payload += "\n\"escaped\"";
		ASSERT_NE(root, nullptr);
		ASSERT_NE(cJSON_AddStringToObject(root, "payload", payload.c_str()), nullptr);
		char *serialized MORPH_CLEANUP(morph_json_text_cleanup) =
			morph_json_print_copy(root);
		ASSERT_NE(serialized, nullptr);
		morph_json_cleanup(&root);
		cJSON *parsed MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(serialized);
		ASSERT_NE(parsed, nullptr);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "payload")),
			     payload.c_str());
	}
}

TEST(UtilBase64, FileChunksPreservePaddingAndEnforceLimit)
{
	char path[] = "/tmp/morph_b64_chunks_XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	const char *prefix = "data:video/mp4;base64,";
	for (size_t length : {1u, 2u, 3u, (unsigned)BUFSIZ - 1,
		(unsigned)BUFSIZ, (unsigned)BUFSIZ + 1, (unsigned)BUFSIZ * 3 + 2}) {
		std::vector<unsigned char> data(length);
		for (size_t i = 0; i < length; i++)
			data[i] = (unsigned char)(i % 251);
		ASSERT_EQ(ftruncate(fd, 0), 0);
		ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
		ASSERT_EQ(write(fd, data.data(), length), (ssize_t)length);
		char *actual = nullptr;
		ASSERT_EQ(base64_encode_file_prefixed_copy(path, prefix, length, &actual), 0);
		char *expected = base64_encode_copy(data.data(), length);
		ASSERT_NE(expected, nullptr);
		EXPECT_STREQ(actual, (std::string(prefix) + expected).c_str());
		free(actual);
		free(expected);
		actual = nullptr;
		EXPECT_EQ(base64_encode_file_prefixed_copy(path, prefix, length - 1, &actual),
			  -EFBIG);
		EXPECT_EQ(actual, nullptr);
	}
	close(fd);
	unlink(path);
}

TEST(UtilName, AsciiNamesUseExplicitPunctuation)
{
	EXPECT_TRUE(morph_name_valid("Az09_-", "_-"));
	EXPECT_TRUE(morph_name_valid("owner.repo/ref+1", "-_.+/"));
	EXPECT_FALSE(morph_name_valid(nullptr, "_-"));
	EXPECT_FALSE(morph_name_valid("", "_-"));
	EXPECT_FALSE(morph_name_valid("has space", "_-"));
	EXPECT_FALSE(morph_name_valid("a.b", "_-"));
	EXPECT_TRUE(morph_name_valid("a.b", "_.-"));
	EXPECT_FALSE(morph_name_valid("na\xc3\xafve", "_-"));
	EXPECT_TRUE(morph_name_valid("Ab09", nullptr));
	EXPECT_FALSE(morph_name_char_valid('\0', "_-"));
	EXPECT_FALSE(morph_name_char_valid(static_cast<char>(0xff), "_-"));
}

TEST(UtilName, EnvironmentNamesEnforceFirstCharacter)
{
	for (const char *name : {"API_KEY", "_", "_token2", "token", "A0"})
		EXPECT_TRUE(morph_env_name_valid(name)) << name;
	for (const char *name : {"", "0TOKEN", "API-KEY", "a.b", "a b"})
		EXPECT_FALSE(morph_env_name_valid(name)) << name;
	EXPECT_FALSE(morph_env_name_valid(nullptr));
}

TEST(UtilBase64, PrefixedCopyRetainsInputsAndRejectsOccupiedOutput)
{
	char path[] = "/tmp/morph-base64-owner-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "abc", 3), 3);
	ASSERT_EQ(close(fd), 0);
	char prefix[] = "copy:";
	char *encoded MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	ASSERT_EQ(base64_encode_file_prefixed_copy(path, prefix, 3, &encoded), 0);
	char *owner = encoded;
	prefix[0] = 'X';
	ASSERT_EQ(unlink(path), 0);
	EXPECT_STREQ(encoded, "copy:YWJj");
	EXPECT_EQ(base64_encode_file_prefixed_copy(path, prefix, 3, &encoded), -EALREADY);
	EXPECT_EQ(encoded, owner);
	EXPECT_STREQ(encoded, "copy:YWJj");
	morph_charp_cleanup(&encoded);
	EXPECT_EQ(base64_encode_file_prefixed_copy(path, prefix, 3, &encoded), -ENOENT);
	EXPECT_EQ(encoded, nullptr);
}
