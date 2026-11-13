#include <gtest/gtest.h>
#include "nb/err.h"

#include "agent/tool_registry.h"
#include "nb/cleanup.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

struct ownership_payload {
	int *destroyed;
};

static int ownership_tool(const char *, struct tool_result *, void *)
{
	return 0;
}

static void ownership_destroy(void *opaque)
{
	auto *payload = static_cast<ownership_payload *>(opaque);
	++*payload->destroyed;
	delete payload;
}

static void ownership_spec_init(struct tool_spec *spec, const char *name, int *destroyed)
{
	spec->origin = TOOL_ORIGIN_BUILTIN;
	spec->name = name;
	spec->description = "Ownership test";
	spec->input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec->output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec->exec = ownership_tool;
	spec->user_data = new ownership_payload{destroyed};
	spec->user_data_destroy = ownership_destroy;
}

TEST(TestToolOwnership, BorrowRegistrationRejectsAnOwnerWithoutConsumingIt)
{
	int destroyed = 0;
	{
		struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
		tool_registry_init(nullptr, &registry);
		struct tool_spec source NB_CLEANUP(tool_spec_cleanup) = {};
		ownership_spec_init(&source, "owned", &destroyed);
		void *original = source.user_data;
		EXPECT_EQ(tool_register_borrow(&registry, &source), NB_ERR_INVAL);
		EXPECT_EQ(tool_register_replace_borrow(&registry, &source, 0), NB_ERR_INVAL);
		EXPECT_EQ(source.user_data, original);
		EXPECT_EQ(source.user_data_destroy, ownership_destroy);
		EXPECT_EQ(registry.count, 0);
		EXPECT_EQ(destroyed, 0);
	}
	EXPECT_EQ(destroyed, 1);
}

TEST(TestToolOwnership, SuccessfulTakeClearsSourceAndRetainsDescriptorMetadata)
{
	int destroyed = 0;
	struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct tool_spec source NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&source, "owned", &destroyed);
	void *original = source.user_data;
	ASSERT_EQ(tool_register_take(&registry, &source), 0);
	EXPECT_EQ(source.user_data, nullptr);
	EXPECT_EQ(source.user_data_destroy, nullptr);
	EXPECT_STREQ(source.name, "owned");
	EXPECT_EQ(source.exec, ownership_tool);
	ASSERT_NE(tool_borrow(&registry, "owned"), nullptr);
	EXPECT_EQ(tool_borrow(&registry, "owned")->user_data, original);
	EXPECT_EQ(destroyed, 0);
	ASSERT_EQ(tool_unregister(&registry, "owned"), 0);
	EXPECT_EQ(destroyed, 1);
}

TEST(TestToolOwnership, FailedTakePreservesSourceAndExistingRegistration)
{
	int destroyed = 0;
	struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct tool_spec original NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&original, "duplicate", &destroyed);
	void *registered_data = original.user_data;
	ASSERT_EQ(tool_register_take(&registry, &original), 0);
	struct tool_spec rejected NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&rejected, "duplicate", &destroyed);
	void *rejected_data = rejected.user_data;
	EXPECT_EQ(tool_register_take(&registry, &rejected), NB_ERR_EXISTS);
	EXPECT_EQ(rejected.user_data, rejected_data);
	EXPECT_EQ(rejected.user_data_destroy, ownership_destroy);
	EXPECT_EQ(registry.count, 1);
	EXPECT_EQ(tool_borrow(&registry, "duplicate")->user_data, registered_data);
	EXPECT_EQ(destroyed, 0);
	tool_spec_cleanup(&rejected);
	EXPECT_EQ(destroyed, 1);
	tool_registry_cleanup(&registry);
	EXPECT_EQ(destroyed, 2);
}

TEST(TestToolOwnership, ReplacementTakesOnlyAfterValidationAndRetainsOldExecution)
{
	int destroyed = 0;
	struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct tool_spec first NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&first, "versioned", &destroyed);
	ASSERT_EQ(tool_register_take(&registry, &first), 0);
	struct tool_handle old NB_CLEANUP(tool_handle_release) = {};
	ASSERT_EQ(tool_registry_acquire(&registry, "versioned", &old), 0);
	struct tool_spec replacement NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&replacement, "versioned", &destroyed);
	void *replacement_data = replacement.user_data;
	EXPECT_EQ(tool_register_replace_take(&registry, &replacement, TOOL_FLAG_DYNAMIC),
		  NB_ERR_EXISTS);
	EXPECT_EQ(replacement.user_data, replacement_data);
	EXPECT_EQ(tool_borrow(&registry, "versioned")->user_data, old.user_data);
	EXPECT_EQ(destroyed, 0);
	ASSERT_EQ(tool_register_replace_take(&registry, &replacement, 0), 1);
	EXPECT_EQ(replacement.user_data, nullptr);
	EXPECT_EQ(replacement.user_data_destroy, nullptr);
	EXPECT_EQ(tool_borrow(&registry, "versioned")->user_data, replacement_data);
	EXPECT_EQ(destroyed, 0);
	tool_handle_release(&old);
	EXPECT_EQ(destroyed, 1);
	ASSERT_EQ(tool_unregister(&registry, "versioned"), 0);
	EXPECT_EQ(destroyed, 2);
}

