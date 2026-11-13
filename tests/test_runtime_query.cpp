#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/tools/runtime_query.h"
#include "agent/memory.h"
#include "credits.h"
#include "session.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/utf8.h"
#include "agent/tokenizer.h"
#include "persistence/memory_repository.h"
#include "persistence/memory_summary_store.h"
#include "persistence/memory_search.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static int CountOccurrences(const char *text, const char *needle)
{
	int count = 0;
	size_t needle_len;

	if (!text || !needle || !*needle)
		return 0;
	needle_len = std::strlen(needle);
	while ((text = std::strstr(text, needle)) != nullptr) {
		count++;
		text += needle_len;
	}
	return count;
}

class RuntimeQueryTest : public ::testing::Test {
protected:
	struct db db;
	struct config cfg;
	struct tool_registry tools;
	struct tool_runtime_context rt{};
	char db_path[256];

	void SetUp() override {
		memset(&db, 0, sizeof(db));
		config_set_defaults(NULL, &cfg);
		tool_registry_init(NULL, &tools);
		snprintf(db_path, sizeof(db_path),
			 "/tmp/morph_runtime_query_%d.db", getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(NULL, &db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(runtime_query_tools_init(&tools), 0);
	}

	void TearDown() override {
		tool_registry_cleanup(&tools);
		db_close(&db);
		std::remove(db_path);
	}

	void SetRuntime(const struct session &s) {

		memset(&rt, 0, sizeof(rt));
		rt.db = &db;
		rt.config = &cfg;
		rt.user_id = "local";
		rt.credit_session_id = s.display_id[0] ? s.display_id : s.name;
		rt.memory_session_id = s.id;
		rt.restrict_memory_to_user = 0;
	}
};

TEST_F(RuntimeQueryTest, RegistersReadOnlyTools)
{
	struct tool_entry *credits = tool_borrow(&tools, "credits");
	struct tool_entry *memory_tool = tool_borrow(&tools, "memory");

	ASSERT_NE(credits, nullptr);
	ASSERT_NE(memory_tool, nullptr);
	EXPECT_TRUE((credits->flags & TOOL_FLAG_READONLY) != 0);
	EXPECT_TRUE((memory_tool->flags & TOOL_FLAG_READONLY) != 0);
	EXPECT_NE(std::strstr(memory_tool->desc.description,
			      "injected context reports omitted"),
		  nullptr);

	cJSON *schema = cJSON_Parse(memory_tool->desc.input_schema);
	ASSERT_NE(schema, nullptr);
	cJSON *required = cJSON_GetObjectItem(schema, "required");
	ASSERT_TRUE(cJSON_IsArray(required));
	ASSERT_EQ(cJSON_GetArraySize(required), 2);
	EXPECT_STREQ(cJSON_GetArrayItem(required, 0)->valuestring, "type");
	EXPECT_STREQ(cJSON_GetArrayItem(required, 1)->valuestring, "scope");
	cJSON_Delete(schema);
}

TEST_F(RuntimeQueryTest, PreferenceQueryRecoversWholeValuesOmittedByContextBudget)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "preference-recovery", "mock", &session), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &session), 0);
	SetRuntime(session);
	ASSERT_EQ(preference_bind(&db, session.id, "local", ""), 0);
	std::string value(900, 'x');
	value += "FULL_PREFERENCE_END";
	ASSERT_EQ(preference_set(&db, session.id, "personal", "custom.rule", value.c_str(),
				 "Explicit request", "input"), 0);
	struct memory_options opts = {};
	opts.enabled = 1;
	opts.max_reply_chars = 512;
	char *context NB_CLEANUP(nb_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "hello", &opts, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(strstr(context, "custom.rule"), nullptr);
	EXPECT_NE(strstr(context, "omitted records"), nullptr);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
		"{\"scope\":\"session\",\"type\":\"preferences\"}", &result), 0);
	ASSERT_NE(result.data, nullptr);
	cJSON *text = cJSON_GetObjectItemCaseSensitive(result.data, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(strstr(text->valuestring, value.c_str()), nullptr);
}

