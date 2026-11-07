#include <gtest/gtest.h>
#include "mcp/mcp_internal.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Small ownership wrappers around the production codec for existing fixtures. */
static char *mcp_build_request(int id, const char *method, const char *params)
{
	struct mcp_client client = {};
	struct mcp_wire_request wire = {};
	int rc = mcp_protocol_encode(&client, mcp_protocol_borrow("2025-06-18"), id, method, params,
				     &wire);
	char *json = rc == 0 ? strdup(wire.json) : nullptr;
	mcp_wire_request_cleanup(&wire);
	return json;
}

static int mcp_parse_result(const char *json, char **out)
{
	cJSON *parsed = json ? cJSON_Parse(json) : nullptr;
	cJSON *id = cJSON_GetObjectItem(parsed, "id");
	int request_id = cJSON_IsNumber(id) ? id->valueint : 1;
	cJSON_Delete(parsed);
	struct mcp_wire_reply reply = {};
	reply.json = const_cast<char *>(json);
	cJSON *object = nullptr;
	int rc = mcp_protocol_decode(nullptr, &reply, request_id, &object);
	if (rc == 0 && out) {
		char *text MORPH_CLEANUP(morph_json_text_cleanup) =
			morph_json_print_copy(cJSON_GetObjectItem(object, "result"));
		*out = text ? strdup(text) : nullptr;
		if (!*out)
			rc = -ENOMEM;
	}
	cJSON_Delete(object);
	return rc;
}

/* ----- Registry tests ----- */

class McpRegistryTest : public ::testing::Test {
protected:
	struct mcp_registry reg;

	void SetUp() override {
		mcp_registry_init(NULL, &reg);
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

	struct mcp_client *client = mcp_registry_borrow(&reg, "find-me");
	ASSERT_NE(client, nullptr);
	EXPECT_STREQ(client->config.name, "find-me");
	EXPECT_EQ(client->config.transport, MCP_TRANSPORT_STDIO);
	EXPECT_EQ(morph_strmap_borrow(&reg.by_name, "find-me"), client);
}

TEST_F(McpRegistryTest, GetNonexistentServer) {
	struct mcp_client *client = mcp_registry_borrow(&reg, "does-not-exist");
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
	EXPECT_EQ(mcp_registry_borrow(nullptr, "x"), nullptr);
}

TEST_F(McpRegistryTest, NullConfig) {
	EXPECT_EQ(mcp_registry_add(&reg, nullptr), -EINVAL);
}

TEST_F(McpRegistryTest, NullName) {
	EXPECT_EQ(mcp_registry_borrow(&reg, nullptr), nullptr);
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
		struct mcp_client *client = mcp_registry_borrow(&reg, "check-state");
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
		struct mcp_client *client = mcp_registry_borrow(&reg, "auto-srv");
		ASSERT_NE(client, nullptr);
		EXPECT_EQ(client->config.auto_connect, 1);
		EXPECT_EQ(client->config.connect_timeout, 30);
	}

	TEST_F(McpRegistryTest, AutoConnectDefaultsZero) {
		struct mcp_server_config cfg = make_stdio_cfg("lazy-srv");
		mcp_registry_add(&reg, &cfg);
		struct mcp_client *client = mcp_registry_borrow(&reg, "lazy-srv");
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
	EXPECT_EQ(rc, MORPH_ERR_PROTOCOL);
	free(result);
}

TEST_F(McpJsonRpcTest, ParseResultErrorWithoutCode) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"message\":\"unknown\"}}";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_EQ(rc, MORPH_ERR_PROTOCOL);
	free(result);
}

TEST_F(McpJsonRpcTest, ParseResultInvalidJson) {
	const char *resp = "not json at all";
	char *result = nullptr;
	int rc = mcp_parse_result(resp, &result);
	EXPECT_EQ(rc, MORPH_ERR_PARSE);
	free(result);
}

TEST_F(McpJsonRpcTest, RejectsNonObjectResult) {
	const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
	int rc = mcp_parse_result(resp, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_PROTOCOL);
}

TEST_F(McpJsonRpcTest, ParseResultNullInput) {
	char *result = nullptr;
	int rc = mcp_parse_result(nullptr, &result);
	EXPECT_EQ(rc, MORPH_ERR_PARSE);
	free(result);
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
	EXPECT_EQ(mcp_list_tools_copy(nullptr, &tools, &count), -EINVAL);
}

