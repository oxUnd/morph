#include <gtest/gtest.h>
#include "nb/err.h"
extern "C" {
#include "agent/tools/dynamic_tools.h"
#include "agent/tools/dynamic_tool_store.h"
#include "agent/tool_context.h"
#include "config/config.h"
#include "cJSON.h"
#include "nb/file.h"
#include "nb/cleanup.h"
#include "cJSON.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static std::string json_escape(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		switch (ch) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		default:
			out += ch;
			break;
		}
	}
	return out;
}

static int static_tool_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	(void)args_json;
	(void)user_data;
	return tool_result_success_json_text_copy(result, "{\"static\":true}");
}

static std::string json_string_field(const char *json, const char *field)
{
	cJSON *root = cJSON_Parse(json);
	if (!root)
		return "";
	cJSON *item = cJSON_GetObjectItem(root, field);
	std::string out;
	if (cJSON_IsString(item) && item->valuestring)
		out = item->valuestring;
	cJSON_Delete(root);
	return out;
}

struct DynamicToolsTest : public ::testing::Test {
	std::string root;
	struct config cfg;
	struct tool_registry reg;
	struct tool_context *tctx;

	void SetUp() override
	{
		char tmpl[] = "/tmp/morph_dynamic_tools_XXXXXX";
		char *dir = mkdtemp(tmpl);
		ASSERT_NE(dir, nullptr);
		root = dir;
		config_set_defaults(NULL, &cfg);
		snprintf(cfg.dynamic_tools.session_dir,
			 sizeof(cfg.dynamic_tools.session_dir),
			 "%s/session", root.c_str());
		snprintf(cfg.dynamic_tools.persistent_dir,
			 sizeof(cfg.dynamic_tools.persistent_dir),
			 "%s/persistent", root.c_str());
		cfg.dynamic_tools.default_timeout_seconds = 5;
		cfg.dynamic_tools.promote_requires_approval = 0;
		tool_registry_init(NULL, &reg);
		tctx = tool_context_create(NULL, root.c_str(), root.c_str());
		ASSERT_NE(tctx, nullptr);
	}

	void TearDown() override
	{
		tool_context_destroy(tctx);
		tool_registry_cleanup(&reg);
		std::string cmd = "rm -rf " + root;
		(void)system(cmd.c_str());
	}

	std::string create_args(const std::string &name,
				const std::string &source)
	{
		std::string args =
			"{\"name\":\"" + name + "\","
			"\"description\":\"test dynamic tool\","
			"\"input_schema\":{\"type\":\"object\",\"properties\":{},"
			"\"additionalProperties\":false},"
			"\"output_schema\":{\"type\":\"object\",\"properties\":{}},"
			"\"source_js\":\"" + json_escape(source) + "\"";
		args += "}";
		return args;
	}
};

TEST_F(DynamicToolsTest, ManagementContextSurvivesCreateToolRemoval)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	ASSERT_EQ(tool_unregister(&reg, "tool_create"), 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(tool_exec(&reg, "tool_history", "{\"name\":\"not_created\"}", &result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "name")), "not_created");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(result.data, "checkpoints")), 0);
}

TEST_F(DynamicToolsTest, RetainedManagementHandleOwnsContextAfterAllToolsAreRemoved)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_handle retained NB_CLEANUP(tool_handle_release) = {};
	ASSERT_EQ(tool_registry_acquire(&reg, "tool_history", &retained), 0);
	const char *names[] = {"tool_create",  "tool_promote", "tool_delete",
			       "tool_history", "tool_diff",    "tool_rollback"};
	for (const char *name : names)
		ASSERT_EQ(tool_unregister(&reg, name), 0);
	EXPECT_EQ(reg.count, 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(retained.exec("{\"name\":\"not_created\"}", &result, retained.user_data), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "name")), "not_created");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(result.data, "checkpoints")), 0);
}

TEST_F(DynamicToolsTest, CreateAndCallSessionTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) {"
		"  return { text: String(args.text).toUpperCase() };"
		"}";
	std::string args = create_args("upper_js", source);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create", args.c_str(), &result), 0);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "upper_js", "{\"text\":\"hello\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	cJSON *root_json = cJSON_Parse(result.text.ptr);
	ASSERT_NE(root_json, nullptr);
	cJSON *text = cJSON_GetObjectItem(root_json, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_STREQ(text->valuestring, "HELLO");
	cJSON_Delete(root_json);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolCreateUsesUpdatedSessionDirectory)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools,
				     "default"), 0);
	ASSERT_EQ(dynamic_tools_set_session_id(&reg, "sess_1234abcd"), 0);
	std::string source =
		"function run(args) {"
		"  return { ok: true };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("session_scoped", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string expected = root +
		"/session/sess_1234abcd/session_scoped/tool.js";
	std::string old = root + "/session/default/session_scoped/tool.js";
	EXPECT_TRUE(nb_file_exists(expected.c_str()));
	EXPECT_FALSE(nb_file_exists(old.c_str()));
}

TEST_F(DynamicToolsTest, LargeSchemasAndDescriptionsSurviveCreationAndReload)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	const std::string description = std::string(24000, 'd') + "description_tail";
	const std::string schema_text = std::string(100000, 's') + "schema_tail";
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	ASSERT_NE(args, nullptr);
	ASSERT_NE(cJSON_AddStringToObject(args, "name", "large_contract"), nullptr);
	ASSERT_NE(cJSON_AddStringToObject(args, "description", description.c_str()), nullptr);
	ASSERT_NE(cJSON_AddStringToObject(args, "source_js",
		"function run(args) { return { value: 1 }; }"), nullptr);
	for (const char *key : {"input_schema", "output_schema"}) {
		auto *schema = cJSON_AddObjectToObject(args, key);
		ASSERT_NE(schema, nullptr);
		ASSERT_NE(cJSON_AddStringToObject(schema, "type", "object"), nullptr);
		ASSERT_NE(cJSON_AddObjectToObject(schema, "properties"), nullptr);
		ASSERT_NE(cJSON_AddStringToObject(schema, "description", schema_text.c_str()),
		nullptr);
	}
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(args);
	ASSERT_NE(json, nullptr);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(tool_exec(&reg, "tool_create", json, &result), 0);
	for (int iteration = 0; iteration < 2; iteration++) {
		auto *entry = tool_borrow(&reg, "large_contract");
		ASSERT_NE(entry, nullptr);
		EXPECT_STREQ(entry->desc.description, description.c_str());
		for (const char *schema : {entry->desc.input_schema, entry->desc.output_schema}) {
			cJSON *parsed NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(schema);
			ASSERT_NE(parsed, nullptr);
			EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(parsed,
			"description")),
				schema_text.c_str());
		}
		if (iteration == 0) {
			tool_registry_cleanup(&reg);
			tool_registry_init(nullptr, &reg);
			ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
		}
	}
}

