#include <gtest/gtest.h>
#include "nb/log.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <thread>

extern "C" {
int error_probe_return(struct nb_log *, int, int *, int *);
int error_probe_set(struct nb_log *, int, int *);
int error_probe_errno(struct nb_log *, int);
int error_probe_without_context(int);
long long error_probe_wide(void);
int error_probe_local_name(void);
int error_probe_assignment(void);
int error_release_probe_return(struct nb_log *, int, int *, int *);
int error_release_probe_set(struct nb_log *, int, int *);
int error_release_probe_errno(struct nb_log *, int);
int error_release_probe_without_context(int);
long long error_release_probe_wide(void);
int error_release_probe_local_name(void);
int error_release_probe_assignment(void);
}

#include "errors.h"

class LogTest : public ::testing::Test {
protected:
  struct nb_log *logger = nullptr;
  void SetUp() override
  {
	  char tmpl[] = "/tmp/morph_log_test_XXXXXX";
	  int fd = mkstemp(tmpl);
	  ASSERT_NE(fd, -1);
	  close(fd);
	  tmpfile = tmpl;
	  (logger = nb_log_new_file(NB_LOG_DEBUG, tmpfile.c_str(), 5 * 1024 * 1024, 3));
  }
	void TearDown() override {
		(nb_log_free(logger), logger = nullptr);
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
		/* Format: timestamp [LEVEL] message. */
		auto lpos = line.find("[INFO]");
		if (!level) {
			lpos = line.find('[');
			if (lpos == std::string::npos) return false;
			lpos = line.find(']', lpos);
			if (lpos == std::string::npos) return false;
			lpos = line.find(']', lpos + 1);
			if (lpos == std::string::npos) return false;
			lpos += 2; /* Skip "] ". */
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
	EXPECT_NO_FATAL_FAILURE((nb_log_free(logger), logger = nullptr));
	(logger = nb_log_new_file(NB_LOG_INFO, tmpfile.c_str(), 5 * 1024 * 1024, 3));
}

TEST_F(LogTest, WriteAll) {
	nb_log_write(logger, NB_LOG_INFO, "test message %d", 42);
	nb_log_write(logger, NB_LOG_WARN, "warning message");
	nb_log_write(logger, NB_LOG_ERROR, "error message");
}

TEST_F(LogTest, WriteDebug) {
	nb_log_write(logger, NB_LOG_DEBUG, "debug message");
}

TEST_F(LogTest, LevelFiltering) {
	nb_log_set_level(logger, NB_LOG_WARN);
	nb_log_write(logger, NB_LOG_INFO, "should not appear");
	nb_log_write(logger, NB_LOG_WARN, "should appear");
	nb_log_write(logger, NB_LOG_ERROR, "should also appear");
	nb_log_set_level(logger, NB_LOG_DEBUG);
}

TEST_F(LogTest, LevelStrings) {
	nb_log_write(logger, NB_LOG_DEBUG, "debug");
	nb_log_write(logger, NB_LOG_INFO, "info");
	nb_log_write(logger, NB_LOG_WARN, "warn");
	nb_log_write(logger, NB_LOG_ERROR, "error");
}

TEST_F(LogTest, NullFile) {
	(nb_log_free(logger), logger = nullptr);
	(logger = nb_log_new_file(NB_LOG_INFO, nullptr, 5 * 1024 * 1024, 3));
	nb_log_write(logger, NB_LOG_INFO, "test without file");
	(nb_log_free(logger), logger = nullptr);
}

TEST_F(LogTest, FormatString) {
	nb_log_write(logger, NB_LOG_INFO, "formatted: %s %d %f", "hello", 42, 3.14);
}

TEST_F(LogTest, ContentWrittenAndFlushed) {
	nb_log_write(logger, NB_LOG_INFO, "hello world");
	nb_log_write(logger, NB_LOG_WARN, "warning");
	nb_log_write(logger, NB_LOG_ERROR, "oops %d", 99);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 3);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "hello world"));
	EXPECT_TRUE(line_matches(lines[1], "WARN", "warning"));
	EXPECT_TRUE(line_matches(lines[2], "ERROR", "oops 99"));
}

