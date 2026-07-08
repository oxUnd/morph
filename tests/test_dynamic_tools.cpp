#include <gtest/gtest.h>
extern "C" {
#include "agent/tools/dynamic_tools.h"
#include "agent/tool_context.h"
#include "config.h"
#include "cJSON.h"
#include "util/file.h"
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
	return tool_result_set_json(result, "{\"static\":true}");
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
		config_set_defaults(&cfg);
		snprintf(cfg.dynamic_tools.session_dir,
			 sizeof(cfg.dynamic_tools.session_dir),
			 "%s/session", root.c_str());
		snprintf(cfg.dynamic_tools.persistent_dir,
			 sizeof(cfg.dynamic_tools.persistent_dir),
			 "%s/persistent", root.c_str());
		cfg.dynamic_tools.default_timeout_seconds = 5;
		cfg.dynamic_tools.promote_requires_approval = 0;
		tool_registry_init(&reg);
		tctx = tool_context_create(root.c_str(), root.c_str());
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
			"\"args_schema\":{\"type\":\"object\"},"
			"\"source_js\":\"" + json_escape(source) + "\"";
		args += "}";
		return args;
	}
};

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
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root_json = cJSON_Parse(result.text.data);
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
	EXPECT_TRUE(file_exists(expected.c_str()));
	EXPECT_FALSE(file_exists(old.c_str()));
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
	struct tool_context *tctx2 = tool_context_create(root.c_str(),
							 root.c_str());

	tool_registry_init(&reg2);
	ASSERT_NE(tctx2, nullptr);
	ASSERT_EQ(dynamic_tools_init(&reg2, tctx2, &cfg.dynamic_tools,
				     "sess"), 0);
	EXPECT_NE(tool_lookup(&reg2, "session_reload"), nullptr);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg2, "session_reload", "{\"value\":41}",
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"value\":42"),
		  std::string::npos);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&reg2);
	tool_context_destroy(tctx2);
	tool_registry_init(&reg);
}

TEST_F(DynamicToolsTest, SetSessionIdLoadsExistingSessionTools)
{
	std::string dir = root + "/session/sess_later/later_tool";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"later_tool\","
		"\"description\":\"later session tool\","
		"\"args_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) {"
		"  return { text: String(args.text).toUpperCase() };"
		"}";

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), source, strlen(source)), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools,
				     "default"), 0);
	EXPECT_EQ(tool_lookup(&reg, "later_tool"), nullptr);
	ASSERT_EQ(dynamic_tools_set_session_id(&reg, "sess_later"), 0);
	ASSERT_NE(tool_lookup(&reg, "later_tool"), nullptr);

	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "later_tool", "{\"text\":\"hello\"}",
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"text\":\"HELLO\""),
		  std::string::npos);
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
		"\"args_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), source, strlen(source)), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, ""),
		  0);
	EXPECT_EQ(tool_lookup(&reg, "default_tool"), nullptr);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("empty_session_tool", source).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find(
			  "create_tool_dir"),
		  std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_FALSE(file_exists((root +
		"/session/default/empty_session_tool/tool.js").c_str()));
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"registered\""),
		  std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("replace_me", second).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"updated\""),
		  std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "replace_me", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"value\":2"),
		  std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolCreateDoesNotOverwriteStaticTool)
{
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "static_tool", "static", "{}",
				static_tool_exec, nullptr, nullptr), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string source =
		"function run(args) { return { static: false }; }";
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("static_tool", source).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("already exists"),
		  std::string::npos);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "static_tool", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"static\":true"),
		  std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ServerProfileDeniesFileReadByDefault)
{
	strncpy(cfg.dynamic_tools.mode, "server",
		sizeof(cfg.dynamic_tools.mode) - 1);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string file = root + "/secret.txt";
	ASSERT_EQ(file_write_all(file.c_str(), "secret", 6), 0);
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("capability denied"),
		  std::string::npos);
	tool_result_cleanup(&result);
}

TEST_F(DynamicToolsTest, ToolUsesProfileCapabilitiesWithoutArgs)
{
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	std::string file = root + "/note.txt";
	ASSERT_EQ(file_write_all(file.c_str(), "secret", 6), 0);
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
		char *meta = file_read_all(meta_path.c_str(), &meta_len);

		ASSERT_NE(meta, nullptr);
		(void)meta_len;
		EXPECT_EQ(strstr(meta, "capabilities"), nullptr);
		free(meta);
	}

	std::string call = "{\"path\":\"" + file + "\"}";
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "read_profile", call.c_str(), &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"text\":\"secret\""),
		  std::string::npos);
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
		"\"args_schema\":{\"type\":\"object\"},"
		"\"capabilities\":[\"image\"]}";
	const char *source =
		"function run(args) {"
		"  return { text: morph.fs.readText(args.path) };"
		"}";

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), source, strlen(source)), 0);
	ASSERT_EQ(file_write_all(file.c_str(), "loaded", 6), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);

	std::string call = "{\"path\":\"" + file + "\"}";
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "old_caps", call.c_str(), &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"text\":\"loaded\""),
		  std::string::npos);
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
	struct tool_context *tctx2 = tool_context_create(root.c_str(),
							 root.c_str());
	tool_registry_init(&reg2);
	ASSERT_EQ(dynamic_tools_init(&reg2, tctx2, &cfg.dynamic_tools,
				     "other"), 0);
	EXPECT_NE(tool_lookup(&reg2, "inc_js"), nullptr);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg2, "inc_js", "{\"value\":41}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"value\":42"),
		  std::string::npos);
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
	struct tool_entry *entry = tool_lookup(&reg, "delete_session");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_SESSION);
	std::string dir = root + "/session/sess/delete_session";
	ASSERT_TRUE(file_exists(dir.c_str()));

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"delete_session\"}", &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_lookup(&reg, "delete_session"), nullptr);
	EXPECT_FALSE(file_exists(dir.c_str()));
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
	struct tool_entry *entry = tool_lookup(&reg, "delete_persistent");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);
	std::string dir = root + "/persistent/delete_persistent";
	ASSERT_TRUE(file_exists(dir.c_str()));

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"delete_persistent\"}", &result), 0);
	tool_result_cleanup(&result);
	EXPECT_EQ(tool_lookup(&reg, "delete_persistent"), nullptr);
	EXPECT_FALSE(file_exists(dir.c_str()));
}

