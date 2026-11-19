#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/adapters/input/state.h"

#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/application.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/jobs.h"
#include "sapi/cli/models/jobs.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/size.h>
}

#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

static std::string collect_terminal_output(int master)
{
	std::string bytes;
	char chunk[BUFSIZ];
	for (;;) {
		struct pollfd pending = {master, POLLIN, 0};
		int ready = poll(&pending, 1, bytes.empty() ? 1000 : 20);
		if (ready < 0 && errno == EINTR)
			continue;

		EXPECT_GE(ready, 0);
		if (ready <= 0)
			break;

		ssize_t count = read(master, chunk, sizeof(chunk));
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		EXPECT_GT(count, 0);
		if (count <= 0)
			break;

		bytes.append(chunk, static_cast<size_t>(count));
	}

	return bytes;
}

class CliTerminalTest : public ::testing::Test
{
      protected:
	struct cli_application ctx{};

	void SetUp() override
	{
		ASSERT_EQ(cli_input_init(&ctx), 0);
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
	}

	void TearDown() override
	{
		cli_presentation_cleanup(&ctx);
		cli_input_cleanup(&ctx);
		cli_jobs_model_cleanup(ctx.jobs);
		ctx.jobs = NULL;
		cli_feed_cleanup(&ctx);
		cli_output_set_color_enabled(&ctx, 1);
	}
};

TEST_F(CliTerminalTest, TerminalColumnsValidateEnvironmentAndPreserveCallerFallback)
{
	std::unique_ptr<FILE, decltype(&fclose)> output(tmpfile(), fclose);
	ASSERT_NE(output, nullptr);

	const char *configured = getenv("COLUMNS");
	const bool had_columns = configured != nullptr;
	const std::string saved = configured ? configured : "";
	const struct {
		const char *value;
		int expected;
	} cases[] = {{"60", 60},
		     {"0", 100},
		     {"-1", 100},
		     {"80x", 100},
		     {"999999999999999999999999", 100},
		     {"", 100}};

	for (const auto &item : cases) {
		EXPECT_EQ(setenv("COLUMNS", item.value, 1), 0);
		EXPECT_EQ(nt_terminal_columns(fileno(output.get()), 100), item.expected)
			<< item.value;
	}

	if (had_columns)
		EXPECT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		EXPECT_EQ(unsetenv("COLUMNS"), 0);
}

TEST_F(CliTerminalTest, NonTerminalStatusPrintsOnlyWhenChanged)
{
	cli_output_set_color_enabled(&ctx, 0);
	ASSERT_EQ(cli_feed_init(&ctx, stdout, -1), 0);

	testing::internal::CaptureStdout();
	cli_feed_live_set(&ctx, "Thinking…");
	cli_feed_live_set(&ctx, "Thinking…");
	cli_feed_live_set(&ctx, "Running tool…");
	cli_feed_live_set(&ctx, "unsafe\nstatus");
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(output.find("Thinking…"), output.rfind("Thinking…"));
	EXPECT_NE(output.find("Running tool…"), std::string::npos);
	EXPECT_NE(output.find("unsafe status"), std::string::npos);
	EXPECT_EQ(output.find("unsafe\nstatus"), std::string::npos);
	EXPECT_EQ(cli_feed_live_active(&ctx), 1);
	cli_feed_live_clear(&ctx);
	EXPECT_EQ(cli_feed_live_active(&ctx), 0);
}