TEST(TestToolOwnership, BorrowedRegistrationLeavesDestructionWithItsOwner)
{
	int destroyed = 0;
	{
		struct tool_spec owner NB_CLEANUP(tool_spec_cleanup) = {};
		ownership_spec_init(&owner, "owner", &destroyed);
		struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
		tool_registry_init(nullptr, &registry);
		struct tool_spec borrowed{};
		borrowed.name = "borrowed";
		borrowed.description = "Borrowed data";
		borrowed.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
		borrowed.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
		borrowed.exec = ownership_tool;
		borrowed.user_data = owner.user_data;
		ASSERT_EQ(tool_register_borrow(&registry, &borrowed), 0);
		ASSERT_EQ(tool_unregister(&registry, "borrowed"), 0);
		EXPECT_EQ(owner.user_data, borrowed.user_data);
		EXPECT_EQ(destroyed, 0);
	}
	EXPECT_EQ(destroyed, 1);
}

TEST(TestToolOwnership, DescriptorCloneRejectsOccupiedOwnerAndMoveReplacesIt)
{
	struct tool_desc first_view{};
	first_view.name = "first";
	first_view.description = "First descriptor";
	struct tool_desc next_view{};
	next_view.name = "next";
	next_view.description = "Next descriptor";
	struct tool_desc first NB_CLEANUP(tool_desc_cleanup) = {};
	struct tool_desc next NB_CLEANUP(tool_desc_cleanup) = {};
	ASSERT_EQ(tool_desc_clone(&first_view, &first), 0);
	const char *original = first.name;
	EXPECT_EQ(tool_desc_clone(&next_view, &first), NB_ERR_EXISTS);
	EXPECT_EQ(first.name, original);
	EXPECT_STREQ(first.name, "first");
	ASSERT_EQ(tool_desc_clone(&next_view, &next), 0);
	const char *transferred = next.name;
	tool_desc_move(&first, &next);
	EXPECT_EQ(first.name, transferred);
	EXPECT_STREQ(first.description, "Next descriptor");
	EXPECT_EQ(next.name, nullptr);
	EXPECT_EQ(next.description, nullptr);
	EXPECT_EQ(next.input_schema, nullptr);
	tool_desc_move(&first, &first);
	EXPECT_EQ(first.name, transferred);
}

TEST(TestToolOwnership, AcquiringIntoRetainedHandlePreservesItsBinding)
{
	int destroyed = 0;
	struct tool_registry registry NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&spec, "first", &destroyed);
	ASSERT_EQ(tool_register_take(&registry, &spec), 0);
	struct tool_handle handle NB_CLEANUP(tool_handle_release) = {};
	ASSERT_EQ(tool_registry_acquire(&registry, "first", &handle), 0);
	auto *binding = handle.binding;
	EXPECT_EQ(tool_registry_acquire(&registry, "first", &handle), NB_ERR_STATE);
	EXPECT_EQ(handle.binding, binding);
	ASSERT_EQ(tool_unregister(&registry, "first"), 0);
	EXPECT_EQ(destroyed, 0);
	tool_handle_release(&handle);
	EXPECT_EQ(destroyed, 1);
}

TEST(TestToolOwnership, RegistryCopyRollsBackEarlierEntriesAndRejectsPopulatedTarget)
{
	int destroyed = 0;
	struct tool_registry source NB_CLEANUP(tool_registry_cleanup) = {};
	struct tool_registry target NB_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &source);
	tool_registry_init(nullptr, &target);
	for (const char *name : {"first", "invalid"}) {
		struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {};
		ownership_spec_init(&spec, name, &destroyed);
		ASSERT_EQ(tool_register_take(&source, &spec), 0);
	}
	auto *invalid = tool_borrow(&source, "invalid");
	ASSERT_NE(invalid, nullptr);
	free(const_cast<char *>(invalid->desc.input_schema));
	invalid->desc.input_schema = strdup("invalid JSON");
	ASSERT_NE(invalid->desc.input_schema, nullptr);
	EXPECT_LT(tool_registry_copy(&target, &source), 0);
	EXPECT_EQ(target.count, 0);
	EXPECT_EQ(target.entries, nullptr);
	EXPECT_EQ(destroyed, 0);
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {};
	ownership_spec_init(&spec, "retained", &destroyed);
	ASSERT_EQ(tool_register_take(&target, &spec), 0);
	EXPECT_EQ(tool_registry_copy(&target, &source), NB_ERR_STATE);
	EXPECT_NE(tool_borrow(&target, "retained"), nullptr);
}
