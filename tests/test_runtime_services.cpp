#include "runtime_test_support.hpp"

extern "C" {
#include "agent/tool.h"
#include "db/scheduled_task.h"
#include "runtime/runtime_internal.h"
#include "runtime/sync.h"
#include "cJSON.h"
}

#include <sqlite3.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

static void create_session_import_fixture(const std::string &path, const char *name)
{
	struct db source{};
	struct session session{};
	ASSERT_EQ(db_open(&source, path.c_str()), 0);
	ASSERT_EQ(db_init_schema(&source), 0);
	ASSERT_EQ(session_create(&source, name, "import-model", &session), 0);
	ASSERT_EQ(session.id, 1);
	ASSERT_EQ(message_add(&source, session.id, "user", "imported question", 3), 0);
	ASSERT_EQ(message_add(&source, session.id, "assistant", "imported answer", 3), 0);
	ASSERT_EQ(db_exec(&source,
		"INSERT INTO model_history_items(session_id,sequence_no,kind,role,content,created_at) "
		"VALUES(1,1,'user_message','user','imported question',1),"
		"(1,2,'assistant_message','assistant','imported answer',2);"
		"CREATE TABLE sync_ui_messages(session_id,turn_id,core_message_id,seq,type,content,"
		"attachments_json,structured_data,agent_ui_ir,hitl_verdict,created_at,updated_at);"
		"INSERT INTO sync_ui_messages VALUES(1,'turn',1,0,'user','imported question',"
		"NULL,NULL,NULL,NULL,1000,1000),"
		"(1,'turn',NULL,1,'thought','working',NULL,NULL,NULL,NULL,1000,1000),"
		"(1,'turn',2,2,'final','imported answer',NULL,NULL,NULL,NULL,2000,2000);"), 0);
	db_close(&source);
}

TEST_F(RuntimeFacadeTest, SessionImportPreservesLocalHistoryAndRemapsDisplayAndModelHistory)
{
	Open();
	struct session current{};
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	ASSERT_EQ(message_add(&instance->context.database, current.id, "user", "local question", 2), 0);
	std::string source = directory + "/incoming.db";
	create_session_import_fixture(source, current.name);
	struct runtime_session_import_result result{};
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 0);
	struct session *sessions = nullptr;
	int count = 0;
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	ASSERT_EQ(count, 2);
	int64_t imported = sessions[0].id == current.id ? sessions[1].id : sessions[0].id;
	runtime_session_list_free(sessions);
	EXPECT_EQ(message_count(&instance->context.database, current.id), 1);
	EXPECT_EQ(message_count(&instance->context.database, imported), 2);
	char *json = nullptr;
	ASSERT_EQ(runtime_session_transcript_json(instance, imported, &json), 0);
	cJSON *root = cJSON_Parse(json);
	free(json);
	ASSERT_NE(root, nullptr);
	cJSON *messages = cJSON_GetObjectItem(root, "messages");
	cJSON *display = cJSON_GetObjectItem(root, "display");
	EXPECT_EQ(cJSON_GetArraySize(display), 3);
	EXPECT_EQ(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "id")->valueint,
		cJSON_GetObjectItem(cJSON_GetArrayItem(display, 0), "core_message_id")->valueint);
	cJSON_Delete(root);
	ASSERT_EQ(runtime_session_select_existing(instance, imported, nullptr), 0);
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	ASSERT_NE(instance->context.react->history_items, nullptr);
	EXPECT_STREQ(instance->context.react->history_items->content, "imported question");
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 1);
}

