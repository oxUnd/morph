#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"

extern "C" {
#include "agent/agent_control.h"
#include "agent/tools/multi_agents.h"
#include "agent/tool.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "session.h"
#include "util/utf8.h"
#include "util/json.h"
#include "util/cleanup.h"
}

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class AgentEventBatchingTest : public testing::Test
{
      protected:
	AgentHttpServer server;
	struct db database{};
	struct session session{};
	struct tool_registry tools{};
	struct model *model = nullptr;
	struct tokenizer *tokenizer = nullptr;
	struct agent_control *control = nullptr;
	struct tool_runtime_context runtime{};
	struct config_agents config{};
	std::mutex live_mutex;
	std::string reasoning;
	std::vector<int64_t> live_sequences;
	int live_deltas = 0;
	int first_delta_stored = -1;
	bool subscribe_on_start = false;
	int subscription_rc = -EINPROGRESS;

	void SetUp() override
	{
		server.start();
		ASSERT_EQ(db_open(nullptr, &database, ":memory:"), 0);
		ASSERT_EQ(db_init_schema(&database), 0);
		ASSERT_EQ(session_create(&database, "stream root", "test-model", &session), 0);
		tool_registry_init(nullptr, &tools);
		std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
		model = model_llm_create(nullptr, "deepseek", "test-model", url.c_str(),
					 "test-key");
		ASSERT_NE(model, nullptr);
		model->retry_count = 0;
		tokenizer = tokenizer_create(nullptr, "test-model", 128000);
		config.enabled = 1;
		config.max_threads = 6;
		config.max_depth = 2;
		strcpy(config.multi_agent_mode, "proactive");
		control =
			agent_control_create(&tools, model, tokenizer, nullptr, &config, &database);
		ASSERT_NE(control, nullptr);
		ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
		ASSERT_EQ(multi_agents_register(&tools, control), 0);
		runtime.db = &database;
		runtime.memory_session_id = session.id;
		runtime.agent_id = "/root";
		ASSERT_EQ(agent_control_set_event_callback(control, live_event, this), 0);
	}

	void TearDown() override
	{
		agent_control_destroy(control);
		tool_registry_cleanup(&tools);
		model_destroy(model);
		tokenizer_destroy(tokenizer);
		db_close(&database);
	}

	static int live_event(const struct morph_event *event, void *opaque)
	{
		auto *self = static_cast<AgentEventBatchingTest *>(opaque);
		if (self->subscribe_on_start && event->name &&
		    !strcmp(event->name, "agent.started")) {
			char **events = nullptr;
			int count = 0;
			int64_t frontier = 0;
			self->subscription_rc = agent_control_subscribe(
				self->control, "/root/stream", &events, &count, &frontier);
			agent_control_free_events(events, count);
		}
		const cJSON *sequence =
			cJSON_GetObjectItemCaseSensitive(event->data, "agent_event_sequence");
		if (cJSON_IsNumber(sequence)) {
			std::lock_guard<std::mutex> lock(self->live_mutex);
			self->live_sequences.push_back((int64_t)sequence->valuedouble);
		}
		if (!event->name || strcmp(event->name, "react.reasoning.delta"))
			return 0;
		const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(event->data, "text"));
		std::lock_guard<std::mutex> lock(self->live_mutex);
		self->reasoning += text ? text : "";
		self->live_deltas++;
		if (self->first_delta_stored < 0) {
			sqlite3_stmt *statement = nullptr;
			if (sqlite3_prepare_v2(
				    self->database.handle,
				    "SELECT count(*) FROM agent_events WHERE "
				    "json_extract(event_json,'$.name')='react.reasoning.delta'",
				    -1, &statement, nullptr) == SQLITE_OK &&
			    sqlite3_step(statement) == SQLITE_ROW)
				self->first_delta_stored = sqlite3_column_int(statement, 0);
			sqlite3_finalize(statement);
		}
		return 0;
	}

	void spawn()
	{
		cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(
			R"({"task_name":"stream","message":"Inspect independently","fork_turns":"none"})");
		cJSON *out MORPH_CLEANUP(morph_json_cleanup) = nullptr;
		EXPECT_EQ(agent_control_call(control, "spawn_agent", args, &runtime, &out), 0);
	}

	std::vector<std::string> events()
	{
		char **items = nullptr;
		int count = 0;
		EXPECT_EQ(agent_control_events_copy(control, "/root/stream", &items, &count), 0);
		std::vector<std::string> result;
		for (int i = 0; i < count; i++)
			result.emplace_back(items[i]);
		agent_control_free_events(items, count);
		return result;
	}
};