TEST_F(McpNullParamTest, ListToolsNullOutTools) {
	int count = 0;
	EXPECT_EQ(mcp_list_tools_copy(&client, nullptr, &count), -EINVAL);
}

TEST_F(McpNullParamTest, ListToolsNullOutCount) {
	struct mcp_tool_desc *tools = nullptr;
	EXPECT_EQ(mcp_list_tools_copy(&client, &tools, nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, CallToolNullClient) {
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(mcp_call_tool(nullptr, "tool", "{}", &result), -EINVAL);
}

TEST_F(McpNullParamTest, CallToolNullName) {
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(mcp_call_tool(&client, nullptr, "{}", &result), -EINVAL);
}

TEST_F(McpNullParamTest, CallToolNullOut) {
	EXPECT_EQ(mcp_call_tool(&client, "tool", "{}", nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, ListResourcesNullClient) {
	struct mcp_resource_desc *res = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_resources_copy(nullptr, &res, &count), -EINVAL);
}

TEST_F(McpNullParamTest, ReadResourceNullClient) {
	struct tool_result content;
	tool_result_init(&content);
	EXPECT_EQ(mcp_read_resource(nullptr, "uri://x", &content), -EINVAL);
	tool_result_cleanup(&content);
}

TEST_F(McpNullParamTest, ReadResourceNullUri) {
	struct tool_result content;
	tool_result_init(&content);
	EXPECT_EQ(mcp_read_resource(&client, nullptr, &content), -EINVAL);
	tool_result_cleanup(&content);
}

TEST_F(McpNullParamTest, ReadResourceNullOut) {
	EXPECT_EQ(mcp_read_resource(&client, "uri://x", nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, ListPromptsNullClient) {
	struct mcp_prompt_desc *prompts = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_prompts_copy(nullptr, &prompts, &count), -EINVAL);
}

TEST_F(McpNullParamTest, GetPromptNullClient) {
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(mcp_get_prompt(nullptr, "prompt", "{}", &result), -EINVAL);
}

TEST_F(McpNullParamTest, GetPromptNullName) {
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(mcp_get_prompt(&client, nullptr, "{}", &result), -EINVAL);
}

TEST_F(McpNullParamTest, GetPromptNullOut) {
	EXPECT_EQ(mcp_get_prompt(&client, "prompt", "{}", nullptr), -EINVAL);
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
	tool_registry_init(NULL, &reg);
	EXPECT_EQ(mcp_register_server_tools(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerToolsNullReg) {
	EXPECT_EQ(mcp_register_server_tools(&client, nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, RegisterServerResourcesNullClient) {
	struct tool_registry reg;
	tool_registry_init(NULL, &reg);
	EXPECT_EQ(mcp_register_server_resources(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerResourcesNullReg) {
	EXPECT_EQ(mcp_register_server_resources(&client, nullptr), -EINVAL);
}

TEST_F(McpNullParamTest, RegisterServerPromptsNullClient) {
	struct tool_registry reg;
	tool_registry_init(NULL, &reg);
	EXPECT_EQ(mcp_register_server_prompts(nullptr, &reg), -EINVAL);
	tool_registry_cleanup(&reg);
}

TEST_F(McpNullParamTest, RegisterServerPromptsNullReg) {
	EXPECT_EQ(mcp_register_server_prompts(&client, nullptr), -EINVAL);
}

/* ----- Struct size / layout checks ----- */

class McpStructTest : public ::testing::Test {};

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
	EXPECT_STREQ(MCP_PROTOCOL_VERSION, "2026-07-28");
}

TEST_F(McpStructTest, TransportEnumValues) {
	EXPECT_EQ(MCP_TRANSPORT_STDIO, 0);
	EXPECT_EQ(MCP_TRANSPORT_STREAMABLE_HTTP, 1);
}

/* Descriptor cleanup supports partially populated discovery results. */

TEST(McpOwnedDescriptorsTest, ToolDescriptorsReleaseAllIndependentFields)
{
	struct mcp_tool_desc *tools =
		static_cast<struct mcp_tool_desc *>(calloc(3, sizeof(*tools)));
	ASSERT_NE(tools, nullptr);
	tools[0].name = strdup("tool_a");
	tools[0].title = strdup("Tool A");
	tools[0].description = strdup("description");
	tools[0].input_schema = strdup("{}");
	tools[1].name = strdup("tool_b");
	EXPECT_STREQ(tools[0].name, "tool_a");
	mcp_tool_descs_free(tools, 3);
	mcp_tool_descs_free(nullptr, 0);
}

TEST(McpOwnedDescriptorsTest, ResourcesAndPromptsReleasePartialResults)
{
	struct mcp_resource_desc *resources =
		static_cast<struct mcp_resource_desc *>(calloc(2, sizeof(*resources)));
	struct mcp_prompt_desc *prompts =
		static_cast<struct mcp_prompt_desc *>(calloc(2, sizeof(*prompts)));
	ASSERT_NE(resources, nullptr);
	ASSERT_NE(prompts, nullptr);
	resources[0].uri = strdup("resource://owned");
	resources[0].name = strdup("resource");
	resources[0].description = strdup("description");
	resources[0].mime_type = strdup("text/plain");
	prompts[0].name = strdup("prompt");
	prompts[0].description = strdup("description");
	prompts[0].arguments_schema = strdup("{}");
	mcp_resource_descs_free(resources, 2);
	mcp_prompt_descs_free(prompts, 2);
	mcp_resource_descs_free(nullptr, 0);
	mcp_prompt_descs_free(nullptr, 0);
}

/* ----- mcp_strdup_result behavior ----- */

class McpStrdupTest : public ::testing::Test {};

TEST_F(McpStrdupTest, StrdupResultNullInput) {
	EXPECT_EQ(mcp_parse_result(nullptr, nullptr), MORPH_ERR_PARSE);
}

/* ----- SSE JSON extraction from MCP HTTP responses ----- */

class McpSseExtractTest : public ::testing::Test {};

TEST_F(McpSseExtractTest, BareJsonObjectReturnsNull) {
	const char *raw = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\"}}";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	EXPECT_EQ(json, nullptr) << "Plain JSON (no SSE envelope) returns NULL; caller falls back to raw";
}

TEST_F(McpSseExtractTest, SseWithEventMessage) {
	const char *raw = "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\"}}\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	ASSERT_NE(json, nullptr) << "SSE with 'event: message' must extract data";
	EXPECT_NE(strstr(json, "protocolVersion"), nullptr);
	free(json);
}

TEST_F(McpSseExtractTest, SseDataOnlyNoEvent) {
	const char *raw = "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[]}}\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	ASSERT_NE(json, nullptr);
	EXPECT_NE(strstr(json, "tools"), nullptr);
	free(json);
}

TEST_F(McpSseExtractTest, SseMultilineData) {
	const char *raw = "data: {\"jsonrpc\":\"2.0\",\r\ndata: \"id\":1}\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	ASSERT_NE(json, nullptr);
	EXPECT_NE(strstr(json, "jsonrpc"), nullptr);
	free(json);
}

TEST_F(McpSseExtractTest, SseWithCustomEvent) {
	const char *raw = "event: ping\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":1}\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	ASSERT_NE(json, nullptr) << "Custom event types must also extract data";
	EXPECT_NE(strstr(json, "jsonrpc"), nullptr);
	free(json);
}

TEST_F(McpSseExtractTest, NullInput) {
	char *json = mcp_http_sse_json_create(nullptr, 0);
	EXPECT_EQ(json, nullptr);
}

TEST_F(McpSseExtractTest, EmptyInput) {
	char *json = mcp_http_sse_json_create("", 0);
	EXPECT_EQ(json, nullptr);
}

TEST_F(McpSseExtractTest, SseEventOnlyNoData) {
	const char *raw = "event: message\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	EXPECT_EQ(json, nullptr) << "No data line means no extraction";
}

TEST_F(McpSseExtractTest, ParseFullMcpInitializeResponse) {
	const char *raw = "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{\"listChanged\":true},\"prompts\":{\"listChanged\":true},\"resources\":{\"listChanged\":true}},\"serverInfo\":{\"name\":\"exa-search-server\",\"version\":\"3.2.1\"}}}\r\n\r\n";
	char *json = mcp_http_sse_json_create(raw, strlen(raw));
	ASSERT_NE(json, nullptr);
	char *result = nullptr;
	int rc = mcp_parse_result(json, &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	cJSON *obj = cJSON_Parse(result);
	ASSERT_NE(obj, nullptr);
	cJSON *pv = cJSON_GetObjectItem(obj, "protocolVersion");
	ASSERT_NE(pv, nullptr);
	EXPECT_STREQ(pv->valuestring, "2025-06-18");
	cJSON_Delete(obj);
	free(result);
	free(json);
}

/* ----- MCP HTTP auth token URL substitution ----- */

class McpHttpAuthUrlTest : public ::testing::Test {
protected:
	void TearDown() override {
		unsetenv("MORPH_TEST_MCP_TOKEN");
	}
};

TEST_F(McpHttpAuthUrlTest, DetectsAuthTokenPlaceholder) {
	EXPECT_EQ(mcp_http_url_uses_auth_token("https://x/mcp"), 0);
	EXPECT_NE(mcp_http_url_uses_auth_token(
		"https://x/mcp?api_key=${auth_token}"), 0);
}

TEST_F(McpHttpAuthUrlTest, BuildsUrlWithEncodedAuthToken) {
	struct mcp_server_config cfg;
	char *url = nullptr;

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.http_url,
		"https://api.example.com/mcp?key=${auth_token}&mode=test",
		sizeof(cfg.http_url) - 1);
	strncpy(cfg.http_auth_token_env, "MORPH_TEST_MCP_TOKEN",
		sizeof(cfg.http_auth_token_env) - 1);
	setenv("MORPH_TEST_MCP_TOKEN", "a b+c/=:?", 1);

	int rc = mcp_http_request_url_create(&cfg, &url);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(url, nullptr);
	EXPECT_STREQ(url,
		     "https://api.example.com/mcp?key=a%20b%2Bc%2F%3D%3A%3F&mode=test");
	free(url);
}

TEST_F(McpHttpAuthUrlTest, BuildsPlainUrlWithoutTokenEnv) {
	struct mcp_server_config cfg;
	char *url = nullptr;

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.http_url, "https://api.example.com/mcp",
		sizeof(cfg.http_url) - 1);

	int rc = mcp_http_request_url_create(&cfg, &url);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(url, nullptr);
	EXPECT_STREQ(url, "https://api.example.com/mcp");
	free(url);
}

TEST_F(McpHttpAuthUrlTest, MissingTokenEnvFailsWhenPlaceholderIsUsed) {
	struct mcp_server_config cfg;
	char *url = nullptr;

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.http_url, "https://api.example.com/mcp?key=${auth_token}",
		sizeof(cfg.http_url) - 1);
	strncpy(cfg.http_auth_token_env, "MORPH_TEST_MCP_TOKEN",
		sizeof(cfg.http_auth_token_env) - 1);

	int rc = mcp_http_request_url_create(&cfg, &url);
	EXPECT_EQ(rc, MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(url, nullptr);
}

/* Exercise framing independently of a particular MCP server implementation. */
#include <thread>
#include <chrono>
#include <string>
#include <unistd.h>

class McpStdioFramingTest : public ::testing::Test {
protected:
	struct mcp_client client = {};
	int request_read = -1;
	int response_write = -1;

	void SetUp() override
	{
		int requests[2], responses[2];
		ASSERT_EQ(pipe(requests), 0);
		ASSERT_EQ(pipe(responses), 0);
		client.stdin_fd = requests[1];
		request_read = requests[0];
		client.stdout_fd = responses[0];
		response_write = responses[1];
		client.server_pid = -1;
		client.next_req_id = 1;
		client.protocol = mcp_protocol_borrow("2025-06-18");
		pthread_mutex_init(&client.lock, nullptr);
	}

	void TearDown() override
	{
		mcp_disconnect(&client);
		close(request_read);
		close(response_write);
		pthread_mutex_destroy(&client.lock);
	}

	void send(const std::string &message)
	{
		size_t sent = 0;
		while (sent < message.size()) {
			ssize_t count = write(response_write, message.data() + sent, message.size() - sent);
			if (count <= 0)
				return;
			sent += static_cast<size_t>(count);
		}
	}
};

TEST_F(McpStdioFramingTest, ReadsResponsesLargerThanOldFixedBuffer)
{
	std::string padding(300000, 'x');
	std::thread producer([&] {
		send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"padding\":\"" + padding + "\"}}\n");
	});
	char *response = nullptr;
	int rc = mcp_request(&client, "tools/list", nullptr, &response);
	producer.join();
	EXPECT_EQ(rc, 0);
	ASSERT_NE(response, nullptr);
	EXPECT_GT(strlen(response), padding.size());
	free(response);
}

TEST_F(McpStdioFramingTest, SkipsNotificationsAndPreservesFollowingBufferedResponse)
{
	send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"first\":true}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"second\":true}}\n");
	char *response = nullptr;
	EXPECT_EQ(mcp_request(&client, "tools/list", nullptr, &response), 0);
	ASSERT_NE(response, nullptr);
	EXPECT_NE(strstr(response, "first"), nullptr);
	free(response);
	response = nullptr;
	EXPECT_EQ(mcp_request(&client, "prompts/list", nullptr, &response), 0);
	ASSERT_NE(response, nullptr);
	EXPECT_NE(strstr(response, "second"), nullptr);
	free(response);
}

TEST_F(McpStdioFramingTest, NotificationsDoNotResetRequestDeadline)
{
	client.config.connect_timeout = 1;
	client.connecting = 1;
	std::thread producer([&] {
		for (int i = 0; i < 24; i++) {
			send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n");
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	});
	char *response = nullptr;
	auto start = std::chrono::steady_clock::now();
	int rc = mcp_request(&client, "initialize", nullptr, &response);
	auto elapsed = std::chrono::steady_clock::now() - start;
	producer.join();
	EXPECT_EQ(rc, -ETIMEDOUT);
	EXPECT_EQ(response, nullptr);
	EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
}

TEST_F(McpStdioFramingTest, DeadServerReturnsPipeErrorWithoutSignallingHost)
{
	close(request_read);
	request_read = -1;
	char *response = nullptr;
	EXPECT_EQ(mcp_request(&client, "initialize", nullptr, &response), -EPIPE);
	EXPECT_EQ(response, nullptr);
}

TEST_F(McpStdioFramingTest, DiscoveryPreservesLargeSchemasAndDescriptions)
{
	client.connected = 1;
	client.supports_tools = 1;
	client.next_req_id = 1;
	strcpy(client.config.name, "peekaboo");
	std::string description(24000, 'd');
	std::string schema = "{\"type\":\"object\",\"description\":\"" +
		std::string(100000, 's') + "\"}";
	std::thread producer([&] {
		send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[{\"name\":\"verify_state\","
			"\"description\":\"" + description + "\",\"inputSchema\":" + schema +
			"}]}}\n");
	});
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	int rc = mcp_register_server_tools(&client, &registry);
	producer.join();
	EXPECT_EQ(rc, 1);
	auto *entry = tool_borrow(&registry, "mcp_peekaboo__verify_state");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(std::string(entry->desc.input_schema), schema);
	EXPECT_EQ(std::string(entry->desc.description), description);
	tool_registry_cleanup(&registry);
}

TEST_F(McpStdioFramingTest, DiscoveryFollowsPagesAndExceedsOldToolCount)
{
	client.connected = 1;
	client.supports_tools = 1;
	client.next_req_id = 1;
	strcpy(client.config.name, "many");
	std::thread producer([&] {
		for (int page = 0; page < 2; page++) {
			std::string response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(page + 1) +
				",\"result\":{\"tools\":[";
			for (int i = 0; i < 100; i++) {
				if (i) response += ",";
				response += "{\"name\":\"tool_" + std::to_string(page * 100 + i) +
					"\",\"inputSchema\":{\"type\":\"object\"}}";
			}
			response += page == 0 ? "] ,\"nextCursor\":\"page2\"}}\n" : "]}}\n";
			send(response);
		}
	});
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	int rc = mcp_register_server_tools(&client, &registry);
	producer.join();
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(registry.count, 200);
	EXPECT_NE(tool_borrow(&registry, "mcp_many__tool_199"), nullptr);
	tool_registry_cleanup(&registry);
}

TEST_F(McpStdioFramingTest, RejectsCyclicPaginationWithoutPartialResults)
{
	client.next_req_id = 1;
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[],\"nextCursor\":\"same\"}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[],\"nextCursor\":\"same\"}}\n");
	struct mcp_tool_desc *entries = nullptr;
	int count = 0;
	EXPECT_EQ(mcp_list_tools_copy(&client, &entries, &count), MORPH_ERR_PROTOCOL);
	EXPECT_EQ(entries, nullptr);
	EXPECT_EQ(count, 0);
}

TEST_F(McpStdioFramingTest, LargeArgumentsAndEscapedNameAreSentIntact)
{
	client.next_req_id = 1;
	std::string payload(200000, 'a');
	std::string args = "{\"payload\":\"" + payload + "\"}";
	std::string request;
	std::thread server([&] {
		char ch;
		while (read(request_read, &ch, 1) == 1 && ch != '\n')
			request += ch;
		send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}\n");
	});
	struct tool_result result;
	tool_result_init(&result);
	int rc = mcp_call_tool(&client, "a\"b\\c", args.c_str(), &result);
	server.join();
	EXPECT_EQ(rc, 0);
	cJSON *json = cJSON_Parse(request.c_str());
	ASSERT_NE(json, nullptr);
	cJSON *params = cJSON_GetObjectItem(json, "params");
	EXPECT_STREQ(cJSON_GetObjectItem(params, "name")->valuestring, "a\"b\\c");
	cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
	EXPECT_EQ(std::string(cJSON_GetObjectItem(arguments, "payload")->valuestring), payload);
	cJSON_Delete(json);
	tool_result_cleanup(&result);
}

TEST_F(McpStdioFramingTest, ResourceContentIsNotTruncated)
{
	client.next_req_id = 1;
	std::string text(200000, 'r');
	std::thread server([&] {
		send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"contents\":[{\"text\":\"" + text + "\"},"
			"{\"text\":\"tail\"}]}}\n");
	});
	struct tool_result result;
	tool_result_init(&result);
	int rc = mcp_read_resource(&client, "test://resource", &result);
	server.join();
	EXPECT_EQ(rc, 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(result.data, "text")->valuestring,
		(text + "\ntail").c_str());
	tool_result_cleanup(&result);
}

TEST_F(McpStdioFramingTest, LongPromptSchemaAndResourceMetadataSurviveDiscovery)
{
	client.next_req_id = 1;
	std::string description(20000, 'd');
	std::string uri = "test://" + std::string(10000, 'u');
	std::thread server([&] {
		send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"prompts\":[{\"name\":\"review\","
			"\"arguments\":[{\"name\":\"subject\",\"description\":\"" + description +
			"\",\"required\":true}]}],\"nextCursor\":\"next\"}}\n");
		send("{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"prompts\":[{\"name\":\"second\"}]}}\n");
		send("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"resources\":[{\"uri\":\"" + uri +
			"\",\"name\":\"resource\",\"description\":\"" + description + "\"}]}}\n");
	});
	struct mcp_prompt_desc *prompts = nullptr;
	int count = 0;
	int prompt_rc = mcp_list_prompts_copy(&client, &prompts, &count);
	struct mcp_resource_desc *resources = nullptr;
	int resource_count = 0;
	int resource_rc = mcp_list_resources_copy(&client, &resources, &resource_count);
	server.join();
	EXPECT_EQ(prompt_rc, 0);
	EXPECT_EQ(count, 2);
	if (prompts) {
		cJSON *schema = cJSON_Parse(prompts[0].arguments_schema);
		cJSON *property = cJSON_GetObjectItem(cJSON_GetObjectItem(schema, "properties"), "subject");
		ASSERT_NE(property, nullptr);
		EXPECT_EQ(std::string(cJSON_GetObjectItem(property, "description")->valuestring), description);
		cJSON_Delete(schema);
	}
	EXPECT_EQ(resource_rc, 0);
	EXPECT_EQ(resource_count, 1);
	if (resources) {
		EXPECT_EQ(std::string(resources[0].uri), uri);
		EXPECT_EQ(std::string(resources[0].description), description);
	}
	mcp_prompt_descs_free(prompts, count);
	mcp_resource_descs_free(resources, resource_count);
}

TEST_F(McpStdioFramingTest, PreservesStructuredAndNonTextToolResults)
{
	client.next_req_id = 1;
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[],\"structuredContent\":{\"count\":42}}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hello\"},"
		"{\"type\":\"audio\",\"data\":\"YQ==\",\"mimeType\":\"audio/wav\"}],"
		"\"structuredContent\":{\"count\":43},\"_meta\":{\"extra\":true}}}\n");
	struct tool_result result;
	tool_result_init(&result);
	EXPECT_EQ(mcp_call_tool(&client, "empty", "{}", &result), 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	ASSERT_NE(result.data, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(
		cJSON_GetObjectItem(result.data, "structuredContent"), "count")->valueint, 42);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(result.data, "content")), 0);
	tool_result_clear(&result);
	EXPECT_EQ(mcp_call_tool(&client, "mixed", "{}", &result), 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	EXPECT_STREQ(cJSON_GetObjectItem(result.data, "content")->valuestring, "hello");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(result.data, "contentBlocks")), 2);
	EXPECT_NE(cJSON_GetObjectItem(result.data, "_meta"), nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(
		cJSON_GetObjectItem(result.data, "structuredContent"), "count")->valueint, 43);
	tool_result_cleanup(&result);
}

