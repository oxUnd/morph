#include <gtest/gtest.h>
#include "util/log.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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
			tmpfile = nullptr;
		}
	}

	std::vector<std::string> read_lines() {
		std::vector<std::string> lines;
		if (!tmpfile) return lines;
		std::ifstream f(tmpfile);
		std::string line;
		while (std::getline(f, line))
			lines.push_back(line);
		return lines;
	}

	bool line_matches(const std::string &line, const char *level,
			  const char *msg) {
		// format: [2026-05-16 12:34:56] [LEVEL] message
		auto lpos = line.find("[INFO]");
		if (!level) {
			lpos = line.find('[');
			if (lpos == std::string::npos) return false;
			lpos = line.find(']', lpos);
			if (lpos == std::string::npos) return false;
			lpos = line.find(']', lpos + 1);
			if (lpos == std::string::npos) return false;
			lpos += 2; // skip "] "
			return line.substr(lpos) == msg;
		}
		if (lpos == std::string::npos) {
			auto dbg = line.find("[DEBUG]");
			auto wrn = line.find("[WARN]");
			auto err = line.find("[ERROR]");
			if (strcmp(level, "DEBUG") == 0) lpos = dbg;
			else if (strcmp(level, "WARN") == 0) lpos = wrn;
			else if (strcmp(level, "ERROR") == 0) lpos = err;
			if (lpos == std::string::npos) return false;
		}
		auto end = line.find(']', lpos);
		if (end == std::string::npos) return false;
		std::string actual_level = line.substr(lpos + 1, end - lpos - 1);
		if (actual_level != level) return false;
		std::string rest = line.substr(end + 2);
		return rest == msg;
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

TEST_F(LogTest, ContentWrittenAndFlushed) {
	log_info("hello world");
	log_warn("warning");
	log_err("oops %d", 99);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 3);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "hello world"));
	EXPECT_TRUE(line_matches(lines[1], "WARN", "warning"));
	EXPECT_TRUE(line_matches(lines[2], "ERROR", "oops 99"));
}

TEST_F(LogTest, MultipleWritesCount) {
	for (int i = 0; i < 100; i++)
		log_info("line %d", i);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 100);
	for (int i = 0; i < 100; i++) {
		char expected[32];
		snprintf(expected, sizeof(expected), "line %d", i);
		EXPECT_TRUE(line_matches(lines[i], "INFO", expected));
	}
}

TEST_F(LogTest, FflushBeforeShutdown) {
	log_shutdown();

	auto fp = std::fopen(tmpfile, "w");
	ASSERT_TRUE(fp);
	std::fclose(fp);

	log_init(tmpfile, LOG_INFO);
	log_info("before any close");

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 1);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "before any close"));
}