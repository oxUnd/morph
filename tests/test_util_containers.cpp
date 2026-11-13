#include <gtest/gtest.h>
#include "nb/err.h"

#include "nb/cleanup.h"
#include "cJSON.h"
#include "encoding_fixture.h"
#include <unistd.h>
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include "nb/str.h"
#include "nb/queue.h"
#include "nb/str.h"

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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_1 = NB_OK;

	char id[64];

	ASSERT_EQ(((file_status_1 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				     : !(nb_buf_clear(&file_value_1),
					 nb_buf_append_cstr(&file_value_1, "turn_"))
					     ? NB_ERR_NOMEM
					     : nb_random_hex(&file_value_1, 16, NULL))) != NB_OK
			   ? file_status_1
		   : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_1),
		  0);
	EXPECT_EQ(strncmp(id, "turn_", 5), 0);
	EXPECT_EQ(strlen(id), 37u);
	EXPECT_TRUE(is_lower_hex_string(id + 5));
}

TEST(UtilId, RandomIdNbytesControlsHexLength)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_2 = NB_OK;

	char id[16];

	ASSERT_EQ(((file_status_2 =
			    ((4) == 0 || (4) > 16 ? NB_ERR_INVAL
			     : !(nb_buf_clear(&file_value_2), nb_buf_append_cstr(&file_value_2, ""))
				     ? NB_ERR_NOMEM
				     : nb_random_hex(&file_value_2, 4, NULL))) != NB_OK
			   ? file_status_2
		   : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_2),
		  0);
	EXPECT_EQ(strlen(id), 8u);
	EXPECT_TRUE(is_lower_hex_string(id));
}

TEST(UtilId, RejectsInvalidArgsAndSmallBuffer)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_3 = NB_OK;
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_4 = NB_OK;
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_6 = NB_OK;
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {};
	int file_status_7 = NB_OK;

	char id[8];

	EXPECT_EQ(((file_status_3 = (nullptr == nullptr || (16) == 0 || (16) > 16 ? NB_ERR_INVAL
				     : !(nb_buf_clear(&file_value_3),
					 nb_buf_append_cstr(&file_value_3, nullptr))
					     ? NB_ERR_NOMEM
					     : nb_random_hex(&file_value_3, 16, NULL))) != NB_OK
			   ? file_status_3
		   : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_3),
		  NB_ERR_INVAL);
	EXPECT_EQ(((file_status_4 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				     : !(nb_buf_clear(&file_value_4),
					 nb_buf_append_cstr(&file_value_4, "turn_"))
					     ? NB_ERR_NOMEM
					     : nb_random_hex(&file_value_4, 16, NULL))) != NB_OK
			   ? file_status_4
		   : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), nullptr, 64)) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_4),
		  NB_ERR_INVAL);
	EXPECT_EQ(((file_status_5 = ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				     : !(nb_buf_clear(&file_value_5),
					 nb_buf_append_cstr(&file_value_5, "turn_"))
					     ? NB_ERR_NOMEM
					     : nb_random_hex(&file_value_5, 16, NULL))) != NB_OK
			   ? file_status_5
		   : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_5),
		  NB_ERR_NO_SPACE);
	EXPECT_EQ(((file_status_6 =
			    ((0) == 0 || (0) > 16 ? NB_ERR_INVAL
			     : !(nb_buf_clear(&file_value_6), nb_buf_append_cstr(&file_value_6, ""))
				     ? NB_ERR_NOMEM
				     : nb_random_hex(&file_value_6, 0, NULL))) != NB_OK
			   ? file_status_6
		   : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_6),
		  NB_ERR_INVAL);
	EXPECT_EQ(((file_status_7 =
			    ((17) == 0 || (17) > 16 ? NB_ERR_INVAL
			     : !(nb_buf_clear(&file_value_7), nb_buf_append_cstr(&file_value_7, ""))
				     ? NB_ERR_NOMEM
				     : nb_random_hex(&file_value_7, 17, NULL))) != NB_OK
			   ? file_status_7
		   : (file_status_7 = nb_str_copy_to(nb_buf_str(&file_value_7), id, sizeof(id))) ==
				   NB_ERR_RANGE
			   ? NB_ERR_NO_SPACE
			   : file_status_7),
		  NB_ERR_INVAL);
}

TEST(UtilArray, PushGetPopAndClear)
{
	nb_vec values NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&values, sizeof(int));
	ASSERT_TRUE(nb_vec_reserve(&values, 2));
	for (int i = 0; i < 10; i++) {
		int value = i * 3;
		ASSERT_TRUE(nb_vec_push_copy(&values, &value));
	}
	ASSERT_EQ(nb_vec_len(&values), 10u);
	for (int i = 0; i < 10; i++)
		EXPECT_EQ(*static_cast<int *>(nb_vec_at(&values, (size_t)i)), i * 3);
	EXPECT_EQ(nb_vec_at(&values, 10), nullptr);
	nb_vec_pop(&values);
	EXPECT_EQ(nb_vec_len(&values), 9u);
	nb_vec_clear(&values);
	EXPECT_TRUE(nb_vec_empty(&values));
}

