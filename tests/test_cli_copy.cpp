#include "runtime_test_support.hpp"

extern "C" {
#include "sapi/cli/application.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "sapi/cli/adapters/runtime/events.h"
#include "sapi/cli/presentation/output.h"
#include "util/file.h"
#include "util/child_io.h"
}

#include <optional>
#include <cstdlib>
#include <vector>
#include <sys/stat.h>

class CliCopyTest : public RuntimeFacadeTest {
protected:
	struct db db{};
	struct session current{};
	struct cli_application ctx{};
	std::string binaries;
	std::string copied;
	std::vector<std::pair<std::string, std::optional<std::string>>> environment;

	void SetUp() override
	{
		RuntimeFacadeTest::SetUp();
		WriteConfig("[memory]\nenabled=false\n[features]\nhooks=false\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		Open(&options);
		ASSERT_EQ(runtime_session_create_and_select(instance, "copy-test", &current), 0);
		ASSERT_EQ(db_open(nullptr, &db, database.c_str()), 0);
		ctx.runtime = instance;
		cli_output_set_color_enabled(&ctx, 0);
		binaries = directory + "/bin";
		copied = directory + "/copied.txt";
		ASSERT_TRUE(std::filesystem::create_directory(binaries));
		SetEnvironment("PATH", binaries + ":" + (getenv("PATH") ? getenv("PATH") : "/bin"));
		SetEnvironment("MORPH_COPY_TEST_FILE", copied);
		Backends("/bin/cat > \"$MORPH_COPY_TEST_FILE\"");
	}

	void TearDown() override
	{
		for (auto it = environment.rbegin(); it != environment.rend(); ++it) {
			if (it->second)
				setenv(it->first.c_str(), it->second->c_str(), 1);
			else
				unsetenv(it->first.c_str());
		}
		db_close(&db);
		RuntimeFacadeTest::TearDown();
	}

	void SetEnvironment(const char *name, const std::string &value)
	{
		const char *old = getenv(name);
		environment.emplace_back(name,
			old ? std::make_optional(std::string(old)) : std::nullopt);
		ASSERT_EQ(setenv(name, value.c_str(), 1), 0);
	}

	void Backends(const std::string &body)
	{
		for (const char *name : {"pbcopy", "wl-copy", "xclip", "xsel"}) {
			std::string path = binaries + "/" + name;
			std::string script = "#!/bin/sh\n" + body + "\n";
			ASSERT_EQ(file_write_all(path.c_str(), script.c_str(), script.size()), 0);
			ASSERT_EQ(chmod(path.c_str(), 0700), 0);
		}
	}

	void Add(const char *kind, const char *content, bool active = true)
	{
		struct model_history_insert item{};
		item.session_id = current.id;
		item.kind = kind;
		item.role = "assistant";
		item.content = content;
		item.active = active;
		ASSERT_EQ(model_history_add(&db, &item, nullptr), 0);
	}

	std::string Clipboard()
	{
		size_t length = 0;
		char *bytes = file_read_all(copied.c_str(), &length);
		if (!bytes)
			return {};
		std::string result(bytes, length);
		free(bytes);
		return result;
	}
};

TEST_F(CliCopyTest, SelectsLastAnswerWithoutPreamblesToolsOrCompactionFiltering)
{
	const char *answer = "# Answer\r\n\r\n中文🙂  \r\n```sh\r\necho '$HOME'  \r\n```\r\n";
	Add("assistant_message", "Old answer");
	Add("assistant_message", answer, false);
	Add("assistant_tool_calls", "Preamble");
	Add("tool_result", "Tool output");
	Add("compaction_summary", "Internal summary");
	Add("assistant_message", "");
	char *text = nullptr;
	ASSERT_EQ(runtime_session_last_answer(instance, &text), 0);
	ASSERT_NE(text, nullptr);
	EXPECT_STREQ(text, answer);
	free(text);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&ctx, "/copy");
	std::string receipt = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(Clipboard(), answer);
	EXPECT_NE(receipt.find("copied last answer to clipboard"), std::string::npos);
	EXPECT_EQ(receipt.find("$HOME"), std::string::npos);
}