TEST_F(RuntimeQueryTest, LongUnicodePastTaskCanBeRecoveredInBoundedUtf8Pages)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "episode-recovery", "mock", &session), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &session), 0);
	SetRuntime(session);
	std::string request;
	std::string answer;
	for (int i = 0; i < 6000; i++)
		request += "界";
	for (int i = 0; i < 3000; i++)
		answer += "雪";
	request += "REQUEST_TAIL_MARKER";
	answer += "ANSWER_TAIL_MARKER";
	std::string tool_name = "mcp__" + std::string(400, 't') + "_TOOL_TAIL_MARKER";
	struct react_step step = {};
	step.type = REACT_STEP_ACTION;
	step.tool_name = const_cast<char *>(tool_name.c_str());
	struct memory_options options = {};
	options.enabled = 1;
	options.save_tasks_enabled = 1;
	options.max_reply_chars = 400;
	ASSERT_EQ(memory_save_turn(&db, session.id, request.c_str(), answer.c_str(), &step, 1,
				   &options),
		  0);
	char *context NB_CLEANUP(nb_charp_cleanup) = nullptr;
	ASSERT_EQ(memory_reply_text_create(&db, session.id, "previous", &options, &context), 0);
	ASSERT_NE(context, nullptr);
	EXPECT_EQ(strstr(context, "REQUEST_TAIL_MARKER"), nullptr);
	EXPECT_NE(strstr(context, "omitted records"), nullptr);
	std::string recovered;
	size_t offset = 0;
	int pages = 0;
	for (;;) {
		std::string args = "{\"type\":\"episodes\",\"scope\":\"session\","
			"\"max_episodes\":1,\"max_chars\":2000,\"offset_chars\":" +
			std::to_string(offset) + "}";
		struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
		tool_result_init(&result);
		result.runtime = &rt;
		ASSERT_EQ(tool_exec(&tools, "memory", args.c_str(), &result), 0);
		ASSERT_NE(result.data, nullptr);
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "text"));
		ASSERT_NE(text, nullptr);
		EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr(text), NULL), NB_UTF8_OK);
		EXPECT_LE(nb_utf8_count(nb_str_from_cstr(text)), 2000u);
		EXPECT_LE(tokenizer_estimate_tokens(result.text.ptr),
			  cfg.context.tool_result_max_tokens);
		recovered += text;
		pages++;
		if (!cJSON_IsTrue(cJSON_GetObjectItem(result.data, "has_more")))
			break;
		size_t next = static_cast<size_t>(
			cJSON_GetObjectItem(result.data, "next_offset_chars")->valuedouble);
		ASSERT_GT(next, offset);
		offset = next;
		ASSERT_LT(pages, 20);
	}
	EXPECT_GT(pages, 1);
	EXPECT_NE(recovered.find(request), std::string::npos);
	EXPECT_NE(recovered.find(answer), std::string::npos);
	EXPECT_NE(recovered.find(tool_name), std::string::npos);
}

