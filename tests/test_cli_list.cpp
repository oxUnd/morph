#include <gtest/gtest.h>
#include "nb/err.h"

extern "C" {
#include "nb/cleanup.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/commands/media.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/services/composer.h"
#include "nb/file.h"
#include "event/event.h"
#include "nb/utf8.h"
#include "cJSON.h"
#include "db/database.h"

}

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <memory>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <atomic>
#include <thread>
#include <termios.h>
#include <unistd.h>

static void resume_terminal_pick(struct cli_application *context, const char *keys,
				int64_t expected, unsigned short rows)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	struct winsize size{};
	size.ws_row = rows;
	size.ws_col = 120;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	struct termios before{}, after{};
	ASSERT_EQ(tcgetattr(slave, &before), 0);
	int saved_input = dup(STDIN_FILENO);
	int saved_output = dup(STDOUT_FILENO);
	ASSERT_GE(saved_input, 0);
	ASSERT_GE(saved_output, 0);
	const char *term = getenv("TERM");
	std::string saved_term = term ? term : "";
	setenv("TERM", "xterm-256color", 1);
	fflush(stdout);
	ASSERT_GE(dup2(slave, STDIN_FILENO), 0);
	ASSERT_GE(dup2(slave, STDOUT_FILENO), 0);
	clearerr(stdin);
	std::string output;
	bool sent = false;
	std::atomic<bool> finished{false};
	std::thread keyboard([&] {
		while (!finished.load()) {
			struct pollfd input{master, POLLIN, 0};
			if (poll(&input, 1, 100) <= 0)
				continue;
			char buffer[4096];
			ssize_t length = read(master, buffer, sizeof(buffer));
			if (length > 0)
				output.append(buffer, static_cast<size_t>(length));
			if (!sent && output.find("Cancel") != std::string::npos) {
				(void)write(master, keys, strlen(keys));
				sent = true;
			}
		}
	});
	int rc = cli_command_dispatch(context, "/resume");
	fflush(stdout);
	int restored = tcgetattr(slave, &after);
	dup2(saved_input, STDIN_FILENO);
	dup2(saved_output, STDOUT_FILENO);
	close(saved_input);
	close(saved_output);
	clearerr(stdin);
	finished.store(true);
	keyboard.join();
	close(slave);
	close(master);
	if (term) setenv("TERM", saved_term.c_str(), 1);
	else unsetenv("TERM");
	EXPECT_TRUE(sent) << output;
	EXPECT_NE(output.find("\033[?1049h"), std::string::npos);
	EXPECT_NE(output.find("\033[H\033[J"), std::string::npos);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(restored, 0);
	const tcflag_t flags = ICANON | ECHO | ISIG | IEXTEN;
	EXPECT_EQ(before.c_lflag & flags, after.c_lflag & flags);
	EXPECT_EQ(before.c_iflag, after.c_iflag);
	struct session current{};
	ASSERT_EQ(runtime_session_current(context->runtime, &current), 0);
	EXPECT_EQ(current.id, expected);
	EXPECT_NE(output.find("SESSION"), std::string::npos);
}