TEST_F(RuntimeFacadeTest, SessionImportBranchesChangedHistoryAndRollsBackOnMalformedData)
{
	Open();
	std::string source = directory + "/incoming.db";
	create_session_import_fixture(source, "remote");
	struct runtime_session_import_result result{};
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	struct db changed{};
	ASSERT_EQ(db_open(&changed, source.c_str()), 0);
	ASSERT_EQ(message_add(&changed, 1, "user", "second question", 2), 0);
	db_close(&changed);
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 1);
	struct session *sessions = nullptr;
	int before = 0;
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &before, 0), 0);
	EXPECT_EQ(before, 3);
	runtime_session_list_free(sessions);
	ASSERT_EQ(db_open(&changed, source.c_str()), 0);
	ASSERT_EQ(db_exec(&changed, "DROP TABLE messages; CREATE TABLE messages(session_id INTEGER, content TEXT); INSERT INTO messages VALUES(1,'broken');"), 0);
	db_close(&changed);
	EXPECT_NE(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 0);
	int after = 0;
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &after, 0), 0);
	EXPECT_EQ(after, before);
	runtime_session_list_free(sessions);
}

TEST_F(RuntimeFacadeTest, SessionImportWriteFailureRollsBackReceiptAndSession)
{
	Open();
	std::string source = directory + "/incoming.db";
	create_session_import_fixture(source, "remote");
	ASSERT_EQ(db_exec(&instance->context.database,
		"CREATE TRIGGER reject_import BEFORE INSERT ON messages "
		"BEGIN SELECT RAISE(ABORT,'injected write failure'); END;"), 0);
	struct runtime_session_import_result result{};
	EXPECT_NE(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 0);
	struct session *sessions = nullptr;
	int count = 0;
	ASSERT_EQ(runtime_session_list_all(instance, &sessions, &count, 0), 0);
	EXPECT_EQ(count, 1);
	runtime_session_list_free(sessions);
	ASSERT_EQ(db_exec(&instance->context.database, "DROP TRIGGER reject_import"), 0);
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 1);
	EXPECT_EQ(result.unchanged, 0);
}

TEST_F(RuntimeFacadeTest, SessionImportFromPublishedBackupIsAtomicAndRepeatable)
{
	Open();
	const auto source_dir = directory + "/source";
	const auto remote = directory + "/remote";
	std::filesystem::create_directories(source_dir);
	std::filesystem::create_directories(remote);
	create_session_import_fixture(source_dir + "/data.db", "remote snapshot");
	struct morph_sync_config cfg{};
	cfg.enabled = 1;
	cfg.retention_days = 30;
	snprintf(cfg.source_dir, sizeof(cfg.source_dir), "%s", source_dir.c_str());
	snprintf(cfg.sync_dir, sizeof(cfg.sync_dir), "%s", remote.c_str());
	snprintf(cfg.include[0], sizeof(cfg.include[0]), "%s", "data.db");
	cfg.include_count = 1;
	struct morph_sync_status status{};
	ASSERT_EQ(morph_sync_once(&cfg, &status), 0) << status.last_error;
	struct morph_sync_backup *backups = nullptr;
	int count = 0;
	ASSERT_EQ(morph_sync_backups(&cfg, "data.db", &backups, &count), 0);
	ASSERT_EQ(count, 1);
	std::string snapshot = backups[0].snapshot_id;
	morph_sync_backups_free(backups);
	struct runtime_session_import_result result{};
	ASSERT_EQ(runtime_sync_import_backup(instance, &cfg, snapshot.c_str(), &result), 0);
	EXPECT_EQ(result.imported, 1);
	ASSERT_EQ(runtime_sync_import_backup(instance, &cfg, snapshot.c_str(), &result), 0);
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 1);
	EXPECT_NE(runtime_sync_import_backup(instance, &cfg, "missing", &result), 0);
	EXPECT_EQ(result.imported, 0);
}