TEST_F(RuntimeQueryTest, MemoryPageBudgetIncludesEscapingAndCompatibilityFields)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "escaped-page", "mock", &session), 0);
	SetRuntime(session);
	struct memory_options options = {};
	options.enabled = 1;
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &options), 0);
	std::string value;
	for (int i = 0; i < 500; i++)
		value += "\"\\\n界";
	ASSERT_EQ(memory_store_upsert_fact(&db, session.id, "escaped", value.c_str(), "source",
					  "context", 0.8), 0);
	cfg.context.tool_result_max_tokens = 512;
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory", "{\"scope\":\"session\",\"type\":\"facts\"}",
			    &result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_LE(tokenizer_estimate_tokens(result.text.ptr), cfg.context.tool_result_max_tokens);
	EXPECT_GT(cJSON_GetObjectItem(result.data, "returned_chars")->valueint, 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result.data, "has_more")));
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "text"));
	ASSERT_NE(text, nullptr);
	EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr(text), NULL), NB_UTF8_OK);
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
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "credits", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
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
	opts.capture_input_enabled = 1;
	opts.save_tasks_enabled = 1;
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(memory_accept_input(&db, s.id, "My name is Alice.", "input", &opts), 0);
	ASSERT_EQ(memory_save_turn(&db, s.id, "Please always reply in Chinese and be concise.",
				   "好的，我之后会用中文简洁回答。", &action, 1, &opts),
		  0);

	ASSERT_EQ(preference_set(&db, s.id, "personal", "response.language", "Chinese", "explicit",
				 "pref"),
		  0);
	ASSERT_EQ(memory_store_upsert_fact(&db, s.id, "goal", "Check project", "explicit",
					   "project", .5),
		  0);
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"session\",\"type\":\"facts\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	cJSON *root = cJSON_Parse(result.text.ptr);
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
	opts.capture_input_enabled = 1;
	opts.save_tasks_enabled = 1;

	ASSERT_EQ(memory_save_turn(&db, first.id, "Call me Ada.", "Okay, Ada.", nullptr, 1, &opts),
		  0);
	ASSERT_EQ(memory_save_turn(&db, second.id, "Call me Grace.", "Okay, Grace.", nullptr, 1,
				   &opts),
		  0);

	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	cJSON *root = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root, nullptr);
	cJSON *scope = cJSON_GetObjectItem(root, "scope");
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(scope));
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_STREQ(scope->valuestring, "session");
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_one"), nullptr);
	EXPECT_EQ(std::strstr(text->valuestring, "runtime_memory_two"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"all\",\"type\":\"all\"}", &result),
		  0);
	ASSERT_NE(result.text.ptr, nullptr);
	root = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root, nullptr);
	text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_one"), nullptr);
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_two"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryUsesConfiguredPastTaskLimitByDefault)
{
	struct session s;
	struct tool_result result;
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT INTO memory_episodes"
		"(session_id,summary_text,created_at) VALUES(?,?,?)";

	ASSERT_EQ(session_create(&db, "runtime_memory_limit", "gpt-test", &s),
		  0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	cfg.memory.max_past_tasks = 2;

	ASSERT_EQ(sqlite3_prepare_v2(db.handle, sql, -1, &stmt, nullptr),
		  SQLITE_OK);
	for (int i = 1; i <= 3; i++) {
		char summary[32];

		std::snprintf(summary, sizeof(summary), "episode-%d", i);
		sqlite3_bind_int64(stmt, 1, s.id);
		sqlite3_bind_text(stmt, 2, summary, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, i);
		ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
		ASSERT_EQ(sqlite3_reset(stmt), SQLITE_OK);
		ASSERT_EQ(sqlite3_clear_bindings(stmt), SQLITE_OK);
	}
	sqlite3_finalize(stmt);

	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"session\",\"type\":\"episodes\"}",
			    &result), 0);
	cJSON *root = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root, nullptr);
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_EQ(CountOccurrences(text->valuestring, "episode-"), 2);
	cJSON_Delete(root);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(
			  &tools, "memory",
			  "{\"scope\":\"session\",\"type\":\"episodes\","
			  "\"max_episodes\":1}",
			  &result),
		  0);
	root = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root, nullptr);
	text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_EQ(CountOccurrences(text->valuestring, "episode-"), 1);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, ExplicitPreferenceToolReplacesGenericPreferenceAndRejectsOldEvidence)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "preferences", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	struct model_history_insert input = {};
	input.session_id = session.id;
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.turn_id = "one";
	input.content = "以后代码缩进使用四个空格";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"code.indent\",\"value\":\"four spaces\",\"scope\":\"personal\","
		"\"evidence\":\"以后代码缩进使用四个空格\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "four spaces"), nullptr);
	tool_result_cleanup(&result);
	input.turn_id = "two";
	input.content = "以后代码缩进使用 Tab";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"code.indent\",\"value\":\"tab\",\"scope\":\"personal\","
		"\"evidence\":\"以后代码缩进使用 Tab\"}", &result), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_LT(
		tool_exec(
			&tools, "memory_preference",
			"{\"key\":\"code.indent\",\"value\":\"four spaces\",\"scope\":\"personal\","
			"\"evidence\":\"以后代码缩进使用四个空格\"}",
			&result),
		0);
	tool_result_cleanup(&result);
	char *effective = preference_render_copy(&db, session.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "code.indent: tab"), nullptr);
	EXPECT_EQ(strstr(effective, "four spaces"), nullptr);
	free(effective);
}