TEST(UtilArray, PushNReturnsContiguousSlots)
{
	nb_vec values NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&values, sizeof(int));
	int *slots = static_cast<int *>(nb_vec_push_n(&values, 4));
	ASSERT_NE(slots, nullptr);
	for (int i = 0; i < 4; i++)
		slots[i] = i + 10;
	EXPECT_EQ(nb_vec_len(&values), 4u);
	EXPECT_EQ(*static_cast<int *>(nb_vec_at(&values, 3)), 13);
}

TEST(UtilArray, AutomaticCleanupSupportsGrowth)
{
	nb_vec values NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&values, sizeof(int));
	for (int i = 0; i < 128; i++)
		ASSERT_TRUE(nb_vec_push_copy(&values, &i));
	EXPECT_EQ(*static_cast<int *>(nb_vec_at(&values, 127)), 127);
}

TEST(UtilArray, RejectsOverflowCapacities)
{
	nb_vec values NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&values, sizeof(int));
	EXPECT_FALSE(nb_vec_reserve(&values, SIZE_MAX / sizeof(int) + 1));
	EXPECT_EQ(values.data, nullptr);
	EXPECT_EQ(nb_vec_push_n(&values, SIZE_MAX), nullptr);
	EXPECT_TRUE(nb_vec_empty(&values));
}

TEST(UtilBuf, AppendPrintfCstrAndTruncate)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_TRUE(nb_buf_append_cstr(&buffer, "abc"));
	ASSERT_TRUE(nb_buf_append_char(&buffer, 'd'));
	ASSERT_TRUE(nb_buf_printf(&buffer, "-%d", 42));
	EXPECT_STREQ(nb_buf_cstr(&buffer), "abcd-42");
	nb_str view = nb_buf_str(&buffer);
	EXPECT_EQ(view.len, 7u);
	EXPECT_EQ(memcmp(view.ptr, "abcd-42", view.len), 0);
	nb_buf_truncate(&buffer, 4);
	EXPECT_STREQ(nb_buf_cstr(&buffer), "abcd");
	nb_buf_truncate(&buffer, 99);
	EXPECT_EQ(nb_buf_len(&buffer), 4u);
	size_t capacity = 0;
	char *text = nb_buf_detach(&buffer, &capacity);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, "abcd");
	EXPECT_EQ(buffer.ptr, nullptr);
	nb_free(text, capacity);
}

TEST(UtilBuf, AppendBinaryAndReset)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {};
	const char bytes[] = {'a', '\0', 'b'};
	ASSERT_TRUE(nb_buf_append(&buffer, bytes, sizeof(bytes)));
	EXPECT_EQ(nb_buf_len(&buffer), sizeof(bytes));
	EXPECT_EQ(memcmp(nb_buf_cstr(&buffer), bytes, sizeof(bytes)), 0);
	nb_buf_clear(&buffer);
	EXPECT_TRUE(nb_buf_empty(&buffer));
	EXPECT_STREQ(nb_buf_cstr(&buffer), "");
}

TEST(UtilBuf, DetachRequiresCapacityAndPreservesSourceOnInvalidOutput)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_TRUE(nb_buf_append_cstr(&buffer, "abcdef"));
	char *original = buffer.ptr;
	EXPECT_EQ(nb_buf_detach(&buffer, nullptr), nullptr);
	EXPECT_EQ(buffer.ptr, original);
	size_t capacity = 0;
	char *text = nb_buf_detach(&buffer, &capacity);
	ASSERT_EQ(text, original);
	EXPECT_EQ(buffer.ptr, nullptr);
	EXPECT_STREQ(text, "abcdef");
	nb_free(text, capacity);
}

TEST(UtilBuf, RejectsOverflowAndRemembersFailure)
{
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_TRUE(nb_buf_append_cstr(&buffer, "original"));
	EXPECT_FALSE(nb_buf_reserve(&buffer, SIZE_MAX));
	EXPECT_FALSE(nb_buf_ok(&buffer));
	EXPECT_FALSE(nb_buf_append_cstr(&buffer, "discarded"));
	nb_buf_clear_error(&buffer);
	EXPECT_TRUE(nb_buf_ok(&buffer));
	EXPECT_STREQ(nb_buf_cstr(&buffer), "original");
}

struct QueueNode {
	int value;
	int seq;
	nb_queue link;
};