TEST_F(RuntimeFacadeTest, SessionImportPreservesSubagentRelationsAndRejectsOrphans)
{
	Open();
	std::string source = directory + "/incoming.db";
	create_session_import_fixture(source, "parent");
	struct db changed{};
	struct session child{};
	ASSERT_EQ(db_open(&changed, source.c_str()), 0);
	ASSERT_EQ(session_create(&changed, "child", "model", &child), 0);
	ASSERT_EQ(db_exec(&changed,
		"INSERT INTO sub_agent_tasks(task_id,parent_session_id,child_session_id,"
		"agent_name,description,mode,status,started_at) "
		"VALUES('task-a',1,2,'researcher','find evidence','delegate',2,1);"
		"INSERT INTO sub_agent_events(task_id,event_json,created_at) "
		"VALUES('task-a','{}',1);"), 0);
	db_close(&changed);
	struct runtime_session_import_result result{};
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 2);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(instance->context.database.handle,
		"SELECT p.name,c.name FROM sub_agent_tasks t JOIN sessions p ON p.id=t.parent_session_id "
		"JOIN sessions c ON c.id=t.child_session_id WHERE t.task_id='task-a'",
		-1, &stmt, nullptr), SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_STREQ((const char *)sqlite3_column_text(stmt, 0), "parent");
	EXPECT_STREQ((const char *)sqlite3_column_text(stmt, 1), "child");
	sqlite3_finalize(stmt);
	ASSERT_EQ(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 0);
	EXPECT_EQ(result.unchanged, 2);
	ASSERT_EQ(db_open(&changed, source.c_str()), 0);
	ASSERT_EQ(db_exec(&changed, "PRAGMA foreign_keys=OFF; UPDATE messages SET session_id=999;"), 0);
	db_close(&changed);
	EXPECT_NE(runtime_session_import_file(instance, source.c_str(), "device-a", &result), 0);
	EXPECT_EQ(result.imported, 0);
}

static int facade_test_tool(const char *, struct tool_result *result, void *)
{
	return tool_result_success_text(result, "ok");
}

TEST_F(RuntimeFacadeTest, TranscriptReadsDetachedSessionWithoutSelectingIt)
{
	Open();
	struct session current{};
	struct session imported{};
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	ASSERT_EQ(runtime_session_create_detached(instance, "imported", &imported), 0);
	ASSERT_EQ(message_add(&instance->context.database, imported.id,
		"user", "Imported question", 3), 0);
	ASSERT_EQ(message_add(&instance->context.database, imported.id,
		"assistant", "Imported answer", 3), 0);
	char *json = nullptr;
	ASSERT_EQ(runtime_session_transcript_json(instance, imported.id, &json), 0);
	cJSON *root = cJSON_Parse(json);
	free(json);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(root, "identity")->valuestring,
		imported.display_id);
	cJSON *items = cJSON_GetObjectItem(root, "messages");
	ASSERT_EQ(cJSON_GetArraySize(items), 2);
	EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(items, 0),
		"content")->valuestring, "Imported question");
	cJSON_Delete(root);
	int64_t selected = 0;
	ASSERT_EQ(runtime_session_current_id(instance, &selected), 0);
	EXPECT_EQ(selected, current.id);
	json = nullptr;
	EXPECT_NE(runtime_session_transcript_json(instance, imported.id + 100,
		&json), 0);
	EXPECT_EQ(json, nullptr);
	ASSERT_EQ(runtime_session_transcript_json(instance, current.id, &json), 0);
	root = cJSON_Parse(json);
	free(json);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")), 0);
	cJSON_Delete(root);
}

static int facade_task_runner(const struct scheduled_task *,
			      struct scheduled_task_action_result *result, void *)
{
	result->completed = 1;
	result->body = strdup("task completed");
	return result->body ? 0 : -ENOMEM;
}

static void facade_notification_counter(const struct notification *, void *user)
{
	(*static_cast<int *>(user))++;
}