TEST_F(RuntimeQueryTest, InputPreparationDoesNotPromoteComplaintOrTemporaryRequest)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "no-inference", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	struct memory_options options = {};
	options.enabled = 1;
	options.capture_input_enabled = 1;
	for (const char *text : {"Why do you always use English?", "这次用英文回答",
				 "¿Por qué siempre respondes en inglés?"}) {
		ASSERT_EQ(memory_accept_input(&db, session.id, text, text, &options), 0);
		char *effective NB_CLEANUP(nb_charp_cleanup) =
			preference_render_copy(&db, session.id, 0);
		ASSERT_NE(effective, nullptr);
		EXPECT_STREQ(effective, "");
	}
}

TEST_F(RuntimeQueryTest, ExtractionOfSameInputCannotContradictCommittedLanguage)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "same-event", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	struct model_history_insert input = {};
	input.session_id = session.id;
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.turn_id = "same-input";
	input.content = "以后都用中文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	struct memory_options options = {};
	options.enabled = 1;
	options.capture_input_enabled = 1;
	ASSERT_EQ(memory_accept_input(&db, session.id, input.content, input.turn_id, &options), 0);
	ASSERT_EQ(preference_set(&db, session.id, "personal", "response.language", "Chinese",
				 input.content, "history:1"),
		  0);
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_LT(tool_exec(&tools, "memory_preference",
			    "{\"key\":\"preferred_language\",\"value\":\"English\",\"scope\":"
			    "\"personal\","
			    "\"evidence\":\"以后都用中文\"}",
			    &result),
		  0);
	tool_result_cleanup(&result);
	char *effective = preference_render_copy(&db, session.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	EXPECT_EQ(strstr(effective, "English"), nullptr);
	free(effective);
}

TEST_F(RuntimeQueryTest, ProjectSearchRecallsDerivedKnowledgeWithSourcesAndReportsScope)
{
	struct session previous, current;
	ASSERT_EQ(session_create(&db, "search-previous", "mock", &previous), 0);
	ASSERT_EQ(session_create(&db, "search-current", "mock", &current), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &previous), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &current), 0);
	ASSERT_EQ(preference_bind(&db, previous.id, "local", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, current.id, "local", "/project"), 0);
	ASSERT_EQ(memory_store_add_note(&db, previous.id, "project", "composer_layout",
					"Composer uses explicit insets", "Verified layout source"),
		  0);
	SetRuntime(current);
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"type\":\"all\",\"scope\":\"project\","
			    "\"query\":\"composer\"}",
			    &result),
		  0);
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(root, "scope")), "project");
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
	ASSERT_NE(text, nullptr);
	EXPECT_NE(strstr(text, "Composer uses explicit insets"), nullptr);
	EXPECT_NE(strstr(text, "Verified layout source"), nullptr);
	EXPECT_NE(strstr(text, previous.display_id), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, PlainTaskAndTipTypesWorkAlongsideOlderTypeNames)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "plain-memory-types", "mock", &session), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	ASSERT_EQ(memory_store_add_note(&db, session.id, "tip", "composer_checks",
					"Run composer tests", "Observed passing tests"),
		  0);
	SetRuntime(session);
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory", "{\"type\":\"tips\",\"scope\":\"session\"}", &result),
		  0);
	EXPECT_NE(strstr(result.text.ptr, "Run composer tests"), nullptr);
	tool_result_cleanup(&result);
	struct memory_options options = {};
	options.enabled = 1;
	options.save_tasks_enabled = 1;
	ASSERT_EQ(memory_save_turn(&db, session.id, "Verify composer", "Checked", nullptr, 1,
				   &options),
		  0);
	for (const char *type : {"past_tasks", "episodes"}) {
		std::string args = std::string("{\"type\":\"") + type +
				   "\",\"scope\":\"session\",\"max_past_tasks\":1}";
		tool_result_init(&result);
		result.runtime = &rt;
		ASSERT_EQ(tool_exec(&tools, "memory", args.c_str(), &result), 0);
		EXPECT_NE(strstr(result.text.ptr, "Verify composer"), nullptr);
		tool_result_cleanup(&result);
	}
}

