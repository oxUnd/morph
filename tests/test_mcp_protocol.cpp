#include <gtest/gtest.h>

extern "C" {
#include "mcp/mcp_internal.h"
#include "util/base64.h"
}

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

class McpCodecTest : public ::testing::Test {
protected:
	struct mcp_client client = {};
	struct arena *arena = nullptr;
	struct mcp_wire_request wire = {};

	void SetUp() override
	{
		client.config.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
		client.protocol = mcp_protocol_find(MCP_PROTOCOL_VERSION);
		pthread_mutex_init(&client.lock, nullptr);
		arena = arena_create(0);
		ASSERT_NE(arena, nullptr);
	}

	void TearDown() override
	{
		morph_array_cleanup(&wire.headers);
		arena_destroy(arena);
		mcp_protocol_reset(&client);
		pthread_mutex_destroy(&client.lock);
	}

	int encode(const char *method, const char *params)
	{
		morph_array_cleanup(&wire.headers);
		return mcp_protocol_encode(&client, client.protocol, 7,
			method, params, arena, &wire);
	}

	int cache(const char *schema)
	{
		cJSON *root = cJSON_Parse(schema);
		int rc = mcp_protocol_cache_tool(&client, "tool", root);
		cJSON_Delete(root);
		return rc;
	}

	std::string header(const char *name)
	{
		std::string prefix = std::string(name) + ": ";
		for (size_t i = 0; i < wire.headers.nelts; i++) {
			std::string value = *static_cast<const char **>(
				morph_array_get(&wire.headers, i));
			if (value.compare(0, prefix.size(), prefix) == 0)
				return value.substr(prefix.size());
		}
		return "";
	}
};

TEST_F(McpCodecTest, ModernEnvelopePreservesCallerMetadataAndAdvertisesOnlyImplementedCapabilities)
{
	ASSERT_EQ(encode("ping", "{\"_meta\":{\"progressToken\":23,"
		"\"io.modelcontextprotocol/clientCapabilities\":{\"sampling\":{}}}}"), 0);
	cJSON *root = cJSON_Parse(wire.json);
	cJSON *params = cJSON_GetObjectItem(root, "params");
	cJSON *meta = cJSON_GetObjectItem(params, "_meta");
	ASSERT_NE(meta, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(meta, "progressToken")->valueint, 23);
	EXPECT_STREQ(cJSON_GetObjectItem(meta,
		"io.modelcontextprotocol/protocolVersion")->valuestring, MCP_PROTOCOL_VERSION);
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(meta,
		"io.modelcontextprotocol/clientCapabilities")), 0);
	EXPECT_EQ(header("Mcp-Method"), "ping");
	EXPECT_EQ(header("MCP-Protocol-Version"), MCP_PROTOCOL_VERSION);
	cJSON_Delete(root);
}

TEST_F(McpCodecTest, LegacyEnvelopeDoesNotAcquireModernFields)
{
	client.protocol = mcp_protocol_find("2025-06-18");
	client.session_id = strdup("session");
	ASSERT_EQ(encode("ping", nullptr), 0);
	cJSON *root = cJSON_Parse(wire.json);
	EXPECT_EQ(cJSON_GetObjectItem(root, "params"), nullptr);
	EXPECT_EQ(header("Mcp-Method"), "");
	EXPECT_EQ(header("Mcp-Session-Id"), "session");
	EXPECT_EQ(header("MCP-Protocol-Version"), "2025-06-18");
	cJSON_Delete(root);
	EXPECT_EQ(mcp_protocol_find("2099-01-01"), nullptr);
}

TEST_F(McpCodecTest, NestedMirrorsUseExactPropertyPathsAndPrimitiveValues)
{
	ASSERT_EQ(cache(R"({"type":"object","properties":{
		"scope":{"type":"object","properties":{
			"tenant":{"type":"string","x-mcp-header":"Tenant"},
			"count":{"type":"integer","x-mcp-header":"Count"},
			"enabled":{"type":"boolean","x-mcp-header":"Enabled"},
			"optional":{"type":"string","x-mcp-header":"Optional"}}},
		"missing":{"type":"string","x-mcp-header":"Missing"}}})"), 0);
	ASSERT_EQ(encode("tools/call", R"({"name":"tool","arguments":{
		"tenant":"wrong","scope":{"tenant":"right","count":9007199254740991,
		"enabled":false,"optional":null}}})"), 0);
	EXPECT_EQ(header("Mcp-Param-Tenant"), "right");
	EXPECT_EQ(header("Mcp-Param-Count"), "9007199254740991");
	EXPECT_EQ(header("Mcp-Param-Enabled"), "false");
	EXPECT_EQ(header("Mcp-Param-Missing"), "");
	EXPECT_EQ(header("Mcp-Param-Optional"), "");
	EXPECT_EQ(wire.headers.nelts, 6U);
}

