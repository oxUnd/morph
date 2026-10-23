#include "model_metadata_test_support.hpp"

using SessionTitleTest = RuntimeMetadataTest;


TEST(SessionTitleParser, StrictJsonAndSafeNormalization)
{
	const char *invalid_responses[] = {nullptr, "title", "[]", "{}", "{\"title\":3}",
		"{\"title\":\"\"}", "{\"title\":\"x\",\"extra\":1}",
		"{\"title\":\"x\"} trailing", "{\"title\":\"\\u001b[31mred\"}"};
	for (const char *invalid : invalid_responses) {
		char *title = runtime_title_parse(invalid);
		EXPECT_EQ(title, nullptr);
		free(title);
	}
	char *title = runtime_title_parse(R"({"title":"  \"Fix\n  session titles!\"  "})");
	ASSERT_NE(title, nullptr);
	EXPECT_STREQ(title, "Fix session titles");
	free(title);
}

TEST(SessionTitleParser, TruncatesByUnicodeCharacters)
{
	std::string input;
	for (int i = 0; i < 50; i++)
		input += "猫";
	char *title = runtime_title_parse(("{\"title\":\"" + input + "\"}").c_str());
	ASSERT_NE(title, nullptr);
	EXPECT_EQ(utf8valid(title), nullptr);
	EXPECT_EQ(utf8len(title), SESSION_TITLE_MAX_CHARS);
	free(title);
}

TEST_F(SessionTitleTest, SavesToOriginAfterSwitchAndRecordsSeparateUsage)
{
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	struct session other{};
	ASSERT_EQ(runtime_session_create_and_select(instance, "Other task", &other), 0);
	Release();
	ASSERT_TRUE(WaitName(origin.id, "Fix session naming"));
	EXPECT_STREQ(runtime_session_current_name(instance), "Other task");
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 0);
	struct credit_summary summary{};
	ASSERT_EQ(credit_summary_session(&instance->context.database, origin.display_id,
		&summary), 0);
	EXPECT_EQ(summary.event_count, 1);
	ASSERT_EQ(runtime_session_select_existing(instance, origin.id, nullptr), 0);
	EXPECT_STREQ(runtime_session_current_name(instance), "Fix session naming");
}

TEST_F(SessionTitleTest, StoresModelSummaryRatherThanPromptPrefix)
{
	answer = R"({"title":"改用模型总结会话标题"})";
	Start();
	const std::string input =
		"我还是希望不要文本直接截取做name，而是靠总结，"
		"现在还是截取，感觉很生硬！";
	ASSERT_EQ(Generate(input), 0);
	ASSERT_TRUE(WaitRequest());
	cJSON *body = cJSON_Parse(request_body.c_str());
	ASSERT_NE(body, nullptr);
	const cJSON *messages = cJSON_GetObjectItem(body, "messages");
	const char *instructions = cJSON_GetStringValue(cJSON_GetObjectItem(
		cJSON_GetArrayItem(messages, 0), "content"));
	ASSERT_NE(instructions, nullptr);
	EXPECT_NE(strstr(instructions, "do not simply copy or truncate"), nullptr);
	cJSON_Delete(body);
	Release();
	ASSERT_TRUE(WaitName(instance->context.current_session.id,
		"改用模型总结会话标题"));
	struct session current{};
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_NE(input.find(current.name), 0u);
}

TEST_F(SessionTitleTest, ManualRenameWinsEvenWhenNameIsUnchanged)
{
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(runtime_session_rename_and_update(instance, origin.id, origin.name), 0);
	Release();
	runtime_title_stop(instance);
	EXPECT_TRUE(WaitName(origin.id, origin.name));
}

TEST_F(SessionTitleTest, DeletedSessionsAreNotRecreated)
{
	Start();
	int64_t id = instance->context.current_session.id;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(runtime_session_delete_and_update(instance, id), 0);
	Release();
	runtime_title_stop(instance);
	struct session session{};
	EXPECT_EQ(session_get_by_id(&instance->context.database, id, &session), -ENOENT);
}

TEST_F(SessionTitleTest, DuplicateTitlesReceiveStableIdentitySuffix)
{
	Start();
	struct session origin = instance->context.current_session;
	struct session other{};
	ASSERT_EQ(runtime_session_create_detached(instance, "Fix session naming", &other), 0);
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	EXPECT_TRUE(WaitName(origin.id,
		"Fix session naming [" + std::string(origin.display_id) + "]"));
}