TEST_F(LogTest, MultipleWritesCount) {
	for (int i = 0; i < 100; i++)
		nb_log_write(logger, NB_LOG_INFO, "line %d", i);

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 100);
	for (int i = 0; i < 100; i++) {
		char expected[32];
		snprintf(expected, sizeof(expected), "line %d", i);
		EXPECT_TRUE(line_matches(lines[i], "INFO", expected));
	}
}

TEST_F(LogTest, FflushBeforeShutdown) {
	(nb_log_free(logger), logger = nullptr);

	auto fp = std::fopen(tmpfile.c_str(), "w");
	ASSERT_TRUE(fp);
	std::fclose(fp);

	(logger = nb_log_new_file(NB_LOG_INFO, tmpfile.c_str(), 5 * 1024 * 1024, 3));
	nb_log_write(logger, NB_LOG_INFO, "before any close");

	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 1);
	EXPECT_TRUE(line_matches(lines[0], "INFO", "before any close"));
}

TEST_F(LogTest, InstancesHaveIndependentDestinationsLevelsAndLifetimes)
{
	struct nb_log *other = nullptr;
	const std::string other_path = tmpfile + ".second";

	ASSERT_NE((other = nb_log_new_file(NB_LOG_WARN, other_path.c_str(), 5 * 1024 * 1024, 3)),
		  nullptr);
	nb_log_write(logger, NB_LOG_INFO, "first destination");
	nb_log_write(other, NB_LOG_INFO, "filtered message");
	nb_log_write(other, NB_LOG_WARN, "second destination");
	(nb_log_free(logger), logger = nullptr);
	nb_log_write(other, NB_LOG_ERROR, "still open");
	nb_log_free(other);
	other = nullptr;
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

TEST_F(LogTest, ExplicitErrorsPreserveCodesAndLocationsWithoutTerminalOutput)
{
	int code_evaluations = 0, logger_evaluations = 0, set_evaluations = 0;
	testing::internal::CaptureStdout();
	testing::internal::CaptureStderr();
	int returned =
		error_probe_return(logger, MORPH_ERR_DB, &code_evaluations, &logger_evaluations);
	int assigned = error_probe_set(logger, NB_ERR_PROTOCOL, &set_evaluations);
	std::string errors = testing::internal::GetCapturedStderr();
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(returned, MORPH_ERR_DB);
	EXPECT_EQ(assigned, NB_ERR_PROTOCOL);
	EXPECT_EQ(code_evaluations, 1);
	EXPECT_EQ(set_evaluations, 1);
	EXPECT_EQ(logger_evaluations, 1);
	EXPECT_TRUE(errors.empty()) << errors;
	EXPECT_TRUE(output.empty()) << output;
	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 2u);
	{
		EXPECT_NE(lines[0].find("[DEBUG] Error trace: database error"), std::string::npos);
		EXPECT_NE(lines[1].find("Error trace: protocol error"), std::string::npos);
		for (const auto &line : lines)
			EXPECT_NE(line.find("error_macro_probe.c:"), std::string::npos);
	}
}

TEST_F(LogTest, MissingContextSuccessAndFilteredDiagnosticsStaySilent)
{
	int codes = 0, contexts = 0;
	testing::internal::CaptureStderr();
	int returned = error_probe_without_context(0);
	int assigned = error_probe_without_context(1);
	int success = error_probe_return(logger, 0, &codes, &contexts);
	nb_log_write(nullptr, NB_LOG_DEBUG, "unbound diagnostic");
	nb_log_set_level(logger, NB_LOG_INFO);
	nb_log_write(logger, NB_LOG_DEBUG, "filtered diagnostic");
	std::string errors = testing::internal::GetCapturedStderr();
	EXPECT_TRUE(errors.empty()) << errors;
	EXPECT_EQ(returned, MORPH_ERR_DB);
	EXPECT_EQ(assigned, MORPH_ERR_DB);
	EXPECT_EQ(success, 0);
	EXPECT_EQ(codes, 1);
	EXPECT_EQ(contexts, 1);
	EXPECT_TRUE(read_lines().empty());
}

