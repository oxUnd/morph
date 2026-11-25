#include <gtest/gtest.h>

extern "C" {
#include "agent/react.h"
#include "models/llm.h"
#include "persistence/orm/checkpoint_store.h"
#include "persistence/orm/credit_store.h"
#include "persistence/orm/database.h"
#include "persistence/orm/fcgi_events.h"
#include "persistence/orm/fcgi_sessions.h"
#include "persistence/orm/fcgi_users.h"
#include "persistence/orm/memory_repository.h"
#include "persistence/orm/memory_search.h"
#include "persistence/orm/memory_summary_store.h"
#include "persistence/orm/preferences.h"
#include "persistence/orm/runtime_store.h"
#include "persistence/orm/session.h"
#include "persistence/orm/session_history.h"
#include "persistence/orm/unit_of_work.h"
#include "runtime/runtime.h"
#include "sapi/fastcgi/session_store.h"
#include <nb/cleanup.h>
#include <nb/cancel.h>
#include <nb/file.h>
#include <nb/log.h>
#include <nb/time.h>
}

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static void postgresql_history_cleanup(struct model_history_item **owner)
{
	model_history_free_list(*owner);
	*owner = nullptr;
}

static void postgresql_runtime_cleanup(struct runtime **owner)
{
	runtime_close(*owner);
	*owner = nullptr;
}

static void postgresql_store_cleanup(struct session_store **owner)
{
	session_store_close(*owner);
	*owner = nullptr;
}

class PostgresqlTest : public ::testing::Test {
protected:
	struct db database{};
	struct session session{};
	struct nb_log *logger = nullptr;
	std::string log_path;
	std::string directory;
	std::string prefix;

	struct db_options Options(int timeout = 30000)
	{
		struct db_options options{};
		options.backend = DB_BACKEND_POSTGRESQL;
		options.url_env = "MORPH_TEST_POSTGRESQL_URL";
		options.tls_mode = DB_TLS_DISABLE;
		options.query_timeout_ms = timeout;
		return options;
	}

	void SetUp() override
	{
		ASSERT_NE(getenv("MORPH_TEST_POSTGRESQL_URL"), nullptr);
		char pattern[] = "/tmp/morph-postgresql-test-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		log_path = directory + "/database.log";
		logger = nb_log_new_file(NB_LOG_DEBUG, log_path.c_str(), 2U * 1024U * 1024U, 1);
		ASSERT_NE(logger, nullptr);
		prefix = std::filesystem::path(directory).filename().string();
		const struct db_options options = Options();
		ASSERT_EQ(db_open_options(logger, &database, &options), NB_OK);
		ASSERT_EQ(db_init_schema(&database), NB_OK);
		ASSERT_EQ(session_create(&database, prefix.c_str(), "postgresql-test", &session),
			  NB_OK);
	}

	void TearDown() override
	{
		if (session.id && db_is_open(&database))
			EXPECT_EQ(session_delete(&database, session.id), NB_OK);

		db_close(&database);
		if (HasFailure() && !log_path.empty()) {
			std::ifstream log(log_path);
			std::cerr << "PostgreSQL diagnostic log:\n" << log.rdbuf();
		}

		nb_log_free(logger);
		logger = nullptr;
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	int AddHistory(struct db *db, const char *key, int64_t *id = nullptr)
	{
		struct model_history_insert input{};
		input.session_id = session.id;
		input.turn_id = "postgresql-turn";
		input.kind = "user_message";
		input.role = "user";
		input.content = "postgresql history";
		input.idempotency_key = key;
		input.active = 1;
		input.token_count = 3;
		return model_history_add(db, &input, id);
	}

	void Write(const std::string &name, const std::string &text)
	{
		ASSERT_EQ(nb_file_write_all((directory + "/" + name).c_str(), text.data(),
					    text.size(), nullptr), NB_OK);
	}

	std::string Read(const std::string &name)
	{
		std::ifstream stream(directory + "/" + name);
		return std::string(std::istreambuf_iterator<char>(stream),
				   std::istreambuf_iterator<char>());
	}
};

TEST_F(PostgresqlTest, FullSchemaAndConnectionSnapshots)
{
	EXPECT_EQ(db_backend(&database), DB_BACKEND_POSTGRESQL);
	EXPECT_STREQ(database.path, "");
	EXPECT_FALSE(db_is_memory(&database));
	ASSERT_EQ(db_init_schema(&database), NB_OK);
	struct db_connection_config *connection NB_CLEANUP(db_connection_config_cleanup) = nullptr;
	struct db other NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_connection_config_copy(&database, &connection), NB_OK);
	ASSERT_EQ(db_open_connection(logger, &other, connection), NB_OK);
	struct session restored{};
	ASSERT_EQ(session_get_by_id(&other, session.id, &restored), NB_OK);
	EXPECT_STREQ(restored.name, session.name);
	EXPECT_TRUE(db_connection_config_matches(connection, &other));
}