TEST_F(McpCodecTest, HeaderEncodingPreservesUnicodeWhitespaceControlsAndSentinels)
{
	const char *values[] = {"", "Hello, 世界", " padded ", "line1\nline2", "=?base64?literal?="};
	for (const char *value : values) {
		cJSON *params = cJSON_CreateObject();
		cJSON_AddStringToObject(params, "uri", value);
		char *json = cJSON_PrintUnformatted(params);
		ASSERT_EQ(encode("resources/read", json), 0);
		char *encoded = base64_encode(reinterpret_cast<const unsigned char *>(value),
			strlen(value));
		ASSERT_NE(encoded, nullptr);
		EXPECT_EQ(header("Mcp-Name"), "=?base64?" + std::string(encoded) + "?=");
		free(encoded);
		free(json);
		cJSON_Delete(params);
	}
}

TEST_F(McpCodecTest, RejectsInvalidOrUnreachableAnnotationsAndDuplicateHeaderNames)
{
	const char *invalid[] = {
		R"({"x-mcp-header":"Root","type":"string"})",
		R"({"properties":{"a":{"type":"number","x-mcp-header":"A"}}})",
		R"({"properties":{"a":{"type":"string","x-mcp-header":""}}})",
		R"({"properties":{"a":{"type":"string","x-mcp-header":"Bad\r\nName"}}})",
		R"({"properties":{"a":{"type":"string","x-mcp-header":"A"},
		"b":{"type":"string","x-mcp-header":"a"}}})",
		R"({"oneOf":[{"properties":{"a":{"type":"string","x-mcp-header":"A"}}}]})",
		R"({"properties":{"a":{"items":{"type":"string","x-mcp-header":"A"}}}})",
		R"({"$defs":{"a":{"type":"string","x-mcp-header":"A"}}})"
	};
	for (const char *schema : invalid)
		EXPECT_EQ(cache(schema), -EINVAL) << schema;
	EXPECT_EQ(cache(R"({"type":"object","properties":{"x-mcp-header":{
		"type":"string","enum":[{"x-mcp-header":"this is instance data"}]}}})"), 0);
}

TEST_F(McpCodecTest, RejectsUnsafeIntegerValuesAndDoesNotReuseStaleSessionHeaders)
{
	ASSERT_EQ(cache(R"({"properties":{"n":{"type":"integer","x-mcp-header":"N"}}})"), 0);
	EXPECT_EQ(encode("tools/call", R"({"name":"tool","arguments":{"n":1.5}})"), -EINVAL);
	EXPECT_EQ(encode("tools/call", R"({"name":"tool","arguments":{"n":9007199254740992}})"),
		-EINVAL);
	client.session_id = strdup("stale-legacy-session");
	ASSERT_EQ(encode("ping", nullptr), 0);
	EXPECT_EQ(header("Mcp-Session-Id"), "");
	EXPECT_EQ(encode("ping", "[]"), MORPH_ERR_PARSE);
	EXPECT_EQ(encode("ping", "{\"_meta\":false}"), -EINVAL);
}

