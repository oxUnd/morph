#include <gtest/gtest.h>

extern "C" {
#include "runtime/output.h"
}

#include <filesystem>
#include <string>

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
		ASSERT_EQ(db_open(&db, database.c_str()), 0);
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
	config_set_defaults(&config);
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
	char *json = runtime_output_get_json_by_path(&db, "/tmp/generated.png");
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
	char *json = runtime_output_get_json_by_path(&db,
						      "/tmp/mixed-result.mp4");
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
	ASSERT_EQ(db_open(&db, database.c_str()), 0);
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