static std::vector<cJSON *> cli_test_parse_ndjson(const std::string &output)
{
	std::vector<cJSON *> events;
	size_t start = 0;

	while (start < output.size()) {
		size_t end = output.find('\n', start);
		std::string line = output.substr(start,
			end == std::string::npos ? std::string::npos : end - start);

		if (!line.empty())
			events.push_back(cJSON_Parse(line.c_str()));
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return events;
}

static void cli_test_delete_events(std::vector<cJSON *> &events)
{
	for (cJSON *event : events)
		cJSON_Delete(event);
	events.clear();
}

static int cli_test_write_png(const char *path)
{
	static const unsigned char png[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
		0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x6b,
		0xc4, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
		0x54, 0x18, 0xd3, 0x63, 0xf8, 0xcf, 0xc0, 0x80,
		0x15, 0x71, 0xd1, 0x41, 0x2b, 0x00, 0x28, 0x3f,
		0x4f, 0xc1, 0x6e, 0xec, 0xdf, 0x61, 0x00, 0x00,
		0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
		0x60, 0x82
	};
	FILE *file = std::fopen(path, "wb");

	if (!file)
		return -1;
	size_t written = std::fwrite(png, 1, sizeof(png), file);
	std::fclose(file);
	return written == sizeof(png) ? 0 : -1;
}

TEST(CliMediaInputTest, RemovesLegacyMediaCommands)
{

	EXPECT_EQ(cli_command_find_borrow("/img"), nullptr);
	EXPECT_EQ(cli_command_find_borrow("/paste"), nullptr);
	EXPECT_EQ(cli_command_find_borrow("/video"), nullptr);
	EXPECT_EQ(cli_command_find_borrow("/vid"), nullptr);
	EXPECT_NE(cli_command_find_borrow("/image"), nullptr);
}

TEST(CliMediaInputTest, QuotedImagePathAttachesWithoutCommand)
{
	char pattern[] = "/tmp/morph-cli-media-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	std::string database;
	std::string image;
	std::string input;
	int handled = 0;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	image = std::string(directory) + "/sample image.png";
	input = "\"" + image + "\"";
	ASSERT_EQ(cli_test_write_png(image.c_str()), 0);
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;

	testing::internal::CaptureStdout();
	int rc = cli_media_handle_path(&context, input.c_str(), &handled);
	(void)testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(handled, 1);
	EXPECT_STREQ(context.image_path, image.c_str());

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, CompactsWhitespaceWithoutBreakingUtf8)
{
	char output[128];

	ASSERT_GT(cli_list_compact_text(
		output, sizeof(output), "  飞书\n\tMarkdown   renderer  "), 0u);
	EXPECT_STREQ(output, "飞书 Markdown renderer");
}

TEST(CliListTest, CompactionRemovesTerminalControlSequences)
{
	char output[128];

	ASSERT_GT(cli_list_compact_text(output, sizeof(output),
		"safe\033[31mred\033[0m\033]52;c;secret\a end"), 0u);
	EXPECT_STREQ(output, "safered end");
	EXPECT_EQ(std::strchr(output, '\033'), nullptr);
}

TEST(CliListTest, UsesConfiguredColumnsWhenStdoutIsNotTerminal)
{
	const char *previous = std::getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	bool had_previous = previous != nullptr;

	ASSERT_EQ(setenv("COLUMNS", "72", 1), 0);
	EXPECT_EQ(cli_list_columns(), 72);
	if (had_previous)
		ASSERT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		ASSERT_EQ(unsetenv("COLUMNS"), 0);
}

TEST(CliListTest, LongItemUsesCompactContinuation)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	testing::internal::CaptureStdout();
	cli_list_item(_cli_output_context_ptr, "", 1, nullptr, "a_very_long_registered_tool_name",
		      "First line\nSecond line with more details", 12, 40);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("a_very_long_registered_tool_name"),
		  std::string::npos);
	EXPECT_NE(output.find("First line Second line"), std::string::npos);
	EXPECT_NE(output.find("…"), std::string::npos);
}

TEST(CliListTest, JsonFieldRendersNestedTree)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	testing::internal::CaptureStdout();
	cli_list_json_field(_cli_output_context_ptr, "Input schema",
			    "{\"type\":\"object\",\"properties\":{"
			    "\"query\":{\"type\":\"string\"}}}",
			    1, 80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Input schema"), std::string::npos);
	EXPECT_NE(output.find("properties"), std::string::npos);
	EXPECT_NE(output.find("query"), std::string::npos);
	EXPECT_NE(output.find("string"), std::string::npos);
}

TEST(CliListTest, SessionRowKeepsRelatedFieldsTogether)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;
	testing::internal::CaptureStdout();
	cli_list_row(_cli_output_context_ptr, "ae2c6f9a", "查一下开放平台、前端架构绩效",
		     "3,086 tokens · 07-29 15:49", 0, 0, 80);
	cli_list_row(_cli_output_context_ptr, "a6f7b14c", "default", "0 tokens · 07-29 15:52", 1, 1,
		     80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("├"), std::string::npos);
	EXPECT_NE(output.find("└"), std::string::npos);
	EXPECT_NE(output.find("ae2c6f9a"), std::string::npos);
	EXPECT_NE(output.find("3,086 tokens"), std::string::npos);
	EXPECT_NE(output.find("07-29 15:49"), std::string::npos);
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_NE(output.find("◯"), std::string::npos);
	EXPECT_EQ(output.find("○"), std::string::npos);
	EXPECT_EQ(output.find("current"), std::string::npos);
	EXPECT_EQ(output.find("Model"), std::string::npos);
}

