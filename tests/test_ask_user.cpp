#include <gtest/gtest.h>
#include "agent/tools/ask_user.h"
#include "agent/tool.h"
#include <stdlib.h>
#include <string.h>

static char *g_last_answer;
static int g_last_choices_count;
static char g_last_selection_mode[16];
static int g_last_min_choices;
static int g_last_max_choices;

static int set_mock_answers(char ***answers,
			    int *answers_count,
			    const char *const *values,
			    int values_count)
{
	if (!answers || !answers_count)
		return -EINVAL;
	*answers = nullptr;
	*answers_count = 0;
	if (values_count <= 0)
		return 0;
	char **out = static_cast<char **>(calloc(values_count, sizeof(char *)));
	if (!out)
		return -ENOMEM;
	for (int i = 0; i < values_count; i++) {
		out[i] = strdup(values[i] ? values[i] : "");
		if (!out[i]) {
			for (int j = 0; j < i; j++)
				free(out[j]);
			free(out);
			return -ENOMEM;
		}
	}
	*answers = out;
	*answers_count = values_count;
	return 0;
}

static int mock_ask_user_cb(const char *question,
			    const char *const *choices,
			    int choices_count,
			    const char *selection_mode,
			    int min_choices,
			    int max_choices,
			    char ***answers,
			    int *answers_count,
			    void *user_data)
{
	(void)question;
	(void)user_data;
	g_last_choices_count = choices_count;
	strncpy(g_last_selection_mode, selection_mode ? selection_mode : "",
		sizeof(g_last_selection_mode) - 1);
	g_last_min_choices = min_choices;
	g_last_max_choices = max_choices;
	if (choices && choices_count > 0) {
		int count = strcmp(g_last_selection_mode, "multi") == 0 ?
			(min_choices > 0 ? min_choices : 1) : 1;
		if (count > choices_count)
			count = choices_count;
		return set_mock_answers(answers, answers_count, choices, count);
	} else if (g_last_answer) {
		const char *values[] = { g_last_answer };
		return set_mock_answers(answers, answers_count, values, 1);
	} else {
		const char *values[] = { "mock answer" };
		return set_mock_answers(answers, answers_count, values, 1);
	}
}

static int mock_ask_user_error_cb(const char *question,
				  const char *const *choices,
				  int choices_count,
				  const char *selection_mode,
				  int min_choices,
				  int max_choices,
				  char ***answers,
				  int *answers_count,
				  void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)selection_mode;
	(void)min_choices;
	(void)max_choices;
	(void)answers;
	(void)answers_count;
	(void)user_data;
	return -ENOTTY;
}

static int mock_ask_user_empty_cb(const char *question,
				  const char *const *choices,
				  int choices_count,
				  const char *selection_mode,
				  int min_choices,
				  int max_choices,
				  char ***answers,
				  int *answers_count,
				  void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)selection_mode;
	(void)min_choices;
	(void)max_choices;
	(void)user_data;
	return set_mock_answers(answers, answers_count, nullptr, 0);
}

static int mock_ask_user_invalid_empty_string_cb(const char *question,
						const char *const *choices,
						int choices_count,
						const char *selection_mode,
						int min_choices,
						int max_choices,
						char ***answers,
						int *answers_count,
						void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)selection_mode;
	(void)min_choices;
	(void)max_choices;
	(void)user_data;
	const char *values[] = { "" };
	return set_mock_answers(answers, answers_count, values, 1);
}

static int mock_ask_user_multi_cb(const char *question,
				  const char *const *choices,
				  int choices_count,
				  const char *selection_mode,
				  int min_choices,
				  int max_choices,
				  char ***answers,
				  int *answers_count,
				  void *user_data)
{
	(void)question;
	(void)user_data;
	g_last_choices_count = choices_count;
	strncpy(g_last_selection_mode, selection_mode ? selection_mode : "",
		sizeof(g_last_selection_mode) - 1);
	g_last_min_choices = min_choices;
	g_last_max_choices = max_choices;
	int count = min_choices > 0 ? min_choices : 1;
	if (max_choices > 0 && count > max_choices)
		count = max_choices;
	if (count > choices_count)
		count = choices_count;
	return set_mock_answers(answers, answers_count, choices, count);
}

static int mock_ask_user_data_cb(const char *question,
				 const char *const *choices,
				 int choices_count,
				 const char *selection_mode,
				 int min_choices,
				 int max_choices,
				 char ***answers,
				 int *answers_count,
				 void *user_data)
{
	(void)question;
	(void)choices;
	(void)choices_count;
	(void)selection_mode;
	(void)min_choices;
	(void)max_choices;
	if (!user_data)
		return -EINVAL;
	const char *values[] = { (const char *)user_data };
	return set_mock_answers(answers, answers_count, values, 1);
}

class AskUserToolTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	void SetUp() override {
		tool_registry_init(&tools);
		g_last_answer = nullptr;
		g_last_choices_count = 0;
		g_last_selection_mode[0] = '\0';
		g_last_min_choices = 0;
		g_last_max_choices = 0;
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
	cJSON *mode = cJSON_GetObjectItem(result.data, "selection_mode");
	ASSERT_TRUE(cJSON_IsString(mode));
	EXPECT_STREQ(mode->valuestring, "single");
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	ASSERT_EQ(cJSON_GetArraySize(answers), 1);
	cJSON *answer = cJSON_GetArrayItem(answers, 0);
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
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	ASSERT_EQ(cJSON_GetArraySize(answers), 1);
	cJSON *answer = cJSON_GetArrayItem(answers, 0);
	ASSERT_TRUE(cJSON_IsString(answer));
	EXPECT_STREQ(answer->valuestring, "red");
	EXPECT_STREQ(g_last_selection_mode, "single");
	EXPECT_EQ(g_last_min_choices, 1);
	EXPECT_EQ(g_last_max_choices, 1);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, MultiSelectReturnsAnswersArray) {
	ask_user_init(&tools, mock_ask_user_multi_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"Pick checks\","
			   "\"selection_mode\":\"multi\","
			   "\"min_choices\":2,"
			   "\"max_choices\":3,"
			   "\"choices\":[\"build\",\"install\",\"launch\"]}",
			   &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(g_last_selection_mode, "multi");
	EXPECT_EQ(g_last_min_choices, 2);
	EXPECT_EQ(g_last_max_choices, 3);
	cJSON *mode = cJSON_GetObjectItem(result.data, "selection_mode");
	ASSERT_TRUE(cJSON_IsString(mode));
	EXPECT_STREQ(mode->valuestring, "multi");
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	ASSERT_EQ(cJSON_GetArraySize(answers), 2);
	EXPECT_STREQ(cJSON_GetArrayItem(answers, 0)->valuestring, "build");
	EXPECT_STREQ(cJSON_GetArrayItem(answers, 1)->valuestring, "install");
	cJSON *legacy_answer = cJSON_GetObjectItem(result.data, "answer");
	EXPECT_EQ(legacy_answer, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, MultiSelectClampsImpossibleLimits) {
	ask_user_init(&tools, mock_ask_user_multi_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"Pick checks\","
			   "\"selection_mode\":\"multi\","
			   "\"min_choices\":5,"
			   "\"max_choices\":7,"
			   "\"choices\":[\"build\",\"install\"]}",
			   &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_EQ(g_last_min_choices, 2);
	EXPECT_EQ(g_last_max_choices, 2);
	cJSON *min = cJSON_GetObjectItem(result.data, "min_choices");
	cJSON *max = cJSON_GetObjectItem(result.data, "max_choices");
	ASSERT_TRUE(cJSON_IsNumber(min));
	ASSERT_TRUE(cJSON_IsNumber(max));
	EXPECT_EQ(min->valueint, 2);
	EXPECT_EQ(max->valueint, 2);
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	ASSERT_EQ(cJSON_GetArraySize(answers), 2);
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
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	EXPECT_EQ(cJSON_GetArraySize(answers), 0);
	tool_result_cleanup(&result);
}

TEST_F(AskUserToolTest, EmptyStringAnswerIsNoInput) {
	ask_user_init(&tools, mock_ask_user_invalid_empty_string_cb, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&tools, "ask_user",
			   "{\"question\":\"hello?\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.data, nullptr);
	cJSON *no_input = cJSON_GetObjectItem(result.data, "no_input");
	ASSERT_TRUE(cJSON_IsBool(no_input));
	EXPECT_TRUE(cJSON_IsTrue(no_input));
	cJSON *answers = cJSON_GetObjectItem(result.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers));
	EXPECT_EQ(cJSON_GetArraySize(answers), 0);
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
	cJSON *answers1 = cJSON_GetObjectItem(result1.data, "answers");
	cJSON *answers2 = cJSON_GetObjectItem(result2.data, "answers");
	ASSERT_TRUE(cJSON_IsArray(answers1));
	ASSERT_TRUE(cJSON_IsArray(answers2));
	ASSERT_EQ(cJSON_GetArraySize(answers1), 1);
	ASSERT_EQ(cJSON_GetArraySize(answers2), 1);
	EXPECT_STREQ(cJSON_GetArrayItem(answers1, 0)->valuestring, "first");
	EXPECT_STREQ(cJSON_GetArrayItem(answers2, 0)->valuestring, "second");

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