TEST_F(RuntimeFacadeTest, SessionCrudAndLookupAreConsistent)
{
	struct session first{};
	struct session second{};
	struct session found{};
	struct session selected{};
	int created = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &first), 0);
	ASSERT_GT(first.id, 0);
	ASSERT_EQ(runtime_session_select(instance, "alpha", &second, &created), 0);
	EXPECT_EQ(created, 1);
	EXPECT_STREQ(runtime_session_current_name(instance), "alpha");
	EXPECT_EQ(runtime_session_find_ref(instance, second.display_id, &found), 0);
	EXPECT_EQ(found.id, second.id);
	EXPECT_EQ(runtime_session_find_ref(instance,
		std::to_string(second.id).c_str(), &found), 0);
	EXPECT_EQ(found.id, second.id);
	ASSERT_EQ(runtime_session_rename_and_update(instance, second.id, "renamed"), 0);
	EXPECT_STREQ(runtime_session_current_name(instance), "renamed");
	ASSERT_EQ(runtime_session_select_existing(instance, first.id, &selected), 0);
	EXPECT_EQ(selected.id, first.id);
	EXPECT_EQ(runtime_session_delete_and_update(instance, second.id), 0);
	EXPECT_EQ(runtime_session_find_ref(instance, "renamed", &found), -ENOENT);
}

TEST_F(RuntimeFacadeTest, DetachedSessionDoesNotChangeCurrentSession)
{
	struct session current{};
	struct session detached{};
	int64_t current_id = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	ASSERT_EQ(runtime_session_create_detached(instance, "background", &detached), 0);
	ASSERT_NE(detached.id, current.id);
	ASSERT_EQ(runtime_session_current_id(instance, &current_id), 0);
	EXPECT_EQ(current_id, current.id);
}

TEST_F(RuntimeFacadeTest, SessionSelectionDefersModelHistoryLoadingUntilTurn)
{
	struct session first{};
	struct session second{};
	sqlite3 *db = nullptr;
	int created = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &first), 0);
	ASSERT_EQ(runtime_session_select(instance, "second", &second, &created), 0);
	ASSERT_EQ(sqlite3_open(database.c_str(), &db), SQLITE_OK);
	const std::string sql =
		"INSERT INTO messages(session_id,role,content,token_count,compressed,created_at) "
		"VALUES(" + std::to_string(first.id) + ",'user','saved history',2,0,1)";
	ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
	sqlite3_close(db);
	db = nullptr;

	ASSERT_EQ(runtime_session_select(instance, first.name, &first, &created), 0);
	EXPECT_EQ(instance->context.react->messages, nullptr);
	EXPECT_EQ(instance->context.react->history_items, nullptr);
	ASSERT_EQ(runtime_session_reload_current(instance), 0);
	ASSERT_NE(instance->context.react->messages, nullptr);
	EXPECT_STREQ(instance->context.react->messages->content, "saved history");
}