TEST_F(PostgresqlTest, ConcurrentHistoryIsOrderedAndIdempotentAcrossOwners)
{
	struct db other NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_reopen(logger, &other, &database), NB_OK);
	std::atomic<int> errors{0};
	std::atomic<bool> start{false};
	auto append = [&](struct db *db, const char *owner) {
		while (!start.load())
			std::this_thread::yield();

		for (int i = 0; i < 20; i++) {
			std::string key = std::string(owner) + "-" + std::to_string(i);
			if (AddHistory(db, key.c_str()) != NB_OK)
				errors.fetch_add(1);
		}
	};
	std::thread first(append, &database, "first");
	std::thread second(append, &other, "second");
	start.store(true);
	first.join();
	second.join();
	ASSERT_EQ(errors.load(), 0);
	int count = 0;
	struct model_history_item *rows NB_CLEANUP(postgresql_history_cleanup) =
		model_history_list(&database, session.id, 1, &count);
	ASSERT_EQ(count, 40);
	int64_t expected = 1;
	for (auto *row = rows; row; row = row->next)
		EXPECT_EQ(row->sequence_no, expected++);

	int64_t first_id = 0;
	int64_t second_id = 0;
	std::thread first_retry([&] {
		if (AddHistory(&database, "same-logical-input", &first_id) != NB_OK)
			errors.fetch_add(1);
	});
	std::thread second_retry([&] {
		if (AddHistory(&other, "same-logical-input", &second_id) != NB_OK)
			errors.fetch_add(1);
	});
	first_retry.join();
	second_retry.join();
	EXPECT_EQ(errors.load(), 0);
	EXPECT_GT(first_id, 0);
	EXPECT_EQ(first_id, second_id);
	EXPECT_EQ(model_history_count(&database, session.id, 1), 41);
}

TEST_F(PostgresqlTest, ConcurrentLegacyMigrationAcrossOwnersIsIdempotent)
{
	ASSERT_EQ(message_add(&database, session.id, "user", "legacy objective", 2), NB_OK);
	ASSERT_EQ(message_add(&database, session.id, "assistant", "legacy answer", 2), NB_OK);
	struct db first NB_CLEANUP(db_close) = {};
	struct db second NB_CLEANUP(db_close) = {};
	struct db third NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_reopen(logger, &first, &database), NB_OK);
	ASSERT_EQ(db_reopen(logger, &second, &database), NB_OK);
	ASSERT_EQ(db_reopen(logger, &third, &database), NB_OK);
	std::vector<struct db *> owners{&database, &first, &second, &third};
	std::vector<std::thread> workers;
	std::atomic<bool> start{false};
	std::atomic<int> failures{0};
	for (auto *owner : owners) {
		workers.emplace_back([&, owner] {
			while (!start.load())
				std::this_thread::yield();

			if (model_history_migrate_messages(owner, session.id) != NB_OK)
				failures.fetch_add(1);
		});
	}

	start.store(true);
	for (auto &worker : workers)
		worker.join();

	ASSERT_EQ(failures.load(), 0);
	EXPECT_EQ(model_history_count(&database, session.id, 1), 2);
	ASSERT_EQ(model_history_migrate_messages(&database, session.id), NB_OK);
	int count = 0;
	struct model_history_item *items NB_CLEANUP(postgresql_history_cleanup) =
		model_history_list(&database, session.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_EQ(items->sequence_no, 1);
	EXPECT_STREQ(items->content, "legacy objective");
	ASSERT_NE(items->next, nullptr);
	EXPECT_EQ(items->next->sequence_no, 2);
	EXPECT_STREQ(items->next->content, "legacy answer");
}

