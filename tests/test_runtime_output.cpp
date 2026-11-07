#include <gtest/gtest.h>

extern "C" {
#include "runtime/output.h"
#include "persistence/output_store.h"
#include "util/cleanup.h"
}

#include <cstring>
#include <filesystem>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

class RuntimeOutputTest : public ::testing::Test {
protected:
	std::string directory;
	std::string database;
	struct db db{};

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-runtime-output-XXXXXX";
		char *created = mkdtemp(pattern);
		ASSERT_NE(created, nullptr);
		directory = created;
		database = directory + "/data.db";
		ASSERT_EQ(db_open(NULL, &db, database.c_str()), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
	}

	void TearDown() override
	{
		db_close(&db);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}
};

static int count_created(const struct morph_event *event, void *user_data)
{
	if (event && event->name && std::string(event->name) == "output.created")
		(*static_cast<int *>(user_data))++;
	return 0;
}

TEST_F(RuntimeOutputTest, RecordsRecipeMetadataReferencesAndEmitsEvent)
{
	struct config config{};
	struct tool_artifact_list artifacts{};
	struct react_output_event event{};
	int events = 0;
	config_set_defaults(NULL, &config);
	std::strncpy(config.models.image.provider, "test-provider",
		      sizeof(config.models.image.provider) - 1);
	std::strncpy(config.models.image.model, "test-image",
		      sizeof(config.models.image.model) - 1);
	artifacts.count = 1;
	artifacts.items[0].kind = TOOL_ARTIFACT_IMAGE;
	std::strncpy(artifacts.items[0].path, "/tmp/generated.png",
		      sizeof(artifacts.items[0].path) - 1);
	std::strncpy(artifacts.items[0].mime, "image/png",
		      sizeof(artifacts.items[0].mime) - 1);
	artifacts.items[0].width = 2048;
	artifacts.items[0].height = 2048;
	event.type = REACT_STEP_OBSERVATION;
	event.status = REACT_OUTPUT_COMPLETED;
	event.tool_name = "img_gen";
	event.tool_args = "{\"prompt\":\"a lighthouse\",\"style\":\"film\","
		"\"reference_image\":\"/tmp/reference.png\"}";
	event.tool_call_id = "call-1";
	event.artifacts = &artifacts;
	struct runtime_output_context context{
		&db, &config, 0, "make this cinematic", "turn-1",
		count_created, &events
	};
	ASSERT_EQ(runtime_output_record_event(&context, &event), 0);
	EXPECT_EQ(events, 1);
	char *json = runtime_output_json_by_path_copy(&db, "/tmp/generated.png");
	ASSERT_NE(json, nullptr);
	cJSON *root = cJSON_Parse(json);
	free(json);
	ASSERT_NE(root, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(root, "prompt")->valuestring,
		     "a lighthouse");
	EXPECT_STREQ(cJSON_GetObjectItem(root, "request_prompt")->valuestring,
		     "make this cinematic");
	EXPECT_STREQ(cJSON_GetObjectItem(root, "provider")->valuestring,
		     "test-provider");
	EXPECT_EQ(cJSON_GetObjectItem(root, "width")->valueint, 2048);
	cJSON *recipe = cJSON_GetObjectItem(root, "recipe");
	ASSERT_TRUE(cJSON_IsObject(recipe));
	cJSON *references = cJSON_GetObjectItem(recipe, "references");
	ASSERT_TRUE(cJSON_IsArray(references));
	ASSERT_EQ(cJSON_GetArraySize(references), 1);
	EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(references, 0), "path")
			     ->valuestring, "/tmp/reference.png");
	cJSON_Delete(root);
}