#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>

TEST(McpHttpPayloadTest, LongBearerTokenAndSessionHeaderAreNotTruncated)
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(listener, 0);
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), length), 0);
	ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length), 0);
	ASSERT_EQ(listen(listener, 1), 0);
	std::string request;
	std::thread server([&] {
		struct pollfd descriptor = {};
		descriptor.fd = listener;
		descriptor.events = POLLIN;
		if (poll(&descriptor, 1, 5000) <= 0)
			return;
		int connection = accept(listener, nullptr, nullptr);
		if (connection < 0)
			return;
		struct timeval timeout = {3, 0};
		setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		char bytes[BUFSIZ];
		while (request.find("\r\n\r\n") == std::string::npos) {
			ssize_t received = read(connection, bytes, sizeof(bytes));
			if (received <= 0)
				break;
			request.append(bytes, static_cast<size_t>(received));
		}
		const char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Connection: close\r\nContent-Length: 36\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
		write(connection, response, sizeof(response) - 1);
		close(connection);
	});
	std::string token(5000, 't');
	std::string session(5000, 's');
	setenv("MORPH_TEST_MCP_LONG_TOKEN", token.c_str(), 1);
	struct mcp_client client = {};
	client.config.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
	client.next_req_id = 1;
	pthread_mutex_init(&client.lock, nullptr);
	snprintf(client.config.http_url, sizeof(client.config.http_url),
		"http://127.0.0.1:%u/mcp", static_cast<unsigned>(ntohs(address.sin_port)));
	strcpy(client.config.http_auth_token_env, "MORPH_TEST_MCP_LONG_TOKEN");
	int init_rc = http_init(NULL);
	int connect_rc = init_rc == 0 ? mcp_connect_http(&client) : init_rc;
	client.session_id = strdup(session.c_str());
	client.protocol = mcp_protocol_borrow("2025-06-18");
	int rc = connect_rc == 0 ? mcp_ping(&client) : connect_rc;
	server.join();
	close(listener);
	EXPECT_EQ(rc, 0);
	EXPECT_NE(request.find("Authorization: Bearer " + token + "\r\n"), std::string::npos);
	EXPECT_NE(request.find("Mcp-Session-Id: " + session + "\r\n"), std::string::npos);
	mcp_disconnect(&client);
	pthread_mutex_destroy(&client.lock);
	http_cleanup();
	unsetenv("MORPH_TEST_MCP_LONG_TOKEN");
}