TEST_F(DynamicToolsTest, ToolDeleteRejectsStaticTool)
{
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &reg, "static_tool",
				"static", "{}", static_tool_exec, nullptr,
				nullptr), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"), 0);
	struct tool_result result;

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"static_tool\"}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("only delete dynamic"),
		  std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_NE(tool_lookup(&reg, "static_tool"), nullptr);
}

TEST_F(DynamicToolsTest, ToolDeleteSessionReloadsShadowedPersistentTool)
{
	std::string dir = root + "/persistent/shadowed_tool";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"shadowed_tool\","
		"\"description\":\"persistent shadowed tool\","
		"\"args_schema\":{\"type\":\"object\"}}";
	const char *persistent_source =
		"function run(args) { return { value: 10 }; }";
	std::string session_source =
		"function run(args) { return { value: 20 }; }";
	struct tool_result result;

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), persistent_source,
				 strlen(persistent_source)), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);
	struct tool_entry *entry = tool_lookup(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("shadowed_tool",
					session_source).c_str(),
			    &result), 0);
	tool_result_cleanup(&result);
	entry = tool_lookup(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_SESSION);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"shadowed_tool\"}", &result), 0);
	tool_result_cleanup(&result);
	entry = tool_lookup(&reg, "shadowed_tool");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->origin, TOOL_ORIGIN_DYNAMIC_PERSISTENT);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "shadowed_tool", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"value\":10"),
		  std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_FALSE(file_exists((root +
		"/session/sess/shadowed_tool").c_str()));
}

TEST_F(DynamicToolsTest, ToolDeleteRefusesPersistentPathMismatch)
{
	std::string dir = root + "/persistent/wrong_dir";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"mismatch_tool\","
		"\"description\":\"mismatch\","
		"\"args_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), source, strlen(source)), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);
	ASSERT_NE(tool_lookup(&reg, "mismatch_tool"), nullptr);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"mismatch_tool\"}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("deletion refused"),
		  std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_TRUE(file_exists(dir.c_str()));
	EXPECT_NE(tool_lookup(&reg, "mismatch_tool"), nullptr);
}

TEST_F(DynamicToolsTest, ToolDeleteRefusesSessionPathMismatch)
{
	std::string dir = root + "/session/sess/wrong_session_dir";
	std::string meta = dir + "/tool.json";
	std::string js = dir + "/tool.js";
	const char *meta_json =
		"{\"name\":\"session_mismatch\","
		"\"description\":\"mismatch\","
		"\"args_schema\":{\"type\":\"object\"}}";
	const char *source =
		"function run(args) { return { ok: true }; }";
	struct tool_result result;

	ASSERT_EQ(file_ensure_dir(dir.c_str()), 0);
	ASSERT_EQ(file_write_all(meta.c_str(), meta_json, strlen(meta_json)),
		  0);
	ASSERT_EQ(file_write_all(js.c_str(), source, strlen(source)), 0);
	ASSERT_EQ(dynamic_tools_init(&reg, tctx, &cfg.dynamic_tools, "sess"),
		  0);
	ASSERT_NE(tool_lookup(&reg, "session_mismatch"), nullptr);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "tool_delete",
			    "{\"name\":\"session_mismatch\"}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("deletion refused"),
		  std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_TRUE(file_exists(dir.c_str()));
	EXPECT_NE(tool_lookup(&reg, "session_mismatch"), nullptr);
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
	EXPECT_TRUE(file_exists(output.c_str()));
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"width\":16"),
		  std::string::npos);
	EXPECT_TRUE(file_exists(output.c_str()));
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"width\":80"),
		  std::string::npos);
	EXPECT_NE(std::string(result.text.data).find("\"height\":48"),
		  std::string::npos);
	EXPECT_TRUE(file_exists(output.c_str()));
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"width\":36"),
		  std::string::npos);
	EXPECT_NE(std::string(result.text.data).find("\"height\":20"),
		  std::string::npos);
	EXPECT_TRUE(file_exists(output.c_str()));
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"width\":64"),
		  std::string::npos);
	EXPECT_NE(std::string(result.text.data).find("\"height\":60"),
		  std::string::npos);
	EXPECT_TRUE(file_exists(output.c_str()));
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
	ASSERT_EQ(tool_exec(&reg, "tool_create",
			    create_args("bad_require", source).c_str(),
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("forbidden token"),
		  std::string::npos);
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("\"value\":42"),
		  std::string::npos);
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
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find("capability denied"),
		  std::string::npos);
	tool_result_cleanup(&result);
}
