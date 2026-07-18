#include "runtime_test_support.hpp"

extern "C" {
#include "util/file.h"
}

#include <cerrno>
#include <cstring>
#include <filesystem>

TEST(RuntimeSyncFacadeTest, ConfigMappingCopiesAndClampsValues)
{
	struct config_sync input{};
	struct morph_sync_config output{};
	input.enabled = 1;
	input.interval_seconds = 45;
	input.retention_days = 12;
	std::strncpy(input.include[0], "config.toml",
		sizeof(input.include[0]) - 1);
	input.include_count = 1;
	ASSERT_EQ(runtime_sync_config_from_config(&input, "/source", "/fallback",
		nullptr, &output), 0);
	EXPECT_EQ(output.enabled, 1);
	EXPECT_EQ(output.interval_seconds, 45);
	EXPECT_EQ(output.retention_days, 12);
	EXPECT_STREQ(output.source_dir, "/source");
	EXPECT_STREQ(output.sync_dir, "/fallback");
	EXPECT_EQ(output.include_count, 1);
	EXPECT_STREQ(output.include[0], "config.toml");
	EXPECT_EQ(runtime_sync_config_from_config(nullptr, "/source", nullptr,
		nullptr, &output), -EINVAL);
}

TEST_F(RuntimeFacadeTest, DisabledSyncHasStableEmptyStatus)
{
	struct morph_sync_status status{};
	Open();
	EXPECT_EQ(runtime_sync_start_instance(instance, nullptr, nullptr), 0);
	EXPECT_EQ(runtime_sync_running(instance), 0);
	EXPECT_EQ(runtime_sync_status_instance(instance, &status), 0);
	EXPECT_EQ(status.running, 0);
	runtime_sync_stop_instance(instance);
}

TEST_F(RuntimeFacadeTest, OneShotSyncCopiesConfiguredFile)
{
	const std::string remote = directory + "/remote";
	WriteConfig("[sync]\n"
		"enabled = true\n"
		"dir = \"" + remote + "\"\n"
		"interval_seconds = 0\n"
		"retention_days = 30\n"
		"include = [\"payload.txt\"]\n");
	std::ofstream payload(directory + "/payload.txt");
	payload << "runtime-sync";
	payload.close();
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	struct morph_sync_status status{};
	ASSERT_EQ(runtime_sync_now_instance(instance, nullptr, nullptr, &status), 0);
	EXPECT_TRUE(std::filesystem::exists(remote + "/data/payload.txt"));
	EXPECT_GE(status.copied, 1);
}

TEST_F(RuntimeFacadeTest, MissingManifestAndInvalidRestoreAreReported)
{
	const std::string remote = directory + "/remote";
	WriteConfig("[sync]\n"
		"enabled = true\n"
		"dir = \"" + remote + "\"\n"
		"interval_seconds = 0\n"
		"include = [\"config.toml\"]\n");
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	struct runtime_sync_conflict *items = nullptr;
	int count = 0;
	EXPECT_EQ(runtime_sync_conflicts(instance, &items, &count), -ENOENT);
	EXPECT_EQ(runtime_sync_restore(instance, 0), -EINVAL);
	runtime_sync_conflicts_free(nullptr);
}
