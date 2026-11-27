#include "runtime_test_support.hpp"
#include <nb/err.h>
#include "cJSON.h"

extern "C" {
#include "sapi/cli/services/share_store.h"
#include "sapi/cli/presentation/share.h"
#include "sapi/cli/commands/registry.h"
#include <nb/file.h>
}

#include <filesystem>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>

class ShareTest : public RuntimeFacadeTest {
protected:
	struct db db{};
	struct session current{};
	void SetUp() override
	{
		RuntimeFacadeTest::SetUp();
		WriteConfig("[memory]\nenabled=false\n[features]\nhooks=false\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		Open(&options);
		ASSERT_EQ(runtime_session_create_and_select(instance, "share-test", &current), 0);
		ASSERT_EQ(db_open(NULL, &db, database.c_str()), 0);
	}
	void TearDown() override
	{
		db_close(&db);
		RuntimeFacadeTest::TearDown();
	}
	void Add(const char *turn, const char *kind, const char *text,
		 const char *payload = nullptr, bool active = true, const char *key = nullptr)
	{
		struct model_history_insert item{};
		item.session_id = current.id;
		item.turn_id = turn;
		item.kind = kind;
		item.content = text;
		item.payload_json = payload;
		item.idempotency_key = key;
		item.tool_name = "exec";
		item.role = strcmp(kind, "user_message") == 0 ? "user" : "assistant";
		item.active = active;
		EXPECT_EQ(model_history_add(&db, &item, nullptr), 0);
	}
	cJSON *Document()
	{
		cJSON *document = nullptr;
		EXPECT_EQ(runtime_session_share_current(instance, &document), 0);
		return document;
	}
};

TEST_F(ShareTest, LatestCompleteTurnIncludesSteeringAndToolsButNotReasoning)
{
	Add("old", "user_message", "Old question");
	Add("old", "assistant_message", "Old answer");
	Add("new", "user_message", "中文问题", nullptr, false);
	Add("new", "assistant_tool_calls", "Internal reasoning must not appear");
	Add("new", "tool_result", "Stored output", "{\"status\":\"completed\"}", false);
	Add("new", "user_message", "Additional constraint", nullptr, false);
	Add("new", "assistant_message", "**Final answer**", "{\"reasoning_content\":\"secret\"}", false);
	Add("unfinished", "user_message", "Not answered yet");
	cJSON *document = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "questions")), 2);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "steps")), 1);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "**Final answer**");
	char *json = cJSON_PrintUnformatted(document);
	EXPECT_EQ(std::string(json).find("reasoning"), std::string::npos);
	EXPECT_EQ(std::string(json).find("secret"), std::string::npos);
	free(json);
	cJSON_Delete(document);
}

TEST_F(ShareTest, AgentMailAndHookFeedbackStayInOrderedProcessOutsideHumanQuestions)
{
	Add("team", "user_message", "Review the change");
	Add("team", "tool_result", "Spawned /root/reviewer");
	Add("team", "user_message", "Message Type: FINAL_ANSWER\nSender: /root/reviewer\n"
		"Recipient: /root\nPayload:\n<review> complete", "{\"source\":\"agent_mail\"}");
	Add("team", "user_message", "Keep the public API stable", "{\"source\":\"user\"}");
	Add("team", "user_message", "Hook feedback", "{\"source\":\"hook\"}");
	Add("team", "tool_result", "Validation complete");
	Add("team", "assistant_message", "# Integrated answer");
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = Document();
	ASSERT_NE(document, nullptr);
	const cJSON *questions = cJSON_GetObjectItem(document, "questions");
	ASSERT_EQ(cJSON_GetArraySize(questions), 2);
	EXPECT_STREQ(cJSON_GetArrayItem(questions, 0)->valuestring, "Review the change");
	EXPECT_STREQ(cJSON_GetArrayItem(questions, 1)->valuestring, "Keep the public API stable");
	const cJSON *steps = cJSON_GetObjectItem(document, "steps");
	ASSERT_EQ(cJSON_GetArraySize(steps), 4);
	EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(steps, 1), "kind")->valuestring,
		"agent_message");
	EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(steps, 2), "kind")->valuestring,
		"hook");
	nb_buf html NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(cli_share_html(document, 1793172600, &html), 0);
	std::string rendered = nb_buf_cstr(&html);
	auto question_end = rendered.find("</section>");
	ASSERT_NE(question_end, std::string::npos);
	EXPECT_EQ(rendered.substr(0, question_end).find("Sender:"), std::string::npos);
	EXPECT_NE(rendered.find("Sub-agent messages"), std::string::npos);
	EXPECT_NE(rendered.find("View messages"), std::string::npos);
	EXPECT_NE(rendered.find("&lt;review&gt; complete"), std::string::npos);
	EXPECT_NE(rendered.find("<h1>Integrated answer</h1>"), std::string::npos);
}