class McpStdioProtocolTest : public ::testing::Test {
protected:
	struct mcp_client client = {};
	std::string directory;
	std::string log_path;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-mcp-adapter-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		directory = pattern;
		log_path = directory + "/wire.log";
		std::ofstream server(directory + "/server.sh");
		server << R"SH(
while IFS= read -r line; do
 printf '%s\n' "$line" >> "$1"
 id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
 case "$line" in
 *'"method":"server/discover"'*)
   case "$2" in
   exit) exit 0 ;;
   silent) continue ;;
   legacy*) printf '{"jsonrpc":"2.0","id":%s,"error":{"code":-32602,"message":"Initialize first"}}\n' "$id" ;;
   unsupported) printf '{"jsonrpc":"2.0","id":%s,"error":{"code":-32022,"message":"Unsupported version","data":{"supported":["2099-01-01"]}}}\n' "$id" ;;
   malformed) printf '{"jsonrpc":"2.0","id":%s,"result":{}}\n' "$id" ;;
   *) printf '{"jsonrpc":"2.0","id":%s,"result":{"resultType":"complete","supportedVersions":["2026-07-28"],"capabilities":{"tools":{}},"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"modern","version":"1"}}}}\n' "$id" ;;
   esac
   ;;
 *'"method":"initialize"'*)
   version=2025-06-18
   [ "$2" = legacy_november ] && version=2025-11-25
   [ "$2" = legacy_unknown ] && version=2099-01-01
   printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":"%s","capabilities":{"tools":{}},"serverInfo":{"name":"legacy","version":"1"}}}\n' "$id" "$version"
   ;;
 *'"method":"ping"'*)
   type=complete
   [ "$2" = input ] && type=input_required
   [ "$2" = extension ] && type=unadvertised_extension
   printf '{"jsonrpc":"2.0","id":%s,"result":{"resultType":"%s"}}\n' "$id" "$type"
   ;;
 esac
done
)SH";
		client.config.transport = MCP_TRANSPORT_STDIO;
		strcpy(client.config.name, "adapter");
		strcpy(client.config.command, "/bin/sh");
		snprintf(client.config.cmd_args[0], sizeof(client.config.cmd_args[0]),
			"%s/server.sh", directory.c_str());
		snprintf(client.config.cmd_args[1], sizeof(client.config.cmd_args[1]),
			"%s", log_path.c_str());
		client.config.cmd_args_count = 3;
		client.next_req_id = 1;
		client.stdin_fd = -1;
		client.stdout_fd = -1;
		client.server_pid = -1;
		pthread_mutex_init(&client.lock, nullptr);
	}

	void TearDown() override
	{
		mcp_disconnect(&client);
		pthread_mutex_destroy(&client.lock);
		std::filesystem::remove_all(directory);
	}

	int connect(const char *mode)
	{
		snprintf(client.config.cmd_args[2], sizeof(client.config.cmd_args[2]), "%s", mode);
		return mcp_ensure_connected(&client);
	}

	std::string wire_log()
	{
		std::ifstream stream(log_path);
		return std::string(std::istreambuf_iterator<char>(stream), {});
	}
};

TEST_F(McpStdioProtocolTest, ModernDiscoveryAndRequestsUseMetadataWithoutHandshake)
{
	ASSERT_EQ(connect("modern"), 0);
	EXPECT_STREQ(client.negotiated_version, MCP_PROTOCOL_VERSION);
	EXPECT_STREQ(client.server_name, "modern");
	EXPECT_EQ(client.supports_tools, 1);
	ASSERT_EQ(mcp_ping(&client), 0);
	std::string log = wire_log();
	EXPECT_EQ(log.find("\"method\":\"initialize\""), std::string::npos);
	EXPECT_EQ(log.find("notifications/initialized"), std::string::npos);
	std::istringstream lines(log);
	std::string line;
	while (std::getline(lines, line)) {
		cJSON *request = cJSON_Parse(line.c_str());
		cJSON *meta = cJSON_GetObjectItem(cJSON_GetObjectItem(request, "params"), "_meta");
		EXPECT_NE(meta, nullptr);
		cJSON_Delete(request);
	}
}

TEST_F(McpStdioProtocolTest, LegacyErrorsFallBackToCleanHandshakeAndSelectedVersion)
{
	ASSERT_EQ(connect("legacy"), 0);
	EXPECT_STREQ(client.negotiated_version, "2025-06-18");
	ASSERT_EQ(mcp_ping(&client), 0);
	std::string log = wire_log();
	EXPECT_NE(log.find("notifications/initialized"), std::string::npos);
	EXPECT_NE(log.find("\"protocolVersion\":\"2025-11-25\""), std::string::npos);
	EXPECT_NE(log.find("\"capabilities\":{}"), std::string::npos);
}