TEST_F(DynamicToolsTest, OutputOverflowReportsItsBudgetBeforeParsingPartialJson)
{
	cfg.dynamic_tools.default_max_output_bytes = 256;
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	const std::string args = create_args("large_output",
		"function run() { return { text: 'x'.repeat(4096) }; }");
	ASSERT_EQ(tool_exec(&reg, "tool_create", args.c_str(), &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_exec(&reg, "large_output", "{}", &result), NB_ERR_TOO_BIG);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("256-byte capture limit"), std::string::npos);
	EXPECT_EQ(std::string(result.text.ptr).find("invalid dynamic tool response"),
		  std::string::npos);
}

TEST_F(DynamicToolsTest, InitLoadsExistingSessionTools)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) {"
		"  return { value: args.value + 1 };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("session_reload", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg);

	struct tool_registry reg2;
	struct tool_context *tctx2 = tool_context_create(NULL, root.c_str(), root.c_str());

	tool_registry_init(NULL, &reg2);
	ASSERT_NE(tctx2, nullptr);
	ASSERT_EQ(dynamic_tools_init(&reg2, tctx2, &cfg.dynamic_tools,
				     "sess"), 0);
	EXPECT_NE(tool_borrow(&reg2, "session_reload"), nullptr);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg2, "session_reload", "{\"value\":41}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":42"), std::string::npos);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg2);
	tool_context_destroy(tctx2);
	tool_registry_init(NULL, &reg);
}

TEST_F(DynamicToolsTest, SetSessionIdLoadsExistingSessionTools)
{
	std::string dir = root + "/session/sess_later/later_tool";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"later_tool\","
		"\"description\":\"later session tool\","
		"\"input_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) {"
		"  return { text: String(args.text).toUpperCase() };"
		"}";

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), source, strlen(source), NULL), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools,
				     "default"), 0);
	EXPECT_EQ(tool_borrow(&reg, "later_tool"), nullptr);
	ASSERT_EQ(dynamic_tools_set_session_id(&reg, "sess_later"), 0);
	ASSERT_NE(tool_borrow(&reg, "later_tool"), nullptr);

	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "later_tool", "{\"text\":\"hello\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"text\":\"HELLO\""), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, EmptySessionDoesNotFallbackToDefault)
{
	std::string dir = root + "/session/default/default_tool";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"default_tool\","
		"\"description\":\"default session tool\","
		"\"input_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), source, strlen(source), NULL), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, ""),
		  0);
	EXPECT_EQ(tool_borrow(&reg, "default_tool"), nullptr);

	tool_result_init(&result);
	ASSERT_LT(tool_exec(&reg, "tool_create", create_args("empty_session_tool", source).c_str(),
			    &result),
		  0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("create_tool_dir"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_FALSE(
		nb_file_exists((root + "/session/default/empty_session_tool/tool.js").c_str()));
}

TEST_F(DynamicToolsTest, ToolCreateUpdatesExistingDynamicTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string first =
		"function run(args) {"
		"  return { value: 1 };"
		"}";
	std::string second =
		"function run(args) {"
		"  return { value: 2 };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("replace_me", first).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"registered\""), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("replace_me", second).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"updated\""), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "replace_me", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":2"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolHistoryDiffAndRollbackUpdatedTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string first =
		"function run(args) {"
		"  return { value: 1 };"
		"}";
	std::string second =
		"function run(args) {"
		"  return { value: 2 };"
		"}";
	struct tool_result result;
	std::string checkpoint_id;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("versioned_tool", first).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("versioned_tool", second).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	checkpoint_id = json_string_field(result.text.ptr, "checkpoint_id");
	EXPECT_EQ(checkpoint_id, "0002");
	EXPECT_NE(std::string(result.text.ptr).find("\"old_hash\""), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_history",
			    "{\"name\":\"versioned_tool\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"id\":\"0002\""), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("\"before_state\":\"present\""),
		  std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_diff",
			    "{\"name\":\"versioned_tool\",\"checkpoint_id\":\"0002\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("--- a/tool.js"), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("-function run(args)"), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("+function run(args)"), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_rollback",
			    "{\"name\":\"versioned_tool\",\"checkpoint_id\":\"0002\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"rolled_back\""), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "versioned_tool", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":1"), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_history",
			    "{\"name\":\"versioned_tool\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_EQ(std::string(result.text.ptr).find("\"id\":\"0002\""), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("\"id\":\"0001\""), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolRollbackCreationRemovesSessionTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) {"
		"  return { value: 7 };"
		"}";
	struct tool_result result;
	std::string dir = root + "/session/sess/new_then_undo";

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("new_then_undo", source).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"checkpoint_id\":\"0001\""),
		  std::string::npos);
	tool_result_cleanup(&result);
	ASSERT_NE(tool_borrow(&reg, "new_then_undo"), nullptr);
	ASSERT_TRUE(nb_file_exists(dir.c_str()));

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_rollback",
			    "{\"name\":\"new_then_undo\",\"checkpoint_id\":\"0001\"}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"state\":\"absent\""), std::string::npos);
	tool_result_cleanup(&result);

	EXPECT_EQ(tool_borrow(&reg, "new_then_undo"), nullptr);
	EXPECT_FALSE(nb_file_exists(dir.c_str()));
}