TEST_F(LogTest, ErrnoDiagnosticsPreserveTheOriginalErrnoAndFallback)
{
	for (int assign : {0, 1}) {
		errno = ENOENT;
		int result = error_probe_errno(logger, assign);
		int saved_errno = errno;
		EXPECT_EQ(result, NB_ERR_NOT_FOUND);
		EXPECT_EQ(saved_errno, ENOENT);
		errno = 0;
		result = error_probe_errno(logger, assign);
		saved_errno = errno;
		EXPECT_EQ(result, NB_ERR_IO);
		EXPECT_EQ(saved_errno, 0);
	}
	int codes = 0, contexts = 0;
	errno = ERANGE;
	int unknown = error_probe_return(logger, -9999, &codes, &contexts);
	int saved_errno = errno;
	EXPECT_EQ(unknown, -9999);
	EXPECT_EQ(saved_errno, ERANGE);
}

TEST_F(LogTest, ConcurrentDiagnosticsUseExplicitOwnersAndSurviveOtherShutdown)
{
	struct nb_log *other = nullptr;
	const std::string other_path = tmpfile + ".diagnostics";
	ASSERT_NE((other = nb_log_new_file(NB_LOG_DEBUG, other_path.c_str(), 5 * 1024 * 1024, 3)),
		  nullptr);
	testing::internal::CaptureStderr();
	std::thread first([this] {
		for (int i = 0; i < 20; i++)
			nb_log_write(logger, NB_LOG_DEBUG, "first diagnostic %d", i);
	});
	std::thread second([&other] {
		for (int i = 0; i < 20; i++)
			nb_log_write(other, NB_LOG_DEBUG, "second diagnostic %d", i);
	});
	first.join();
	second.join();
	(nb_log_free(logger), logger = nullptr);
	nb_log_write(logger, NB_LOG_DEBUG, "closed owner");
	nb_log_write(other, NB_LOG_DEBUG, "other owner still active");
	nb_log_free(other);
	other = nullptr;
	std::string errors = testing::internal::GetCapturedStderr();
	EXPECT_TRUE(errors.empty()) << errors;
	auto lines = read_lines();
	ASSERT_EQ(lines.size(), 20u);
	for (const auto &line : lines)
		EXPECT_NE(line.find("first diagnostic"), std::string::npos);
	std::ifstream stream(other_path);
	const std::string content((std::istreambuf_iterator<char>(stream)), {});
	EXPECT_EQ(content.find("first diagnostic"), std::string::npos);
	EXPECT_NE(content.find("second diagnostic 19"), std::string::npos);
	EXPECT_NE(content.find("other owner still active"), std::string::npos);
	std::remove(other_path.c_str());
}

TEST_F(LogTest, ExplicitErrorControlFlowMatchesAcrossBuildModes)
{
	for (auto probe : {error_probe_return, error_release_probe_return}) {
		for (int code : {0, 5, static_cast<int>(MORPH_ERR_DB)}) {
			int codes = 0, contexts = 0;
			EXPECT_EQ(probe(logger, code, &codes, &contexts), code);
			EXPECT_EQ(codes, 1);
			EXPECT_EQ(contexts, 1);
		}
	}
	for (auto probe : {error_probe_set, error_release_probe_set}) {
		int evaluations = 0;
		EXPECT_EQ(probe(logger, MORPH_ERR_DB, &evaluations), MORPH_ERR_DB);
		EXPECT_EQ(evaluations, 1);
	}
	for (auto probe : {error_probe_errno, error_release_probe_errno}) {
		for (int assign : {0, 1}) {
			errno = 0;
			EXPECT_EQ(probe(logger, assign), NB_ERR_IO);
			errno = ENOENT;
			EXPECT_EQ(probe(logger, assign), NB_ERR_NOT_FOUND);
		}
	}
	EXPECT_EQ(error_probe_wide(), 4294967296LL);
	EXPECT_EQ(error_release_probe_wide(), 4294967296LL);
	EXPECT_EQ(error_probe_local_name(), -5);
	EXPECT_EQ(error_release_probe_local_name(), -5);
	EXPECT_EQ(error_probe_assignment(), -5);
	EXPECT_EQ(error_release_probe_assignment(), -5);
	EXPECT_EQ(error_probe_without_context(0), MORPH_ERR_DB);
	EXPECT_EQ(error_release_probe_without_context(0), MORPH_ERR_DB);
}