static int queue_node_cmp(const nb_queue *a, const nb_queue *b)
{
	const QueueNode *na = NB_QUEUE_DATA(a, QueueNode, link);
	const QueueNode *nb = NB_QUEUE_DATA(b, QueueNode, link);

	if (na->value == nb->value)
		return 0;
	return na->value < nb->value ? -1 : 1;
}

static std::vector<int> queue_values(nb_queue *head)
{
	std::vector<int> values;
	nb_queue *q;

	NB_QUEUE_FOREACH(q, head)
	{
		QueueNode *node = NB_QUEUE_DATA(q, QueueNode, link);
		values.push_back(node->value);
	}
	return values;
}

TEST(UtilQueue, InsertRemoveAndData)
{
	nb_queue head;
	QueueNode a = {1, 0, {nullptr, nullptr}};
	QueueNode b = {2, 1, {nullptr, nullptr}};

	nb_queue_init(&head);
	EXPECT_TRUE(nb_queue_empty(&head));

	nb_queue_insert_head(&head, &a.link);
	nb_queue_insert_tail(&head, &b.link);
	EXPECT_EQ(nb_queue_head(&head), &a.link);
	EXPECT_EQ(nb_queue_last(&head), &b.link);
	EXPECT_EQ(NB_QUEUE_DATA(nb_queue_head(&head), QueueNode, link), &a);

	nb_queue_remove(&a.link);
	EXPECT_EQ(a.link.next, nullptr);
	EXPECT_EQ(a.link.prev, nullptr);
	EXPECT_EQ(nb_queue_head(&head), &b.link);
}

TEST(UtilQueue, SplitAndAdd)
{
	nb_queue head;
	nb_queue tail;
	QueueNode nodes[4];

	nb_queue_init(&head);
	for (int i = 0; i < 4; i++) {
		nodes[i].value = i + 1;
		nodes[i].seq = i;
		nb_queue_insert_tail(&head, &nodes[i].link);
	}

	nb_queue_split(&head, &nodes[2].link, &tail);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 2}));
	EXPECT_EQ(queue_values(&tail), std::vector<int>({3, 4}));

	nb_queue_add(&head, &tail);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 2, 3, 4}));
	EXPECT_TRUE(nb_queue_empty(&tail));
}

TEST(UtilQueue, MiddleAndStableSort)
{
	nb_queue head;
	QueueNode nodes[6];
	const int values[6] = {4, 1, 3, 1, 2, 3};

	nb_queue_init(&head);
	for (int i = 0; i < 6; i++) {
		nodes[i].value = values[i];
		nodes[i].seq = i;
		nb_queue_insert_tail(&head, &nodes[i].link);
	}

	EXPECT_EQ(NB_QUEUE_DATA(nb_queue_middle(&head), QueueNode, link), &nodes[2]);

	nb_queue_sort(&head, queue_node_cmp);
	EXPECT_EQ(queue_values(&head), std::vector<int>({1, 1, 2, 3, 3, 4}));

	nb_queue *q = nb_queue_head(&head);
	QueueNode *first_one = NB_QUEUE_DATA(q, QueueNode, link);
	q = nb_queue_next(q);
	QueueNode *second_one = NB_QUEUE_DATA(q, QueueNode, link);
	EXPECT_EQ(first_one->seq, 1);
	EXPECT_EQ(second_one->seq, 3);
}

TEST(UtilStr, BorrowedBinaryViewAndComparison)
{
	const char raw[] = {'a', '\0', 'b'};
	nb_str view = nb_str_from_parts(raw, sizeof(raw));
	EXPECT_EQ(view.ptr, raw);
	EXPECT_EQ(view.len, sizeof(raw));
	EXPECT_GT(nb_str_cmp(view, NB_STR_LIT("a")), 0);
	EXPECT_TRUE(nb_str_eq_icase(NB_STR_LIT("AbC"), NB_STR_LIT("aBc")));
	EXPECT_TRUE(nb_str_starts_with(NB_STR_LIT("abc"), NB_STR_LIT("ab")));
	EXPECT_LT(nb_str_cmp(NB_STR_LIT("abc"), NB_STR_LIT("abd")), 0);
}

TEST(UtilStr, CharacterLookupAndTrimBorrowInput)
{
	nb_str input = NB_STR_LIT("\t  alpha beta \r\n");
	nb_str trimmed = nb_str_trim(input);
	EXPECT_TRUE(nb_str_eq(trimmed, NB_STR_LIT("alpha beta")));
	EXPECT_EQ(nb_str_find_char(trimmed, 'a'), 0u);
	EXPECT_EQ(nb_str_find_char(trimmed, 'z'), SIZE_MAX);
	EXPECT_TRUE(nb_str_is_empty(nb_str_trim(NB_STR_LIT(" \t\r\n"))));
	EXPECT_TRUE(nb_str_is_empty(NB_STR_NULL));
}