TEST_F(McpStdioFramingTest, MissingListHandlersDisableOnlyTheAffectedCapability)
{
	client.supports_tools = 1;
	client.supports_resources = 1;
	client.supports_prompts = 1;
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32601,"
		"\"message\":\"Unknown method: prompts/list\"}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32601,"
		"\"message\":\"Unknown method: resources/list\"}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32601,"
		"\"message\":\"Unknown method: tools/list\"}}\n");
	struct mcp_prompt_desc *prompts = nullptr;
	struct mcp_resource_desc *resources = nullptr;
	struct mcp_tool_desc *tools = nullptr;
	int count = -1;
	EXPECT_EQ(mcp_list_prompts_copy(&client, &prompts, &count), 0);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(client.supports_prompts, 0);
	EXPECT_EQ(client.supports_resources, 1);
	EXPECT_EQ(client.supports_tools, 1);
	EXPECT_EQ(mcp_list_resources_copy(&client, &resources, &count), 0);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(client.supports_resources, 0);
	EXPECT_EQ(client.supports_tools, 1);
	EXPECT_EQ(mcp_list_tools_copy(&client, &tools, &count), 0);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(client.supports_tools, 0);
}

TEST_F(McpStdioFramingTest, MidPaginationMethodNotFoundRemainsAnError)
{
	client.supports_prompts = 1;
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"prompts\":[{\"name\":\"first\"}],"
		"\"nextCursor\":\"next\"}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32601,"
		"\"message\":\"Missing handler\"}}\n");
	struct mcp_prompt_desc *prompts = nullptr;
	int count = -1;
	EXPECT_EQ(mcp_list_prompts_copy(&client, &prompts, &count), -ENOSYS);
	EXPECT_EQ(prompts, nullptr);
	EXPECT_EQ(count, 0);
	EXPECT_EQ(client.supports_prompts, 1);
}