TEST_F(SessionTitleTest, InFlightRequestsAreDeduplicatedAndInputIsBounded)
{
	Start();
	std::string prompt;
	for (int i = 0; i < 1000; i++)
		prompt += "猫";
	ASSERT_EQ(Generate(prompt), 0);
	ASSERT_TRUE(WaitRequest());
	ASSERT_EQ(Generate("A second request"), 0);
	cJSON *body = cJSON_Parse(request_body.c_str());
	ASSERT_NE(body, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(body, "tools"), nullptr);
	const cJSON *messages = cJSON_GetObjectItem(body, "messages");
	ASSERT_EQ(cJSON_GetArraySize(messages), 2);
	size_t bytes = 0;
	for (int i = 0; i < 2; i++) {
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(
			cJSON_GetArrayItem(messages, i), "content"));
		ASSERT_NE(text, nullptr);
		EXPECT_EQ(utf8valid(text), nullptr);
		bytes += strlen(text);
	}
	EXPECT_LE(bytes, SESSION_TITLE_PROMPT_MAX_BYTES);
	cJSON_Delete(body);
	Release();
	EXPECT_TRUE(WaitName(instance->context.current_session.id, "Fix session naming"));
}

TEST_F(SessionTitleTest, ShutdownCancelsAStalledRequest)
{
	Start();
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	auto before = TitleClock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(TitleClock::now() - before, std::chrono::seconds(3));
	EXPECT_EQ(title_phases, (std::vector<std::string>{"begin", "end"}));
}

static int foreground_reply(struct model *, struct arena *, const char *,
	struct chat_message *, int, struct tool_desc *, int,
	struct chat_response *response, sse_callback, void *)
{
	response->content = strdup("Task complete.");
	return response->content ? 200 : -ENOMEM;
}

TEST_F(SessionTitleTest, ForegroundTurnFinishesWhileTitleResponseIsBlocked)
{
	Start();
	instance->context.llm->chat_with_tools = foreground_reply;
	instance->context.llm->chat_with_tools_stream = nullptr;
	struct runtime_request request{};
	request.model_input = "Fix session titles";
	request.stored_user_input = request.model_input;
	request.generate_session_title = 1;
	struct runtime_result result{};
	auto before = TitleClock::now();
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	EXPECT_LT(TitleClock::now() - before, std::chrono::seconds(1));
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_STREQ(result.final_text, "Task complete.");
	ASSERT_TRUE(WaitRequest());
	struct session origin = instance->context.current_session;
	EXPECT_TRUE(WaitName(origin.id, origin.name));
	Release();
	ASSERT_TRUE(WaitName(origin.id, "Fix session naming"));
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 2);
	EXPECT_STREQ(runtime_session_current_name(instance), "Fix session naming");
}

TEST_F(SessionTitleTest, ExplicitlyNamedTurnsDoNotScheduleTitles)
{
	Start();
	instance->context.llm->chat_with_tools = foreground_reply;
	instance->context.llm->chat_with_tools_stream = nullptr;
	ASSERT_EQ(runtime_session_rename_and_update(instance,
		instance->context.current_session.id, "My chosen name"), 0);
	struct runtime_request request{};
	request.model_input = "Fix session titles";
	request.stored_user_input = request.model_input;
	struct runtime_result result{};
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), 0);
	for (auto *job : instance->title_jobs)
		EXPECT_EQ(job, nullptr);
	EXPECT_STREQ(runtime_session_current_name(instance), "My chosen name");
}

TEST_F(SessionTitleTest, InvalidOutputDoesNotRenameOrEnterConversation)
{
	answer = "I cannot generate a title";
	Start();
	struct session origin = instance->context.current_session;
	ASSERT_EQ(Generate(), 0);
	ASSERT_TRUE(WaitRequest());
	Release();
	server.join();
	/* Wait for the isolated request to finish billing before joining its worker. */
	auto deadline = TitleClock::now() + std::chrono::seconds(3);
	struct credit_summary summary{};
	do {
		ASSERT_EQ(credit_summary_session(&instance->context.database,
			origin.display_id, &summary), 0);
		if (summary.event_count)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (TitleClock::now() < deadline);
	EXPECT_EQ(summary.event_count, 1);
	runtime_title_stop(instance);
	EXPECT_TRUE(WaitName(origin.id, origin.name));
	EXPECT_EQ(message_count(&instance->context.database, origin.id), 0);
}