TEST(UtilJson, SerializedDocumentHasIndependentOwnership)
{
	for (int i = 0; i < 5; i++) {
		cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		std::string payload(1024 * 1024, 'x');
		payload += "\n\"escaped\"";
		ASSERT_NE(root, nullptr);
		ASSERT_NE(cJSON_AddStringToObject(root, "payload", payload.c_str()), nullptr);
		char *serialized NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(root);
		ASSERT_NE(serialized, nullptr);
		cJSON_DeleteOwner(&root);
		cJSON *parsed NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(serialized);
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
		ASSERT_EQ(image_file_payload_copy(path, prefix, length, &actual), 0);
		char *expected = test_encode_bytes(data.data(), length);
		ASSERT_NE(expected, nullptr);
		EXPECT_STREQ(actual, (std::string(prefix) + expected).c_str());
		free(actual);
		free(expected);
		actual = nullptr;
		EXPECT_EQ(image_file_payload_copy(path, prefix, length - 1, &actual),
			  NB_ERR_TOO_BIG);
		EXPECT_EQ(actual, nullptr);
	}
	close(fd);
	unlink(path);
}

TEST(UtilName, AsciiNamesUseExplicitPunctuation)
{
	EXPECT_TRUE(nb_str_is_alnum(nb_str_from_cstr("Az09_-"), "_-"));
	EXPECT_TRUE(nb_str_is_alnum(nb_str_from_cstr("owner.repo/ref+1"), "-_.+/"));
	EXPECT_FALSE(nb_str_is_alnum(nb_str_from_cstr(nullptr), "_-"));
	EXPECT_FALSE(nb_str_is_alnum(nb_str_from_cstr(""), "_-"));
	EXPECT_FALSE(nb_str_is_alnum(nb_str_from_cstr("has space"), "_-"));
	EXPECT_FALSE(nb_str_is_alnum(nb_str_from_cstr("a.b"), "_-"));
	EXPECT_TRUE(nb_str_is_alnum(nb_str_from_cstr("a.b"), "_.-"));
	EXPECT_FALSE(nb_str_is_alnum(nb_str_from_cstr("na\xc3\xafve"), "_-"));
	EXPECT_TRUE(nb_str_is_alnum(nb_str_from_cstr("Ab09"), nullptr));
	EXPECT_FALSE(nb_ascii_is_alnum_or((unsigned char)('\0'), "_-"));
	EXPECT_FALSE(nb_ascii_is_alnum_or((unsigned char)(static_cast<char>(0xff)), "_-"));
}

TEST(UtilName, EnvironmentNamesEnforceFirstCharacter)
{
	for (const char *name : {"API_KEY", "_", "_token2", "token", "A0"})
		EXPECT_TRUE((nb_str_is_alnum(nb_str_from_cstr(name), "_") &&
			     !((name)[0] >= '0' && (name)[0] <= '9')))
			<< name;
	for (const char *name : {"", "0TOKEN", "API-KEY", "a.b", "a b"})
		EXPECT_FALSE((nb_str_is_alnum(nb_str_from_cstr(name), "_") &&
			      !((name)[0] >= '0' && (name)[0] <= '9')))
			<< name;
	EXPECT_FALSE((nb_str_is_alnum(nb_str_from_cstr(nullptr), "_") &&
		      !(nb_str_from_cstr(nullptr).ptr[0] >= '0' &&
			nb_str_from_cstr(nullptr).ptr[0] <= '9')));
}

TEST(UtilBase64, PrefixedCopyRetainsInputsAndRejectsOccupiedOutput)
{
	char path[] = "/tmp/morph-base64-owner-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "abc", 3), 3);
	ASSERT_EQ(close(fd), 0);
	char prefix[] = "copy:";
	char *encoded NB_CLEANUP(nb_charp_cleanup) = nullptr;
	ASSERT_EQ(image_file_payload_copy(path, prefix, 3, &encoded), 0);
	char *owner = encoded;
	prefix[0] = 'X';
	ASSERT_EQ(unlink(path), 0);
	EXPECT_STREQ(encoded, "copy:YWJj");
	EXPECT_EQ(image_file_payload_copy(path, prefix, 3, &encoded), NB_ERR_STATE);
	EXPECT_EQ(encoded, owner);
	EXPECT_STREQ(encoded, "copy:YWJj");
	nb_charp_cleanup(&encoded);
	EXPECT_EQ(image_file_payload_copy(path, prefix, 3, &encoded), NB_ERR_NOT_FOUND);
	EXPECT_EQ(encoded, nullptr);
}