TEST(CliListTest, ResumePagesAndSelectsWithoutClearingScreen)
{
	char pattern[] = "/tmp/morph-cli-resume-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string database = std::string(directory) + "/data.db";
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	for (int i = 0; i < 22; i++) {
		struct session session{};
		std::string name = "resume-" + std::to_string(i);
		ASSERT_EQ(runtime_session_create_detached(runtime, name.c_str(),
			&session), 0);
	}
	struct session *sessions = nullptr;
	int count = 0;
	ASSERT_EQ(runtime_session_list_query(runtime, &sessions, &count, 0,
		nullptr), 0);
	ASSERT_GT(count, 20);
	int64_t expected = sessions[20].id;
	int64_t arrow_selected = sessions[13].id;
	int64_t small_selected = sessions[2].id;
	runtime_session_list_free(sessions);

	FILE *input = tmpfile();
	ASSERT_NE(input, nullptr);
	cli_output_fputs(&context, "p\ninvalid\nn\np\nn\nn\nn\n10\n1\n", input);
	rewind(input);
	int saved_stdin = dup(STDIN_FILENO);
	ASSERT_GE(saved_stdin, 0);
	ASSERT_GE(dup2(fileno(input), STDIN_FILENO), 0);
	clearerr(stdin);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/resume");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_GE(dup2(saved_stdin, STDIN_FILENO), 0);
	close(saved_stdin);
	fclose(input);
	clearerr(stdin);
	EXPECT_EQ(rc, 0);
	struct session current{};
	ASSERT_EQ(runtime_session_current(runtime, &current), 0);
	EXPECT_EQ(current.id, expected);
	EXPECT_TRUE(std::regex_search(
		output, std::regex("[0-9]{2}-[0-9]{2} "
				   "[0-9]{2}:[0-9]{2}")));
	EXPECT_NE(output.find("page 1/3"), std::string::npos);
	EXPECT_NE(output.find("page 2/3"), std::string::npos);
	EXPECT_NE(output.find("page 3/3"), std::string::npos);
	EXPECT_NE(output.find("already on the first page"), std::string::npos);
	EXPECT_NE(output.find("already on the last page"), std::string::npos);
	EXPECT_NE(output.find("invalid selection"), std::string::npos);
	EXPECT_EQ(output.find("\033[2J"), std::string::npos);
	EXPECT_EQ(output.find("\033[?1049h"), std::string::npos);
	for (const char *answer : {"q\n", "\n", ""}) {
		input = tmpfile();
		ASSERT_NE(input, nullptr);
		cli_output_fputs(&context, answer, input);
		rewind(input);
		saved_stdin = dup(STDIN_FILENO);
		ASSERT_GE(saved_stdin, 0);
		ASSERT_GE(dup2(fileno(input), STDIN_FILENO), 0);
		clearerr(stdin);
		testing::internal::CaptureStdout();
		rc = cli_command_dispatch(&context, "/resume");
		(void)testing::internal::GetCapturedStdout();
		EXPECT_GE(dup2(saved_stdin, STDIN_FILENO), 0);
		close(saved_stdin);
		fclose(input);
		clearerr(stdin);
		EXPECT_EQ(rc, 0);
		ASSERT_EQ(runtime_session_current(runtime, &current), 0);
		EXPECT_EQ(current.id, expected);
	}
	resume_terminal_pick(&context, "\033[B\033[A\033[C\033[B\r", arrow_selected, 24);
	resume_terminal_pick(&context, "\033[B\033[B\033[B\033[A\r", small_selected, 6);
	resume_terminal_pick(&context, "\033", small_selected, 24);
	resume_terminal_pick(&context, "\003", small_selected, 24);

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, ResumeGroupsSessionsByWorkdir)
{
	char pattern[] = "/tmp/morph-cli-resume-groups-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string database = std::string(directory) + "/data.db";
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	struct db db{};
	struct session current_session{};
	struct session other_session{};
	struct session legacy_session{};

	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	ASSERT_EQ(db_open(nullptr, &db, database.c_str()), 0);
	ASSERT_EQ(session_create_with_workdir(&db, "in-project", "test",
		runtime_workdir_borrow(runtime), &current_session), 0);
	ASSERT_EQ(session_create_with_workdir(&db, "other-project", "test",
		"/tmp/cli-resume-other", &other_session), 0);
	ASSERT_EQ(session_create_with_workdir(&db, "legacy-session", "test", "",
		&legacy_session), 0);
	db_close(&db);

	FILE *input = tmpfile();
	ASSERT_NE(input, nullptr);
	cli_output_fputs(&context, "q\n", input);
	rewind(input);
	int saved_stdin = dup(STDIN_FILENO);
	ASSERT_GE(saved_stdin, 0);
	ASSERT_GE(dup2(fileno(input), STDIN_FILENO), 0);
	clearerr(stdin);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/resume");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_GE(dup2(saved_stdin, STDIN_FILENO), 0);
	close(saved_stdin);
	fclose(input);
	clearerr(stdin);
	EXPECT_EQ(rc, 0);

	size_t current = output.find("current");
	size_t other = output.find("/tmp/cli-resume-other");
	size_t legacy = output.find("unknown workdir");

	ASSERT_NE(current, std::string::npos) << output;
	ASSERT_NE(other, std::string::npos) << output;
	ASSERT_NE(legacy, std::string::npos) << output;
	EXPECT_LT(current, other);
	EXPECT_LT(current, legacy);
	EXPECT_NE(output.find("other-project"), std::string::npos);
	EXPECT_NE(output.find("legacy-session"), std::string::npos);

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, QuitPrintsResumeCommandsOnlyForSavedInteractiveSessions)
{
	char pattern[] = "/tmp/morph-cli-quit-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string database = std::string(directory) + "/data.db";
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	options.create_new_session = 1;
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	context.running = 1;
	cli_output_set_color_enabled(&context, 0);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/quit");
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(context.running, 0);
	EXPECT_NE(output.find("Goodbye!"), std::string::npos);
	EXPECT_EQ(output.find("To resume"), std::string::npos);
	struct session session{};
	ASSERT_EQ(runtime_session_create_and_select(runtime, "quit-test", &session), 0);
	for (auto mode : {CLI_PRESENT_INTERACTIVE, CLI_PRESENT_ONCE_PLAIN,
			 CLI_PRESENT_EVENTS_JSON}) {
		context.running = 1;
		context.presentation_mode = mode;
		context.event_cb = cli_runtime_events_callback;
		context.event_user_data = &context;
		testing::internal::CaptureStdout();
		rc = cli_command_dispatch(&context, "/q");
		output = testing::internal::GetCapturedStdout();
		EXPECT_EQ(rc, 0);
		EXPECT_EQ(context.running, 0);
		EXPECT_NE(output.find("Goodbye!"), std::string::npos);
		if (mode == CLI_PRESENT_INTERACTIVE) {
			std::string reference = std::string("resume ") + session.display_id;
			EXPECT_NE(output.find("morph " + reference), std::string::npos);
			EXPECT_NE(output.find("morphctl " + reference), std::string::npos);
			EXPECT_NE(output.find(std::string("-w ") + runtime_workdir_borrow(runtime)),
				  std::string::npos);
		} else {
			EXPECT_EQ(output.find("To resume"), std::string::npos);
			EXPECT_EQ(output.find("morphctl"), std::string::npos);
		}
		if (mode == CLI_PRESENT_EVENTS_JSON) {
			auto events = cli_test_parse_ndjson(output);
			EXPECT_EQ(events.size(), 2u);
			cli_test_delete_events(events);
		}
	}
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, HelpUsesTreeLayout)
{
	struct cli_application _cli_output_context = {0};
	struct cli_application *_cli_output_context_ptr = &_cli_output_context;

	testing::internal::CaptureStdout();
	cli_print_help(_cli_output_context_ptr);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Commands"), std::string::npos);
	EXPECT_NE(output.find("├"), std::string::npos);
	EXPECT_NE(output.find("└"), std::string::npos);
	EXPECT_NE(output.find("Core"), std::string::npos);
	EXPECT_NE(output.find("/help"), std::string::npos);
	EXPECT_NE(output.find("/resume"), std::string::npos);
	EXPECT_EQ(cli_command_find_borrow("/list"), nullptr);
	EXPECT_EQ(cli_command_find_borrow("/ls"), nullptr);
	EXPECT_EQ(output.find("/list"), std::string::npos);
	EXPECT_EQ(output.find("/ls"), std::string::npos);

}

