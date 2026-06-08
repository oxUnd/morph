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
	EXPECT_STREQ(result.text.data, "mock answer");
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
	EXPECT_STREQ(result.text.data, "red");
	EXPECT_EQ(g_last_choices_count, 3);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, MissingQuestion) {
	ask_user_init(&tools, mock_ask_user_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "missing"), nullptr);
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
	EXPECT_NE(strstr(result.text.data, "no input"), nullptr);
	tool_result_cleanup(&result);
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
	tool_register(&reg2, "ask_user",
		"Ask the user a question.",
		"{\"type\":\"object\",\"properties\":{"
		"\"question\":{\"type\":\"string\"}"
		"},\"required\":[\"question\"]}",
		nullptr, nullptr, nullptr);
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