TEST_F(McpStdioFramingTest, OtherRemoteErrorsAndMalformedResultsAreNotMissingCapabilities)
{
	client.supports_prompts = 1;
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,"
		"\"message\":\"Internal error\"}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32601}}\n");
	struct mcp_prompt_desc *prompts = nullptr;
	int count = -1;
	for (int i = 0; i < 3; i++) {
		EXPECT_EQ(mcp_list_prompts_copy(&client, &prompts, &count), MORPH_ERR_PROTOCOL);
		EXPECT_EQ(client.supports_prompts, 1);
		EXPECT_EQ(prompts, nullptr);
		EXPECT_EQ(count, 0);
	}
}

TEST_F(McpHttpAuthUrlTest, UrlCreationRejectsAnOccupiedOwner)
{
	struct mcp_server_config config = {};
	strcpy(config.http_url, "https://example.com/mcp");
	char *url MORPH_CLEANUP(morph_charp_cleanup) = strdup("original");
	ASSERT_NE(url, nullptr);
	char *original = url;
	EXPECT_EQ(mcp_http_request_url_create(&config, &url), -EEXIST);
	EXPECT_EQ(url, original);
	EXPECT_STREQ(url, "original");
}

TEST_F(McpStdioFramingTest, RequestRejectsAnOccupiedOwnerBeforeSending)
{
	char *response MORPH_CLEANUP(morph_charp_cleanup) = strdup("original");
	ASSERT_NE(response, nullptr);
	char *original = response;
	int request_id = client.next_req_id;
	EXPECT_EQ(mcp_request(&client, "ping", nullptr, &response), -EEXIST);
	EXPECT_EQ(response, original);
	EXPECT_STREQ(response, "original");
	EXPECT_EQ(client.next_req_id, request_id);
	struct pollfd descriptor = {};
	descriptor.fd = request_read;
	descriptor.events = POLLIN;
	EXPECT_EQ(poll(&descriptor, 1, 0), 0);
}