TEST_F(RuntimeOutputTest, DuplicateArtifactPathIsIdempotent)
{
	struct tool_artifact_list artifacts{};
	struct react_output_event event{};
	artifacts.count = 1;
	artifacts.items[0].kind = TOOL_ARTIFACT_VIDEO;
	std::strncpy(artifacts.items[0].path, "/tmp/result.mp4",
		      sizeof(artifacts.items[0].path) - 1);
	event.type = REACT_STEP_OBSERVATION;
	event.status = REACT_OUTPUT_COMPLETED;
	event.tool_name = "vid_gen";
	event.tool_args = "{\"prompt\":\"waves\"}";
	event.artifacts = &artifacts;
	struct runtime_output_context context{&db, nullptr, 0, "request", "turn",
					      nullptr, nullptr};
	ASSERT_EQ(runtime_output_record_event(&context, &event), 0);
	ASSERT_EQ(runtime_output_record_event(&context, &event), 0);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
		"SELECT COUNT(*) FROM outputs WHERE path='/tmp/result.mp4'",
		-1, &stmt, nullptr), SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
	sqlite3_finalize(stmt);
}

TEST_F(RuntimeOutputTest, ConcurrentOutputRecordsKeepTheirOwnInsertedIdentity)
{
	std::atomic<int> ready{0};
	std::atomic<int> errors{0};
	std::vector<std::thread> workers;

	for (int worker = 0; worker < 4; worker++) {
		workers.emplace_back([&, worker] {
			ready.fetch_add(1);
			while (ready.load() != 4)
				std::this_thread::yield();
			for (int i = 0; i < 32; i++) {
				std::string path = directory + "/" + std::to_string(worker) + "-" +
						   std::to_string(i) + ".png";
				struct output_record_input record{};
				int64_t id = 0;
				int64_t found = 0;

				record.kind = "image";
				record.path = path.c_str();
				if (output_store_insert(&db, &record, &id) != 0 || id <= 0 ||
				    output_store_find_id(&db, path.c_str(), &found) != 0 ||
				    found != id)
					errors.fetch_add(1);
			}
		});
	}
	for (auto &worker : workers)
		worker.join();
	EXPECT_EQ(errors.load(), 0);
}

TEST_F(RuntimeOutputTest, RecordsVideoImageAndAudioReferences)
{
	struct tool_artifact_list artifacts{};
	struct react_output_event event{};
	artifacts.count = 1;
	artifacts.items[0].kind = TOOL_ARTIFACT_VIDEO;
	std::strncpy(artifacts.items[0].path, "/tmp/mixed-result.mp4",
		      sizeof(artifacts.items[0].path) - 1);
	event.type = REACT_STEP_OBSERVATION;
	event.status = REACT_OUTPUT_COMPLETED;
	event.tool_name = "vid_gen";
	event.tool_args = "{\"prompt\":\"waves\","
		"\"reference_images\":[\"/tmp/frame.png\"],"
		"\"reference_videos\":[\"https://example.com/motion.mp4\"],"
		"\"reference_audios\":[\"/tmp/music.mp3\"]}";
	event.artifacts = &artifacts;
	struct runtime_output_context context{&db, nullptr, 0, "request", "turn",
					      nullptr, nullptr};
	ASSERT_EQ(runtime_output_record_event(&context, &event), 0);
	char *json = runtime_output_json_by_path_copy(&db, "/tmp/mixed-result.mp4");
	ASSERT_NE(json, nullptr);
	cJSON *root = cJSON_Parse(json);
	free(json);
	ASSERT_NE(root, nullptr);
	cJSON *references = cJSON_GetObjectItem(
		cJSON_GetObjectItem(root, "recipe"), "references");
	ASSERT_TRUE(cJSON_IsArray(references));
	EXPECT_EQ(cJSON_GetArraySize(references), 3);
	EXPECT_STREQ(cJSON_GetObjectItem(cJSON_GetArrayItem(references, 2),
					 "kind")->valuestring, "audio");
	cJSON_Delete(root);
}

