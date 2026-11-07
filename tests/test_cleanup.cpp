#include <gtest/gtest.h>
#include "cleanup_probe.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/array.h"
#include "util/buf.h"

#include <errno.h>
#include <string.h>

TEST(Cleanup, CScopeExitRunsInDependencyOrder)
{
	for (int early : {0, 1}) {
		int order[2] = {};
		int count = 0;

		EXPECT_EQ(cleanup_probe_run(early, order, &count), early ? -ECANCELED : 0);
		ASSERT_EQ(count, 2);
		EXPECT_EQ(order[0], 2);
		EXPECT_EQ(order[1], 1);
	}
}

TEST(Cleanup, StealLeavesSourceEmptyAndNewOwnerAlive)
{
	char *received MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	{
		char *source MORPH_CLEANUP(morph_charp_cleanup) = strdup("owned text");
		ASSERT_NE(source, nullptr);
		received = morph_charp_steal(&source);
		EXPECT_EQ(source, nullptr);
		EXPECT_EQ(morph_charp_steal(&source), nullptr);
	}
	ASSERT_NE(received, nullptr);
	EXPECT_STREQ(received, "owned text");
	morph_charp_cleanup(&received);
	EXPECT_EQ(received, nullptr);
}

static void *cleanup_test_fail_allocation(size_t)
{
	return nullptr;
}

struct CleanupJsonAllocationFailure {
	CleanupJsonAllocationFailure()
	{
		cJSON_Hooks hooks = {cleanup_test_fail_allocation, free};
		cJSON_InitHooks(&hooks);
	}
	~CleanupJsonAllocationFailure()
	{
		cJSON_InitHooks(nullptr);
	}
};

TEST(Cleanup, JsonTakeClearsSourceOnSuccess)
{
	char *text MORPH_CLEANUP(morph_charp_cleanup) = strdup("transferred");
	ASSERT_NE(text, nullptr);
	cJSON *value MORPH_CLEANUP(morph_json_cleanup) = morph_json_string_take(&text);
	ASSERT_NE(value, nullptr);
	EXPECT_EQ(text, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(value), "transferred");
	EXPECT_EQ(morph_json_string_take(&text), nullptr);
	EXPECT_EQ(morph_json_string_take(nullptr), nullptr);
}

TEST(Cleanup, JsonTakeRetainsSourceOnAllocationFailure)
{
	char *text MORPH_CLEANUP(morph_charp_cleanup) = strdup("retained");
	ASSERT_NE(text, nullptr);
	char *original = text;
	{
		CleanupJsonAllocationFailure failure;
		EXPECT_EQ(morph_json_string_take(&text), nullptr);
	}
	EXPECT_EQ(text, original);
	EXPECT_STREQ(text, "retained");
}

TEST(Cleanup, JsonContainerTakePreservesOwnersUntilAccepted)
{
	cJSON *object MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON *item MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateString("value");
	ASSERT_NE(object, nullptr);
	ASSERT_NE(item, nullptr);
	cJSON *original = item;
	EXPECT_EQ(morph_json_object_take(nullptr, "key", &item), -EINVAL);
	EXPECT_EQ(item, original);
	{
		CleanupJsonAllocationFailure failure;
		EXPECT_EQ(morph_json_object_take(object, "key", &item), -ENOMEM);
	}
	EXPECT_EQ(item, original);
	EXPECT_EQ(cJSON_GetArraySize(object), 0);
	EXPECT_EQ(morph_json_object_take(object, "key", &item), 0);
	EXPECT_EQ(item, nullptr);
	EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(object, "key"), original);
	cJSON *borrowed = original;
	EXPECT_EQ(morph_json_object_take(object, "duplicate", &borrowed), -EINVAL);
	EXPECT_EQ(borrowed, original);
	morph_json_cleanup(&item);
	EXPECT_STREQ(cJSON_GetStringValue(original), "value");

	cJSON *array MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	ASSERT_NE(array, nullptr);
	EXPECT_EQ(morph_json_array_take(array, &array), -EINVAL);
	EXPECT_EQ(morph_json_array_take(array, &object), 0);
	EXPECT_EQ(object, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(array), 1);
}

TEST(Cleanup, BufferMoveReplacesDestinationAndEmptiesSource)
{
	morph_buf_t source MORPH_CLEANUP(morph_buf_cleanup) = {};
	morph_buf_t destination MORPH_CLEANUP(morph_buf_cleanup) = {};
	ASSERT_EQ(morph_buf_init(&source, 16), 0);
	ASSERT_EQ(morph_buf_init(&destination, 16), 0);
	ASSERT_EQ(morph_buf_puts(&source, "new text"), 0);
	ASSERT_EQ(morph_buf_puts(&destination, "old text"), 0);
	char *original = source.data;
	morph_buf_move(&destination, &source);
	EXPECT_EQ(source.data, nullptr);
	EXPECT_EQ(destination.data, original);
	morph_buf_move(&destination, &destination);
	morph_buf_cleanup(&source);
	EXPECT_STREQ(morph_buf_cstr_borrow(&destination), "new text");
}

TEST(Cleanup, ArrayTakeRejectsOccupiedDestinationWithoutLosingEitherOwner)
{
	morph_array_t source MORPH_CLEANUP(morph_array_cleanup) = {};
	morph_array_t destination MORPH_CLEANUP(morph_array_cleanup) = {};
	ASSERT_EQ(morph_array_init(&source, 1, sizeof(int)), 0);
	ASSERT_EQ(morph_array_init(&destination, 1, sizeof(int)), 0);
	*static_cast<int *>(morph_array_push_borrow(&source)) = 42;
	void *source_storage = source.elts;
	void *destination_storage = destination.elts;
	EXPECT_EQ(morph_array_take(&destination, &source), -EEXIST);
	EXPECT_EQ(source.elts, source_storage);
	EXPECT_EQ(destination.elts, destination_storage);
	morph_array_cleanup(&destination);
	EXPECT_EQ(morph_array_take(&destination, &source), 0);
	EXPECT_EQ(source.elts, nullptr);
	EXPECT_EQ(destination.elts, source_storage);
	morph_array_cleanup(&source);
	EXPECT_EQ(*static_cast<int *>(morph_array_borrow(&destination, 0)), 42);
}
