#include <gtest/gtest.h>
#include "mcp/mcp.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <string.h>

extern "C" {
	char *mcp_build_request(int id, const char *method, const char *params_json);
	int mcp_parse_result(const char *resp_json, char **out_result);
}

/* ----- Registry tests ----- */

class McpRegistryTest : public ::testing::Test {
protected:
	struct mcp_registry reg;

	void SetUp() override {
		mcp_registry_init(&reg);
	}

	void TearDown() override {
		mcp_registry_cleanup(&reg);
	}

	struct mcp_server_config make_stdio_cfg(const char *name) {
		struct mcp_server_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		strncpy(cfg.name, name, MCP_NAME_MAX - 1);
		cfg.transport = MCP_TRANSPORT_STDIO;
		return cfg;
	}

	struct mcp_server_config make_http_cfg(const char *name, const char *url) {
		struct mcp_server_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		strncpy(cfg.name, name, MCP_NAME_MAX - 1);
		cfg.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
		strncpy(cfg.http_url, url, sizeof(cfg.http_url) - 1);
		return cfg;
	}
};

TEST_F(McpRegistryTest, InitEmpty) {
	EXPECT_EQ(reg.count, 0);
	EXPECT_EQ(mcp_registry_count(&reg), 0);
}

TEST_F(McpRegistryTest, AddOneServer) {
	struct mcp_server_config cfg = make_stdio_cfg("test-server");
	int rc = mcp_registry_add(&reg, &cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	EXPECT_EQ(mcp_registry_count(&reg), 1);
}

TEST_F(McpRegistryTest, AddMultipleServers) {
	struct mcp_server_config cfg1 = make_stdio_cfg("server-a");
	struct mcp_server_config cfg2 = make_stdio_cfg("server-b");
	struct mcp_server_config cfg3 = make_http_cfg("server-c", "http://localhost:3000/mcp");

	EXPECT_EQ(mcp_registry_add(&reg, &cfg1), 0);
	EXPECT_EQ(mcp_registry_add(&reg, &cfg2), 0);
	EXPECT_EQ(mcp_registry_add(&reg, &cfg3), 0);
	EXPECT_EQ(reg.count, 3);
}

TEST_F(McpRegistryTest, GetExistingServer) {
	struct mcp_server_config cfg = make_stdio_cfg("find-me");
	mcp_registry_add(&reg, &cfg);

	struct mcp_client *client = mcp_registry_get(&reg, "find-me");
	ASSERT_NE(client, nullptr);
	EXPECT_STREQ(client->config.name, "find-me");
	EXPECT_EQ(client->config.transport, MCP_TRANSPORT_STDIO);
	EXPECT_EQ(morph_strmap_get(&reg.by_name, "find-me"), client);
}

TEST_F(McpRegistryTest, GetNonexistentServer) {
	struct mcp_client *client = mcp_registry_get(&reg, "does-not-exist");
	EXPECT_EQ(client, nullptr);
}

TEST_F(McpRegistryTest, DuplicateNameRejected) {
	struct mcp_server_config cfg = make_stdio_cfg("dup");
	EXPECT_EQ(mcp_registry_add(&reg, &cfg), 0);
	EXPECT_EQ(mcp_registry_add(&reg, &cfg), -EEXIST);
	EXPECT_EQ(reg.count, 1);
}

TEST_F(McpRegistryTest, EmptyNameRejected) {
	struct mcp_server_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	EXPECT_EQ(mcp_registry_add(&reg, &cfg), -EINVAL);
	EXPECT_EQ(reg.count, 0);
}

TEST_F(McpRegistryTest, NullRegistry) {
	struct mcp_server_config cfg = make_stdio_cfg("x");
	EXPECT_EQ(mcp_registry_add(nullptr, &cfg), -EINVAL);
	EXPECT_EQ(mcp_registry_count(nullptr), 0);
	EXPECT_EQ(mcp_registry_get(nullptr, "x"), nullptr);
}

TEST_F(McpRegistryTest, NullConfig) {
	EXPECT_EQ(mcp_registry_add(&reg, nullptr), -EINVAL);
}

TEST_F(McpRegistryTest, NullName) {
	EXPECT_EQ(mcp_registry_get(&reg, nullptr), nullptr);
}

TEST_F(McpRegistryTest, CleanupNull) {
	mcp_registry_cleanup(nullptr);
}

TEST_F(McpRegistryTest, MaxServers) {
	struct mcp_server_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	for (int i = 0; i < MCP_MAX_SERVERS; i++) {
		snprintf(cfg.name, MCP_NAME_MAX, "srv-%d", i);
		EXPECT_EQ(mcp_registry_add(&reg, &cfg), 0);
	}
	snprintf(cfg.name, MCP_NAME_MAX, "overflow");
	EXPECT_EQ(mcp_registry_add(&reg, &cfg), -ENOSPC);
	EXPECT_EQ(reg.count, MCP_MAX_SERVERS);
}

	TEST_F(McpRegistryTest, ClientInitialState) {
		struct mcp_server_config cfg = make_stdio_cfg("check-state");
		mcp_registry_add(&reg, &cfg);
		struct mcp_client *client = mcp_registry_get(&reg, "check-state");
		ASSERT_NE(client, nullptr);

		EXPECT_EQ(client->connected, 0);
		EXPECT_EQ(client->next_req_id, 1);
		EXPECT_EQ(client->server_pid, -1);
		EXPECT_EQ(client->stdin_fd, -1);
		EXPECT_EQ(client->stdout_fd, -1);
		EXPECT_EQ(client->supports_tools, 0);
		EXPECT_EQ(client->supports_resources, 0);
		EXPECT_EQ(client->supports_prompts, 0);
	}

	TEST_F(McpRegistryTest, AutoConnectFieldsPreserved) {
		struct mcp_server_config cfg = make_stdio_cfg("auto-srv");
		cfg.auto_connect = 1;
		cfg.connect_timeout = 30;
		mcp_registry_add(&reg, &cfg);
		struct mcp_client *client = mcp_registry_get(&reg, "auto-srv");
		ASSERT_NE(client, nullptr);
		EXPECT_EQ(client->config.auto_connect, 1);
		EXPECT_EQ(client->config.connect_timeout, 30);
	}

	TEST_F(McpRegistryTest, AutoConnectDefaultsZero) {
		struct mcp_server_config cfg = make_stdio_cfg("lazy-srv");
		mcp_registry_add(&reg, &cfg);
		struct mcp_client *client = mcp_registry_get(&reg, "lazy-srv");
		ASSERT_NE(client, nullptr);
		EXPECT_EQ(client->config.auto_connect, 0);
		EXPECT_EQ(client->config.connect_timeout, 0);
	}

/* ----- JSON-RPC request building ----- */

class McpJsonRpcTest : public ::testing::Test {
protected:
	void TearDown() override {
	}
};

TEST_F(McpJsonRpcTest, BuildRequestNoParams) {
	char *req = mcp_build_request(1, "tools/list", nullptr);
	ASSERT_NE(req, nullptr);

	cJSON *obj = cJSON_Parse(req);
	ASSERT_NE(obj, nullptr);
	cJSON *v = cJSON_GetObjectItem(obj, "jsonrpc");
	ASSERT_NE(v, nullptr);
	EXPECT_STREQ(v->valuestring, "2.0");

	v = cJSON_GetObjectItem(obj, "id");
	ASSERT_NE(v, nullptr);
	EXPECT_EQ((int)v->valuedouble, 1);

	v = cJSON_GetObjectItem(obj, "method");
	ASSERT_NE(v, nullptr);
	EXPECT_STREQ(v->valuestring, "tools/list");

	v = cJSON_GetObjectItem(obj, "params");
	EXPECT_EQ(v, nullptr);

	cJSON_Delete(obj);
	free(req);
}

TEST_F(McpJsonRpcTest, BuildRequestWithParams) {
	char *req = mcp_build_request(42, "tools/call", "{\"name\":\"test\",\"arguments\":{}}");
	ASSERT_NE(req, nullptr);

	cJSON *obj = cJSON_Parse(req);
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ((int)cJSON_GetObjectItem(obj, "id")->valuedouble, 42);
	EXPECT_STREQ(cJSON_GetObjectItem(obj, "method")->valuestring, "tools/call");

	cJSON *params = cJSON_GetObjectItem(obj, "params");
	ASSERT_NE(params, nullptr);
	cJSON *name = cJSON_GetObjectItem(params, "name");
	ASSERT_NE(name, nullptr);
	EXPECT_STREQ(name->valuestring, "test");

	cJSON_Delete(obj);
	free(req);
}

TEST_F(McpJsonRpcTest, BuildRequestEmptyParams) {
	char *req = mcp_build_request(1, "ping", "");
	ASSERT_NE(req, nullptr);

	cJSON *obj = cJSON_Parse(req);
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(obj, "params"), nullptr);

	cJSON_Delete(obj);
	free(req);
}