TEST_F(McpStdioProtocolTest, NovemberLegacyVersionIsSupported)
{
	ASSERT_EQ(connect("legacy_november"), 0);
	EXPECT_STREQ(client.negotiated_version, "2025-11-25");
}

TEST_F(McpStdioProtocolTest, ServerExitingOnProbeIsRestartedForLegacyHandshake)
{
	ASSERT_EQ(connect("exit"), 0);
	EXPECT_STREQ(client.negotiated_version, "2025-06-18");
	EXPECT_EQ(mcp_ping(&client), 0);
}

TEST_F(McpStdioProtocolTest, SilentProbeHasBoundedTimeoutAndFallsBack)
{
	client.config.connect_timeout = 1;
	auto start = std::chrono::steady_clock::now();
	ASSERT_EQ(connect("silent"), 0);
	EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
	EXPECT_STREQ(client.negotiated_version, "2025-06-18");
}

TEST_F(McpStdioProtocolTest, RecognizedModernErrorNeverDowngrades)
{
	EXPECT_EQ(connect("unsupported"), -EPROTONOSUPPORT);
	EXPECT_EQ(wire_log().find("\"method\":\"initialize\""), std::string::npos);
	EXPECT_EQ(client.connected, 0);
	EXPECT_EQ(client.protocol, nullptr);
}

TEST_F(McpStdioProtocolTest, MalformedDiscoveryAndUnknownLegacyVersionAreRejected)
{
	EXPECT_EQ(connect("malformed"), MORPH_ERR_PROTOCOL);
	EXPECT_EQ(wire_log().find("\"method\":\"initialize\""), std::string::npos);
	EXPECT_EQ(connect("legacy_unknown"), -EPROTONOSUPPORT);
	EXPECT_EQ(client.connected, 0);
}

TEST_F(McpStdioProtocolTest, IncompleteAndUnknownResultsAreNotReportedAsSuccess)
{
	ASSERT_EQ(connect("input"), 0);
	EXPECT_EQ(mcp_ping(&client), -ENOTSUP);
	mcp_disconnect(&client);
	ASSERT_EQ(connect("extension"), 0);
	EXPECT_EQ(mcp_ping(&client), MORPH_ERR_PROTOCOL);
}

struct McpHttpReply {
	int status = 200;
	std::string body;
	std::string headers;
	std::string content_type = "application/json";
};

struct McpHttpRequest {
	std::string headers;
	std::string body;
};

/* A real HTTP peer: inspect what libcurl sends, not a mocked transport API. */
class McpProtocolHttpServer {
	int listener = -1;
	std::atomic<bool> stopping{false};
	std::thread worker;
	std::mutex lock;
	std::vector<McpHttpRequest> requests;
	std::function<McpHttpReply(const McpHttpRequest &)> handler;

	void serve(int connection)
	{
		struct timeval timeout = {2, 0};
		setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		std::string raw;
		char buffer[BUFSIZ];
		size_t end = std::string::npos;
		size_t body_size = 0;
		for (;;) {
			ssize_t count = read(connection, buffer, sizeof(buffer));
			if (count <= 0)
				return;
			raw.append(buffer, static_cast<size_t>(count));
			end = raw.find("\r\n\r\n");
			if (end == std::string::npos)
				continue;
			size_t length = raw.find("Content-Length: ");
			if (length != std::string::npos)
				body_size = std::stoul(raw.substr(length + 16));
			if (raw.size() >= end + 4 + body_size)
				break;
		}
		McpHttpRequest request{raw.substr(0, end), raw.substr(end + 4, body_size)};
		{
			std::lock_guard<std::mutex> guard(lock);
			requests.push_back(request);
		}
		McpHttpReply reply = handler(request);
		std::string response = "HTTP/1.1 " + std::to_string(reply.status) + " Reply\r\n"
			"Connection: close\r\nContent-Type: " + reply.content_type + "\r\n"
			"Content-Length: " + std::to_string(reply.body.size()) + "\r\n" +
			reply.headers + "\r\n" + reply.body;
		size_t offset = 0;
		while (offset < response.size()) {
			ssize_t count = write(connection, response.data() + offset, response.size() - offset);
			if (count <= 0)
				break;
			offset += static_cast<size_t>(count);
		}
	}

public:
	unsigned port = 0;