TEST_F(PostgresqlTest, GuardDeadlineRollsBackAndLeavesConnectionReusable)
{
	struct db other NB_CLEANUP(db_close) = {};
	const struct db_options options = Options(150);
	ASSERT_EQ(db_open_options(logger, &other, &options), NB_OK);
	ASSERT_EQ(store_transaction_begin(&database), NB_OK);
	const int64_t started = nb_time_monotonic_ns();
	EXPECT_NE(store_transaction_begin(&other), NB_OK);
	EXPECT_LT(nb_time_monotonic_ns() - started, INT64_C(3000000000));
	ASSERT_EQ(store_transaction_rollback(&database), NB_OK);
	ASSERT_EQ(store_transaction_begin(&other), NB_OK);
	ASSERT_EQ(store_transaction_commit(&other), NB_OK);
	EXPECT_EQ(AddHistory(&other, "after-timeout"), NB_OK);
}

TEST_F(PostgresqlTest, CancellationInterruptsGuardWaitAndAllowsReuse)
{
	struct db other NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_reopen(logger, &other, &database), NB_OK);
	struct nb_cancel cancel;
	nb_cancel_init(&cancel);
	ASSERT_EQ(db_set_cancel(&other, &cancel), NB_OK);
	ASSERT_EQ(store_transaction_begin(&database), NB_OK);
	const int64_t started = nb_time_monotonic_ns();
	std::thread canceller([&] {
		nb_time_sleep_ms(100);
		nb_cancel_request(&cancel);
	});
	const int result = store_transaction_begin(&other);
	canceller.join();
	EXPECT_EQ(result, NB_ERR_CANCELLED);
	EXPECT_LT(nb_time_monotonic_ns() - started, INT64_C(3000000000));
	ASSERT_EQ(store_transaction_rollback(&database), NB_OK);
	ASSERT_EQ(db_set_cancel(&other, nullptr), NB_OK);
	ASSERT_EQ(store_transaction_begin(&other), NB_OK);
	ASSERT_EQ(store_transaction_commit(&other), NB_OK);
	EXPECT_EQ(AddHistory(&other, "after-cancellation"), NB_OK);
}

TEST_F(PostgresqlTest, CreditTodayAndSessionTotals)
{
	struct credit_store_event event{};
	event.user_id = prefix.c_str();
	event.session_id = session.display_id;
	event.kind = "text";
	event.provider = "test";
	event.model = "test";
	event.currency = "USD";
	event.input_tokens = 5;
	event.credits = 7;
	event.estimated_cost = 0.25;
	ASSERT_EQ(credit_store_record_event(&database, &event), NB_OK);
	event.credits = 3;
	ASSERT_EQ(credit_store_record_event(&database, &event), NB_OK);
	struct credit_store_summary today{};
	struct credit_store_summary total{};
	struct credit_store_summary by_session{};
	ASSERT_EQ(credit_store_summary_today(&database, prefix.c_str(), &today), NB_OK);
	ASSERT_EQ(credit_store_summary_total(&database, prefix.c_str(), &total), NB_OK);
	ASSERT_EQ(credit_store_summary_session(&database, session.display_id, &by_session), NB_OK);
	EXPECT_EQ(today.credits, 10);
	EXPECT_EQ(today.event_count, 2);
	EXPECT_DOUBLE_EQ(today.estimated_cost, 0.5);
	EXPECT_EQ(total.credits, today.credits);
	EXPECT_EQ(by_session.credits, today.credits);
}

