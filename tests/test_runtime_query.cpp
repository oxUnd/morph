#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/tools/runtime_query.h"
#include "agent/memory.h"
#include "credits.h"
#include "session.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

class RuntimeQueryTest : public ::testing::Test {
protected:
	struct db db;
	struct config cfg;
	struct tool_registry tools;
	char db_path[256];

	void SetUp() override {
		memset(&db, 0, sizeof(db));
		config_set_defaults(&cfg);
		tool_registry_init(&tools);
		snprintf(db_path, sizeof(db_path),
			 "/tmp/morph_runtime_query_%d.db", getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(runtime_query_tools_init(&tools), 0);
	}

	void TearDown() override {
		tool_runtime_set_current(nullptr);
		tool_registry_cleanup(&tools);
		db_close(&db);
		std::remove(db_path);
	}

	void SetRuntime(const struct session &s) {
		struct tool_runtime_context rt;

		memset(&rt, 0, sizeof(rt));
		rt.db = &db;
		rt.config = &cfg;
		rt.user_id = "local";
		rt.credit_session_id = s.display_id[0] ? s.display_id : s.name;
		rt.memory_session_id = s.id;
		rt.restrict_memory_to_user = 0;
		tool_runtime_set_current(&rt);
	}
};

TEST_F(RuntimeQueryTest, RegistersReadOnlyTools)
{
	struct tool_entry *credits = tool_lookup(&tools, "credits");
	struct tool_entry *memory_tool = tool_lookup(&tools, "memory");

	ASSERT_NE(credits, nullptr);
	ASSERT_NE(memory_tool, nullptr);
	EXPECT_TRUE((credits->flags & TOOL_FLAG_READONLY) != 0);
	EXPECT_TRUE((memory_tool->flags & TOOL_FLAG_READONLY) != 0);
}

TEST_F(RuntimeQueryTest, CreditsReportsTodaySessionAndTotal)
{
	struct session s;
	struct credit_event event;
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_credits", "gpt-test", &s), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	cfg.credits.input_token_credit_coef = 1.0;
	cfg.credits.daily_limit = 5;

	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = s.display_id;
	event.kind = "model_text";
	event.provider = "openai";
	event.model = "gpt-test";
	event.input_tokens = 7;
	ASSERT_EQ(credit_record_event(&db, &cfg.credits, &event, nullptr), 0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "credits", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = result.data;
	ASSERT_NE(root, nullptr);
	cJSON *today = cJSON_GetObjectItem(root, "today");
	cJSON *session = cJSON_GetObjectItem(root, "session");
	cJSON *total = cJSON_GetObjectItem(root, "total");
	ASSERT_TRUE(cJSON_IsObject(today));
	ASSERT_TRUE(cJSON_IsObject(session));
	ASSERT_TRUE(cJSON_IsObject(total));
	EXPECT_EQ(cJSON_GetObjectItem(today, "credits")->valueint, 7);
	EXPECT_EQ(cJSON_GetObjectItem(session, "credits")->valueint, 7);
	EXPECT_EQ(cJSON_GetObjectItem(total, "credits")->valueint, 7);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(root, "over_daily_limit")));
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryQueriesSessionAndType)
{
	struct session s;
	struct memory_options opts;
	struct react_step action = {};
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_memory", "gpt-test", &s), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	memset(&opts, 0, sizeof(opts));
	opts.enabled = 1;
	opts.hot_path_enabled = 1;
	opts.cold_path_enabled = 1;
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  &action, 1, &opts),
		  0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"session\",\"type\":\"facts\"}",
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(std::strstr(text->valuestring, "facts"), nullptr);
	EXPECT_EQ(std::strstr(text->valuestring, "episodes"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryAllScopeIncludesMultipleSessions)
{
	struct session first;
	struct session second;
	struct memory_options opts;
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_memory_one", "gpt-test", &first),
		  0);
	ASSERT_EQ(session_create(&db, "runtime_memory_two", "gpt-test", &second),
		  0);
	ASSERT_EQ(session_ensure_display_id(&db, &first), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &second), 0);
	SetRuntime(first);
	memset(&opts, 0, sizeof(opts));
	opts.enabled = 1;
	opts.hot_path_enabled = 1;
	opts.cold_path_enabled = 1;

	ASSERT_EQ(memory_consolidate_turn(&db, first.id,
					  "Call me Ada.",
					  "Okay, Ada.", nullptr, 1, &opts),
		  0);
	ASSERT_EQ(memory_consolidate_turn(&db, second.id,
					  "Call me Grace.",
					  "Okay, Grace.", nullptr, 1, &opts),
		  0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory", "{\"scope\":\"all\"}", &result),
		  0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_one"), nullptr);
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_two"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}