	explicit McpProtocolHttpServer(std::function<McpHttpReply(const McpHttpRequest &)> fn)
		: handler(std::move(fn))
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener < 0)
			return;
		struct sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t length = sizeof(address);
		if (bind(listener, reinterpret_cast<sockaddr *>(&address), length) != 0 ||
		    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0 ||
		    listen(listener, 4) != 0)
			return;
		port = ntohs(address.sin_port);
		worker = std::thread([this] {
			while (!stopping) {
				struct pollfd descriptor = {};
				descriptor.fd = listener;
				descriptor.events = POLLIN;
				if (poll(&descriptor, 1, 50) <= 0)
					continue;
				int connection = accept(listener, nullptr, nullptr);
				if (connection >= 0) {
					serve(connection);
					close(connection);
				}
			}
		});
	}

	~McpProtocolHttpServer()
	{
		stopping = true;
		if (worker.joinable())
			worker.join();
		if (listener >= 0)
			close(listener);
	}

	std::vector<McpHttpRequest> captured()
	{
		std::lock_guard<std::mutex> guard(lock);
		return requests;
	}
};

static std::string rpc_result(cJSON *request, const std::string &result)
{
	cJSON *id = cJSON_GetObjectItem(request, "id");
	std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id->valueint) +
		",\"result\":" + result + "}";
	cJSON *object = cJSON_Parse(body.c_str());
	char *encoded = cJSON_PrintUnformatted(object);
	std::string compact = encoded ? encoded : body;
	free(encoded);
	cJSON_Delete(object);
	return compact;
}

static std::string rpc_error(cJSON *request, int code)
{
	cJSON *id = cJSON_GetObjectItem(request, "id");
	return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id->valueint) +
		",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"Rejected\"}}";
}

class McpHttpProtocolTest : public ::testing::Test {
protected:
	struct mcp_client client = {};

	void SetUp() override
	{
		ASSERT_EQ(http_init(NULL), 0);
		client.config.transport = MCP_TRANSPORT_STREAMABLE_HTTP;
		client.next_req_id = 1;
		pthread_mutex_init(&client.lock, nullptr);
	}

	void TearDown() override
	{
		mcp_disconnect(&client);
		pthread_mutex_destroy(&client.lock);
		http_cleanup();
	}

	int connect(McpProtocolHttpServer &server)
	{
		snprintf(client.config.http_url, sizeof(client.config.http_url),
			"http://127.0.0.1:%u/mcp", server.port);
		return mcp_ensure_connected(&client);
	}
};

TEST_F(McpHttpProtocolTest, ModernCallsMirrorSchemaHeadersAndCorrelateSseResponses)
{
	McpProtocolHttpServer server([](const McpHttpRequest &wire) {
		McpHttpReply reply;
		cJSON *request = cJSON_Parse(wire.body.c_str());
		std::string method = cJSON_GetObjectItem(request, "method")->valuestring;
		if (method == "server/discover") {
			reply.body = rpc_result(request, R"({"supportedVersions":["2026-07-28"],
				"capabilities":{"tools":{}},"resultType":"complete"})");
			reply.headers = "Mcp-Session-Id: ignored-modern-session\r\n";
		} else if (method == "tools/list") {
			reply.body = rpc_result(request, R"({"tools":[{"name":"tool","inputSchema":{
				"type":"object","properties":{"tenant":{"type":"string",
				"x-mcp-header":"Tenant"}}}},{"name":"bad","inputSchema":{
				"properties":{"x":{"type":"number","x-mcp-header":"Bad"}}}}]})");
		} else if (method == "tools/call") {
			reply.content_type = "text/event-stream";
			reply.body = "event: message\ndata: {\"jsonrpc\":\"2.0\","
				"\"method\":\"notifications/progress\",\"params\":{}}\n\n"
				"data: {\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{}}\n\n"
				"data: " + rpc_result(request, R"({"resultType":"complete",
				"content":[],"structuredContent":{"ok":true}})") + "\n\n";
		} else {
			reply.status = 400;
			reply.body = rpc_error(request, -32601);
		}
		cJSON_Delete(request);
		return reply;
	});
	ASSERT_NE(server.port, 0U);
	ASSERT_EQ(connect(server), 0);
	EXPECT_EQ(client.session_id, nullptr);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(mcp_call_tool(&client, nullptr, "tool", "{\"tenant\":\"a\\nb\"}", &result), 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(
		cJSON_GetObjectItem(result.data, "structuredContent"), "ok")));
	tool_result_cleanup(&result);
	auto requests = server.captured();
	ASSERT_EQ(requests.size(), 3U);
	EXPECT_NE(requests.back().headers.find("Mcp-Param-Tenant: =?base64?YQpi?="),
		std::string::npos);
	EXPECT_NE(requests.back().headers.find("Mcp-Name: tool"), std::string::npos);
	for (const auto &wire : requests) {
		EXPECT_NE(wire.headers.find("MCP-Protocol-Version: 2026-07-28"), std::string::npos);
		EXPECT_NE(wire.headers.find("Mcp-Method: "), std::string::npos);
		EXPECT_EQ(wire.headers.find("Mcp-Session-Id:"), std::string::npos);
		EXPECT_NE(wire.body.find("io.modelcontextprotocol/clientCapabilities"),
			std::string::npos);
	}
	EXPECT_FALSE(morph_strmap_contains(&client.tool_schemas, "bad"));
}