TEST_F(McpJsonRpcTest, BuildRequestIdZero) {
	char *req = mcp_build_request(0, "initialize", "{}");
	ASSERT_NE(req, nullptr);

	cJSON *obj = cJSON_Parse(req);
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ((int)cJSON_GetObjectItem(obj, "id")->valuedouble, 0);

	cJSON_Delete(obj);
	free(req);
}

/* ----- JSON-RPC result parsing ----- */

TEST_F(McpJsonRpcTest, ParseResultSuccess) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[]}}";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	EXPECT_NE(strstr(result, "tools"), nullptr);
	free(result);
}

TEST_F(McpJsonRpcTest, ParseResultError) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_NE(rc, 0);
	EXPECT_EQ(rc, -32600);
	free(result);
}

TEST_F(McpJsonRpcTest, ParseResultErrorWithoutCode) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"message\":\"unknown\"}}";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_EQ(rc, -1);
	free(result);
}

TEST_F(McpJsonRpcTest, ParseResultInvalidJson) {
	const char *resp = "not json at all";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_EQ(rc, MORPH_ERR_PARSE);
}

TEST_F(McpJsonRpcTest, ParseResultNullOut) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
	int rc = mcp_parse_result(resp, nullptr);
	EXPECT_EQ(rc, 0);
}