TEST_F(CliTerminalTest, TtyViewportCoalescesFramesAroundHistory)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 24;
	size.ws_row = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_output_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);

	cli_feed_live_set(&ctx, "first viewport frame");
	char bytes[BUFSIZ];
	ssize_t count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string first(bytes);
	EXPECT_NE(first.find("first viewport"), std::string::npos);
	EXPECT_NE(first.find("\033[2K"), std::string::npos);

	cli_feed_live_set(&ctx, "coalesced update");
	errno = 0;
	count = read(master, bytes, sizeof(bytes));
	EXPECT_EQ(count, -1);
	EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

	cli_feed_render_frame(&ctx, 1);
	cli_feed_history_begin(&ctx);
	fprintf(output, "committed history\n");
	fflush(output);
	cli_feed_history_end(&ctx);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	std::string restored(bytes);
	size_t history = restored.find("committed history");
	ASSERT_NE(history, std::string::npos);
	EXPECT_LT(restored.find("coalesced update"), history);
	EXPECT_NE(restored.find("coalesced update", history), std::string::npos);

	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, TtyViewportRendersAndConsumesPromptQueue)
{
	struct cli_command_job job{};
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	const char *slave_name = ptsname(master);
	ASSERT_NE(slave_name, nullptr);
	int slave = open(slave_name, O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 48;
	size.ws_row = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ASSERT_EQ(cli_command_job_init(&job), 0);
	ctx.input_job = &job;
	ASSERT_EQ(cli_command_job_prompt(&job, "first adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "second adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "third adjustment", nullptr), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "fourth adjustment", nullptr), 0);
	cli_output_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);

	cli_feed_queue_changed(&ctx);
	std::string queue_only = collect_terminal_output(master);
	ASSERT_FALSE(queue_only.empty());
	EXPECT_NE(queue_only.find("first adjustment"), std::string::npos);
	EXPECT_EQ(queue_only.find("Thinking…"), std::string::npos);

	cli_feed_live_set(&ctx, "Thinking…");
	cli_feed_render_frame(&ctx, 1);
	std::string queued = collect_terminal_output(master);
	ASSERT_FALSE(queued.empty());
	size_t first = queued.find("first adjustment");
	size_t second = queued.find("second adjustment");
	size_t status = queued.find("Thinking…");
	ASSERT_NE(first, std::string::npos);
	ASSERT_NE(second, std::string::npos);
	ASSERT_NE(status, std::string::npos);
	EXPECT_LT(first, second);
	EXPECT_LT(status, queued.find("Messages queued"));
	EXPECT_LT(queued.find("Messages queued"), first);
	EXPECT_NE(queued.find("├ first adjustment"), std::string::npos);
	EXPECT_NE(queued.find("+1 more queued"), std::string::npos);
	EXPECT_LT(second, queued.find("third adjustment"));
	EXPECT_EQ(queued.find("fourth adjustment"), std::string::npos);

	char *consumed = cli_command_job_take_prompt(&job, nullptr);
	ASSERT_NE(consumed, nullptr);
	EXPECT_STREQ(consumed, "first adjustment");
	free(consumed);
	cli_feed_render_frame(&ctx, 1);
	std::string redrawn = collect_terminal_output(master);
	ASSERT_FALSE(redrawn.empty());
	EXPECT_EQ(redrawn.find("first adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("second adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("third adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("fourth adjustment"), std::string::npos);
	EXPECT_NE(redrawn.find("\033[1A"), std::string::npos);

	ctx.input_job = nullptr;
	cli_feed_cleanup(&ctx);
	cli_command_job_cleanup(&job);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, IndependentActivityOverridesToolAndKeepsTimerAfterResize)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ctx.presentation_ready = 1;
	ctx.turn_active = 1;
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	cJSON *data = cJSON_Parse("{\"tool\":\"exec\",\"tool_call_id\":\"active\",\"args\":{"
				  "\"command\":\"sleep 30\"}}");
	struct morph_event event{MORPH_EVENT_TOOL, "tool.call", "begin", nullptr, data, "turn"};
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_presentation_event(&ctx, &event), 0);
	(void)testing::internal::GetCapturedStdout();
	char bytes[BUFSIZ];
	while (read(master, bytes, sizeof(bytes)) > 0) {}

	cli_feed_activity_set(&ctx, "Compacting context…");
	cli_feed_render_frame(&ctx, 1);
	ssize_t count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("Compacting context…  0s"), std::string::npos);
	EXPECT_EQ(std::string(bytes).find("exec"), std::string::npos);
	usleep(1100000);
	size.ws_col = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_feed_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("1s"), std::string::npos);
	EXPECT_GE(cli_feed_activity_elapsed_ms(&ctx), 1000);
	size.ws_col = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	EXPECT_EQ(cli_feed_columns(&ctx), 12);
	cli_feed_render_frame(&ctx, 1);
	count = read(master, bytes, sizeof(bytes) - 1);
	ASSERT_GT(count, 0);
	bytes[count] = '\0';
	EXPECT_NE(std::string(bytes).find("Widen term…"), std::string::npos);
	EXPECT_EQ(std::string(bytes).find('\n'), std::string::npos);
	cli_feed_live_clear(&ctx);
	EXPECT_EQ(cli_feed_activity_elapsed_ms(&ctx), -1);
	cJSON_Delete(data);
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, FinishedTurnCommitsOneDurationRow)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	cli_feed_turn_begin(&ctx);
	cli_feed_render_frame(&ctx, 1);
	std::string active = collect_terminal_output(master);
	EXPECT_NE(active.find("esc to interrupt"), std::string::npos);
	ctx.presenter->final_rendered = 1;
	cli_feed_turn_end(&ctx, 0);
	cli_feed_turn_end(&ctx, 0);
	std::string completed = collect_terminal_output(master);
	EXPECT_NE(completed.find("Worked for"), std::string::npos);
	EXPECT_EQ(completed.find("Worked for"), completed.rfind("Worked for"));
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, JobsShowCompactSummaryAndOnlyExpandTheWaitedCommand)
{
	cJSON *data = cJSON_Parse(R"({"session_id":"proc_job12345","owner_session_id":0,
		"description":"编译项目","command":"cmake --build build -j8","status":"running",
		"duration_ms":42000,"stdout":"PRIVATE LOG SHOULD STAY IN DETAILS"})");
	ASSERT_NE(data, nullptr);
	morph_event event{};
	event.name = "process.started";
	event.data = data;
	ASSERT_EQ(cli_jobs_event(&ctx, &event), 1);
	nb_buf output{};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 128) ? 0 : NB_ERR_NOMEM), 0);
	EXPECT_EQ(cli_jobs_render(ctx.jobs, &output, 80, 0, cli_jobs_now()), 1);
	std::string text = nb_buf_cstr(&output);
	EXPECT_EQ(text.find("◉ 1 background job running · /jobs"), 0u);
	EXPECT_EQ(text.find("cmake --build"), std::string::npos);
	EXPECT_EQ(text.find("PRIVATE LOG"), std::string::npos);
	event.name = "background.wait.begin";
	ASSERT_EQ(cli_jobs_event(&ctx, &event), 1);
	EXPECT_EQ((ctx.jobs && cli_jobs_model_waiting(ctx.jobs)), 1);
	for (int columns : {120, 80, 60, 40}) {
		nb_buf_clear(&output);
		EXPECT_EQ(cli_jobs_render(ctx.jobs, &output, columns, 0, cli_jobs_now()), 2);
		text = nb_buf_cstr(&output);
		EXPECT_EQ(text.find("◉ Waiting for"), 0u) << text;
		EXPECT_NE(text.find("/jobs"), std::string::npos) << text;
		EXPECT_NE(text.find("Esc"), std::string::npos) << text;
		EXPECT_NE(text.find("42s"), std::string::npos) << text;
		EXPECT_NE(text.find("\n  └ cmake --build"), std::string::npos) << text;
		EXPECT_EQ(text.find("PRIVATE LOG"), std::string::npos);
		for (size_t offset = 0; offset < text.size();) {
			size_t end = text.find('\n', offset);
			auto line = text.substr(offset, end - offset);
			EXPECT_LT(nb_utf8_width_cstr(line.c_str()), static_cast<size_t>(columns));
			if (end == std::string::npos) break;

			offset = end + 1;
		}
	}

	event.name = "background.wait.end";
	ASSERT_EQ(cli_jobs_event(&ctx, &event), 1);
	EXPECT_EQ((ctx.jobs && cli_jobs_model_waiting(ctx.jobs)), 0);
	nb_buf_cleanup(&output);
	cJSON_Delete(data);
}