TEST_F(DynamicToolsTest, ToolCreateDoesNotOverwriteStaticTool)
{
	struct tool_spec static_spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "static_tool",
		.description = "static",
		.input_schema = TOOL_EMPTY_INPUT_SCHEMA,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = static_tool_exec,
	};
	ASSERT_EQ(tool_register_borrow(&reg, &static_spec), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) { return { static: false }; }";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_LT(
		tool_exec(&reg, "tool_create", create_args("static_tool", source).c_str(), &result),
		0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("already exists"), std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "static_tool", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"static\":true"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ServerProfileDeniesFileReadByDefault)
{
	strncpy(cfg.dynamic_tools.mode, "server",
		sizeof(cfg.dynamic_tools.mode) - 1);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string file = root + "/secret.txt";
	ASSERT_EQ(nb_file_write_all(file.c_str(), "secret", 6, NULL), 0);
	std::string source =
		"function run(args) {"
		"  return morph.fs.readText(args.path);"
		"}";
	std::string args = create_args("read_secret", source);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create", args.c_str(), &result), 0);
	tool_result_cleanup(&result);

	std::string call = "{\"path\":\"" + file + "\"}";
	tool_result_init(&result);
	EXPECT_NE(tool_exec(&reg, "read_secret", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("capability denied"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolUsesProfileCapabilitiesWithoutArgs)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string file = root + "/note.txt";
	ASSERT_EQ(nb_file_write_all(file.c_str(), "secret", 6, NULL), 0);
	std::string source =
		"function run(args) {"
		"  return { text: morph.fs.readText(args.path) };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("read_profile", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	{
		std::string meta_path =
			root + "/session/sess/read_profile/tool.json";
		size_t meta_len = 0;
		char *meta =
			(nb_file_read_all(meta_path.c_str(), &file_value_1, 0, NULL) == NB_OK &&
					 (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) +
								       1)) != NULL
				 ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
					   nb_buf_len(&file_value_1) + 1),
				    *(&meta_len) = nb_buf_len(&file_value_1),
				    nb_charp_steal(&file_copy_1))
				 : NULL);

		ASSERT_NE(meta, nullptr);
		(void)meta_len;
		EXPECT_EQ(strstr(meta, "capabilities"), nullptr);
		free(meta);
	}

	std::string call = "{\"path\":\"" + file + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "read_profile", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"text\":\"secret\""), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, RelativeFsPathsAndExecUseToolContextRoots)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	std::string workdir = root + "/work";
	std::string output_dir = root + "/configured-output";
	std::string input = workdir + "/input.txt";
	std::string output = output_dir + "/result.txt";
	std::string source =
		"function run(args) {"
		"  const text = morph.fs.readText('input.txt');"
		"  morph.fs.writeText('result.txt', text + '-written');"
		"  return { text: text, cwd: morph.exec('pwd').trim() };"
		"}";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(workdir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_mkdir(output_dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(input.c_str(), "from-workdir", 12, NULL), 0);
	tool_context_destroy(tctx);
	tctx = tool_context_create(NULL, workdir.c_str(), output_dir.c_str());
	ASSERT_NE(tctx, nullptr);
	strncpy(cfg.dynamic_tools.local.allowed_read_paths[0],
		workdir.c_str(), DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
	strncpy(cfg.dynamic_tools.local.allowed_write_paths[0],
		output_dir.c_str(), DYNAMIC_TOOL_ALLOW_LEN_MAX - 1);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("relative_fs", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "relative_fs", "{}", &result), 0)
		<< (result.text.ptr ? result.text.ptr : "(no result)");
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_EQ(json_string_field(result.text.ptr, "text"), "from-workdir");
	EXPECT_EQ(json_string_field(result.text.ptr, "cwd"), tool_context_workdir_borrow(tctx));
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	EXPECT_FALSE(nb_file_exists((workdir + "/result.txt").c_str()));
	{
		size_t len = 0;
		char *written =
			(nb_file_read_all(output.c_str(), &file_value_2, 0, NULL) == NB_OK &&
					 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) +
								       1)) != NULL
				 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
					   nb_buf_len(&file_value_2) + 1),
				    *(&len) = nb_buf_len(&file_value_2),
				    nb_charp_steal(&file_copy_2))
				 : NULL);

		ASSERT_NE(written, nullptr);
		EXPECT_EQ(std::string(written, len), "from-workdir-written");
		free(written);
	}
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, RelativeMediaPathsUseToolContextRoots)
{
	std::string workdir = root + "/media-work";
	std::string output_dir = workdir + "/output";
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 24, height: 12 });"
		"  morph.canvas.toFile({ canvas: canvas, output: 'source.png' });"
		"  await morph.image.resize({"
		"    input: 'output/source.png',"
		"    output: 'resized.png',"
		"    width: 12"
		"  });"
		"  return await morph.image.metadata({"
		"    input: 'output/resized.png'"
		"  });"
		"}";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(workdir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_mkdir(output_dir.c_str(), true, NULL), 0);
	tool_context_destroy(tctx);
	tctx = tool_context_create(NULL, workdir.c_str(), output_dir.c_str());
	ASSERT_NE(tctx, nullptr);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("relative_media", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "relative_media", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"width\":12"), std::string::npos);
	EXPECT_TRUE(nb_file_exists((output_dir + "/source.png").c_str()));
	EXPECT_TRUE(nb_file_exists((output_dir + "/resized.png").c_str()));
	EXPECT_FALSE(nb_file_exists((workdir + "/source.png").c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, PersistentToolIgnoresStoredCapabilities)
{
	std::string dir = root + "/persistent/old_caps";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	std::string file = root + "/old_note.txt";
	const char *meta_json =
		"{\"name\":\"old_caps\","
		"\"description\":\"old tool\","
		"\"input_schema\":{\"type\":\"object\"},"
		"\"capabilities\":[\"image\"]}";
	const char *source =
		"function run(args) {"
		"  return { text: morph.fs.readText(args.path) };"
		"}";

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), source, strlen(source), NULL), 0);
	ASSERT_EQ(nb_file_write_all(file.c_str(), "loaded", 6, NULL), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);

	std::string call = "{\"path\":\"" + file + "\"}";
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "old_caps", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"text\":\"loaded\""), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, PromoteLoadsInNewRegistry)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) { return { value: args.value + 1 }; }";
	std::string args = create_args("inc_js", source);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create", args.c_str(), &result), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_promote", "{\"name\":\"inc_js\"}",
			    &result), 0);
	tool_result_cleanup(&result);

	struct tool_registry reg2;
	struct tool_context *tctx2 = tool_context_create(NULL, root.c_str(), root.c_str());
	tool_registry_init(NULL, &reg2);
	ASSERT_EQ(dynamic_tools_init(&reg2, tctx2, &cfg.dynamic_tools,
				     "other"), 0);
	EXPECT_NE(tool_borrow(&reg2, "inc_js"), nullptr);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg2, "inc_js", "{\"value\":41}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":42"), std::string::npos);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg2);
	tool_context_destroy(tctx2);
}