TEST(CliListTest, JsonModeWrapsHelpAsCommandEvents)
{
	struct cli_application context{};

	context.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	context.event_cb = cli_runtime_events_callback;
	context.event_user_data = &context;

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/help");
	std::string output = testing::internal::GetCapturedStdout();
	std::vector<cJSON *> events = cli_test_parse_ndjson(output);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(events.size(), 2u);
	ASSERT_NE(events[0], nullptr);
	ASSERT_NE(events[1], nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[0], "type")), "command");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[0], "name")), "command.started");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[1], "name")), "command.completed");
	cJSON *data = cJSON_GetObjectItem(events[1], "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	const char *captured = cJSON_GetStringValue(
		cJSON_GetObjectItem(data, "output"));
	ASSERT_NE(captured, nullptr);
	EXPECT_NE(std::string(captured).find("Commands"), std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);

	cli_test_delete_events(events);

}

TEST(CliListTest, JsonModeReportsUnknownCommandWithoutBareText)
{
	struct cli_application context{};

	context.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	context.event_cb = cli_runtime_events_callback;
	context.event_user_data = &context;

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/does-not-exist");
	std::string output = testing::internal::GetCapturedStdout();
	std::vector<cJSON *> events = cli_test_parse_ndjson(output);

	ASSERT_EQ(rc, NB_ERR_NOT_FOUND);
	ASSERT_EQ(events.size(), 2u);
	ASSERT_NE(events[0], nullptr);
	ASSERT_NE(events[1], nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[1], "name")), "command.failed");
	cJSON *data = cJSON_GetObjectItem(events[1], "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(data, "error_code")), NB_ERR_NOT_FOUND);
	const char *captured = cJSON_GetStringValue(
		cJSON_GetObjectItem(data, "output"));
	ASSERT_NE(captured, nullptr);
	EXPECT_NE(std::string(captured).find("unknown command"),
		  std::string::npos);

	cli_test_delete_events(events);
}