TEST_F(AgentEventBatchingTest, ImmediateSubscriptionInitializesBeforeFirstChildEvent)
{
	subscribe_on_start = true;
	server.respond = [](const std::string &) -> std::string {
		return R"({"content":"Immediate child result"})";
	};
	spawn();
	agent_control_join(control);
	ASSERT_EQ(subscription_rc, 0);
	ASSERT_FALSE(live_sequences.empty());
	EXPECT_EQ(live_sequences.front(), 1);
	for (size_t i = 1; i < live_sequences.size(); i++)
		EXPECT_EQ(live_sequences[i], live_sequences[i - 1] + 1);
	char **snapshot = nullptr;
	int count = 0;
	int64_t frontier = 0;
	ASSERT_EQ(agent_control_subscribe(control, "/root/stream", &snapshot, &count, &frontier),
		  0);
	EXPECT_GT(count, 0);
	EXPECT_EQ(frontier, live_sequences.back());
	for (int i = 0; i < count; i++) {
		cJSON *record MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(snapshot[i]);
		const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(
			cJSON_GetObjectItemCaseSensitive(record, "data"), "agent_event_sequence");
		ASSERT_TRUE(cJSON_IsNumber(sequence));
		EXPECT_LE((int64_t)sequence->valuedouble, frontier);
	}
	agent_control_free_events(snapshot, count);
}

TEST_F(AgentEventBatchingTest, RestoredSubscriptionPreservesFrontierAcrossFollowup)
{
	server.respond = [](const std::string &) -> std::string {
		return R"({"content":"Restored child result"})";
	};
	spawn();
	agent_control_join(control);
	char **snapshot = nullptr;
	int count = 0;
	int64_t previous = 0;
	ASSERT_EQ(agent_control_subscribe(control, "/root/stream", &snapshot, &count, &previous),
		  0);
	ASSERT_GT(previous, 0);
	agent_control_free_events(snapshot, count);
	snapshot = nullptr;
	agent_control_destroy(control);
	control = agent_control_create(&tools, model, tokenizer, nullptr, &config, &database);
	ASSERT_NE(control, nullptr);
	ASSERT_EQ(agent_control_set_parent_session(control, session.id), 0);
	ASSERT_EQ(agent_control_set_event_callback(control, live_event, this), 0);
	int64_t restored = 0;
	ASSERT_EQ(agent_control_subscribe(control, "/root/stream", &snapshot, &count, &restored),
		  0);
	EXPECT_EQ(restored, previous);
	EXPECT_GT(count, 0);
	agent_control_free_events(snapshot, count);
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) =
		cJSON_Parse(R"({"target":"stream","message":"Follow up after restart"})");
	cJSON *out MORPH_CLEANUP(morph_json_cleanup) = nullptr;
	ASSERT_EQ(agent_control_call(control, "followup_task", args, &runtime, &out), 0);
	agent_control_join(control);
	ASSERT_FALSE(live_sequences.empty());
	EXPECT_EQ(live_sequences.front(), previous + 1);
	for (size_t i = 1; i < live_sequences.size(); i++)
		EXPECT_EQ(live_sequences[i], live_sequences[i - 1] + 1);
}