TEST_F(McpStdioFramingTest, NegativeRemoteStatusRetainsOwnedDiagnostics)
{
	send("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32601,"
	     "\"message\":\"retained diagnostic\"}}\n");
	char *response MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	EXPECT_EQ(mcp_request(&client, "unknown", nullptr, &response), -ENOSYS);
	ASSERT_NE(response, nullptr);
	EXPECT_NE(strstr(response, "retained diagnostic"), nullptr);
	cJSON *error MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(response);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(error, "error"), "code")->valueint,
		  -32601);
}

TEST_F(McpStdioFramingTest, ExchangePreservesOccupiedAndFailedOutput)
{
	struct mcp_wire_request request = {};
	request.id = 1;
	request.json = const_cast<char *>("{}");
	struct mcp_wire_reply reply MORPH_CLEANUP(mcp_wire_reply_cleanup) = {};
	reply.session_id = strdup("original");
	ASSERT_NE(reply.session_id, nullptr);
	char *original = reply.session_id;
	EXPECT_EQ(mcp_stdio_exchange(&client, &request, 1000, &reply), -EEXIST);
	EXPECT_EQ(reply.session_id, original);
	EXPECT_STREQ(reply.session_id, "original");
	mcp_wire_reply_cleanup(&reply);
	reply.http_status = 418;
	close(request_read);
	request_read = -1;
	EXPECT_EQ(mcp_stdio_exchange(&client, &request, 1000, &reply), -EPIPE);
	EXPECT_EQ(reply.http_status, 418);
	EXPECT_EQ(reply.json, nullptr);
	EXPECT_EQ(reply.session_id, nullptr);
}