static int memory_test_user(struct db *database, int64_t session_id, const char *text)
{
	struct model_history_insert input = {};
	input.session_id = session_id;
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.content = text;
	return model_history_add(database, &input, nullptr);
}

TEST_F(RuntimeQueryTest, ModelCanSaveReadCorrectAndForgetProjectNotesWithVersionChecks)
{
	struct session origin, current;
	ASSERT_EQ(session_create(&db, "note-origin", "mock", &origin), 0);
	ASSERT_EQ(session_create(&db, "note-current", "mock", &current), 0);
	ASSERT_EQ(preference_bind(&db, origin.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, current.id, "alice", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, origin.id, "Staging", "Old account",
				   "Original user request", "old", "project"),
		  0);
	cfg.memory.enabled = 1;
	SetRuntime(current);
	ASSERT_EQ(memory_test_user(&db, current.id, "Please correct the saved staging account."),
		  0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"type\":\"notes\",\"scope\":\"project\",\"record_id\":1}", &result),
		  0);
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "text"));
	ASSERT_NE(text, nullptr);
	EXPECT_NE(strstr(text, "Old account"), nullptr);
	EXPECT_NE(strstr(text, "version=0"), nullptr);
	const char *replace =
		"{\"action\":\"replace\",\"type\":\"notes\",\"scope\":\"project\","
		"\"record_id\":1,\"expected_version\":0,\"expected_text\":\"Old account\","
		"\"text\":\"New account\",\"evidence\":\"Please correct the saved staging "
		"account.\"}";
	ASSERT_EQ(tool_exec(&tools, "memory_update", replace, &result), 0);
	EXPECT_LT(tool_exec(&tools, "memory_update", replace, &result), 0);
	ASSERT_EQ(memory_test_user(&db, current.id, "Please forget the saved staging account."), 0);
	ASSERT_EQ(
		tool_exec(
			&tools, "memory_update",
			"{\"action\":\"delete\",\"type\":\"notes\",\"scope\":\"project\","
			"\"record_id\":1,\"expected_version\":1,\"expected_text\":\"New account\","
			"\"evidence\":\"Please forget the saved staging account.\"}",
			&result),
		0);
	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "notes";
	search.project = 1;
	ASSERT_EQ(memory_search_records(&db, current.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.len, 0u);
}

TEST_F(RuntimeQueryTest, ModelNoteSaveIsIdempotentAndRejectsOldOrInventedEvidence)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "note-save", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	ASSERT_EQ(memory_test_user(&db, session.id,
				   "今後のために、ステージング環境が必要だと覚えてください。"),
		  0);
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON_AddStringToObject(args, "action", "save");
	cJSON_AddStringToObject(args, "title", "ステージング環境");
	cJSON_AddStringToObject(args, "text", "Staging is required for integration tests");
	cJSON_AddStringToObject(args, "evidence",
				"今後のために、ステージング環境が必要だと覚えてください。");
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(args);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(tool_exec(&tools, "memory_update", json, &result), 0);
	ASSERT_EQ(tool_exec(&tools, "memory_update", json, &result), 0);
	cJSON_SetValuestring(cJSON_GetObjectItem(args, "text"),
			     "Conflicting interpretation of the same event");
	char *conflict NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(args);
	EXPECT_LT(tool_exec(&tools, "memory_update", conflict, &result), 0);
	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "notes";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	EXPECT_EQ(matches.records.len, 1u);
	ASSERT_EQ(memory_test_user(&db, session.id, "Continue with the next task."), 0);
	EXPECT_LT(tool_exec(&tools, "memory_update", json, &result), 0);
}