TEST_F(CliCopyTest, LegacyAnswersSurviveReloadAndNeverCrossSessions)
{
	ASSERT_EQ(message_add(&db, current.id, "assistant", "Legacy answer", 1), 0);
	ASSERT_EQ(message_add(&db, current.id, "user", "Unanswered question", 1), 0);
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	char *text = nullptr;
	ASSERT_EQ(runtime_session_last_answer(instance, &text), 0);
	EXPECT_STREQ(text, "Legacy answer");
	free(text);
	ASSERT_EQ(runtime_session_create_and_select(instance, "empty", &current), 0);
	EXPECT_EQ(runtime_session_last_answer(instance, &text), -ENODATA);
	EXPECT_EQ(text, nullptr);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&ctx, "/copy");
	std::string receipt = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, -ENODATA);
	EXPECT_NE(receipt.find("no assistant answer"), std::string::npos);
	EXPECT_FALSE(file_exists(copied.c_str()));
}

TEST_F(CliCopyTest, NativeClipboardPreservesAnswersBeyondPipeAndOsc52Limits)
{
	std::string answer;
	for (int i = 0; i < 20000; i++)
		answer += "**中文🙂** with `code` and trailing spaces  \n";
	ASSERT_GT(answer.size(), 100000u);
	ASSERT_EQ(cli_clipboard_copy_text(&ctx, answer.c_str()), CLI_CLIPBOARD_COPIED);
	EXPECT_EQ(Clipboard(), answer);
}

TEST_F(CliCopyTest, ClipboardErrorsDoNotKillTheHostOrReportSuccess)
{
	Backends("exit 1");
	Add("assistant_message", std::string(200000, 'x').c_str());
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&ctx, "/copy");
	std::string receipt = testing::internal::GetCapturedStdout();
	EXPECT_LT(rc, 0);
	EXPECT_NE(receipt.find("copy failed"), std::string::npos);
	EXPECT_EQ(receipt.find("copied last answer"), std::string::npos);
	EXPECT_FALSE(file_exists(copied.c_str()));
}

TEST_F(CliCopyTest, ClipboardHelperTimeoutIsBounded)
{
	Backends("exec /bin/sleep 30");
	int64_t started = morph_io_now_ms();
	EXPECT_EQ(cli_clipboard_copy_text(&ctx, "Answer"), -ETIMEDOUT);
	EXPECT_LT(morph_io_now_ms() - started, 8000);
}

TEST_F(CliCopyTest, CommandArgumentsAndEmptyInputLeaveClipboardAlone)
{
	EXPECT_NE(cli_command_find("/copy"), nullptr);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&ctx, "/copy other");
	std::string receipt = testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_NE(receipt.find("usage: /copy"), std::string::npos);
	EXPECT_EQ(cli_clipboard_copy_text(&ctx, ""), -ENODATA);
	EXPECT_EQ(runtime_session_last_answer(nullptr, nullptr), -EINVAL);
	EXPECT_FALSE(file_exists(copied.c_str()));
}

TEST_F(CliCopyTest, JsonCopyKeepsAnswerOutOfEventsAndReportsNativeFailure)
{
	Add("assistant_message", "PRIVATE_ANSWER");
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ctx.event_cb = cli_runtime_events_callback;
	ctx.event_user_data = &ctx;
	for (bool fail : {false, true}) {
		if (fail)
			Backends("exit 1");
		testing::internal::CaptureStdout();
		int rc = cli_command_dispatch(&ctx, "/copy");
		std::string output = testing::internal::GetCapturedStdout();
		EXPECT_EQ(rc == 0, !fail);
		EXPECT_NE(output.find(fail ? "command.failed" : "command.completed"),
			  std::string::npos);
		EXPECT_EQ(output.find("PRIVATE_ANSWER"), std::string::npos);
		EXPECT_EQ(output.find("\033"), std::string::npos);
	}
	EXPECT_EQ(Clipboard(), "PRIVATE_ANSWER");
}