TEST_F(DynamicToolsTest, ToolDeleteRemovesSessionTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) { return { value: 1 }; }";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("delete_session", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	struct tool_entry *entry = tool_borrow(&reg, "delete_session");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_SESSION);
	std::string dir = root + "/session/sess/delete_session";
	ASSERT_TRUE(nb_file_exists(dir.c_str()));

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"delete_session\"}", &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_borrow(&reg, "delete_session"), nullptr);
	EXPECT_FALSE(nb_file_exists(dir.c_str()));
}

TEST_F(DynamicToolsTest, ToolDeleteRemovesPersistentTool)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) { return { value: 2 }; }";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("delete_persistent", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_promote",
			    "{\"name\":\"delete_persistent\"}", &result), 0);
	tool_result_cleanup(&result);
	struct tool_entry *entry = tool_borrow(&reg, "delete_persistent");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	std::string dir = root + "/persistent/delete_persistent";
	ASSERT_TRUE(nb_file_exists(dir.c_str()));

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"delete_persistent\"}", &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_borrow(&reg, "delete_persistent"), nullptr);
	EXPECT_FALSE(nb_file_exists(dir.c_str()));
}

TEST_F(DynamicToolsTest, ToolDeleteRejectsStaticTool)
{
	struct tool_spec static_spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "static_tool",
		.description = "static",
		.input_schema = TOOL_EMPTY_INPUT_SCHEMA,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = static_tool_exec,
	};
	ASSERT_EQ(tool_register_borrow(&reg, &static_spec), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_LT(tool_exec(&reg, "tool_delete", "{\"name\":\"static_tool\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("only delete dynamic"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_NE(tool_borrow(&reg, "static_tool"), nullptr);
}

TEST_F(DynamicToolsTest, ToolDeleteSessionReloadsShadowedPersistentTool)
{
	std::string dir = root + "/persistent/shadowed_tool";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"shadowed_tool\","
		"\"description\":\"persistent shadowed tool\","
		"\"input_schema\":{\"type\":\"object\"}}";
	const char *persistent_source =
		"function run(args) { return { value: 10 }; }";
	std::string session_source =
		"function run(args) { return { value: 20 }; }";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), persistent_source, strlen(persistent_source), NULL),
		  0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_entry *entry = tool_borrow(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("shadowed_tool",
					session_source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	entry = tool_borrow(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_SESSION);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"shadowed_tool\"}", &result), 0);
	tool_result_cleanup(&result);
	entry = tool_borrow(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "shadowed_tool", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":10"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_FALSE(nb_file_exists((root + "/session/sess/shadowed_tool").c_str()));
}

TEST_F(DynamicToolsTest, ToolDeleteRefusesPersistentPathMismatch)
{
	std::string dir = root + "/persistent/wrong_dir";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"mismatch_tool\","
		"\"description\":\"mismatch\","
		"\"input_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), source, strlen(source), NULL), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);
	ASSERT_NE(tool_borrow(&reg, "mismatch_tool"), nullptr);

	tool_result_init(&result);
	ASSERT_LT(tool_exec(&reg, "tool_delete", "{\"name\":\"mismatch_tool\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("deletion refused"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_TRUE(nb_file_exists(dir.c_str()));
	EXPECT_NE(tool_borrow(&reg, "mismatch_tool"), nullptr);
}

TEST_F(DynamicToolsTest, ToolDeleteRefusesSessionPathMismatch)
{
	std::string dir = root + "/session/sess/wrong_session_dir";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"session_mismatch\","
		"\"description\":\"mismatch\","
		"\"input_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), meta_json, strlen(meta_json), NULL), 0);
	ASSERT_EQ(nb_file_write_all(js.c_str(), source, strlen(source), NULL), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);
	ASSERT_NE(tool_borrow(&reg, "session_mismatch"), nullptr);

	tool_result_init(&result);
	ASSERT_LT(tool_exec(&reg, "tool_delete", "{\"name\":\"session_mismatch\"}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("deletion refused"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_TRUE(nb_file_exists(dir.c_str()));
	EXPECT_NE(tool_borrow(&reg, "session_mismatch"), nullptr);
}

TEST_F(DynamicToolsTest, CanvasApiCreatesImage)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 64, height: 32 });"
		"  const ctx = canvas.getContext('2d');"
		"  ctx.fillStyle = '#ffffff';"
		"  ctx.fillRect(0, 0, 64, 32);"
		"  ctx.fillStyle = '#111111';"
		"  ctx.fillText(args.text, 8, 20);"
		"  morph.canvas.toFile({ canvas: canvas, output: args.output });"
		"  return { output: args.output };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("canvas_make", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string output = root + "/canvas.png";
	std::string call = "{\"text\":\"ok\",\"output\":\"" + output + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "canvas_make", call.c_str(), &result), 0);
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, CanvasTextPositioningAffectsRendering)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function equal(a, b) {"
		"  a = new Uint8Array(a); b = new Uint8Array(b);"
		"  if (a.length !== b.length) return false;"
		"  for (let i = 0; i < a.length; i++)"
		"    if (a[i] !== b[i]) return false;"
		"  return true;"
		"}"
		"let sequence = 0;"
		"function render(dir, align, baseline, stroke) {"
		"  const c = morph.canvas.create({ width: 180, height: 100 });"
		"  const ctx = c.getContext('2d');"
		"  ctx.font = '32px sans-serif';"
		"  ctx.fillStyle = '#111111'; ctx.strokeStyle = '#111111';"
		"  ctx.textAlign = align; ctx.textBaseline = baseline;"
		"  if (stroke) ctx.strokeText('Align', 90, 50);"
		"  else ctx.fillText('Align', 90, 50);"
		"  const path = dir + '/canvas-text-' + sequence++ + '.png';"
		"  c.toFile(path);"
		"  return morph.fs.readFile(path);"
		"}"
		"async function run(args) {"
		"  const defaults = morph.canvas.create({ width: 10, height: 10 })"
		"    .getContext('2d');"
		"  if (defaults.textAlign !== 'start' ||"
		"      defaults.textBaseline !== 'alphabetic')"
		"    throw new Error('canvas text defaults mismatch');"
		"  const left = render(args.dir, 'left', 'alphabetic', false);"
		"  const start = render(args.dir, 'start', 'alphabetic', false);"
		"  const center = render(args.dir, 'center', 'alphabetic', false);"
		"  const right = render(args.dir, 'right', 'alphabetic', false);"
		"  const end = render(args.dir, 'end', 'alphabetic', false);"
		"  if (!equal(left, start) || !equal(right, end) ||"
		"      equal(left, center) || equal(center, right))"
		"    throw new Error('textAlign did not affect fillText');"
		"  const top = render(args.dir, 'center', 'top', false);"
		"  const middle = render(args.dir, 'center', 'middle', false);"
		"  const bottom = render(args.dir, 'center', 'bottom', false);"
		"  if (equal(top, middle) || equal(middle, bottom))"
		"    throw new Error('textBaseline did not affect fillText');"
		"  if (equal(render(args.dir, 'left', 'top', true),"
		"            render(args.dir, 'right', 'bottom', true)))"
		"    throw new Error('text positioning did not affect strokeText');"
		"  return { ok: true };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("canvas_text_position", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string call = "{\"dir\":\"" + json_escape(root) + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "canvas_text_position", call.c_str(), &result),
		  0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"ok\":true"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ImageApiResizesImage)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 64, height: 32 });"
		"  morph.canvas.toFile({ canvas: canvas, output: args.input });"
		"  await morph.image.resize({"
		"    input: args.input, output: args.output, width: 16"
		"  });"
		"  const meta = await morph.image.metadata({ input: args.output });"
		"  return { width: meta.width, height: meta.height };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("image_resize", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string input = root + "/sharp_input.png";
	std::string output = root + "/sharp_output.png";
	std::string call = "{\"input\":\"" + input + "\",\"output\":\"" +
		output + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_resize", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"width\":16"), std::string::npos);
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ImageApiLoadsAndCompositesImage)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 16, height: 16 });"
		"  const ctx = canvas.getContext('2d');"
		"  ctx.fillStyle = '#ff0000';"
		"  ctx.fillRect(0, 0, 16, 16);"
		"  morph.canvas.toFile({ canvas: canvas, output: args.input });"
		"  await morph.image.compose({"
		"    input: args.base, output: args.output,"
		"    overlays: [{ input: args.input, left: 10, top: 8 }]"
		"  });"
		"  const meta = await morph.image.metadata({ input: args.output });"
		"  return { width: meta.width, height: meta.height };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("image_composite", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string input = root + "/sharp_overlay.png";
	std::string base = root + "/sharp_base.png";
	std::string output = root + "/sharp_composite.png";
	std::string make_base =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 80, height: 48 });"
		"  const ctx = canvas.getContext('2d');"
		"  ctx.fillStyle = '#ffffff';"
		"  ctx.fillRect(0, 0, 80, 48);"
		"  morph.canvas.toFile({ canvas: canvas, output: args.output });"
		"  return { output: args.output };"
		"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("image_base", make_base).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_base",
			    ("{\"output\":\"" + base + "\"}").c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	std::string call = "{\"input\":\"" + input + "\",\"base\":\"" +
		base + "\",\"output\":\"" + output + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_composite", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"width\":80"), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("\"height\":48"), std::string::npos);
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ImageApiExtendsImageWithBackground)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 20, height: 10 });"
		"  morph.canvas.toFile({ canvas: canvas, output: args.input });"
		"  await morph.image.extend({"
		"    input: args.input, output: args.output,"
		"    top: 3, bottom: 7, left: 5, right: 11,"
		"    background: { r: 255, g: 255, b: 255, alpha: 1 }"
		"  });"
		"  const meta = await morph.image.metadata({ input: args.output });"
		"  return { width: meta.width, height: meta.height };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("image_extend", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string input = root + "/sharp_extend_input.png";
	std::string output = root + "/sharp_extend_output.png";
	std::string call = "{\"input\":\"" + input + "\",\"output\":\"" +
		output + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_extend", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"width\":36"), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("\"height\":20"), std::string::npos);
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ImageApiFramesImage)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 40, height: 24 });"
		"  const ctx = canvas.getContext('2d');"
		"  ctx.fillStyle = '#4477cc';"
		"  ctx.fillRect(0, 0, 40, 24);"
		"  morph.canvas.toFile({ canvas: canvas, output: args.input });"
		"  const out = await morph.image.frame({"
		"    input: args.input, output: args.output, style: 'neon',"
		"    caption: 'demo', padding: 12"
		"  });"
		"  return { width: out.width, height: out.height };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("image_frame", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string input = root + "/frame_input.jpg";
	std::string output = root + "/frame_output.jpg";
	std::string call = "{\"input\":\"" + input + "\",\"output\":\"" +
		output + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_frame", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"width\":64"), std::string::npos);
	EXPECT_NE(std::string(result.text.ptr).find("\"height\":60"), std::string::npos);
	EXPECT_TRUE(nb_file_exists(output.c_str()));
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolCreateRejectsRequire)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"const sharp = require('sharp');"
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_LT(
		tool_exec(&reg, "tool_create", create_args("bad_require", source).c_str(), &result),
		0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("forbidden token"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, WebAssemblyApiCallsExport)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const bytes = new Uint8Array(["
		"    0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,"
		"    127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,"
		"    1,7,0,32,0,32,1,106,11]);"
		"  const mod = await WebAssembly.instantiate(bytes.buffer, {});"
		"  return { value: mod.instance.exports.add(args.a, args.b) };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("wasm_add", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "wasm_add", "{\"a\":20,\"b\":22}",
			    &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":42"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ServerProfileDeniesImageCapability)
{
	strncpy(cfg.dynamic_tools.mode, "server",
		sizeof(cfg.dynamic_tools.mode) - 1);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) {"
		"  const canvas = morph.canvas.create({ width: 8, height: 8 });"
		"  morph.canvas.toFile({ canvas: canvas, output: args.output });"
		"  return { ok: true };"
		"}";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("canvas_denied", source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);

	std::string output = root + "/denied.png";
	std::string call = "{\"output\":\"" + output + "\"}";
	tool_result_init(&result);
	EXPECT_NE(tool_exec(&reg, "canvas_denied", call.c_str(), &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("capability denied"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, FilesystemAllowlistRejectsSiblingPrefixesAndSymlinks)
{
	std::string allowed = root + "/allowed";
	std::string sibling = root + "/allowed-other";
	ASSERT_EQ(nb_file_mkdir(allowed.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_mkdir(sibling.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all((sibling + "/secret").c_str(), "secret", 6, NULL), 0);
	ASSERT_EQ(symlink(sibling.c_str(), (allowed + "/escape").c_str()), 0);
	snprintf(cfg.dynamic_tools.local.allowed_read_paths[0],
		sizeof(cfg.dynamic_tools.local.allowed_read_paths[0]), "%s", allowed.c_str());
	cfg.dynamic_tools.local.allowed_read_paths_count = 1;
	snprintf(cfg.dynamic_tools.local.allowed_write_paths[0],
		sizeof(cfg.dynamic_tools.local.allowed_write_paths[0]), "%s", allowed.c_str());
	cfg.dynamic_tools.local.allowed_write_paths_count = 1;
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source = "function run(args) { if (args.write) {"
		"morph.fs.writeText(args.path, 'ok'); return {ok:true}; }"
		"return {text:morph.fs.readText(args.path)}; }";
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
		create_args("boundary_probe", source).c_str(), &result), 0);
	tool_result_cleanup(&result);
	for (const std::string &path : {allowed + "/ok", sibling + "/result",
		allowed + "/escape/result"}) {
		tool_result_init(&result);
		std::string args = "{\"write\":true,\"path\":\"" + path + "\"}";
		int rc = tool_exec(&reg, "boundary_probe", args.c_str(), &result);
		if (path == allowed + "/ok")
			EXPECT_EQ(rc, 0);
		else
			EXPECT_NE(rc, 0);
		tool_result_cleanup(&result);
	}
	for (const std::string &path : {sibling + "/secret", allowed + "/escape/secret"}) {
		tool_result_init(&result);
		std::string args = "{\"path\":\"" + path + "\"}";
		EXPECT_NE(tool_exec(&reg, "boundary_probe", args.c_str(), &result), 0);
		tool_result_cleanup(&result);
	}
	EXPECT_FALSE(nb_file_exists((sibling + "/result").c_str()));
}

TEST_F(DynamicToolsTest, MetadataCommitKeepsSourceCoherentAfterInterruptedMirrorWrite)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("committed",
					"function run(args) { return {version: 'committed'}; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	std::string source = root + "/session/sess/committed/tool.js";
	ASSERT_EQ(nb_file_write_all(source.c_str(), "throw new Error('uncommitted');", 31, NULL),
		  0);
	tool_registry_cleanup(&reg);
	tool_registry_init(nullptr, &reg);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "committed", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("committed"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, InvalidSourceNeverCreatesAPublishedToolDirectory)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result result;
	tool_result_init(&result);
	EXPECT_LT(tool_exec(&reg, "tool_create",
			    create_args("invalid_candidate", "function run(").c_str(), &result),
		  0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("check_js"), std::string::npos);
	EXPECT_FALSE(nb_file_exists((root + "/session/sess/invalid_candidate").c_str()));
	EXPECT_EQ(tool_borrow(&reg, "invalid_candidate"), nullptr);
	tool_result_cleanup(&result);
}

static enum tool_operation_verdict grow_registry_during_approval(const tool_operation *, void *user)
{
	auto *reg = static_cast<tool_registry *>(user);
	for (int i = 0; i < 128; ++i) {
		std::string name = "approval_growth_" + std::to_string(i);
		tool_spec spec{};
		spec.name = name.c_str();
		spec.description = "Approval registry growth";
		spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
		spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
		spec.exec = static_tool_exec;
		if (tool_register_borrow(reg, &spec) != 0) {
			return TOOL_OP_DENY;
		}
	}
	return TOOL_OP_ALLOW;
}

TEST_F(DynamicToolsTest, ApprovalRegistryGrowthCannotInvalidateTheCheckpointSource)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("approved_update",
					"function run(args) { return {version: 1}; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	cfg.dynamic_tools.create_requires_approval = 1;
	tool_context_set_operation_approval(tctx, grow_registry_during_approval, &reg);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "tool_create",
			    create_args("approved_update",
					"function run(args) { return {version: 2}; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	tool_context_set_operation_approval(tctx, nullptr, nullptr);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "approved_update", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"version\":2"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, RetainedExecutionKeepsItsSourceAfterUpdateAndDeletion)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("retained", "function run(args) { return { version: 1 }; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	tool_handle retained{};
	ASSERT_EQ(tool_registry_acquire(&reg, "retained", &retained), 0);

	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "tool_create",
			    create_args("retained", "function run(args) { return { version: 2 }; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "retained", "{}", &result), 0);
	EXPECT_NE(std::string(result.text.ptr ? result.text.ptr : "").find("\"version\":2"),
		  std::string::npos);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "tool_delete", "{\"name\":\"retained\"}", &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_borrow(&reg, "retained"), nullptr);

	tool_result_init(&result);
	EXPECT_EQ(retained.exec("{}", &result, retained.user_data), 0);
	EXPECT_EQ(tool_result_finalize(&result), 0);
	EXPECT_NE(std::string(result.text.ptr ? result.text.ptr : "").find("\"version\":1"),
		  std::string::npos);
	tool_result_cleanup(&result);
	tool_handle_release(&retained);
}

TEST_F(DynamicToolsTest, ImageCanvasFilesystemAndWasmBindingsCoexist)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"async function run(args) {"
		"  const canvas = morph.canvas.create({ width: 8, height: 4 });"
		"  const ctx = canvas.getContext('2d');"
		"  ctx.fillStyle = '#ff0000'; ctx.fillRect(0, 0, 8, 4);"
		"  const png = morph.canvas.toBuffer({ canvas });"
		"  morph.fs.writeFile(args.output, png);"
		"  const loaded = morph.fs.readFile(args.output);"
		"  const meta = await morph.image.metadata({ input: args.output });"
		"  const wasm = new Uint8Array(["
		"    0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,"
		"    127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,"
		"    1,7,0,32,0,32,1,106,11]);"
		"  const mod = await WebAssembly.instantiate(wasm.buffer, {});"
		"  return { width: meta.width, height: meta.height,"
		"    bytes: loaded.byteLength, value: mod.instance.exports.add(20, 22) };"
		"}";
	tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create", create_args("all_bindings", source).c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	std::string call = "{\"output\":\"" + root + "/all-bindings.png\"}";
	ASSERT_EQ(tool_exec(&reg, "all_bindings", call.c_str(), &result), 0);
	cJSON *response = cJSON_Parse(result.text.ptr);
	ASSERT_NE(response, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(response, "width")->valueint, 8);
	EXPECT_EQ(cJSON_GetObjectItem(response, "height")->valueint, 4);
	EXPECT_GT(cJSON_GetObjectItem(response, "bytes")->valueint, 0);
	EXPECT_EQ(cJSON_GetObjectItem(response, "value")->valueint, 42);
	cJSON_Delete(response);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, RejectsNodeBufferWhileSupportingHostToBufferMethods)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result result;
	tool_result_init(&result);
	EXPECT_LT(tool_exec(&reg, "tool_create",
			    create_args("node_buffer",
					"function run(args) { return Buffer.alloc(8); }")
				    .c_str(),
			    &result),
		  0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("forbidden token"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, FilesystemRetainsTemporaryBufferAndRejectsInvalidRanges)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	const std::string source =
		"function run(args) {"
		"  morph.fs.writeFile(args.output, {"
		"    get buffer() { return new Uint8Array([17,34,51,68]).buffer; },"
		"    byteOffset: 1, byteLength: 2 });"
		"  const bytes = new Uint8Array(morph.fs.readFile(args.output));"
		"  let rangeError = false; let indexError = false;"
		"  let getterError = false; let detachedError = false;"
		"  try { morph.fs.writeFile(args.output, {"
		"    buffer: new ArrayBuffer(2), byteOffset: 1, byteLength: 2 });"
		"  } catch (error) { rangeError = error instanceof RangeError; }"
		"  try { morph.fs.writeFile(args.output, {"
		"    buffer: new ArrayBuffer(2), byteOffset: -1, byteLength: 1 });"
		"  } catch (error) { indexError = error instanceof RangeError; }"
		"  const sentinel = new Error('buffer getter failed');"
		"  try { morph.fs.writeFile(args.output, {"
		"    get buffer() { throw sentinel; } });"
		"  } catch (error) { getterError = error === sentinel; }"
		"  const detached = new ArrayBuffer(2); detached.transfer();"
		"  try { morph.fs.writeFile(args.output, detached);"
		"  } catch (error) { detachedError = error instanceof TypeError; }"
		"  morph.fs.writeFile(args.output, new ArrayBuffer(0));"
		"  const emptyBytes = morph.fs.readFile(args.output).byteLength;"
		"  return { first: bytes[0], second: bytes[1], rangeError, indexError,"
		"    getterError, detachedError, emptyBytes };"
		"}";
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	ASSERT_EQ(
		tool_exec(&reg, "tool_create", create_args("buffer_view", source).c_str(), &result),
		0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	const std::string call = "{\"output\":\"" + root + "/buffer-view.bin\"}";
	ASSERT_EQ(tool_exec(&reg, "buffer_view", call.c_str(), &result), 0);
	cJSON *response = cJSON_Parse(result.text.ptr);
	ASSERT_NE(response, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(response, "first")->valueint, 34);
	EXPECT_EQ(cJSON_GetObjectItem(response, "second")->valueint, 51);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(response, "rangeError")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(response, "indexError")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(response, "getterError")));
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(response, "detachedError")));
	EXPECT_EQ(cJSON_GetObjectItem(response, "emptyBytes")->valueint, 0);
	cJSON_Delete(response);
}

TEST_F(DynamicToolsTest, LazyImageOwnsEncodedInputAfterSourceMutation)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	const std::string source =
		"function run(args) {"
		"  const canvas = morph.canvas.create({ width: 8, height: 4 });"
		"  const context = canvas.getContext('2d');"
		"  context.fillStyle = '#ff0000'; context.fillRect(0, 0, 8, 4);"
		"  const bytes = new Uint8Array(morph.canvas.toBuffer({ canvas }));"
		"  const image = morph.image.open(bytes);"
		"  bytes.fill(0);"
		"  const result = image.resize(4, 2).png().toBuffer();"
		"  const metadata = morph.image.open(result).metadata();"
		"  return { width: metadata.width, height: metadata.height };"
		"}";
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	ASSERT_EQ(
		tool_exec(&reg, "tool_create", create_args("image_copy", source).c_str(), &result),
		0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "image_copy", "{}", &result), 0);
	cJSON *response = cJSON_Parse(result.text.ptr);
	ASSERT_NE(response, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(response, "width")->valueint, 4);
	EXPECT_EQ(cJSON_GetObjectItem(response, "height")->valueint, 2);
	cJSON_Delete(response);
}

TEST_F(DynamicToolsTest, DefinitionReadCopyAndUpdateUseTheSameSourceErrors)
{
	const std::string dir = root + "/legacy";
	const std::string meta = dir + "/tool.json";
	const std::string source = dir + "/tool.js";
	const std::string destination_meta = root + "/copy.json";
	const std::string destination_source = root + "/copy.js";
	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	struct {
		const char *metadata;
		int error;
	} cases[] = {
		{"{\"name\":\"legacy\"}", NB_ERR_NOT_FOUND},
		{"{invalid", NB_ERR_PARSE},
		{"{\"name\":\"legacy\",\"source\":42}", NB_ERR_FORMAT},
	};
	for (const auto &item : cases) {
		SCOPED_TRACE(item.metadata);
		ASSERT_EQ(
			nb_file_write_all(meta.c_str(), item.metadata, strlen(item.metadata), NULL),
			0);
		struct dynamic_tool_definition definition{};
		EXPECT_EQ(dynamic_tool_store_load_definition(meta.c_str(), source.c_str(),
							     &definition),
			  item.error);
		EXPECT_EQ(definition.document, nullptr);
		dynamic_tool_store_definition_cleanup(&definition);
		EXPECT_EQ(dynamic_tool_store_copy_tool_snapshot(meta.c_str(), source.c_str(),
								destination_meta.c_str(),
								destination_source.c_str()),
			  item.error);
		struct dynamic_tool tool{};
		snprintf(tool.dir, sizeof(tool.dir), "%s", dir.c_str());
		snprintf(tool.source_path, sizeof(tool.source_path), "%s", source.c_str());
		EXPECT_EQ(dynamic_tool_store_write_tool_files(
				  &tool, "function run(args) { return args; }"),
			  item.error);
		EXPECT_EQ(tool.source, nullptr);
		EXPECT_FALSE(nb_file_exists(source.c_str()));
		EXPECT_FALSE(nb_file_exists(destination_meta.c_str()));
		EXPECT_FALSE(nb_file_exists(destination_source.c_str()));
	}
}

TEST_F(DynamicToolsTest, DefinitionCopyUsesCommittedSourceAndPreservesUnknownMetadata)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const std::string dir = root + "/definition";
	const std::string meta = dir + "/tool.json";
	const std::string mirror = dir + "/tool.js";
	const std::string copy_meta = root + "/copied.json";
	const std::string copy_source = root + "/copied.js";
	const char metadata[] = "{\"name\":\"committed\",\"source\":\"committed source\","
				"\"extension\":{\"version\":7}}";
	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), metadata, strlen(metadata), NULL), 0);
	ASSERT_FALSE(nb_file_exists(mirror.c_str()));
	EXPECT_EQ(dynamic_tool_store_copy_tool_snapshot(meta.c_str(), mirror.c_str(),
							copy_meta.c_str(), copy_source.c_str()),
		  0);
	struct dynamic_tool_definition definition{};
	ASSERT_EQ(dynamic_tool_store_load_definition(copy_meta.c_str(), copy_source.c_str(),
						     &definition),
		  0);
	EXPECT_STREQ(definition.name, "committed");
	EXPECT_STREQ(definition.source, "committed source");
	EXPECT_STREQ(definition.input_schema, TOOL_EMPTY_INPUT_SCHEMA);
	EXPECT_STREQ(definition.output_schema, TOOL_OBJECT_OUTPUT_SCHEMA);
	EXPECT_FALSE(definition.legacy_source);
	const cJSON *extension = cJSON_GetObjectItem(definition.document, "extension");
	ASSERT_TRUE(cJSON_IsObject(extension));
	EXPECT_EQ(cJSON_GetObjectItem(extension, "version")->valueint, 7);
	dynamic_tool_store_definition_cleanup(&definition);
	char *copied =
		(nb_file_read_all(copy_source.c_str(), &file_value_3, 0, NULL) == NB_OK &&
				 (file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				   nb_buf_len(&file_value_3) + 1),
			    nb_charp_steal(&file_copy_3))
			 : NULL);
	ASSERT_NE(copied, nullptr);
	EXPECT_STREQ(copied, "committed source");
	free(copied);
}