TEST_F(RuntimeQueryTest, ModelPreferencesSupportLanguagesBeyondEnglishAndChinese)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "multilingual-preference", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	const std::pair<const char *, const char *> cases[] = {
		{"今後は日本語で回答してください。", "Japanese"},
		{"A partir de ahora, responde siempre en español.", "Spanish"},
		{"من الآن فصاعدًا أجب باللغة العربية دائمًا.", "Arabic"},
	};
	for (const auto &entry : cases) {
		ASSERT_EQ(memory_test_user(&db, session.id, entry.first), 0);
		cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "key", "response.language");
		cJSON_AddStringToObject(args, "value", entry.second);
		cJSON_AddStringToObject(args, "scope", "personal");
		cJSON_AddStringToObject(args, "evidence", entry.first);
		char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(args);
		struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
		tool_result_init(&result);
		result.runtime = &rt;
		ASSERT_EQ(tool_exec(&tools, "memory_preference", json, &result), 0);
		char *effective NB_CLEANUP(nb_charp_cleanup) =
			preference_render_copy(&db, session.id, 0);
		EXPECT_NE(strstr(effective, entry.second), nullptr);
	}
}

TEST_F(RuntimeQueryTest, MemoryEditsCannotAccessAnotherProjectOrUser)
{
	struct session current, other;
	ASSERT_EQ(session_create(&db, "edit-current", "mock", &current), 0);
	ASSERT_EQ(session_create(&db, "edit-other", "mock", &other), 0);
	ASSERT_EQ(preference_bind(&db, current.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, other.id, "bob", "/project"), 0);
	ASSERT_EQ(memory_note_save(&db, other.id, "Private", "Other user's memory", "source",
				   "note", "project"),
		  0);
	cfg.memory.enabled = 1;
	SetRuntime(current);
	ASSERT_EQ(memory_test_user(&db, current.id, "Forget that old saved note."), 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	EXPECT_LT(tool_exec(&tools, "memory_update",
			    "{\"action\":\"delete\",\"type\":\"notes\",\"record_id\":1,\"expected_"
			    "version\":0,"
			    "\"expected_text\":\"Other user's memory\",\"evidence\":\"Forget that "
			    "old saved note.\"}",
			    &result),
		  0);
}

TEST_F(RuntimeQueryTest, SavedNotesRedactCredentialsInContentAndSource)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "redact-note", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "alice", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	ASSERT_EQ(memory_test_user(&db, session.id,
				   "Remember staging uses sk-test-do-not-store this key."),
		  0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	result.runtime = &rt;
	ASSERT_EQ(
		tool_exec(&tools, "memory_update",
			  "{\"action\":\"save\",\"title\":\"Staging\","
			  "\"text\":\"Staging uses sk-test-do-not-store this key.\","
			  "\"evidence\":\"Remember staging uses sk-test-do-not-store this key.\"}",
			  &result),
		0);
	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search = {};
	search.kind = "notes";
	ASSERT_EQ(memory_search_records(&db, session.id, &search, &matches), 0);
	ASSERT_EQ(matches.records.len, 1u);
	auto *record = static_cast<struct memory_record *>(nb_vec_at(&matches.records, 0));
	EXPECT_EQ(strstr(record->value, "do-not-store"), nullptr);
	EXPECT_EQ(strstr(record->source, "do-not-store"), nullptr);
	EXPECT_NE(strstr(record->value, "[REDACTED]"), nullptr);
}