TEST_F(ShareTest, LegacyMailboxHeadersRequireSteerKeysAndExplicitUserSourceWins)
{
	const char *mail = "Message Type: FINAL_ANSWER\nSender: /root/worker\n"
		"Recipient: /root\nPayload:\nLegacy report";
	Add("legacy-mail", "user_message", "Main question");
	Add("legacy-mail", "user_message", mail, nullptr, false,
		"turn:legacy-mail:user:steer:1");
	Add("legacy-mail", "user_message", mail, "{\"source\":\"user\"}", true,
		"turn:legacy-mail:user:steer:2");
	Add("legacy-mail", "user_message", mail);
	Add("legacy-mail", "user_message", "Message Type: FINAL_ANSWER is quoted text", nullptr,
		true, "turn:legacy-mail:user:steer:3");
	Add("legacy-mail", "assistant_message", "Root answer");
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "questions")), 4);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "steps")), 1);
}

TEST_F(ShareTest, RestoredContextAndCompactionAreNotHumanQuestionsOrExportedPrompts)
{
	Add("restored", "user_message", "Original human question", nullptr, false);
	Add("restored", "summary", "PRIVATE_COMPACTION_CHECKPOINT");
	Add("restored", "user_message", "PRIVATE_RESUME_INSTRUCTIONS", "{\"source\":\"resume\"}");
	Add("restored", "user_message", "PRIVATE_SYSTEM_CONTEXT", "{\"source\":\"system\"}");
	Add("restored", "user_message", "PRIVATE_RECOVERY_CONTEXT", "{\"source\":\"recovery\"}");
	Add("restored", "user_message", "Continue after restoring", "{\"source\":\"user\"}");
	Add("restored", "assistant_message", "Completed restored work");
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "questions")), 2);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "steps")), 0);
	char *json NB_CLEANUP(nb_charp_cleanup) = cJSON_PrintUnformatted(document);
	ASSERT_NE(json, nullptr);
	EXPECT_EQ(std::string(json).find("PRIVATE_"), std::string::npos);
	EXPECT_NE(std::string(json).find("Continue after restoring"), std::string::npos);
}

TEST_F(ShareTest, SelectingAnExistingSessionDoesNotInventAUserQuestion)
{
	Add("before-resume", "user_message", "Question before restoring");
	Add("before-resume", "assistant_message", "Answer before restoring");
	ASSERT_EQ(runtime_session_create_and_select(instance, "other-session", &current), 0);
	ASSERT_EQ(runtime_session_select(instance, "share-test", &current, nullptr), 0);
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = Document();
	ASSERT_NE(document, nullptr);
	const cJSON *questions = cJSON_GetObjectItem(document, "questions");
	ASSERT_EQ(cJSON_GetArraySize(questions), 1);
	EXPECT_STREQ(cJSON_GetArrayItem(questions, 0)->valuestring, "Question before restoring");
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "Answer before restoring");
}

TEST_F(ShareTest, MailOnlyTurnCannotDisplaceAnAnsweredHumanTurn)
{
	Add("human", "user_message", "Original question");
	Add("human", "assistant_message", "Original answer");
	Add("mail-only", "user_message", "Internal update", "{\"source\":\"agent_mail\"}");
	Add("mail-only", "assistant_message", "Internal answer");
	cJSON *document NB_CLEANUP(cJSON_DeleteOwner) = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "Original answer");
}

TEST_F(ShareTest, SkipsInterruptedAnswerAndDoesNotCrossSessions)
{
	Add("good", "user_message", "Question");
	Add("good", "assistant_message", "Complete answer");
	Add("cancelled", "user_message", "Interrupted question",
	    "{\"turn_outcome\":{\"status\":\"interrupted\"}}");
	Add("cancelled", "assistant_message", "Partial answer");
	cJSON *document = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "Complete answer");
	cJSON_DeleteOwner(&document);
	ASSERT_EQ(runtime_session_create_and_select(instance, "empty", &current), 0);
	EXPECT_EQ(runtime_session_share_current(instance, &document), NB_ERR_NO_DATA);
	EXPECT_EQ(document, nullptr);
}