TEST_F(McpHttpProtocolTest, LegacyFallbackKeepsSessionAndServerSelectedVersion)
{
	const std::string session(5000, 's');
	McpProtocolHttpServer server([&](const McpHttpRequest &wire) {
		McpHttpReply reply;
		cJSON *request = cJSON_Parse(wire.body.c_str());
		std::string method = cJSON_GetObjectItem(request, "method")->valuestring;
		if (method == "server/discover") {
			reply.status = 400;
			reply.body = rpc_error(request, -32600);
		} else if (method == "initialize") {
			reply.body = rpc_result(request, R"({"protocolVersion":"2025-06-18",
				"capabilities":{"tools":{}},"serverInfo":{"name":"legacy","version":"1"}})");
			reply.headers = "Mcp-Session-Id: " + session + "\r\n";
		} else if (method == "notifications/initialized") {
			reply.status = 202;
		} else {
			reply.body = rpc_result(request, "{}");
		}
		cJSON_Delete(request);
		return reply;
	});
	ASSERT_NE(server.port, 0U);
	ASSERT_EQ(connect(server), 0);
	ASSERT_EQ(mcp_ping(&client), 0);
	EXPECT_STREQ(client.negotiated_version, "2025-06-18");
	EXPECT_EQ(std::string(client.session_id), session);
	auto requests = server.captured();
	ASSERT_EQ(requests.size(), 4U);
	EXPECT_EQ(requests[1].headers.find("MCP-Protocol-Version:"), std::string::npos);
	EXPECT_EQ(requests[1].headers.find("Mcp-Method:"), std::string::npos);
	EXPECT_NE(requests[1].body.find("\"protocolVersion\":\"2025-11-25\""),
		std::string::npos);
	EXPECT_NE(requests.back().headers.find("Mcp-Session-Id: " + session), std::string::npos);
	EXPECT_NE(requests.back().headers.find("MCP-Protocol-Version: 2025-06-18"),
		std::string::npos);
	EXPECT_EQ(requests.back().body.find("_meta"), std::string::npos);
	EXPECT_EQ(requests.back().headers.find("Mcp-Method:"), std::string::npos);
}

TEST_F(McpHttpProtocolTest, AuthAndServerFailuresNeverSelectLegacyProtocol)
{
	for (int status : {401, 403, 500, 503}) {
		McpProtocolHttpServer server([status](const McpHttpRequest &) {
			McpHttpReply reply;
			reply.status = status;
			reply.body = "upstream failure";
			return reply;
		});
		ASSERT_NE(server.port, 0U);
		EXPECT_EQ(connect(server), status < 500 ? -EACCES : MORPH_ERR_API);
		EXPECT_EQ(server.captured().size(), 1U);
		EXPECT_EQ(client.connected, 0);
		EXPECT_EQ(client.protocol, nullptr);
	}
}