TEST(CliListTest, SyncQueriesUseGroupedTrees)
{
	const char *configured_columns = getenv("COLUMNS");
	std::string saved_columns = configured_columns ? configured_columns : "";
	bool had_columns = configured_columns != nullptr;
	ASSERT_EQ(setenv("COLUMNS", "120", 1), 0);
	char pattern[] = "/tmp/morph-cli-sync-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	struct morph_sync_backup *backups = nullptr;
	int backup_count = 0;
	std::string root;
	std::string database;
	std::string config_path;
	std::string remote;
	std::ofstream config;
	std::ofstream payload;

	ASSERT_NE(directory, nullptr);
	root = directory;
	database = root + "/data.db";
	config_path = root + "/config.toml";
	remote = root + "/remote";
	config.open(config_path);
	ASSERT_TRUE(config.is_open());
	config << "[sync]\n"
	       << "enabled = true\n"
	       << "dir = \"" << remote << "\"\n"
	       << "interval_seconds = 0\n"
	       << "retention_days = 30\n"
	       << "include = [\"data.db\", \"payload.txt\"]\n";
	config.close();
	ASSERT_TRUE(config.good());
	payload.open(root + "/payload.txt");
	payload << "first";
	payload.close();
	ASSERT_TRUE(payload.good());
	options.db_path = database.c_str();
	options.config_path = config_path.c_str();
	options.workdir = root.c_str();
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/sync now");
	std::string status_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(status_output.find("sync status"), std::string::npos);
	EXPECT_NE(status_output.find("state"), std::string::npos);
	EXPECT_NE(status_output.find("files"), std::string::npos);
	EXPECT_NE(status_output.find("database"), std::string::npos);
	EXPECT_NE(status_output.find("├"), std::string::npos);
	EXPECT_NE(status_output.find("└"), std::string::npos);
	EXPECT_NE(status_output.find("snapshots"), std::string::npos);

	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	std::string conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (0)"), std::string::npos);
	EXPECT_NE(conflict_output.find("└ none"), std::string::npos);
	payload.open(root + "/payload.txt", std::ios::trunc);
	payload << "local change";
	payload.close();
	payload.open(remote + "/data/payload.txt", std::ios::trunc);
	payload << "remote change";
	payload.close();
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync now");
	(void)testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (1)"), std::string::npos);
	EXPECT_NE(conflict_output.find("#1"), std::string::npos);
	EXPECT_NE(conflict_output.find("payload.txt"), std::string::npos);

	ASSERT_EQ(runtime_sync_backups(runtime, "data.db", &backups,
		&backup_count), 0);
	ASSERT_EQ(backup_count, 1);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync backups");
	std::string backup_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(backup_output.find("database backups (1)"),
		  std::string::npos);
	EXPECT_NE(backup_output.find("data.db"), std::string::npos);
	/* Snapshot names now obey the same viewport bound as other tree labels. */
	EXPECT_NE(backup_output.find(std::string(backups[0].snapshot_id).substr(0, 20)),
		  std::string::npos);
	if (strlen(backups[0].snapshot_id) > (size_t)(cli_list_columns() - 6))
		EXPECT_NE(backup_output.find("…"), std::string::npos);
	EXPECT_NE(backup_output.find("created"), std::string::npos);
	std::string restore_command = "/sync restore-db " +
		std::string(backups[0].snapshot_id) + " --yes";
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, restore_command.c_str());
	std::string restore_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_EQ(context.pending_db_restore, 1);
	EXPECT_EQ(context.running, 0);
	EXPECT_NE(restore_output.find("restore prepared"), std::string::npos);
	morph_sync_backups_free(backups);
	runtime_close(runtime);
	runtime = nullptr;
	ASSERT_EQ(morph_sync_apply_db_replace(&context.db_restore_plan), 0);
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	EXPECT_FALSE(nb_file_exists(context.db_restore_plan.journal));

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(root, error);
	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved_columns.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
}