TEST_F(CliTerminalTest, JobActivityRevisionFollowsVisibleJobsAcrossSessionsAndVectorGrowth)
{
	ASSERT_EQ(cli_jobs_model_create(&ctx.jobs), 0);
	cJSON *job NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		R"({"session_id":"proc_wait","owner_session_id":0,"status":"running","duration_ms":42})");
	ASSERT_NE(job, nullptr);
	ASSERT_EQ(cli_jobs_model_remember(ctx.jobs, job), 0);
	cli_jobs_model_note_wait(ctx.jobs, "proc_wait", 1);
	uint64_t revision = ctx.jobs->revision;
	cJSON *other NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		R"({"session_id":"proc_other","owner_session_id":1,"status":"running"})");
	ASSERT_NE(other, nullptr);
	for (int i = 0; i < 20; i++) {
		ASSERT_TRUE(cJSON_SetValuestring(cJSON_GetObjectItem(other, "session_id"),
						 std::to_string(i).c_str()));
		ASSERT_EQ(cli_jobs_model_remember(ctx.jobs, other), 0);
	}

	EXPECT_EQ(ctx.jobs->revision, revision);
	const struct cli_jobs_job *waiting = cli_jobs_model_waiting(ctx.jobs);
	ASSERT_NE(waiting, nullptr);
	EXPECT_STREQ(cli_jobs_job_string(waiting->data, "session_id"), "proc_wait");
	EXPECT_EQ(cli_jobs_model_wait_elapsed(ctx.jobs, waiting->updated + 1000), 1042);
	cli_jobs_model_note_wait(ctx.jobs, "proc_wait", 1);
	cli_jobs_model_note_wait(ctx.jobs, "proc_wait", 0);
	EXPECT_EQ(ctx.jobs->revision, revision);

	cli_jobs_model_set_session(ctx.jobs, 1);
	EXPECT_NE(ctx.jobs->revision, revision);
	EXPECT_EQ(cli_jobs_model_count(ctx.jobs), 20);
	EXPECT_EQ(cli_jobs_model_waiting(ctx.jobs), nullptr);
	cli_jobs_model_set_session(ctx.jobs, 0);
	EXPECT_EQ(cli_jobs_model_count(ctx.jobs), 1);
	ASSERT_NE(cli_jobs_model_waiting(ctx.jobs), nullptr);
	cli_jobs_model_note_wait(ctx.jobs, "proc_wait", 0);
	EXPECT_EQ(cli_jobs_model_waiting(ctx.jobs), nullptr);
}