TEST_F(RuntimeFacadeTest, SessionListFilterModelAndStatsUseRuntimeDatabase)
{
	struct session created{};
	struct session *sessions = nullptr;
	struct session current{};
	int count = 0;
	int messages = -1;
	int tokens = -1;
	int limit = -1;

	Open();
	ASSERT_EQ(runtime_session_create_and_select(instance, "needle-session", &created), 0);
	ASSERT_EQ(runtime_session_list_query(instance, &sessions, &count, 20,
		"needle"), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(sessions[0].id, created.id);
	runtime_session_list_free(sessions);
	EXPECT_GE(runtime_session_count_all(instance), 2);
	ASSERT_EQ(runtime_session_set_model(instance, "test-model"), 0);
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	EXPECT_STREQ(current.model, "test-model");
	EXPECT_EQ(runtime_session_context_stats(instance, &messages, &tokens, &limit), 0);
	EXPECT_EQ(messages, 0);
	EXPECT_EQ(tokens, 0);
	EXPECT_GT(limit, 0);
}

TEST_F(RuntimeFacadeTest, ToolRegistrationIsVisibleThroughFacade)
{
	struct tool_spec spec{};
	struct tool_desc desc{};
	enum tool_origin origin;
	int enabled;
	int before;

	Open();
	before = runtime_tool_count(instance);
	spec.origin = TOOL_ORIGIN_EXT;
	spec.name = "facade_test";
	spec.description = "Runtime facade test tool";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = facade_test_tool;
	ASSERT_EQ(runtime_register_tool(instance, &spec), 0);
	EXPECT_EQ(runtime_tool_count(instance), before + 1);
	ASSERT_EQ(runtime_tool_find(instance, "facade_test", &desc), 0);
	EXPECT_STREQ(desc.name, "facade_test");
	ASSERT_EQ(runtime_tool_enabled(instance, before, &enabled), 0);
	EXPECT_EQ(enabled, 1);
	ASSERT_EQ(runtime_tool_origin(instance, before, &origin), 0);
	EXPECT_EQ(origin, TOOL_ORIGIN_EXT);
	EXPECT_NE(runtime_register_tool(instance, &spec), 0);
	EXPECT_EQ(runtime_tool_find(instance, "missing", &desc), -ENOENT);
}

TEST_F(RuntimeFacadeTest, McpRegistryReturnsCopiedStatusWithoutConnecting)
{
	struct mcp_server_config server{};
	struct runtime_mcp_status status{};

	Open();
	std::strncpy(server.name, "offline", sizeof(server.name) - 1);
	server.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
	std::strncpy(server.http_url, "http://127.0.0.1:1/mcp",
		sizeof(server.http_url) - 1);
	ASSERT_EQ(runtime_add_mcp_server(instance, &server), 0);
	EXPECT_EQ(runtime_mcp_count(instance), 1);
	ASSERT_EQ(runtime_mcp_find(instance, "offline", &status), 0);
	EXPECT_STREQ(status.config.name, "offline");
	EXPECT_EQ(status.connected, 0);
	EXPECT_EQ(runtime_mcp_info(instance, 1, &status), -EINVAL);
	EXPECT_EQ(runtime_mcp_find(instance, "missing", &status), -ENOENT);
}

TEST_F(RuntimeFacadeTest, MediaCreditsAreAttributedToCurrentSession)
{
	struct credit_summary before{};
	struct credit_summary after{};

	Open();
	ASSERT_EQ(runtime_credit_summary_current_get(instance, &before), 0);
	ASSERT_EQ(runtime_credit_record_media(instance, "image_output", 2, 0,
		"test", "image-model", "{\"test\":true}"), 0);
	ASSERT_EQ(runtime_credit_summary_current_get(instance, &after), 0);
	EXPECT_EQ(after.event_count, before.event_count + 1);
	EXPECT_GE(after.credits, before.credits);
}

TEST_F(RuntimeFacadeTest, TaskCrudRunsEntirelyThroughFacade)
{
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task created{};
	struct scheduled_task loaded{};
	struct scheduled_task updated{};
	struct scheduled_task *items = nullptr;
	int count = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	input.source_session_id = current.id;
	input.title = "runtime task";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) + 3600;
	input.max_attempts = 1;
	input.action_type = "notify";
	input.payload_json = "{}";
	input.policy_json = "{}";
	input.notify_json = "{}";
	ASSERT_EQ(runtime_task_create(instance, &input, &created), 0);
	ASSERT_GT(created.id, 0);
	ASSERT_EQ(runtime_task_get(instance, created.id, &loaded), 0);
	EXPECT_STREQ(loaded.title, "runtime task");
	input.title = "updated task";
	ASSERT_EQ(runtime_task_update(instance, created.id, &input, &updated), 0);
	EXPECT_STREQ(updated.title, "updated task");
	ASSERT_EQ(runtime_task_list(instance, nullptr, 20, &items, &count), 0);
	EXPECT_EQ(count, 1);
	scheduled_task_free_list(items, count);
	ASSERT_EQ(runtime_task_cancel(instance, created.id), 0);
	scheduled_task_cleanup(&loaded);
	ASSERT_EQ(runtime_task_get(instance, created.id, &loaded), 0);
	EXPECT_STREQ(loaded.status, "cancelled");
	scheduled_task_cleanup(&created);
	scheduled_task_cleanup(&loaded);
	scheduled_task_cleanup(&updated);
}