TEST(CliListTest, TasksCommandUsesActiveAndCompactHistoryTrees)
{
	char pattern[] = "/tmp/morph-cli-tasks-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_application context{};
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task active NB_CLEANUP(scheduled_task_cleanup) = {};
	struct scheduled_task history NB_CLEANUP(scheduled_task_cleanup) = {};
	std::string database;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	ASSERT_EQ(runtime_session_current(runtime, &current), 0);
	context.runtime = runtime;
	input.source_session_id = current.id;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) + 3600;
	input.max_attempts = 3;
	input.action_type = "agent_run";
	input.policy_json = "{}";
	input.notify_json = "{}";
	input.title = "active task";
	input.payload_json = "{\"prompt\":\"active task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &active), 0);
	input.title = "old task";
	input.payload_json = "{\"prompt\":\"old task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &history), 0);
	ASSERT_EQ(runtime_task_cancel(runtime, history.id), 0);

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/tasks");
	std::string output = testing::internal::GetCapturedStdout();

	ASSERT_EQ(rc, 0);
	EXPECT_NE(output.find("scheduled tasks (2)"), std::string::npos);
	EXPECT_NE(output.find("active"), std::string::npos);
	EXPECT_NE(output.find("history"), std::string::npos);
	EXPECT_NE(output.find("active task details"), std::string::npos);
	EXPECT_NE(output.find("old task"), std::string::npos);
	EXPECT_NE(output.find("cancelled"), std::string::npos);
	EXPECT_EQ(output.find("| ID |"), std::string::npos);

	scheduled_task_cleanup(&active);
	scheduled_task_cleanup(&history);

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, CommandArgumentOverflowIsExplicitForFixedConsumers)
{
	char input[] = "one two three";
	char *arguments[3] = {};
	EXPECT_EQ(cli_argv_split(input, arguments, 3), NB_ERR_TOO_BIG);
	EXPECT_EQ(arguments[2], nullptr);
}

