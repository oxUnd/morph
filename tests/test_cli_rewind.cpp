#include "runtime_test_support.hpp"

extern "C" {
#include <string.h>
#include <stdlib.h>
#include "persistence/checkpoint_store.h"
#include "runtime/checkpoint.h"
#include "sapi/cli/application.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/rewind.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/rewind.h"
#include "sapi/cli/presentation/output.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "cJSON.h"
}

#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

class CliRewindTest : public RuntimeFacadeTest
{
      protected:
	struct cli_application cli{};
	struct db db{};
	struct session session{};
	int64_t checkpoint = 0;
	std::string file;
	std::string output;

	void SetUp() override
	{
		RuntimeFacadeTest::SetUp();
		WriteConfig(
			"[memory]\nenabled=false\n[features]\nhooks=false\n"
			"[permissions]\naccess='workspace'\napproval='never'\nreviewer='user'\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		Open(&options);
		ASSERT_EQ(runtime_session_create_and_select(instance, "rewind-cli", &session), 0);
		ASSERT_EQ(db_open(nullptr, &db, database.c_str()), 0);
		cli.runtime = instance;
		cli.presentation_mode = CLI_PRESENT_ONCE_PLAIN;
		file = directory + "/example.txt";
		ASSERT_EQ(nb_file_write_all(file.c_str(), "before\n", 7, NULL), 0);
		ASSERT_EQ(checkpoint_store_create(&db, session.id, directory.c_str(), "turn_one",
						  "change example", &checkpoint),
			  0);
		ASSERT_EQ(checkpoint_store_capture(&db, checkpoint, file.c_str()), 0);
		ASSERT_EQ(nb_file_write_all(file.c_str(), "after\n", 6, NULL), 0);
		ASSERT_EQ(checkpoint_store_seal(&db, checkpoint), 0);
	}

	void TearDown() override
	{
		db_close(&db);
		RuntimeFacadeTest::TearDown();
	}

	int Run(const std::string &command)
	{
		testing::internal::CaptureStdout();
		int rc = cli_command_dispatch(&cli, command.c_str());
		output = testing::internal::GetCapturedStdout();
		return rc;
	}

	std::string Text()
	{
		nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
		char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

		char *text = (nb_file_read_all(file.c_str(), &file_value_1, 0, NULL) == NB_OK &&
					      (file_copy_1 = (char *)malloc(
						       nb_buf_len(&file_value_1) + 1)) != NULL
				      ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
						nb_buf_len(&file_value_1) + 1),
					 nb_charp_steal(&file_copy_1))
				      : NULL);
		std::string result = text ? text : "";
		free(text);
		return result;
	}
};

TEST_F(CliRewindTest, ListAndPreviewDescribeTheSelectedCheckpoint)
{
	ASSERT_NE(cli_command_find_borrow("/rewind"), nullptr);
	ASSERT_EQ(Run("/rewind list"), 0);
	EXPECT_NE(output.find("change example"), std::string::npos);
	ASSERT_EQ(Run("/rewind preview " + std::to_string(checkpoint)), 0);
	EXPECT_NE(output.find("example.txt"), std::string::npos);
	EXPECT_EQ(Text(), "after\n");
	EXPECT_EQ(Run("/rewind preview 1trailing"), NB_ERR_INVAL);
	EXPECT_NE(output.find("Usage:"), std::string::npos);
	EXPECT_EQ(Text(), "after\n");
}

TEST_F(CliRewindTest, CodeCommandRestoresCapturedCode)
{
	ASSERT_EQ(Run("/rewind code " + std::to_string(checkpoint)), 0);
	EXPECT_EQ(Text(), "before\n");
	EXPECT_NE(output.find("rewound code"), std::string::npos);
}

TEST_F(CliRewindTest, ConflictIsVisibleAndDoesNotOverwriteExternalChanges)
{
	ASSERT_EQ(nb_file_write_all(file.c_str(), "external\n", 9, NULL), 0);
	EXPECT_LT(Run("/rewind code " + std::to_string(checkpoint)), 0);
	EXPECT_EQ(Text(), "external\n");
	EXPECT_NE(output.find("Conflict:"), std::string::npos);
	EXPECT_NE(output.find("example.txt"), std::string::npos);
}

TEST_F(CliRewindTest, ActiveTurnRejectsRewindBeforeAnyRestore)
{
	cli.turn_active = 1;
	EXPECT_EQ(Run("/rewind both " + std::to_string(checkpoint)), NB_ERR_BUSY);
	EXPECT_EQ(Text(), "after\n");
	EXPECT_NE(output.find("wait for the active turn"), std::string::npos);
}