TEST_F(CliTerminalTest, CompletedJobsLeaveOneHistoryReceiptAndNoLiveRows)
{
	cJSON *data = cJSON_Parse(R"({"session_id":"proc_job12345","owner_session_id":0,
		"description":"","command":"printf bad; exit 7","status":"exited","exit_code":7,
		"duration_ms":100})");
	ASSERT_NE(data, nullptr);
	morph_event event{};
	event.name = "process.completed";
	event.data = data;
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_jobs_event(&ctx, &event), 1);
	std::string receipt = testing::internal::GetCapturedStdout();
	EXPECT_NE(receipt.find("exited 7"), std::string::npos);
	nb_buf output{};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 128) ? 0 : NB_ERR_NOMEM), 0);
	EXPECT_EQ(cli_jobs_render(ctx.jobs, &output, 40, 0, cli_jobs_now()), 0);
	EXPECT_EQ(std::string(nb_buf_cstr(&output)).find("└"), std::string::npos);
	EXPECT_TRUE(std::string(nb_buf_cstr(&output)).empty());
	EXPECT_EQ(cli_jobs_model_visible(ctx.jobs), 0);
	nb_buf_cleanup(&output);
	cJSON_Delete(data);
}

TEST_F(CliTerminalTest, CompletedJobClearsLiveRowsWithoutAnActiveTurn)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	cJSON *data = cJSON_Parse(R"({"session_id":"proc_done1234","owner_session_id":0,
		"description":"","command":"printf done","status":"exited","exit_code":0,
		"duration_ms":100})");
	morph_event event{};
	cJSON_ReplaceItemInObject(data, "status", cJSON_CreateString("running"));
	event.name = "process.started";
	event.data = data;
	EXPECT_EQ(cli_jobs_event(&ctx, &event), 1);
	cli_feed_render_frame(&ctx, 1);
	EXPECT_EQ(cli_jobs_model_visible(ctx.jobs), 1);
	EXPECT_NE(collect_terminal_output(master).find("1 background job running"),
		  std::string::npos);
	cJSON_ReplaceItemInObject(data, "status", cJSON_CreateString("exited"));
	event.name = "process.completed";
	testing::internal::CaptureStdout();
	EXPECT_EQ(cli_jobs_event(&ctx, &event), 1);
	(void)testing::internal::GetCapturedStdout();
	EXPECT_EQ(cli_jobs_model_visible(ctx.jobs), 0);
	cli_feed_render_frame(&ctx, 1);
	std::string cleared = collect_terminal_output(master);
	EXPECT_NE(cleared.find("\033[2K"), std::string::npos);
	EXPECT_EQ(cleared.find("background job running"), std::string::npos);
	cJSON_Delete(data);
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, MarkdownPreviewUsesOutputHeightAndSharedFallbackDuringResize)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	winsize size{};
	size.ws_col = 80;
	size.ws_row = 16;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_output_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	std::string text;
	for (int i = 0; i < 30; i++) {
		char line[32];
		snprintf(line, sizeof(line), "preview-%02d", i);
		if (i)
			text += '\n';

		text += line;
	}

	ASSERT_EQ(cli_feed_markdown_preview_set(&ctx, text.data(), text.size(), 0), 0);
	auto take = [&] {
		std::string bytes;
		char buffer[BUFSIZ];
		ssize_t length;
		while ((length = read(master, buffer, sizeof(buffer))) > 0)
			bytes.append(buffer, static_cast<size_t>(length));

		return bytes;
	};
	cli_feed_render_frame(&ctx, 1);
	std::string small = take();
	EXPECT_EQ(small.find("preview-25"), std::string::npos);
	EXPECT_NE(small.find("preview-26"), std::string::npos);
	EXPECT_NE(small.find("preview-29"), std::string::npos);

	for (unsigned short height :
	     {static_cast<unsigned short>(24), static_cast<unsigned short>(0)}) {
		size.ws_row = height;
		ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
		cli_feed_render_frame(&ctx, 1);
		std::string normal = take();
		EXPECT_EQ(normal.find("preview-17"), std::string::npos);
		EXPECT_NE(normal.find("preview-18"), std::string::npos);
		EXPECT_NE(normal.find("preview-29"), std::string::npos);
	}

	size.ws_row = 12;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_feed_render_frame(&ctx, 1);
	EXPECT_EQ(take().find("preview-"), std::string::npos);
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, MarkdownPreviewCountsWrappedRowsWhenClearing)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	winsize size{};
	size.ws_col = 80;
	size.ws_row = 16;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_output_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	auto take = [&] {
		std::string bytes;
		char buffer[BUFSIZ];
		ssize_t length;
		while ((length = read(master, buffer, sizeof(buffer))) > 0)
			bytes.append(buffer, static_cast<size_t>(length));

		return bytes;
	};

	std::string text(200, 'x');
	ASSERT_EQ(cli_feed_markdown_preview_set(&ctx, text.data(), text.size(), 0), 0);
	cli_feed_render_frame(&ctx, 1);
	EXPECT_NE(take().find(text), std::string::npos);

	ASSERT_EQ(cli_feed_markdown_preview_set(&ctx, nullptr, 0, 0), 0);
	cli_feed_render_frame(&ctx, 1);
	std::string cleared = take();
	size_t clear_rows = 0;
	for (size_t pos = 0; (pos = cleared.find("\033[2K", pos)) != std::string::npos;
	     pos += 4)
		clear_rows++;

	EXPECT_GE(clear_rows, 3u);
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}

