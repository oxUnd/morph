#include <gtest/gtest.h>
#include "util/log.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

class LogTest : public ::testing::Test {
protected:
  struct morph_log logger{};
  void SetUp() override
  {
	  char tmpl[] = "/tmp/morph_log_test_XXXXXX";
	  int fd = mkstemp(tmpl);
	  ASSERT_NE(fd, -1);
	  close(fd);
	  tmpfile = tmpl;
	  log_init(&logger, tmpfile.c_str(), LOG_DEBUG);
  }
	void TearDown() override {
		log_shutdown(&logger);
		if (!tmpfile.empty()) {
			std::remove(tmpfile.c_str());
			tmpfile.clear();
		}
	}

	std::vector<std::string> read_lines() {
		std::vector<std::string> lines;
		if (tmpfile.empty()) return lines;
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

	std::string tmpfile;
};

TEST_F(LogTest, InitShutdown) {
	EXPECT_NO_FATAL_FAILURE(log_shutdown(&logger));
	log_init(&logger, tmpfile.c_str(), LOG_INFO);
}

TEST_F(LogTest, WriteAll) {
	log_info(&logger, "test message %d", 42);
	log_warn(&logger, "warning message");
	log_err(&logger, "error message");
}

TEST_F(LogTest, WriteDebug) {
	log_dbg(&logger, "debug message");
}

TEST_F(LogTest, LevelFiltering) {
	log_set_level(&logger, LOG_WARN);
	log_info(&logger, "should not appear");
	log_warn(&logger, "should appear");
	log_err(&logger, "should also appear");
	log_set_level(&logger, LOG_DEBUG);
}

TEST_F(LogTest, LevelStrings) {
	log_write(&logger, LOG_DEBUG, "debug");
	log_write(&logger, LOG_INFO, "info");
	log_write(&logger, LOG_WARN, "warn");
	log_write(&logger, LOG_ERROR, "error");
}

TEST_F(LogTest, NullFile) {
	log_shutdown(&logger);
	log_init(&logger, nullptr, LOG_INFO);
	log_info(&logger, "test without file");
	log_shutdown(&logger);
}

TEST_F(LogTest, FormatString) {
	log_info(&logger, "formatted: %s %d %f", "hello", 42, 3.14);
}

TEST_F(LogTest, ContentWrittenAndFlushed) {
	log_info(&logger, "hello world");
	log_warn(&logger, "warning");
	log_err(&logger, "oops %d", 99);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 3);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "hello world"));
	EXPECT_TRUE(line_matches(lines[1], "WARN", "warning"));
	EXPECT_TRUE(line_matches(lines[2], "ERROR", "oops 99"));
}

TEST_F(LogTest, MultipleWritesCount) {
	for (int i = 0; i < 100; i++)
		log_info(&logger, "line %d", i);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 100);
	for (int i = 0; i < 100; i++) {
		char expected[32];
		snprintf(expected, sizeof(expected), "line %d", i);
		EXPECT_TRUE(line_matches(lines[i], "INFO", expected));
	}
}

TEST_F(LogTest, FflushBeforeShutdown) {
	log_shutdown(&logger);

	auto fp = std::fopen(tmpfile.c_str(), "w");
	ASSERT_TRUE(fp);
	std::fclose(fp);

	log_init(&logger, tmpfile.c_str(), LOG_INFO);
	log_info(&logger, "before any close");

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 1);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "before any close"));
}

TEST_F(LogTest, InstancesHaveIndependentDestinationsLevelsAndLifetimes)
{
	struct morph_log other{};
	const std::string other_path = tmpfile + ".second";

	ASSERT_EQ(log_init(&other, other_path.c_str(), LOG_WARN), 0);
	log_info(&logger, "first destination");
	log_info(&other, "filtered message");
	log_warn(&other, "second destination");
	log_shutdown(&logger);
	log_err(&other, "still open");
	log_shutdown(&other);
	auto first_lines = read_lines();
	ASSERT_EQ(first_lines.size(), 1u);
	EXPECT_TRUE(line_matches(first_lines[0], "INFO", "first destination"));
	std::ifstream stream(other_path);
	const std::string content((std::istreambuf_iterator<char>(stream)), {});
	EXPECT_EQ(content.find("first destination"), std::string::npos);
	EXPECT_EQ(content.find("filtered message"), std::string::npos);
	EXPECT_NE(content.find("second destination"), std::string::npos);
	EXPECT_NE(content.find("still open"), std::string::npos);
	std::remove(other_path.c_str());
}