TEST_F(ShareTest, HtmlIsSelfContainedAndEscapesUntrustedContent)
{
	Add("one", "user_message", "<img src=x onerror=alert(1)>");
	Add("one", "tool_result", "</pre><script>alert(1)</script>");
	Add("one", "assistant_message", "# Heading\n\n**Bold** and `code`\n\n"
		"| A | B |\n|---|---|\n| 1 | 2 |\n\n"
		"<script>alert(2)</script>\n\n![alt](https://example.com/leak)\n\n"
		"[bad](javascript:alert%283%29)");
	cJSON *document = Document();
	nb_buf output{};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 1024) ? 0 : NB_ERR_NOMEM), 0);
	ASSERT_EQ(cli_share_html(document, 1793172600, &output), 0);
	std::string html = nb_buf_cstr(&output);
	EXPECT_NE(html.find("data:image/png;base64,"), std::string::npos);
	EXPECT_NE(html.find("Shared from morph"), std::string::npos);
	EXPECT_NE(html.find("UTC"), std::string::npos);
	EXPECT_NE(html.find("data-set-theme=dark"), std::string::npos);
	EXPECT_NE(html.find("<strong>Bold</strong>"), std::string::npos);
	EXPECT_NE(html.find("<table>"), std::string::npos);
	EXPECT_NE(html.find("&lt;img src=x"), std::string::npos);
	EXPECT_NE(html.find("&lt;/pre&gt;&lt;script&gt;"), std::string::npos);
	EXPECT_EQ(html.find("<script>alert"), std::string::npos);
	EXPECT_EQ(html.find("https://example.com/leak"), std::string::npos);
	EXPECT_EQ(html.find("href=\"javascript:"), std::string::npos);
	EXPECT_EQ(html.find("<details class=process open"), std::string::npos);
	nb_buf_cleanup(&output);
	cJSON_Delete(document);
}

TEST_F(ShareTest, RepeatedExportsPublishDistinctVisibleFiles)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message", "Answer");
	cJSON *document = Document();
	const char *home = getenv("HOME");
	std::string saved = home ? home : "";
	ASSERT_EQ(setenv("HOME", directory.c_str(), 1), 0);
	char *first = nullptr;
	char *second = nullptr;
	int first_rc = cli_share_store_save(document, &first);
	int second_rc = cli_share_store_save(document, &second);
	char *saved_first = first;
	int occupied_rc = cli_share_store_save(document, &first);
	if (home) setenv("HOME", saved.c_str(), 1);
	else unsetenv("HOME");
	ASSERT_EQ(first_rc, 0);
	ASSERT_EQ(second_rc, 0);
	EXPECT_EQ(occupied_rc, NB_ERR_EXISTS);
	EXPECT_EQ(first, saved_first);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_STRNE(first, second);
	EXPECT_EQ(std::filesystem::path(first).filename().string().find("morph-share-"), 0u);
	EXPECT_EQ(std::filesystem::path(first).extension(), ".html");
	EXPECT_TRUE(std::filesystem::exists(second));
	free(first);
	free(second);
	cJSON_Delete(document);
}

TEST_F(ShareTest, LegacyTranscriptExportsLastAnsweredPair)
{
	ASSERT_EQ(message_add(&db, current.id, "user", "Legacy question", 1), 0);
	ASSERT_EQ(message_add(&db, current.id, "assistant", "Legacy answer", 1), 0);
	ASSERT_EQ(message_add(&db, current.id, "user", "Unanswered question", 1), 0);
	cJSON *document = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "Legacy answer");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "steps")), 0);
	cJSON_Delete(document);
}

TEST_F(ShareTest, SaveFailureDoesNotReturnAPathOrLeaveStagingFiles)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message", "Answer");
	cJSON *document = Document();
	std::ofstream blocked(directory + "/Downloads");
	blocked << "not a directory";
	blocked.close();
	const char *home = getenv("HOME");
	std::string saved = home ? home : "";
	ASSERT_EQ(setenv("HOME", directory.c_str(), 1), 0);
	char *path = nullptr;
	int rc = cli_share_store_save(document, &path);
	if (home) setenv("HOME", saved.c_str(), 1);
	else unsetenv("HOME");
	EXPECT_LT(rc, 0);
	EXPECT_EQ(path, nullptr);
	cJSON_Delete(document);
}

TEST_F(ShareTest, MigratedLegacyMessagesRemainShareable)
{
	ASSERT_EQ(message_add(&db, current.id, "user", "Migrated question", 1), 0);
	ASSERT_EQ(message_add(&db, current.id, "assistant", "Migrated answer", 1), 0);
	ASSERT_EQ(model_history_migrate_messages(&db, current.id), 0);
	cJSON *document = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "Migrated answer");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "questions")), 1);
	cJSON_Delete(document);
}

TEST_F(ShareTest, NestedImagesUseTheMarkdownResourcePolicy)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message",
		"before ![![inner](https://example.com/inner)](https://example.com/outer) "
		"middle ![sibling](https://example.com/sibling) after");
	cJSON *document = Document();
	nb_buf output{};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 1024) ? 0 : NB_ERR_NOMEM), 0);
	ASSERT_EQ(cli_share_html(document, 1793172600, &output), 0);
	std::string html = nb_buf_cstr(&output);
	EXPECT_NE(html.find("before "), std::string::npos);
	EXPECT_NE(html.find(" middle "), std::string::npos);
	EXPECT_NE(html.find(" after"), std::string::npos);
	EXPECT_NE(html.find("src=\"data:,\""), std::string::npos);
	EXPECT_NE(html.find("Images omitted from this export."), std::string::npos);
	EXPECT_EQ(html.find("https://example.com"), std::string::npos);
	nb_buf_cleanup(&output);
	cJSON_Delete(document);
}