TEST_F(McpJsonRpcTest, ParseResultNullInput) {
	char *result = nullptr;
	int rc = mcp_parse_result(nullptr, &result);
	EXPECT_EQ(rc, MORPH_ERR_PARSE);
}

/* ----- Null parameter validation ----- */

class McpNullParamTest : public ::testing::Test {
protected:
	struct mcp_client client;

	void SetUp() override {
		memset(&client, 0, sizeof(client));
		client.server_pid = -1;
		client.stdin_fd = -1;
		client.stdout_fd = -1;
		pthread_mutex_init(&client.lock, nullptr);
	}

	void TearDown() override {
		pthread_mutex_destroy(&client.lock);
	}
};

TEST_F(McpNullParamTest, ListToolsNullClient) {
	struct mcp_tool_desc *tools = nullptr;
	int count = 0;
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_list_tools(nullptr, arena, &tools, &count), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListToolsNullArena) {
	struct mcp_tool_desc *tools = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_tools(&client, nullptr, &tools, &count), -EINVAL);
}

TEST_F(McpNullParamTest, ListToolsNullOutTools) {
	int count = 0;
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_list_tools(&client, arena, nullptr, &count), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListToolsNullOutCount) {
	struct mcp_tool_desc *tools = nullptr;
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_list_tools(&client, arena, &tools, nullptr), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, CallToolNullClient) {
	struct arena *arena = arena_create(0);
	char *result = nullptr;
	EXPECT_EQ(mcp_call_tool(nullptr, arena, "tool", "{}", &result), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, CallToolNullName) {
	struct arena *arena = arena_create(0);
	char *result = nullptr;
	EXPECT_EQ(mcp_call_tool(&client, arena, nullptr, "{}", &result), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, CallToolNullOut) {
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_call_tool(&client, arena, "tool", "{}", nullptr), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListResourcesNullClient) {
	struct mcp_resource_desc *res = nullptr;
	int count = 0;
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_list_resources(nullptr, arena, &res, &count), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListResourcesNullArena) {
	struct mcp_resource_desc *res = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_resources(&client, nullptr, &res, &count), -EINVAL);
}

TEST_F(McpNullParamTest, ReadResourceNullClient) {
	struct arena *arena = arena_create(0);
	char *content = nullptr;
	EXPECT_EQ(mcp_read_resource(nullptr, arena, "uri://x", &content), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ReadResourceNullUri) {
	struct arena *arena = arena_create(0);
	char *content = nullptr;
	EXPECT_EQ(mcp_read_resource(&client, arena, nullptr, &content), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ReadResourceNullOut) {
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_read_resource(&client, arena, "uri://x", nullptr), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListPromptsNullClient) {
	struct mcp_prompt_desc *prompts = nullptr;
	int count = 0;
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_list_prompts(nullptr, arena, &prompts, &count), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, ListPromptsNullArena) {
	struct mcp_prompt_desc *prompts = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_prompts(&client, nullptr, &prompts, &count), -EINVAL);
}

TEST_F(McpNullParamTest, GetPromptNullClient) {
	struct arena *arena = arena_create(0);
	char *result = nullptr;
	EXPECT_EQ(mcp_get_prompt(nullptr, arena, "prompt", "{}", &result), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, GetPromptNullName) {
	struct arena *arena = arena_create(0);
	char *result = nullptr;
	EXPECT_EQ(mcp_get_prompt(&client, arena, nullptr, "{}", &result), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, GetPromptNullOut) {
	struct arena *arena = arena_create(0);
	EXPECT_EQ(mcp_get_prompt(&client, arena, "prompt", "{}", nullptr), -EINVAL);
	arena_destroy(arena);
}

TEST_F(McpNullParamTest, PingNullClient) {
	EXPECT_EQ(mcp_ping(nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, InitializeNullClient) {
	EXPECT_EQ(mcp_initialize(nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, DisconnectNullClient) {
	mcp_disconnect(nullptr);
}

TEST_F(McpNullParamTest, EnsureConnectedNullClient) {
	EXPECT_EQ(mcp_ensure_connected(nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, RegisterServerToolsNullClient) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	EXPECT_EQ(mcp_register_server_tools(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerToolsNullReg) {
	EXPECT_EQ(mcp_register_server_tools(&client, nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, RegisterServerResourcesNullClient) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	EXPECT_EQ(mcp_register_server_resources(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerResourcesNullReg) {
	EXPECT_EQ(mcp_register_server_resources(&client, nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, RegisterServerPromptsNullClient) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	EXPECT_EQ(mcp_register_server_prompts(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerPromptsNullReg) {
	EXPECT_EQ(mcp_register_server_prompts(&client, nullptr), -EINVAL);
}

/* ----- Struct size / layout checks ----- */

class McpStructTest : public ::testing::Test {};

TEST_F(McpStructTest, ToolDescSize) {
	struct mcp_tool_desc desc;
	EXPECT_GE(sizeof(desc), (size_t)(MCP_NAME_MAX + MCP_NAME_MAX + MCP_DESC_MAX + MCP_SCHEMA_MAX));
}

TEST_F(McpStructTest, ResourceDescSize) {
	struct mcp_resource_desc desc;
	EXPECT_GE(sizeof(desc), (size_t)(MCP_URI_MAX + MCP_NAME_MAX + MCP_DESC_MAX + 64));
}

TEST_F(McpStructTest, PromptDescSize) {
	struct mcp_prompt_desc desc;
	EXPECT_GE(sizeof(desc), (size_t)(MCP_NAME_MAX + MCP_DESC_MAX + MCP_SCHEMA_MAX));
}

TEST_F(McpStructTest, ServerConfigSize) {
	struct mcp_server_config cfg;
	EXPECT_GT(sizeof(cfg), (size_t)0);
}

TEST_F(McpStructTest, RegistryMaxServers) {
	struct mcp_registry reg;
	EXPECT_GE(MCP_MAX_SERVERS, 1);
	EXPECT_LE(MCP_MAX_SERVERS, 1024);
}

TEST_F(McpStructTest, ProtocolVersionDefined) {
	EXPECT_STREQ(MCP_PROTOCOL_VERSION, "2025-06-18");
}

TEST_F(McpStructTest, TransportEnumValues) {
	EXPECT_EQ(MCP_TRANSPORT_STDIO, 0);
	EXPECT_EQ(MCP_TRANSPORT_STREAMABLE_HTTP, 1);
}

/* ----- Arena lifecycle with MCP operations ----- */

class McpArenaTest : public ::testing::Test {
protected:
	struct arena *arena;

	void SetUp() override {
		arena = arena_create(4096);
	}

	void TearDown() override {
		arena_destroy(arena);
	}
};

TEST_F(McpArenaTest, ArenaAllocForDescriptorArray) {
	void *p = arena_alloc(arena, 10 * sizeof(struct mcp_tool_desc));
	ASSERT_NE(p, nullptr);

	struct mcp_tool_desc *tools = static_cast<struct mcp_tool_desc *>(p);
	tools[0].name[0] = 't';
	tools[0].name[1] = '\0';
	EXPECT_STREQ(tools[0].name, "t");
}

TEST_F(McpArenaTest, ArenaStrdupForResult) {
	const char *src = "{\"content\":\"hello\",\"isError\":false}";
	char *dup = arena_strdup(arena, src);
	ASSERT_NE(dup, nullptr);
	EXPECT_STREQ(dup, src);
}

TEST_F(McpArenaTest, ArenaResetReusesBuffer) {
	arena_alloc(arena, 1024);
	arena_alloc(arena, 512);
	size_t used_before = arena->used;
	EXPECT_GT(used_before, 0);

	arena_reset(arena);
	EXPECT_EQ(arena->used, 0);
	EXPECT_EQ(arena->next, nullptr);

	void *p = arena_alloc(arena, 64);
	ASSERT_NE(p, nullptr);
}

TEST_F(McpArenaTest, ArenaAllocAfterListSimulated) {
	struct mcp_tool_desc *tools =
		static_cast<struct mcp_tool_desc *>(arena_alloc(arena, 3 * sizeof(struct mcp_tool_desc)));
	ASSERT_NE(tools, nullptr);

	strncpy(tools[0].name, "tool_a", MCP_NAME_MAX - 1);
	strncpy(tools[1].name, "tool_b", MCP_NAME_MAX - 1);
	strncpy(tools[2].name, "tool_c", MCP_NAME_MAX - 1);

	char *result = arena_strdup(arena, "{\"status\":\"ok\"}");
	ASSERT_NE(result, nullptr);

	EXPECT_STREQ(tools[0].name, "tool_a");
	EXPECT_STREQ(tools[1].name, "tool_b");
	EXPECT_STREQ(tools[2].name, "tool_c");
	EXPECT_STREQ(result, "{\"status\":\"ok\"}");
}

/* ----- mcp_strdup_result behavior ----- */

class McpStrdupTest : public ::testing::Test {};

TEST_F(McpStrdupTest, StrdupResultNullInput) {
	EXPECT_EQ(mcp_parse_result(nullptr, nullptr), MORPH_ERR_PARSE);
}
