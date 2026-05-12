#include <gtest/gtest.h>
#include "util/log.h"
#include <cstdio>
#include <cstring>

class LogTest : public ::testing::Test {
protected:
	void SetUp() override {
		tmpfile = std::tmpnam(nullptr);
		log_init(tmpfile, LOG_DEBUG);
	}
	void TearDown() override {
		log_shutdown();
		if (tmpfile) {
			std::remove(tmpfile);
		}
	}
	char *tmpfile = nullptr;
};

TEST_F(LogTest, InitShutdown) {
	EXPECT_NO_FATAL_FAILURE(log_shutdown());
	log_init(tmpfile, LOG_INFO);
}

TEST_F(LogTest, WriteAll) {
	log_info("test message %d", 42);
	log_warn("warning message");
	log_err("error message");
}

TEST_F(LogTest, WriteDebug) {
	log_dbg("debug message");
}

TEST_F(LogTest, LevelFiltering) {
	log_set_level(LOG_WARN);
	log_info("should not appear");
	log_warn("should appear");
	log_err("should also appear");
	log_set_level(LOG_DEBUG);
}

TEST_F(LogTest, LevelStrings) {
	log_write(LOG_DEBUG, "debug");
	log_write(LOG_INFO, "info");
	log_write(LOG_WARN, "warn");
	log_write(LOG_ERROR, "error");
}

TEST_F(LogTest, NullFile) {
	log_shutdown();
	log_init(nullptr, LOG_INFO);
	log_info("test without file");
	log_shutdown();
}

TEST_F(LogTest, FormatString) {
	log_info("formatted: %s %d %f", "hello", 42, 3.14);
}