TEST_F(PostgresqlTest, UnicodeMemoryScoresAndPreferenceRevisions)
{
	ASSERT_EQ(preference_bind(&database, session.id, prefix.c_str(), directory.c_str()), NB_OK);
	ASSERT_EQ(memory_store_prepare_session(&database, session.id), NB_OK);
	int64_t before = 0;
	ASSERT_EQ(memory_summary_revision(&database, session.id, &before), NB_OK);
	ASSERT_EQ(memory_note_save(&database, session.id, "école", "Straße", "evidence",
				   "first-note", "project"), NB_OK);
	ASSERT_EQ(memory_note_save(&database, session.id, "other", "Straße elsewhere", "evidence",
				   "second-note", "project"), NB_OK);
	int64_t after = 0;
	ASSERT_EQ(memory_summary_revision(&database, session.id, &after), NB_OK);
	EXPECT_GT(after, before);
	for (const char *query : {"ÉCOLE", "e\u0301cole"}) {
		struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {};
		struct memory_search_options options{};
		options.query = query;
		ASSERT_EQ(memory_search_records(&database, session.id, &options, &matches), NB_OK);
		ASSERT_EQ(matches.records.len, 1u);
		const auto *record = static_cast<const struct memory_record *>(
			nb_vec_at(&matches.records, 0));
		EXPECT_STREQ(record->value, "Straße");
	}

	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {};
	struct memory_search_options search{};
	search.query = "ÉCOLE STRASSE";
	search.limit = 1;
	ASSERT_EQ(memory_search_records(&database, session.id, &search, &matches), NB_OK);
	ASSERT_EQ(matches.records.len, 1u);
	const auto *top = static_cast<const struct memory_record *>(nb_vec_at(&matches.records, 0));
	EXPECT_STREQ(top->value, "Straße");
	EXPECT_EQ(top->score, 6);
	ASSERT_EQ(preference_set(&database, session.id, "project", "language", "French",
				"explicit", nullptr), NB_OK);
	ASSERT_EQ(preference_set(&database, session.id, "session", "language", "German",
				"explicit", nullptr), NB_OK);
	char *render NB_CLEANUP(nb_charp_cleanup) =
		preference_render_copy(&database, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_NE(strstr(render, "German"), nullptr);
	ASSERT_EQ(preference_clear_session(&database, session.id), NB_OK);
	free(render);
	render = preference_render_copy(&database, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_NE(strstr(render, "French"), nullptr);
	EXPECT_EQ(strstr(render, "German"), nullptr);
}

TEST_F(PostgresqlTest, LatestRuntimeTurnUsesInsertionOrder)
{
	ASSERT_EQ(runtime_store_init(&database), NB_OK);
	std::string older = prefix + "-z";
	std::string newer = prefix + "-a";
	struct runtime_store_turn_input input{};
	input.turn_id = older.c_str();
	input.session_id = session.id;
	input.owner_epoch = prefix.c_str();
	input.user_id = prefix.c_str();
	input.idempotency_key = "older";
	input.input = "first";
	input.stored_input = "first";
	input.status = "completed";
	input.final_text = "older answer";
	ASSERT_EQ(runtime_store_turn_put(&database, &input), NB_OK);
	input.turn_id = newer.c_str();
	input.idempotency_key = "newer";
	input.final_text = "newer answer";
	ASSERT_EQ(runtime_store_turn_put(&database, &input), NB_OK);
	input.turn_id = older.c_str();
	input.idempotency_key = "older";
	input.final_text = "updated old answer";
	ASSERT_EQ(runtime_store_turn_put(&database, &input), NB_OK);
	struct runtime_store_turn_record latest NB_CLEANUP(runtime_store_turn_record_cleanup) = {};
	ASSERT_EQ(runtime_store_turn_copy(&database, session.id, nullptr, &latest), NB_OK);
	EXPECT_STREQ(latest.turn_id, newer.c_str());
	EXPECT_STREQ(latest.final_text, "newer answer");
}

TEST_F(PostgresqlTest, CheckpointRestorePreservesOwnersAndIdentityHighWater)
{
	ASSERT_EQ(AddHistory(&database, "before-checkpoint"), NB_OK);
	Write("file.txt", "before");
	int64_t checkpoint = 0;
	ASSERT_EQ(checkpoint_store_create(&database, session.id, directory.c_str(), "pg-turn",
					  "edit", &checkpoint), NB_OK);
	ASSERT_EQ(checkpoint_store_capture(&database, checkpoint, "file.txt"), NB_OK);
	ASSERT_EQ(checkpoint_store_validate_write(&database, checkpoint, "file.txt", 5, nullptr),
		  NB_OK);
	Write("file.txt", "after");
	struct db other NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_reopen(logger, &other, &database), NB_OK);
	EXPECT_EQ(checkpoint_store_validate_write(&other, checkpoint, "file.txt", 5, nullptr),
		  NB_ERR_STALE);
	ASSERT_EQ(checkpoint_store_seal(&other, checkpoint), NB_OK);
	ASSERT_EQ(checkpoint_store_seal(&database, checkpoint), NB_OK);
	int64_t discarded = 0;
	ASSERT_EQ(AddHistory(&database, "discarded-by-restore", &discarded), NB_OK);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(checkpoint_store_restore(&database, session.id, checkpoint,
					   CHECKPOINT_RESTORE_BOTH, &report), NB_OK);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "code_restored")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(report, "conversation_restored")));
	EXPECT_EQ(Read("file.txt"), "before");
	EXPECT_EQ(model_history_count(&database, session.id, 1), 1);
	int64_t after_restore = 0;
	ASSERT_EQ(AddHistory(&database, "after-restore", &after_restore), NB_OK);
	EXPECT_GT(after_restore, discarded);
	EXPECT_EQ(model_history_count(&database, session.id, 1), 2);
}