TEST_F(McpHttpProtocolTest, RecognizedModernErrorsAndMalformedSuccessNeverDowngrade)
{
	for (int code : {-32020, -32021, -32022, 0}) {
		McpProtocolHttpServer server([code](const McpHttpRequest &wire) {
			McpHttpReply reply;
			cJSON *request = cJSON_Parse(wire.body.c_str());
			reply.status = code ? 400 : 200;
			reply.body = code ? rpc_error(request, code) : rpc_result(request, "{}");
			cJSON_Delete(request);
			return reply;
		});
		ASSERT_NE(server.port, 0U);
		EXPECT_EQ(connect(server), code == -32022 ? -EPROTONOSUPPORT : MORPH_ERR_PROTOCOL);
		EXPECT_EQ(server.captured().size(), 1U);
	}
}

TEST_F(McpCodecTest, HeaderListGrowsBeyondSmallFixedArrays)
{
	cJSON *schema = cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
	cJSON *properties = cJSON_GetObjectItem(schema, "properties");
	cJSON *params = cJSON_Parse("{\"name\":\"tool\",\"arguments\":{}}");
	cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
	for (int i = 0; i < 32; i++) {
		std::string name = "p" + std::to_string(i);
		cJSON *property = cJSON_AddObjectToObject(properties, name.c_str());
		cJSON_AddStringToObject(property, "type", "string");
		cJSON_AddStringToObject(property, "x-mcp-header", name.c_str());
		cJSON_AddStringToObject(arguments, name.c_str(), "value");
	}
	ASSERT_EQ(mcp_protocol_cache_tool(&client, "tool", schema), 0);
	char *json = cJSON_PrintUnformatted(params);
	ASSERT_EQ(encode("tools/call", json), 0);
	EXPECT_EQ(wire.headers.nelts, 35U);
	EXPECT_EQ(header("Mcp-Param-p31"), "value");
	free(json);
	cJSON_Delete(schema);
	cJSON_Delete(params);
}

TEST_F(McpCodecTest, DecoderRejectsInvalidFramingAndPreservesCorrelatedErrors)
{
	const char *invalid[] = {
		R"({"jsonrpc":"2.0","id":8,"result":{}})",
		R"({"jsonrpc":"1.0","id":7,"result":{}})",
		R"({"jsonrpc":"2.0","id":7,"result":{},"error":{"code":-1,"message":"x"}})",
		R"({"jsonrpc":"2.0","id":7,"result":null})",
		R"({"jsonrpc":"2.0","id":7,"error":{"message":"x"}})"
	};
	for (const char *json : invalid) {
		struct mcp_wire_reply reply = {};
		reply.json = const_cast<char *>(json);
		cJSON *object = nullptr;
		EXPECT_EQ(mcp_protocol_decode(&reply, 7, &object), MORPH_ERR_PROTOCOL);
		EXPECT_EQ(object, nullptr);
	}
	struct mcp_wire_reply reply = {};
	reply.json = const_cast<char *>(
		R"({"jsonrpc":"2.0","id":7,"error":{"code":-32022,"message":"Unsupported"}})");
	reply.http_status = 400;
	cJSON *object = nullptr;
	EXPECT_EQ(mcp_protocol_decode(&reply, 7, &object), MORPH_ERR_PROTOCOL);
	ASSERT_NE(object, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(cJSON_GetObjectItem(object, "error"), "code")->valueint,
		-32022);
	cJSON_Delete(object);
}

TEST_F(McpStdioProtocolTest, PinnedLegacySkipsProbeAndRejectsDifferentServerVersion)
{
	strcpy(client.config.protocol_version, "2025-06-18");
	ASSERT_EQ(connect("legacy"), 0);
	EXPECT_EQ(wire_log().find("server/discover"), std::string::npos);
	mcp_disconnect(&client);
	EXPECT_EQ(connect("legacy_november"), -EPROTONOSUPPORT);
}

TEST_F(McpStdioProtocolTest, PinnedModernNeverFallsBackToLegacyHandshake)
{
	strcpy(client.config.protocol_version, MCP_PROTOCOL_VERSION);
	EXPECT_EQ(connect("legacy"), -EPROTONOSUPPORT);
	EXPECT_EQ(wire_log().find("\"method\":\"initialize\""), std::string::npos);
	ASSERT_EQ(connect("modern"), 0);
	EXPECT_STREQ(client.negotiated_version, MCP_PROTOCOL_VERSION);
}