TEST_F(DynamicToolsTest, SessionReloadUsesCommittedDefinitionWithoutASourceMirror)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("without_mirror",
					"function run(args) { return { value: 42 }; }")
				    .c_str(),
			    &result),
		  0);
	tool_result_cleanup(&result);
	ASSERT_EQ(unlink((root + "/session/sess/without_mirror/tool.js").c_str()), 0);
	tool_registry_cleanup(&reg);
	tool_registry_init(nullptr, &reg);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&reg, "without_mirror", "{}", &result), 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(std::string(result.text.ptr).find("\"value\":42"), std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, LegacyDefinitionsNormalizeBeforeCopyAndUpdate)
{
	const std::string dir = root + "/legacy";
	const std::string meta = dir + "/tool.json";
	const std::string source = dir + "/tool.js";
	const std::string copied_meta = root + "/copied.json";
	const std::string copied_source = root + "/copied.js";
	const char metadata[] = "{\"name\":\"legacy\",\"input_schema\":{\"type\":\"object\"}}";
	const char original[] = "function run(args) { return { value: 1 }; }";
	const char updated[] = "function run(args) { return { value: 2 }; }";
	ASSERT_EQ(nb_file_mkdir(dir.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(meta.c_str(), metadata, strlen(metadata), NULL), 0);
	ASSERT_EQ(nb_file_write_all(source.c_str(), original, strlen(original), NULL), 0);
	struct dynamic_tool_definition definition{};
	ASSERT_EQ(dynamic_tool_store_load_definition(meta.c_str(), source.c_str(), &definition), 0);
	EXPECT_TRUE(definition.legacy_source);
	EXPECT_STREQ(definition.source, original);
	EXPECT_STREQ(definition.input_schema, TOOL_EMPTY_INPUT_SCHEMA);
	dynamic_tool_store_definition_cleanup(&definition);
	ASSERT_EQ(dynamic_tool_store_copy_tool_snapshot(meta.c_str(), source.c_str(),
							copied_meta.c_str(), copied_source.c_str()),
		  0);
	ASSERT_EQ(unlink(copied_source.c_str()), 0);
	ASSERT_EQ(dynamic_tool_store_load_definition(copied_meta.c_str(), copied_source.c_str(),
						     &definition),
		  0);
	EXPECT_FALSE(definition.legacy_source);
	EXPECT_STREQ(definition.source, original);
	dynamic_tool_store_definition_cleanup(&definition);
	struct dynamic_tool tool{};
	snprintf(tool.name, sizeof(tool.name), "%s", "legacy");
	char input_schema[] = TOOL_EMPTY_INPUT_SCHEMA;
	char output_schema[] = TOOL_OBJECT_OUTPUT_SCHEMA;
	tool.input_schema = input_schema;
	tool.output_schema = output_schema;
	snprintf(tool.dir, sizeof(tool.dir), "%s", dir.c_str());
	snprintf(tool.source_path, sizeof(tool.source_path), "%s", source.c_str());
	ASSERT_EQ(dynamic_tool_store_write_tool_files(&tool, updated), 0);
	free(tool.source);
	ASSERT_EQ(unlink(source.c_str()), 0);
	ASSERT_EQ(dynamic_tool_store_load_definition(meta.c_str(), source.c_str(), &definition), 0);
	EXPECT_FALSE(definition.legacy_source);
	EXPECT_STREQ(definition.source, updated);
	dynamic_tool_store_definition_cleanup(&definition);
}