TEST(CliRewindPicker, EscapeClosesWithoutRestoringOrPrintingAReceipt)
{
	int master = -1;
	int slave = -1;
	struct winsize size{};
	size.ws_col = 80;
	size.ws_row = 24;
	ASSERT_EQ(openpty(&master, &slave, nullptr, nullptr, &size), 0);
	FILE *terminal = fdopen(slave, "r+");
	ASSERT_NE(terminal, nullptr);
	struct cli_application cli{};
	struct morph_tui_application_options options{};
	options.input = terminal;
	options.output = terminal;
	options.external_signals = 1;
	ASSERT_EQ(morph_tui_application_init(&cli.base, &options), 0);
	cJSON *listing NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		"{\"checkpoints\":[{\"id\":1,\"prompt\":\"selected turn\",\"files\":[]}]}");
	ASSERT_NE(listing, nullptr);
	auto apply = +[](struct cli_application *ctx, int64_t, enum runtime_rewind_mode) {
		ctx->turn_active++;
		return 0;
	};
	testing::internal::CaptureStdout();
	cli_presentation_command_input(&cli, "/rewind");
	int shown = cli_rewind_show_take(&cli, &listing, apply);
	cli.application_command = shown == 0;
	int stepped = shown == 0 ? morph_tui_application_step(&cli.base, 0) : shown;
	ssize_t written = write(master, "\033", 1);
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (stepped == 0 && morph_tui_application_depth(&cli.base) > 0 &&
	       std::chrono::steady_clock::now() < deadline) {
		stepped = morph_tui_application_step(&cli.base, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	size_t depth = morph_tui_application_depth(&cli.base);
	morph_tui_application_deinit(&cli.base);
	std::string output = testing::internal::GetCapturedStdout();
	fclose(terminal);
	close(master);
	EXPECT_EQ(shown, 0);
	EXPECT_EQ(stepped, 0);
	EXPECT_EQ(written, 1);
	EXPECT_EQ(listing, nullptr);
	EXPECT_EQ(depth, 0U);
	EXPECT_EQ(cli.turn_active, 0);
	EXPECT_NE(output.find("/rewind"), std::string::npos);
	EXPECT_EQ(output.find("› /rewind"), std::string::npos);
	EXPECT_EQ(output.find("rewound"), std::string::npos);
	EXPECT_EQ(output.find("cancelled"), std::string::npos);
}

TEST(CliRewindReport, FailedCompensationShowsTheFileAndSanitizedRecoveryCopy)
{
	struct cli_application cli{};
	cli_output_set_color_enabled(&cli, 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		"{\"rollback_incomplete\":true,\"effects\":[{\"path\":\"example.txt\","
		"\"applied\":true,\"compensated\":false,\"compensation_error\":-13,"
		"\"recovery_available\":true,\"recovery_path\":\"/tmp/recovery\\u001b[2J\"}]}");
	ASSERT_NE(report, nullptr);
	cJSON *effect = cJSON_GetArrayItem(cJSON_GetObjectItem(report, "effects"), 0);
	cJSON_SetNumberValue(cJSON_GetObjectItem(effect, "compensation_error"), NB_ERR_PERMISSION);
	testing::internal::CaptureStdout();
	cli_rewind_report_print(&cli, report);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Recovery incomplete"), std::string::npos);
	EXPECT_NE(output.find("Could not return example.txt"), std::string::npos);
	EXPECT_NE(output.find(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
					 NB_ERR_PERMISSION)),
		  std::string::npos);
	EXPECT_NE(output.find("Recovery copy: /tmp/recovery"), std::string::npos);
	EXPECT_EQ(output.find('\033'), std::string::npos);
}

TEST(CliRewindReport, IncompleteDatabaseRollbackStillRequiresInspectionAfterFileCompensation)
{
	struct cli_application cli{};
	cli_output_set_color_enabled(&cli, 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_Parse("{\"rollback_incomplete\":true,\"effects\":[{\"path\":\"example.txt\","
			    "\"applied\":true,\"compensated\":true,\"compensation_error\":0}]}");
	ASSERT_NE(report, nullptr);
	testing::internal::CaptureStdout();
	cli_rewind_report_print(&cli, report);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("inspect session history and files"), std::string::npos);
	EXPECT_EQ(output.find("Could not return"), std::string::npos);
	EXPECT_EQ(output.find("Recovery copy:"), std::string::npos);
}

TEST(CliRewindReport, SuccessfulRestoreStillShowsIncompleteCleanupAndItsRecoveryCopy)
{
	struct cli_application cli{};
	cli_output_set_color_enabled(&cli, 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		"{\"code_restored\":true,\"cleanup_incomplete\":true,\"rollback_incomplete\":false,"
		"\"effects\":[{\"path\":\"example.txt\",\"applied\":true,\"compensated\":false,"
		"\"cleanup_error\":-13,\"recovery_available\":true,\"recovery_path\":\"/tmp/"
		"retained\"}]}");
	ASSERT_NE(report, nullptr);
	cJSON *effect = cJSON_GetArrayItem(cJSON_GetObjectItem(report, "effects"), 0);
	cJSON_SetNumberValue(cJSON_GetObjectItem(effect, "cleanup_error"), NB_ERR_PERMISSION);
	testing::internal::CaptureStdout();
	cli_rewind_report_print(&cli, report);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("Temporary-file cleanup is incomplete"), std::string::npos);
	EXPECT_NE(output.find("cleanup failed for example.txt"), std::string::npos);
	EXPECT_NE(output.find(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
					 NB_ERR_PERMISSION)),
		  std::string::npos);
	EXPECT_NE(output.find("Recovery copy: /tmp/retained"), std::string::npos);
	EXPECT_EQ(output.find("Could not return"), std::string::npos);
}