TEST_F(PostgresqlTest, FastcgiSessionsEventsAndActions)
{
	const struct db_options options = Options();
	struct session_store *store NB_CLEANUP(postgresql_store_cleanup) =
		session_store_open_options(logger, &options);
	ASSERT_NE(store, nullptr);
	char user_id[64] = {};
	ASSERT_EQ(store_create_user(store, prefix.c_str(), "postgresql-test-password", "user",
				    user_id),
		  NB_OK);
	char session_id[64] = {};
	std::string name = prefix + "-fastcgi";
	ASSERT_EQ(store_create_session(store, user_id, name.c_str(), "test", session_id), NB_OK);
	EXPECT_EQ(store_session_owned_by(store, session_id, user_id), 1);
	EXPECT_EQ(store_session_owned_by(store, session_id, "another-user"), 0);
	ASSERT_EQ(events_publish(store, session_id, "delta", "{\"text\":\"hello\"}"), NB_OK);
	struct event_record event{};
	ASSERT_EQ(events_wait_after(store, session_id, 0, 0, &event), 1);
	EXPECT_STREQ(event.type, "delta");
	EXPECT_STREQ(event.payload_json, "{\"text\":\"hello\"}");
	free(event.payload_json);
	ASSERT_EQ(actions_enqueue(store, session_id, "continue", "{\"input\":\"next\"}"), NB_OK);
	struct action_record action{};
	ASSERT_EQ(actions_drain_one(store, session_id, &action), 1);
	EXPECT_STREQ(action.type, "continue");
	EXPECT_STREQ(action.payload_json, "{\"input\":\"next\"}");
	free(action.payload_json);
	action.payload_json = nullptr;
	EXPECT_EQ(actions_drain_one(store, session_id, &action), 0);
	EXPECT_EQ(store_delete_session(store, session_id, user_id), NB_OK);
}

struct PostgresqlModelState {
	struct model *model = nullptr;
	int calls = 0;
	std::vector<std::string> inputs;
	std::string tool_arguments;
	bool observed_tool = false;
	bool observed_tool_after_reopen = false;
};

static int postgresql_chat(struct model *self, const char *, struct chat_message *messages,
			    int count, struct tool_desc *, int, struct chat_response *response,
			    sse_callback, void *)
{
	auto *state = static_cast<PostgresqlModelState *>(self->handle);
	state->calls++;
	for (int i = 0; i < count; i++) {
		if (messages[i].content)
			state->inputs.emplace_back(messages[i].content);

		if (messages[i].role && strcmp(messages[i].role, "tool") == 0 &&
		    messages[i].content &&
		    strstr(messages[i].content, "POSTGRES_TOOL_OBSERVATION")) {
			state->observed_tool = true;
			if (state->calls == 3)
				state->observed_tool_after_reopen = true;
		}
	}

	if (state->calls == 1) {
		response->tool_calls = static_cast<struct tool_call *>(
			calloc(1, sizeof(*response->tool_calls)));
		if (!response->tool_calls)
			return NB_ERR_NOMEM;

		response->tool_call_count = 1;
		auto &call = response->tool_calls[0];
		call.id = strdup("postgresql-file-read");
		call.name = strdup("file_read");
		call.input_kind = TOOL_INPUT_JSON;
		call.arguments = strdup(state->tool_arguments.c_str());
		return call.id && call.name && call.arguments ? 200 : NB_ERR_NOMEM;
	}

	response->content = strdup("Thought: complete\nFinal: postgres scripted answer");
	return response->content ? 200 : NB_ERR_NOMEM;
}

