#include <gtest/gtest.h>
#include "nb/err.h"
#include "cleanup_probe.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"

#include <errno.h>
#include <string.h>

TEST(Cleanup, CScopeExitRunsInDependencyOrder)
{
	for (int early : {0, 1}) {
		int order[2] = {};
		int count = 0;

		EXPECT_EQ(cleanup_probe_run(early, order, &count), early ? NB_ERR_CANCELLED : 0);
		ASSERT_EQ(count, 2);
		EXPECT_EQ(order[0], 2);
		EXPECT_EQ(order[1], 1);
	}
}

TEST(Cleanup, StealLeavesSourceEmptyAndNewOwnerAlive)
{
	char *received NB_CLEANUP(nb_charp_cleanup) = nullptr;
	{
		char *source NB_CLEANUP(nb_charp_cleanup) = strdup("owned text");
		ASSERT_NE(source, nullptr);
		received = nb_charp_steal(&source);
		EXPECT_EQ(source, nullptr);
		EXPECT_EQ(nb_charp_steal(&source), nullptr);
	}
	ASSERT_NE(received, nullptr);
	EXPECT_STREQ(received, "owned text");
	nb_charp_cleanup(&received);
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

TEST(Cleanup, JsonStringCopyAndOwnerTransfer)
{
	char *text NB_CLEANUP(nb_charp_cleanup) = strdup("transferred");
	ASSERT_NE(text, nullptr);
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString(text);
	ASSERT_NE(value, nullptr);
	nb_charp_cleanup(&text);
	EXPECT_EQ(text, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(value), "transferred");
	cJSON *received NB_CLEANUP(cJSON_DeleteOwner) = cJSON_StealOwner(&value);
	EXPECT_EQ(value, nullptr);
	ASSERT_NE(received, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(received), "transferred");
	EXPECT_EQ(cJSON_StealOwner(nullptr), nullptr);
}

TEST(Cleanup, JsonStringCopyRetainsSourceOnAllocationFailure)
{
	char *text NB_CLEANUP(nb_charp_cleanup) = strdup("retained");
	ASSERT_NE(text, nullptr);
	char *original = text;
	{
		CleanupJsonAllocationFailure failure;
		EXPECT_EQ(cJSON_CreateString(text), nullptr);
	}
	EXPECT_EQ(text, original);
	EXPECT_STREQ(text, "retained");
}

TEST(Cleanup, NativeJsonContainersPreserveOwnersUntilAccepted)
{
	cJSON *object NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString("value");
	ASSERT_NE(object, nullptr);
	ASSERT_NE(item, nullptr);
	cJSON *original = item;
	EXPECT_FALSE(cJSON_AddItemToObject(nullptr, "key", item));
	EXPECT_EQ(item, original);
	{
		CleanupJsonAllocationFailure failure;
		EXPECT_FALSE(cJSON_AddItemToObject(object, "key", item));
	}
	EXPECT_EQ(item, original);
	EXPECT_EQ(cJSON_GetArraySize(object), 0);
	ASSERT_TRUE(cJSON_AddItemToObject(object, "key", item));
	item = nullptr;
	EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(object, "key"), original);
	EXPECT_FALSE(cJSON_AddItemToObject(object, "duplicate", original));
	EXPECT_EQ(cJSON_GetArraySize(object), 1);
	cJSON_DeleteOwner(&item);
	EXPECT_STREQ(cJSON_GetStringValue(original), "value");
	cJSON *array NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	ASSERT_NE(array, nullptr);
	EXPECT_FALSE(cJSON_AddItemToArray(array, array));
	ASSERT_TRUE(cJSON_AddItemToArray(array, object));
	object = nullptr;
	EXPECT_EQ(cJSON_GetArraySize(array), 1);
}

TEST(Cleanup, BufferMoveSwapsBothOwners)
{
	nb_buf source NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf destination NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_TRUE(nb_buf_append_cstr(&source, "new text"));
	ASSERT_TRUE(nb_buf_append_cstr(&destination, "old text"));
	char *original = source.ptr;
	nb_buf_move(&destination, &source);
	EXPECT_EQ(destination.ptr, original);
	EXPECT_STREQ(nb_buf_cstr(&source), "old text");
	nb_buf_move(&destination, &destination);
	nb_buf_cleanup(&source);
	EXPECT_STREQ(nb_buf_cstr(&destination), "new text");
}

TEST(Cleanup, VectorMovePreservesBothOwners)
{
	nb_vec source NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec destination NB_CLEANUP(nb_vec_cleanup) = {};
	nb_vec_init(&source, sizeof(int));
	nb_vec_init(&destination, sizeof(int));
	int first = 42;
	int second = 21;
	ASSERT_TRUE(nb_vec_push_copy(&source, &first));
	ASSERT_TRUE(nb_vec_push_copy(&destination, &second));
	void *source_storage = source.data;
	void *destination_storage = destination.data;
	nb_vec_move(&destination, &source);
	EXPECT_EQ(source.data, destination_storage);
	EXPECT_EQ(destination.data, source_storage);
	EXPECT_EQ(*static_cast<int *>(nb_vec_at(&source, 0)), 21);
	nb_vec_cleanup(&source);
	EXPECT_EQ(*static_cast<int *>(nb_vec_at(&destination, 0)), 42);
}