TEST_F(ShareTest, MalformedSelectedMetadataReportsParseError)
{
	Add("one", "user_message", "Question");
	Add("one", "tool_result", "Output", "invalid metadata");
	Add("one", "assistant_message", "Answer");
	cJSON *document = nullptr;
	EXPECT_EQ(runtime_session_share_current(instance, &document), NB_ERR_PARSE);
	EXPECT_EQ(document, nullptr);
}

TEST_F(ShareTest, ACompletedTurnDoesNotReadUnrelatedToolMetadata)
{
	Add("old", "user_message", "Old question");
	Add("old", "tool_result", "Unrelated output", "invalid metadata");
	Add("old", "assistant_message", "Old answer");
	Add("new", "user_message", "New question");
	Add("new", "assistant_message", "New answer");
	cJSON *document = Document();
	ASSERT_NE(document, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(document, "answer")->valuestring, "New answer");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(document, "steps")), 0);
	cJSON_Delete(document);
}

TEST_F(ShareTest, ResourceOpenFailurePreservesErrno)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message", "Answer");
	cJSON *document = Document();
	pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0) {
		struct rlimit limit{0, 0};
		if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
			_exit(2);
		nb_buf output{};
		int rc = (nb_buf_init(&output), nb_buf_reserve(&output, 1024) ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = cli_share_html(document, 1793172600, &output);
		nb_buf_cleanup(&output);
		_exit(rc == NB_ERR_RESOURCE ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	cJSON_Delete(document);
}

TEST_F(ShareTest, PartialWriteFailureLeavesNoPublishedOrStagingFile)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message", "Answer");
	cJSON *document = Document();
	pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0) {
		struct rlimit limit{1024, 1024};
		if (setrlimit(RLIMIT_FSIZE, &limit) != 0 ||
		    setenv("HOME", directory.c_str(), 1) != 0)
			_exit(2);
		signal(SIGXFSZ, SIG_IGN);
		char *path = nullptr;
		int rc = cli_share_store_save(document, &path);
		_exit(rc == NB_ERR_TOO_BIG && !path ? 0 : 1);
	}
	int status = 0;
	ASSERT_EQ(waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);
	ASSERT_TRUE(std::filesystem::is_directory(directory + "/Downloads"));
	EXPECT_TRUE(std::filesystem::is_empty(directory + "/Downloads"));
	cJSON_Delete(document);
}

TEST_F(ShareTest, MarkdownExporterPreservesMathFootnotesAndSafeLinks)
{
	Add("one", "user_message", "Question");
	Add("one", "assistant_message",
		"Inline $\\frac{1}{2}$ and display:\n\n$$x^2$$\n\n"
		"- [x] checked\n\nFootnote[^1].\n\n[^1]: note\n\n"
		"[https](HTTPS://example.com) [email](mailto:test@example.com)\n\n"
		"[script](javascript:alert%281%29) [file](file:///private/secret)\n\n"
		"[relative](../secret) [encoded](java&#x73;cript:alert%281%29)");
	cJSON *document = Document();
	nb_buf output{};
	ASSERT_EQ((nb_buf_init(&output), nb_buf_reserve(&output, 1024) ? 0 : NB_ERR_NOMEM), 0);
	ASSERT_EQ(cli_share_html(document, 1793172600, &output), 0);
	std::string html = nb_buf_cstr(&output);
	EXPECT_NE(html.find("class=\"morph-markdown\""), std::string::npos);
	EXPECT_NE(html.find("<math "), std::string::npos);
	EXPECT_NE(html.find("<mfrac>"), std::string::npos);
	EXPECT_NE(html.find("morph-math-display"), std::string::npos);
	EXPECT_NE(html.find("morph-footnote"), std::string::npos);
	EXPECT_NE(html.find("href=\"#morph-"), std::string::npos);
	EXPECT_NE(html.find("checkbox"), std::string::npos);
	EXPECT_NE(html.find("href=\"HTTPS://example.com\""), std::string::npos);
	EXPECT_NE(html.find("href=\"mailto:test@example.com\""), std::string::npos);
	EXPECT_EQ(html.find("href=\"javascript:"), std::string::npos);
	EXPECT_EQ(html.find("file:///private/secret"), std::string::npos);
	EXPECT_EQ(html.find("href=\"../secret\""), std::string::npos);
	nb_buf_cleanup(&output);
	cJSON_Delete(document);
}