static int postgresql_install_model(struct react_context *react, void *context)
{
	react->llm_model = static_cast<PostgresqlModelState *>(context)->model;
	return NB_OK;
}

static void postgresql_model_destroy(struct model *model)
{
	free(model);
}

TEST_F(PostgresqlTest, RuntimeTurnsPersistAndReloadThroughCopiedConnection)
{
	PostgresqlModelState scripted;
	Write("tool.txt", "POSTGRES_TOOL_OBSERVATION\n");
	cJSON *tool_args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	ASSERT_NE(tool_args, nullptr);
	cJSON_AddStringToObject(tool_args, "file_path", (directory + "/tool.txt").c_str());
	char *encoded_args NB_CLEANUP(nb_charp_cleanup) = cJSON_PrintUnformatted(tool_args);
	ASSERT_NE(encoded_args, nullptr);
	scripted.tool_arguments = encoded_args;
	auto model = std::unique_ptr<struct model, decltype(&model_destroy)>(
		static_cast<struct model *>(calloc(1, sizeof(struct model))), model_destroy);
	ASSERT_NE(model, nullptr);
	strcpy(model->provider, "runtime-test");
	strcpy(model->model_id, "scripted");
	strcpy(model->api_key, "test-key");
	model->context_limit = 128000;
	model->max_tokens = 1024;
	model->chat_with_tools = postgresql_chat;
	model->destroy = postgresql_model_destroy;
	model->handle = &scripted;
	scripted.model = model.get();
	std::string config_path = directory + "/config.toml";
	{
		std::ofstream config(config_path);
		config << "[memory]\nenabled=false\n[agents]\nenabled=false\n";
		ASSERT_TRUE(config.good());
	}

	struct db_connection_config *connection NB_CLEANUP(db_connection_config_cleanup) = nullptr;
	ASSERT_EQ(db_connection_config_copy(&database, &connection), NB_OK);
	struct runtime *runtime NB_CLEANUP(postgresql_runtime_cleanup) = nullptr;
	struct runtime_options options{};
	options.logger = logger;
	options.database = connection;
	options.config_path = config_path.c_str();
	options.workdir = directory.c_str();
	options.default_session = session.name;
	options.front_name = "postgresql-test";
	options.after_models_cb = postgresql_install_model;
	options.after_models_user_data = &scripted;
	ASSERT_EQ(runtime_open(&options, &runtime), NB_OK);
	struct runtime_request request{};
	request.model_input = "hello postgres runtime";
	request.stored_user_input = request.model_input;
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	struct runtime_result result{};
	ASSERT_EQ(runtime_execute_turn(runtime, &request, &result), NB_OK);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_EQ(result.persistence_rc, NB_OK);
	ASSERT_NE(result.final_text, nullptr);
	EXPECT_STREQ(result.final_text, "postgres scripted answer");
	postgresql_runtime_cleanup(&runtime);
	ASSERT_EQ(runtime_open(&options, &runtime), NB_OK);
	char *answer NB_CLEANUP(nb_charp_cleanup) = nullptr;
	ASSERT_EQ(runtime_session_last_answer_copy(runtime, &answer), NB_OK);
	EXPECT_STREQ(answer, "postgres scripted answer");
	request.model_input = "continue postgres runtime";
	request.stored_user_input = request.model_input;
	ASSERT_EQ(runtime_execute_turn(runtime, &request, &result), NB_OK);
	EXPECT_EQ(result.persistence_rc, NB_OK);
	EXPECT_EQ(scripted.calls, 3);
	EXPECT_TRUE(scripted.observed_tool);
	EXPECT_TRUE(scripted.observed_tool_after_reopen);
	bool saw_first_input = false;
	for (const auto &input : scripted.inputs)
		saw_first_input |= input.find("hello postgres runtime") != std::string::npos;

	EXPECT_TRUE(saw_first_input);
	EXPECT_GE(model_history_count(&database, session.id, 1), 4);
}