TEST_F(AgentEventBatchingTest, CoalescesReasoningWithoutDelayingSelectedLiveStream)
{
	std::string expected;
	for (int i = 0; i < 2000; i++)
		expected += "思考👩‍💻。";
	auto release = std::make_shared<std::promise<void>>();
	auto ready = release->get_future().share();
	struct tool_spec spec{};
	spec.name = "emit_reasoning";
	spec.description = "Emit deterministic reasoning fragments";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.flags = TOOL_FLAG_READONLY;
	spec.exec = [](const char *, struct tool_result *result, void *) -> int {
		cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		if (!data || !cJSON_AddStringToObject(data, "text", "思考👩‍💻。"))
			return -ENOMEM;
		struct morph_event event{};
		event.type = MORPH_EVENT_REACT;
		event.name = "react.reasoning.delta";
		event.phase = "delta";
		event.data = data;
		event.turn_id = result->runtime->turn_id;
		for (int i = 0; i < 2000; i++) {
			int rc =
				result->runtime->event_cb(&event, result->runtime->event_user_data);
			if (rc != 0)
				return rc;
		}
		return tool_result_success_text_copy(result, "Reasoning emitted");
	};
	ASSERT_EQ(tool_register_borrow(&tools, &spec), 0);
	server.respond = [ready](const std::string &body) -> std::string {
		ready.wait_for(std::chrono::seconds(5));
		if (body.find("call_reasoning") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_reasoning","type":"function",)"
			       R"("function":{"name":"emit_reasoning","arguments":"{}"}}]})";
		return R"({"content":"Verified result"})";
	};
	spawn();
	ASSERT_TRUE(server.await_requests(1));
	ASSERT_EQ(agent_control_select(control, "/root/stream"), 0);
	release->set_value();
	agent_control_join(control);
	std::string stored;
	int records = 0;
	bool left_reasoning = false;
	for (const auto &json : events()) {
		cJSON *event MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(json.c_str());
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(event, "name"));
		if (name && !strcmp(name, "react.reasoning.delta")) {
			EXPECT_FALSE(left_reasoning);
			const char *text = cJSON_GetStringValue(
				cJSON_GetObjectItem(cJSON_GetObjectItem(event, "data"), "text"));
			ASSERT_NE(text, nullptr);
			EXPECT_EQ(utf8valid(text), nullptr);
			EXPECT_LE(strlen(text), 8192u);
			stored += text;
			records++;
		} else if (records) {
			left_reasoning = true;
		}
	}
	EXPECT_EQ(stored, expected);
	EXPECT_EQ(reasoning, expected);
	EXPECT_EQ(first_delta_stored, 0);
	EXPECT_GT(live_deltas, records * 10);
	EXPECT_LT(records, 10);
}

TEST_F(AgentEventBatchingTest, FlushesAtToolStreamKindAndTurnBoundariesWithoutLosingOversizedChunks)
{
	struct tool_spec spec{};
	spec.name = "emit_sequences";
	spec.description = "Emit deterministic stream segments";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.flags = TOOL_FLAG_READONLY;
	spec.exec = [](const char *, struct tool_result *result, void *) -> int {
		struct tool_runtime_context next = *result->runtime;
		int rc = tool_runtime_emit_stream(&next, "emit_sequences", "stdout", "甲");
		if (rc == 0)
			rc = tool_runtime_emit_stream(&next, "emit_sequences", "stdout", "乙");
		if (rc == 0)
			rc = tool_runtime_emit_stream(&next, "emit_sequences", "stderr", "错误");
		next.turn_id = "changed-turn";
		if (rc == 0)
			rc = tool_runtime_emit_stream(&next, "emit_sequences", "stderr", "新回合");
		std::string oversized(9000, 'x');
		if (rc == 0)
			rc = tool_runtime_emit_stream(&next, "emit_sequences", "stderr",
						      oversized.c_str());
		if (rc == 0)
			rc = tool_runtime_emit_stream(&next, "emit_sequences", "stderr", "最后");
		return rc != 0 ? rc : tool_result_success_text_copy(result, "Stream complete");
	};
	ASSERT_EQ(tool_register_borrow(&tools, &spec), 0);
	server.respond = [](const std::string &body) -> std::string {
		if (body.find("call_stream") == std::string::npos)
			return R"({"tool_calls":[{"index":0,"id":"call_stream","type":"function",)"
			       R"("function":{"name":"emit_sequences","arguments":"{}"}}]})";
		return R"({"content":"Verified stream"})";
	};
	spawn();
	agent_control_join(control);
	std::vector<std::string> chunks;
	std::vector<std::string> turns;
	for (const auto &json : events()) {
		cJSON *event MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(json.c_str());
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(event, "name"));
		if (name && !strcmp(name, "tool.stream.delta")) {
			const char *text = cJSON_GetStringValue(
				cJSON_GetObjectItem(cJSON_GetObjectItem(event, "data"), "text"));
			const char *turn =
				cJSON_GetStringValue(cJSON_GetObjectItem(event, "turn_id"));
			chunks.emplace_back(text ? text : "");
			turns.emplace_back(turn ? turn : "");
		}
	}
	ASSERT_EQ(chunks.size(), 5u);
	EXPECT_EQ(chunks[0], "甲乙");
	EXPECT_EQ(chunks[1], "错误");
	EXPECT_EQ(chunks[2], "新回合");
	EXPECT_EQ(chunks[3], std::string(9000, 'x'));
	EXPECT_EQ(chunks[4], "最后");
	EXPECT_NE(turns[1], turns[2]);
	EXPECT_EQ(turns[2], "changed-turn");
}