TEST(RuntimeOutputMigrationTest, AddsRecipeColumnsToExistingOutputsTable)
{
	char pattern[] = "/tmp/morph-runtime-output-migration-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string database = std::string(directory) + "/data.db";
	struct db db{};
	ASSERT_EQ(db_open(NULL, &db, database.c_str()), 0);
	ASSERT_EQ(db_exec(&db,
		"CREATE TABLE outputs(id INTEGER PRIMARY KEY,session_id INTEGER,"
		"kind TEXT NOT NULL,path TEXT NOT NULL,prompt TEXT,model TEXT,"
		"created_at INTEGER NOT NULL)"), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle, "PRAGMA table_info(outputs)",
		-1, &stmt, nullptr), SQLITE_OK);
	bool foundRecipe = false;
	bool foundProvider = false;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
		foundRecipe = foundRecipe || (name && std::string(name) == "recipe_json");
		foundProvider = foundProvider || (name && std::string(name) == "provider");
	}
	sqlite3_finalize(stmt);
	EXPECT_TRUE(foundRecipe);
	EXPECT_TRUE(foundProvider);
	db_close(&db);
	std::error_code ignored;
	std::filesystem::remove_all(directory, ignored);
}

TEST_F(RuntimeOutputTest, ReturnedOutputOwnsItsStringsAfterTheStoredRowIsDeleted)
{
	struct output_record_input input{};
	input.kind = "image";
	input.path = "/tmp/owned-output.png";
	input.prompt = "original prompt";
	input.provider = "provider";
	input.recipe_json = "{\"seed\":42}";
	int64_t id = 0;
	ASSERT_EQ(output_store_insert(&db, &input, &id), 0);
	ASSERT_GT(id, 0);
	struct output_record first MORPH_CLEANUP(output_record_cleanup) = {};
	struct output_record second MORPH_CLEANUP(output_record_cleanup) = {};
	ASSERT_EQ(output_store_copy(&db, input.path, &first), 0);
	ASSERT_EQ(output_store_copy(&db, input.path, &second), 0);
	EXPECT_NE(first.prompt, second.prompt);
	ASSERT_EQ(sqlite3_exec(db.handle, "DELETE FROM outputs", nullptr, nullptr, nullptr),
		  SQLITE_OK);
	output_record_cleanup(&first);
	EXPECT_STREQ(second.prompt, "original prompt");
	EXPECT_STREQ(second.recipe_json, "{\"seed\":42}");
	EXPECT_STREQ(input.prompt, "original prompt");
}

TEST_F(RuntimeOutputTest, MissingOutputReturnsAnEmptyAndSafelyCleanableOwner)
{
	struct output_record record MORPH_CLEANUP(output_record_cleanup) = {};
	EXPECT_EQ(output_store_copy(&db, "/tmp/missing-output.png", &record), -ENODATA);
	EXPECT_EQ(record.path, nullptr);
	EXPECT_EQ(record.prompt, nullptr);
	EXPECT_EQ(record.id, 0);
	output_record_cleanup(&record);
}

TEST_F(RuntimeOutputTest, OutputReadRejectsAnOccupiedOwnerWithoutChangingItsFields)
{
	struct output_record_input input{};
	input.kind = "image";
	input.path = "/tmp/occupied-output.png";
	input.prompt = "owned prompt";
	int64_t id = 0;
	ASSERT_EQ(output_store_insert(&db, &input, &id), 0);
	struct output_record record MORPH_CLEANUP(output_record_cleanup) = {};
	ASSERT_EQ(output_store_copy(&db, input.path, &record), 0);
	char *path = record.path;
	char *prompt = record.prompt;

	EXPECT_EQ(output_store_copy(&db, input.path, &record), -EEXIST);
	EXPECT_EQ(record.path, path);
	EXPECT_EQ(record.prompt, prompt);
	EXPECT_STREQ(record.prompt, "owned prompt");
	output_record_cleanup(&record);
	EXPECT_EQ(output_store_copy(&db, input.path, &record), 0);
}