TEST(CliListTest, ScheduledTaskCommandsPreserveLongPromptsAndAllArguments)
{
	char pattern[] = "/tmp/morph-cli-long-task-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string database = std::string(directory) + "/data.db";
	struct runtime_options options = {};
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	struct runtime *runtime = nullptr;
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	std::unique_ptr<struct runtime, decltype(&runtime_close)> runtime_owner(runtime,
	runtime_close);
	struct cli_application context = {};
	context.runtime = runtime;
	std::string prompt;
	for (int i = 0; i < 300; i++)
		prompt += "word-" + std::to_string(i) + " detailed requirement 中文 ";
	prompt += "PROMPT_END_MARKER";
	ASSERT_GT(prompt.size(), 8192u);
	for (const char *action : {"add +3600", "every 3600"}) {
		std::string command = std::string("/tasks ") + action + " " + prompt;
		testing::internal::CaptureStdout();
		int rc = cli_command_dispatch(&context, command.c_str());
		(void)testing::internal::GetCapturedStdout();
		ASSERT_EQ(rc, 0);
	}
	struct scheduled_task *tasks = nullptr;
	int count = 0;
	ASSERT_EQ(runtime_task_list(runtime, nullptr, 10, &tasks, &count), 0);
	auto free_tasks = [count](struct scheduled_task *items) {
		scheduled_task_free_list(items, count);
	};
	std::unique_ptr<struct scheduled_task, decltype(free_tasks)> tasks_owner(tasks, free_tasks);
	ASSERT_EQ(count, 2);
	int64_t task_id = tasks[0].id;
	for (int i = 0; i < count; i++) {
		cJSON *payload NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tasks[i].payload_json);
		ASSERT_NE(payload, nullptr);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload,
		"prompt")),
			     prompt.c_str());
		EXPECT_LT(strlen(tasks[i].title), static_cast<size_t>(SCHEDULED_TASK_TEXT_MAX));
		EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr(tasks[i].title), NULL), NB_UTF8_OK);
	}
	tasks_owner.reset();
	std::string updated = prompt + " UPDATED_END_MARKER";
	std::string command = "/tasks update " + std::to_string(task_id) + " +7200 " + updated;
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, command.c_str());
	(void)testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	struct scheduled_task task NB_CLEANUP(scheduled_task_cleanup) = {};
	ASSERT_EQ(runtime_task_copy(runtime, task_id, &task), 0);
	cJSON *payload NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(task.payload_json);
	ASSERT_NE(payload, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "prompt")),
		     updated.c_str());
	runtime_owner.reset();
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliMediaInputTest, ComposerKeepsImageReferencesAndAtomicBoundaries)
{
	char pattern[] = "/tmp/morph-composer-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string path = std::string(directory) + "/目录 image.png";
	ASSERT_EQ(cli_test_write_png(path.c_str()), 0);
	struct cli_composer_model composer;
	ASSERT_EQ(cli_composer_model_init(&composer), 0);
	const char *label;
	ASSERT_EQ(cli_composer_service_add_image(&composer, path.c_str(), &label), 0);
	EXPECT_STREQ(label, "[IMAGE#1]");
	ASSERT_EQ(cli_composer_service_add_image(&composer, path.c_str(), &label), 0);
	EXPECT_STREQ(label, "[IMAGE#2]");
	char *expanded = cli_composer_model_expand_copy(&composer, "只看 [IMAGE#2]");
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(std::string(expanded).find(path), std::string::npos);
	EXPECT_EQ(std::string(expanded).find("IMAGE#1"), std::string::npos);
	free(expanded);
	int start;
	int end;
	ASSERT_EQ(cli_composer_model_image_span(&composer, "a[IMAGE#2]b", 10, 1,
					  &start, &end), 1);
	EXPECT_EQ(start, 1);
	EXPECT_EQ(end, 10);
	EXPECT_EQ(cli_composer_model_image_span(&composer, "a[IMAGE#2]b", 10, 0,
					 &start, &end), 0);
	std::string escaped = path;
	escaped.replace(escaped.find(' '), 1, "\\ ");
	std::string input = "before\n\t\"" + path + "\"  " + escaped + "\nafter";
	char *converted = nullptr;
	ASSERT_EQ(cli_composer_service_convert_paths_copy(&composer, input.c_str(), &converted), 2);
	ASSERT_NE(converted, nullptr);
	char *saved = converted;
	EXPECT_EQ(cli_composer_service_convert_paths_copy(&composer, "", &converted),
		  NB_ERR_EXISTS);
	EXPECT_EQ(converted, saved);
	EXPECT_STREQ(converted, "before\n\t[IMAGE#3]  [IMAGE#4]\nafter");
	free(converted);
	cli_composer_model_cleanup(&composer);
	std::filesystem::remove_all(directory);
}