TEST_F(CliTerminalTest, MarkdownPreviewCountsTrailingBlankRowWhenClearing)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	ASSERT_GE(master, 0);
	ASSERT_EQ(grantpt(master), 0);
	ASSERT_EQ(unlockpt(master), 0);
	int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
	ASSERT_GE(slave, 0);
	FILE *output = fdopen(dup(slave), "w");
	ASSERT_NE(output, nullptr);
	ASSERT_NE(fcntl(master, F_SETFL, O_NONBLOCK), -1);
	winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(ioctl(slave, TIOCSWINSZ, &size), 0);
	cli_output_set_color_enabled(&ctx, 1);
	ASSERT_EQ(cli_feed_init(&ctx, output, slave), 0);
	auto take = [&] {
		std::string bytes;
		char buffer[BUFSIZ];
		ssize_t length;
		while ((length = read(master, buffer, sizeof(buffer))) > 0)
			bytes.append(buffer, static_cast<size_t>(length));

		return bytes;
	};

	cli_feed_live_set(&ctx, "Working");
	(void)take();
	std::string text = "已知限制\n";
	ASSERT_EQ(cli_feed_markdown_preview_set(&ctx, text.data(), text.size(), 0), 0);
	cli_feed_render_frame(&ctx, 1);
	(void)take();
	cli_feed_render_frame(&ctx, 1);
	std::string cleared = take();
	size_t clear_rows = 0;
	for (size_t pos = 0; (pos = cleared.find("\033[2K", pos)) != std::string::npos;
	     pos += 4)
		clear_rows++;

	EXPECT_EQ(clear_rows, 3u);
	cli_feed_cleanup(&ctx);
	fclose(output);
	close(slave);
	close(master);
}
