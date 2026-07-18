#ifndef MORPH_RUNTIME_TEST_SUPPORT_HPP
#define MORPH_RUNTIME_TEST_SUPPORT_HPP

#include <gtest/gtest.h>

extern "C" {
#include "runtime/runtime.h"
}

#include <filesystem>
#include <fstream>
#include <string>

class RuntimeFacadeTest : public ::testing::Test {
protected:
	std::string directory;
	std::string database;
	std::string config_path;
	struct runtime *instance = nullptr;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-runtime-facade-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		database = directory + "/data.db";
		config_path = directory + "/config.toml";
	}

	void TearDown() override
	{
		if (instance)
			runtime_close(instance);
		instance = nullptr;
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	void WriteConfig(const std::string &contents)
	{
		std::ofstream file(config_path);
		ASSERT_TRUE(file.is_open());
		file << contents;
		file.close();
		ASSERT_TRUE(file.good());
	}

	void Open(const struct runtime_options *overrides = nullptr)
	{
		struct runtime_options options{};
		if (overrides)
			options = *overrides;
		if (!options.db_path)
			options.db_path = database.c_str();
		if (!options.workdir)
			options.workdir = directory.c_str();
		if (!options.front_name)
			options.front_name = "runtime-test";
		ASSERT_EQ(runtime_open(&options, &instance), 0);
		ASSERT_NE(instance, nullptr);
	}
};

#endif
