#include <gtest/gtest.h>
#include "agent/tools/ask_user.h"
#include "agent/tool.h"
#include <stdlib.h>
#include <string.h>

static char *g_last_answer;
static int g_last_choices_count;

static int mock_ask_user_cb(const char *question,
			    const char *const *choices,
			    int choices_count,
			    char **answer,
			    void *user_data)
{
	(void)question;
	(void)user_data;
	if (!answer)
		return -EINVAL;
	g_last_choices_count = choices_count;
	if (choices && choices_count > 0) {
		*answer = strdup(choices[0]);
	} else if (g_last_answer) {
		*answer = strdup(g_last_answer);
	} else {
		*answer = strdup("mock answer");
	}
	return 0;
}

static int mock_ask_user_error_cb(const char *question,
				  const char *const *choices,
				  int choices_count,
				  char **answer,
				  void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)answer;
	(void)user_data;
	return -ENOTTY;
}

static int mock_ask_user_empty_cb(const char *question,
				  const char *const *choices,
				  int choices_count,
				  char **answer,
				  void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)user_data;
	*answer = strdup("");
	return 0;
}

static int mock_ask_user_data_cb(const char *question,
				 const char *const *choices,
				 int choices_count,
				 char **answer,
				 void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	if (!answer || !user_data)
		return -EINVAL;
	*answer = strdup((const char *)user_data);
	return *answer ? 0 : -ENOMEM;
}

class AskUserToolTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	void SetUp() override {
		tool_registry_init(&tools);
		g_last_answer = nullptr;
		g_last_choices_count = 0;
	}
	void TearDown() override {
		tool_registry_cleanup(&tools);
	}
};

TEST_F(AskUserToolTest, RegisterTool) {
	int rc = ask_user_init(&tools, mock_ask_user_cb, nullptr);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&tools, "ask_user");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "ask_user");
}

TEST_F(AskUserToolTest, NullRegistry) {
	EXPECT_NE(ask_user_init(nullptr, mock_ask_user_cb, nullptr), 0);
}

TEST_F(AskUserToolTest, NullCallback) {
	EXPECT_NE(ask_user_init(&tools, nullptr, nullptr), 0);
}

TEST_F(AskUserToolTest, QuestionOnly) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"What is your name?\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	ASSERT_NE(result.data, nullptr);
	ASSERT_NE(result.ui, nullptr);
	cJSON *kind = cJSON_GetObjectItem(result.data, "kind");
	ASSERT_TRUE(cJSON_IsString(kind));
	EXPECT_STREQ(kind->valuestring, "ask_user_response");
	cJSON *answer = cJSON_GetObjectItem(result.data, "answer");
	ASSERT_TRUE(cJSON_IsString(answer));
	EXPECT_STREQ(answer->valuestring, "mock answer");
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, QuestionWithChoices) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"Pick one\","
			   "\"choices\":[\"red\",\"green\",\"blue\"]}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_EQ(g_last_choices_count, 3);
	ASSERT_NE(result.data, nullptr);
	cJSON *choices = cJSON_GetObjectItem(result.data, "choices");
	ASSERT_TRUE(cJSON_IsArray(choices));
	EXPECT_EQ(cJSON_GetArraySize(choices), 3);
	cJSON *first = cJSON_GetArrayItem(choices, 0);
	ASSERT_TRUE(cJSON_IsString(first));
	EXPECT_STREQ(first->valuestring, "red");
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, MissingQuestion) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "question"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, EmptyQuestion) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"\"}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, CallbackError) {
	ask_user_init(&tools, mock_ask_user_error_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"hello?\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "error"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, EmptyAnswer) {
	ask_user_init(&tools, mock_ask_user_empty_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"hello?\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	ASSERT_NE(result.data, nullptr);
	cJSON *no_input = cJSON_GetObjectItem(result.data, "no_input");
	ASSERT_TRUE(cJSON_IsBool(no_input));
	EXPECT_TRUE(cJSON_IsTrue(no_input));
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, RegistryScopedCallbackData) {
	struct tool_registry other;
	tool_registry_init(&other);
	ask_user_init(&tools, mock_ask_user_data_cb, (void *)"first");
	ask_user_init(&other, mock_ask_user_data_cb, (void *)"second");

	struct tool_result result1;
	struct tool_result result2;
	tool_result_init(&result1);
	tool_result_init(&result2);

	int rc1 = tool_exec(&tools, "ask_user",
			    "{\"question\":\"hello?\"}", &result1);
	int rc2 = tool_exec(&other, "ask_user",
			    "{\"question\":\"hello?\"}", &result2);
	EXPECT_EQ(rc1, 0);
	EXPECT_EQ(rc2, 0);
	ASSERT_NE(result1.text.data, nullptr);
	ASSERT_NE(result2.text.data, nullptr);
	ASSERT_NE(result1.data, nullptr);
	ASSERT_NE(result2.data, nullptr);
	cJSON *answer1 = cJSON_GetObjectItem(result1.data, "answer");
	cJSON *answer2 = cJSON_GetObjectItem(result2.data, "answer");
	ASSERT_TRUE(cJSON_IsString(answer1));
	ASSERT_TRUE(cJSON_IsString(answer2));
	EXPECT_STREQ(answer1->valuestring, "first");
	EXPECT_STREQ(answer2->valuestring, "second");

	tool_result_cleanup(&result1);
	tool_result_cleanup(&result2);
	tool_registry_cleanup(&other);
}

TEST_F(AskUserToolTest, NullArgsJson) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user", nullptr, &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, MalformedJson) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user", "not json", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, NoCallback) {
	struct tool_registry reg2;
	tool_registry_init(&reg2);
	ask_user_init(&reg2, nullptr, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg2, "ask_user",
			   "{\"question\":\"test\"}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg2);
}

TEST_F(AskUserToolTest, ToolNotFound) {
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"test\"}", &result);
	EXPECT_NE(rc, 0);
	tool_result_cleanup(&result);
}