TEST_F(RuntimeFacadeTest, DueTaskCreatesAndAcknowledgesNotification)
{
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task created{};
	struct notification *notifications = nullptr;
	int count = 0;
	int delivered = 0;

	Open();
	ASSERT_EQ(runtime_session_current(instance, &current), 0);
	input.source_session_id = current.id;
	input.title = "due reminder";
	input.kind = "reminder";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) - 1;
	input.max_attempts = 1;
	input.action_type = "notify";
	input.payload_json = "{\"prompt\":\"remember\"}";
	input.policy_json = "{}";
	input.notify_json = "{}";
	ASSERT_EQ(runtime_task_create(instance, &input, &created), 0);
	ASSERT_GE(runtime_tasks_run_due_for_runtime(instance, 10,
		facade_task_runner, nullptr, facade_notification_counter,
		&delivered), 0);
	ASSERT_EQ(runtime_notification_list(instance, 10, &notifications, &count), 0);
	ASSERT_EQ(count, 1);
	EXPECT_EQ(delivered, 1);
	EXPECT_STREQ(notifications[0].title, "due reminder");
	ASSERT_EQ(runtime_notification_mark_read(instance, notifications[0].id), 0);
	notification_free_list(notifications, count);
	notifications = nullptr;
	ASSERT_EQ(runtime_notification_list(instance, 10, &notifications, &count), 0);
	EXPECT_EQ(count, 0);
	notification_free_list(notifications, count);
	scheduled_task_cleanup(&created);
}

TEST_F(RuntimeFacadeTest, EmptyMemoryCanBeRenderedAndCleared)
{
	Open();
	char *rendered = runtime_memory_render_current(instance, 10);
	ASSERT_NE(rendered, nullptr);
	std::free(rendered);
	EXPECT_EQ(runtime_memory_clear_current(instance, MEMORY_CLEAR_ALL), 0);
}

TEST_F(RuntimeFacadeTest, PersistentPermissionsCanBeListedAndCleared)
{
	struct runtime_permission_grant *grants = nullptr;
	int count = -1;
	int deleted = -1;

	Open();
	ASSERT_EQ(runtime_permission_list(instance, &grants, &count), 0);
	EXPECT_EQ(count, 0);
	runtime_permission_list_free(grants);
	EXPECT_EQ(runtime_permission_revoke_id(instance, 123, &deleted), 0);
	EXPECT_EQ(deleted, 0);
	EXPECT_EQ(runtime_permission_clear(instance, 0, &deleted), 0);
	EXPECT_EQ(deleted, 0);
}

TEST(RuntimeFacadeValidationTest, RejectsInvalidFacadeArguments)
{
	struct session session{};
	struct tool_desc tool{};
	enum tool_origin origin;
	int enabled;
	struct credit_summary credits{};
	struct runtime_turn_status status{};

	EXPECT_EQ(runtime_session_current(nullptr, &session), -EINVAL);
	EXPECT_EQ(runtime_session_current_id(nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_session_find_ref(nullptr, "x", &session), -EINVAL);
	EXPECT_EQ(runtime_tool_info(nullptr, 0, &tool), -EINVAL);
	EXPECT_EQ(runtime_tool_enabled(nullptr, 0, &enabled), -EINVAL);
	EXPECT_EQ(runtime_tool_origin(nullptr, 0, &origin), -EINVAL);
	EXPECT_EQ(runtime_turn_status_get(nullptr, &status), -EINVAL);
	EXPECT_EQ(runtime_credit_summary_today_get(nullptr, &credits), -EINVAL);
	EXPECT_EQ(runtime_task_cancel(nullptr, 1), -EINVAL);
	EXPECT_EQ(runtime_sync_status_instance(nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_permission_list(nullptr, nullptr, nullptr), -EINVAL);
	EXPECT_EQ(runtime_permission_clear(nullptr, 0, nullptr), -EINVAL);
	runtime_turn_status_cleanup(nullptr);
	runtime_session_list_free(nullptr);
	runtime_mcp_list_free(nullptr);
}