TEST_F(McpCodecTest, RequestRequiresAnExplicitlySelectedAdapter)
{
	client.protocol = nullptr;
	char *result = nullptr;
	EXPECT_EQ(mcp_request(&client, "ping", nullptr, &result), MORPH_ERR_NOT_INITIALIZED);
	EXPECT_EQ(result, nullptr);
}

TEST_F(McpCodecTest, ResetDropsSessionAdapterCapabilitiesAndCachedSchemas)
{
	ASSERT_EQ(cache("{\"type\":\"object\"}"), 0);
	client.connected = 1;
	client.supports_tools = 1;
	client.session_id = strdup("session");
	strcpy(client.negotiated_version, MCP_PROTOCOL_VERSION);
	mcp_protocol_reset(&client);
	EXPECT_EQ(client.connected, 0);
	EXPECT_EQ(client.protocol, nullptr);
	EXPECT_EQ(client.session_id, nullptr);
	EXPECT_EQ(client.supports_tools, 0);
	EXPECT_EQ(client.negotiated_version[0], '\0');
	EXPECT_EQ(client.tool_schemas.entries, nullptr);
}

TEST_F(McpHttpProtocolTest, LegacyJsonRpcErrorOnHttpSuccessAllowsHandshakeFallback)
{
	McpProtocolHttpServer server([](const McpHttpRequest &wire) {
		McpHttpReply reply;
		cJSON *request = cJSON_Parse(wire.body.c_str());
		std::string method = cJSON_GetObjectItem(request, "method")->valuestring;
		if (method == "server/discover")
			reply.body = rpc_error(request, -32601);
		else if (method == "initialize")
			reply.body = rpc_result(request,
				R"({"protocolVersion":"2025-11-25","capabilities":{}})");
		else
			reply.status = 202;
		cJSON_Delete(request);
		return reply;
	});
	ASSERT_NE(server.port, 0U);
	ASSERT_EQ(connect(server), 0);
	EXPECT_STREQ(client.negotiated_version, "2025-11-25");
	EXPECT_EQ(server.captured().size(), 3U);
}

/* Opt-in live discovery probe. The command must speak stdio MCP; no tool is run. */
TEST(McpLiveServerTest, DiscoveryKeepsUsableCapabilities)
{
	const char *command = getenv("MORPH_TEST_MCP_COMMAND");
	if (!command || !command[0])
		GTEST_SKIP() << "Set MORPH_TEST_MCP_COMMAND to a stdio MCP server executable";
	struct mcp_server_config config = {};
	ASSERT_LT(strlen(command), sizeof(config.command));
	strcpy(config.name, "live");
	strcpy(config.command, command);
	config.connect_timeout = 90;
	struct mcp_registry servers;
	mcp_registry_init(NULL, &servers);
	std::unique_ptr<struct mcp_registry, decltype(&mcp_registry_cleanup)>
		servers_guard(&servers, mcp_registry_cleanup);
	ASSERT_EQ(mcp_registry_add(&servers, &config), 0);
	struct mcp_client *client = mcp_registry_get(&servers, "live");
	ASSERT_NE(client, nullptr);
	ASSERT_EQ(mcp_ensure_connected(client), 0);
	struct tool_registry registry;
	tool_registry_init(NULL, &registry);
	std::unique_ptr<struct tool_registry, decltype(&tool_registry_cleanup)>
		registry_guard(&registry, tool_registry_cleanup);
	int tools = mcp_register_server_tools(client, &registry);
	int resources = mcp_register_server_resources(client, &registry);
	int prompts = mcp_register_server_prompts(client, &registry);
	EXPECT_GE(tools, 0);
	EXPECT_GE(resources, 0);
	EXPECT_GE(prompts, 0);
	EXPECT_EQ(client->connected, 1);
	EXPECT_EQ(registry.count, tools + resources + prompts);
	RecordProperty("tools", tools);
	RecordProperty("resources", resources);
	RecordProperty("prompts", prompts);
	RecordProperty("supports_prompts", client->supports_prompts);
	RecordProperty("protocol", client->negotiated_version);
	struct arena *arena = arena_create(0);
	ASSERT_NE(arena, nullptr);
	struct mcp_tool_desc *descriptors = nullptr;
	int count = 0;
	int rc = mcp_list_tools(client, arena, &descriptors, &count);
	arena_destroy(arena);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(count, tools);
}
