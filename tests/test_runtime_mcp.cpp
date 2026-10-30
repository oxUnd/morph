#include <gtest/gtest.h>

extern "C" {
#include "runtime/runtime_internal.h"
#include "http/client.h"
}

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using McpClock = std::chrono::steady_clock;

class RuntimeMcpBackgroundTest : public ::testing::Test {
protected:
	std::string directory;
	std::string config_path;
	std::string db_path;
	struct runtime *instance = nullptr;
	std::mutex event_lock;
	std::vector<std::string> events;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-mcp-background-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		directory = pattern;
		config_path = directory + "/config.toml";
		db_path = directory + "/data.db";
		ASSERT_EQ(http_init(NULL), 0);
		std::ofstream script(directory + "/server.sh");
		script << R"SH(
: > "$2"
while IFS= read -r line; do
 id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
 case "$line" in
 *'"method":"server/discover"'*)
   printf '{"jsonrpc":"2.0","id":%s,"error":{"code":-32601,"message":"Method not found"}}\n' "$id"
   ;;
 *'"method":"initialize"'*)
   while [ ! -f "$1" ]; do sleep 0.02; done
   printf '%s\n' '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","serverInfo":{"name":"mock","version":"1"},"capabilities":{"tools":{},"resources":{},"prompts":{}}}}' | sed "s/\"id\":1/\"id\":$id/"
   ;;
 *'"method":"tools/list"'*)
   if [ "$3" = large ]; then
     sed "s/\"id\":2/\"id\":$id/" "$(dirname "$1")/large.json"
   elif [ "$3" = invalid ]; then
     printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"valid","inputSchema":{"type":"object","properties":{}}},{"name":"bad","inputSchema":{"type":"invalid"}}]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   elif [ "$3" = two ]; then
     printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"first","inputSchema":{"type":"object","properties":{}}},{"name":"second","inputSchema":{"type":"object","properties":{}}}]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   else
     printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"echo","description":"Mock echo","inputSchema":{"type":"object","properties":{}}}]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   fi
   ;;
 *'"method":"resources/list"'*)
   printf '%s\n' '{"jsonrpc":"2.0","id":3,"result":{"resources":[]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   ;;
 *'"method":"prompts/list"'*)
   if [ "$3" = missing_prompt ]; then
     printf '{"jsonrpc":"2.0","id":%s,"error":{"code":-32601,"message":"Method not found: Unknown method: prompts/list","data":{"detail":"Unknown method: prompts/list"}}}\n' "$id"
   elif [ "$3" = prompt ]; then
     printf '%s\n' '{"jsonrpc":"2.0","id":4,"result":{"prompts":[{"name":"review","arguments":[{"name":"subject","description":"Review subject","required":true}]}]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   else
   printf '%s\n' '{"jsonrpc":"2.0","id":4,"result":{"prompts":[]}}' | sed "s/\"id\":[0-9]*/\"id\":$id/"
   fi
   ;;
 esac
done
)SH";
	}

	void TearDown() override
	{
		runtime_close(instance);
		http_cleanup();
		std::filesystem::remove_all(directory);
	}

	void server(const char *name, int timeout = 0, bool auto_connect = true,
		const char *mode = "valid")
	{
		std::ofstream config(config_path, std::ios::app);
		config << "[[mcp.servers]]\nname = \"" << name << "\"\n"
			<< "transport = \"stdio\"\ncommand = \"/bin/sh\"\nargs = [\""
			<< directory << "/server.sh\", \"" << directory << "/" << name
			<< ".release\", \"" << directory << "/" << name
			<< ".started\", \"" << mode << "\"]\n"
			<< "auto_connect = " << (auto_connect ? "true" : "false") << "\n"
			<< "connect_timeout = " << timeout << "\n";
	}

	int open(bool background = true)
	{
		struct runtime_options options = {};
		options.config_path = config_path.c_str();
		options.db_path = db_path.c_str();
		options.workdir = directory.c_str();
		options.front_name = "test";
		options.auto_connect_mcp = 1;
		options.background_connect_mcp = background;
		options.event_cb = [](const morph_event *event, void *opaque) {
			auto *self = static_cast<RuntimeMcpBackgroundTest *>(opaque);
			char *json = morph_event_to_json_string(event);
			if (json) {
				std::lock_guard<std::mutex> guard(self->event_lock);
				self->events.emplace_back(json);
				free(json);
			}
			return 0;
		};
		options.event_user_data = this;
		return runtime_open(&options, &instance);
	}

	bool wait(const std::function<bool()> &predicate, int milliseconds = 4000)
	{
		auto deadline = McpClock::now() + std::chrono::milliseconds(milliseconds);
		do {
			if (predicate())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		} while (McpClock::now() < deadline);
		for (int i = 0; i < runtime_mcp_count(instance); i++) {
			struct runtime_mcp_status status = {};
			runtime_mcp_info(instance, i, &status);
			std::fprintf(stderr, "MCP test timeout: %s state=%d error=%d\n",
				status.config.name, status.state, status.error_code);
		}
		return false;
	}

	bool state(const char *name, enum runtime_mcp_state expected)
	{
		struct runtime_mcp_status status = {};
		return runtime_mcp_find(instance, name, &status) == 0 && status.state == expected;
	}

	void release(const char *name)
	{
		std::ofstream marker(directory + "/" + name + ".release");
	}
};

TEST_F(RuntimeMcpBackgroundTest, ReturnsBeforeSlowServersAndConnectsIndependently)
{
	server("slow");
	server("fast");
	auto start = McpClock::now();
	ASSERT_EQ(open(), 0);
	EXPECT_LT(McpClock::now() - start, std::chrono::seconds(2));
	ASSERT_TRUE(wait([&] {
		return std::filesystem::exists(directory + "/slow.started") &&
			std::filesystem::exists(directory + "/fast.started");
	}));
	EXPECT_TRUE(state("slow", RUNTIME_MCP_CONNECTING));
	EXPECT_EQ(runtime_mcp_connect(instance, "slow"), -EAGAIN);
	struct mcp_tool_desc *list = nullptr;
	int count = 0;
	EXPECT_EQ(runtime_mcp_list_tools(instance, "slow", &list, &count), -EAGAIN);
	release("fast");
	ASSERT_TRUE(wait([&] { return state("fast", RUNTIME_MCP_READY); }));
	EXPECT_TRUE(state("slow", RUNTIME_MCP_CONNECTING));
	struct tool_desc desc = {};
	EXPECT_EQ(runtime_tool_find(instance, "mcp_fast__echo", &desc), 0);
	EXPECT_EQ(runtime_tool_find(instance, "mcp_slow__echo", &desc), -ENOENT);
	release("slow");
	ASSERT_TRUE(wait([&] { return state("slow", RUNTIME_MCP_READY); }));
	EXPECT_EQ(runtime_tool_find(instance, "mcp_slow__echo", &desc), 0);
}

TEST_F(RuntimeMcpBackgroundTest, PublishesOnlyBetweenTurns)
{
	server("gated", 1);
	ASSERT_EQ(open(), 0);
	pthread_mutex_lock(&instance->context.execution_lock);
	int before = instance->context.tools.count;
	release("gated");
	bool registering = wait([&] { return state("gated", RUNTIME_MCP_REGISTERING); });
	EXPECT_TRUE(registering);
	/* Waiting for a turn boundary must not consume the connection timeout. */
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	EXPECT_TRUE(state("gated", RUNTIME_MCP_REGISTERING));
	EXPECT_EQ(instance->context.tools.count, before);
	pthread_mutex_unlock(&instance->context.execution_lock);
	ASSERT_TRUE(wait([&] { return state("gated", RUNTIME_MCP_READY); }));
	EXPECT_EQ(runtime_tool_count(instance), before + 1);
}

TEST_F(RuntimeMcpBackgroundTest, PublicationWaitsForMutationWithoutBlockingCatalogQueries)
{
	server("mutation_gate", 1);
	ASSERT_EQ(open(), 0);
	pthread_mutex_lock(&instance->context.tools.mutation_mutex);
	int before = instance->context.tools.count;
	release("mutation_gate");
	EXPECT_TRUE(wait([&] { return state("mutation_gate", RUNTIME_MCP_REGISTERING); }));
	std::promise<int> queried;
	auto result = queried.get_future();
	std::thread query([&] { queried.set_value(runtime_tool_count(instance)); });
	auto status = result.wait_for(std::chrono::milliseconds(500));
	EXPECT_EQ(status, std::future_status::ready);
	if (status == std::future_status::ready) {
		EXPECT_EQ(result.get(), before);
	}
	EXPECT_FALSE(state("mutation_gate", RUNTIME_MCP_READY));
	pthread_mutex_unlock(&instance->context.tools.mutation_mutex);
	query.join();
	ASSERT_TRUE(wait([&] { return state("mutation_gate", RUNTIME_MCP_READY); }));
	EXPECT_EQ(runtime_tool_count(instance), before + 1);
}

TEST_F(RuntimeMcpBackgroundTest, TimeoutCleansConnectionAndAllowsRetry)
{
	server("retry", 1);
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return state("retry", RUNTIME_MCP_FAILED); }));
	struct runtime_mcp_status status = {};
	ASSERT_EQ(runtime_mcp_find(instance, "retry", &status), 0);
	EXPECT_EQ(status.error_code, -ETIMEDOUT);
	EXPECT_EQ(status.connected, 0);
	EXPECT_GE(status.elapsed_ms, 1000);
	EXPECT_EQ(instance->context.mcp.servers[0]->server_pid, -1);
	release("retry");
	ASSERT_EQ(runtime_mcp_connect(instance, "retry"), 0);
	int tools = 0, resources = 0, prompts = 0;
	ASSERT_EQ(runtime_mcp_discover(instance, "retry", &tools, &resources, &prompts), 0);
	EXPECT_EQ(tools, 1);
	EXPECT_TRUE(state("retry", RUNTIME_MCP_READY));
	ASSERT_EQ(runtime_mcp_disconnect(instance, "retry"), 0);
	EXPECT_TRUE(state("retry", RUNTIME_MCP_DISCONNECTED));
	ASSERT_EQ(runtime_mcp_connect(instance, "retry"), 0);
	EXPECT_EQ(runtime_mcp_discover(instance, "retry", &tools, &resources, &prompts), 0);
}

TEST_F(RuntimeMcpBackgroundTest, CloseCancelsUnresponsiveServer)
{
	server("hung");
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return std::filesystem::exists(directory + "/hung.started"); }));
	auto start = McpClock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(McpClock::now() - start, std::chrono::seconds(2));
}

TEST_F(RuntimeMcpBackgroundTest, CloseCancelsPendingPublicationWithoutDeadlock)
{
	server("pending");
	ASSERT_EQ(open(), 0);
	pthread_mutex_lock(&instance->context.execution_lock);
	release("pending");
	EXPECT_TRUE(wait([&] { return state("pending", RUNTIME_MCP_REGISTERING); }));
	std::thread closing([&] { runtime_close(instance); });
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	pthread_mutex_unlock(&instance->context.execution_lock);
	closing.join();
	instance = nullptr;
}

TEST_F(RuntimeMcpBackgroundTest, DisconnectCancelsInitializationAndCanReconnect)
{
	server("cancel");
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return std::filesystem::exists(directory + "/cancel.started"); }));
	ASSERT_EQ(runtime_mcp_disconnect(instance, "cancel"), 0);
	EXPECT_TRUE(state("cancel", RUNTIME_MCP_DISCONNECTED));
	release("cancel");
	EXPECT_EQ(runtime_mcp_connect(instance, "cancel"), 0);
	EXPECT_EQ(runtime_mcp_discover(instance, "cancel", nullptr, nullptr, nullptr), 0);
	EXPECT_TRUE(state("cancel", RUNTIME_MCP_READY));
}

TEST_F(RuntimeMcpBackgroundTest, DiscoveryFailureDoesNotPublishPartialTools)
{
	server("invalid", 0, true, "invalid");
	release("invalid");
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return state("invalid", RUNTIME_MCP_FAILED); }));
	struct tool_desc desc = {};
	EXPECT_EQ(runtime_tool_find(instance, "mcp_invalid__valid", &desc), -ENOENT);
}

TEST_F(RuntimeMcpBackgroundTest, DisabledAutoConnectRemainsDisconnected)
{
	server("manual", 0, false);
	ASSERT_EQ(open(), 0);
	EXPECT_TRUE(state("manual", RUNTIME_MCP_DISCONNECTED));
	EXPECT_FALSE(std::filesystem::exists(directory + "/manual.started"));
}

TEST_F(RuntimeMcpBackgroundTest, SynchronousStartupRemainsAvailable)
{
	server("sync");
	release("sync");
	ASSERT_EQ(open(false), 0);
	EXPECT_TRUE(state("sync", RUNTIME_MCP_READY));
	struct tool_desc desc = {};
	EXPECT_EQ(runtime_tool_find(instance, "mcp_sync__echo", &desc), 0);
}

TEST_F(RuntimeMcpBackgroundTest, PublicationCollisionRollsBackNewTools)
{
	server("capacity", 0, true, "two");
	ASSERT_EQ(open(), 0);
	int before = runtime_tool_count(instance);
	struct tool_spec spec = {};
	spec.name = "mcp_capacity__second";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = [](const char *, struct tool_result *, void *) { return 0; };
	ASSERT_EQ(runtime_register_tool(instance, &spec), 0);
	release("capacity");
	ASSERT_TRUE(wait([&] { return state("capacity", RUNTIME_MCP_FAILED); }));
	struct runtime_mcp_status status = {};
	EXPECT_EQ(runtime_mcp_find(instance, "capacity", &status), 0);
	EXPECT_EQ(status.error_code, -EEXIST);
	struct tool_desc desc = {};
	EXPECT_EQ(runtime_tool_count(instance), before + 1);
	EXPECT_EQ(runtime_tool_find(instance, "mcp_capacity__first", &desc), -ENOENT);
	EXPECT_EQ(runtime_tool_find(instance, "mcp_capacity__second", &desc), 0);
}

#include <atomic>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

class McpSilentHttpServer {
	int listener = -1;
	std::atomic<bool> stopping{false};
	std::thread thread;
public:
	std::atomic<bool> request_seen{false};
	unsigned port = 0;

	McpSilentHttpServer()
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
		    listen(listener, 1) != 0)
			return;
		port = ntohs(address.sin_port);
		thread = std::thread([this] {
			int connection = -1;
			while (!stopping) {
				struct pollfd descriptor = {};
				descriptor.fd = connection < 0 ? listener : connection;
				descriptor.events = POLLIN;
				if (poll(&descriptor, 1, 100) <= 0)
					continue;
				if (connection < 0) {
					connection = accept(listener, nullptr, nullptr);
				} else {
					char bytes[BUFSIZ];
					if (read(connection, bytes, sizeof(bytes)) <= 0)
						break;
					request_seen = true;
				}
			}
			if (connection >= 0)
				close(connection);
		});
	}

	~McpSilentHttpServer()
	{
		stopping = true;
		if (thread.joinable())
			thread.join();
		if (listener >= 0)
			close(listener);
	}
};

TEST_F(RuntimeMcpBackgroundTest, HttpCloseCancelsBlockedInitialization)
{
	McpSilentHttpServer http;
	ASSERT_NE(http.port, 0U);
	{
		std::ofstream config(config_path);
		config << "[[mcp.servers]]\nname = \"http\"\ntransport = \"http\"\n"
			<< "url = \"http://127.0.0.1:" << http.port << "/mcp\"\nauto_connect = true\n";
	}
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return http.request_seen.load(); }));
	auto start = McpClock::now();
	runtime_close(instance);
	instance = nullptr;
	EXPECT_LT(McpClock::now() - start, std::chrono::seconds(3));
}

TEST_F(RuntimeMcpBackgroundTest, HttpInitializationHonorsConfiguredDeadline)
{
	McpSilentHttpServer http;
	ASSERT_NE(http.port, 0U);
	{
		std::ofstream config(config_path);
		config << "[[mcp.servers]]\nname = \"http\"\ntransport = \"http\"\n"
			<< "url = \"http://127.0.0.1:" << http.port
			<< "/mcp\"\nauto_connect = true\nconnect_timeout = 1\n";
	}
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return state("http", RUNTIME_MCP_FAILED); }));
	struct runtime_mcp_status status = {};
	ASSERT_EQ(runtime_mcp_find(instance, "http", &status), 0);
	EXPECT_EQ(status.error_code, -ETIMEDOUT);
	EXPECT_EQ(status.connected, 0);
}

TEST_F(RuntimeMcpBackgroundTest, RegistersPromptArgumentsAsObjectSchema)
{
	server("prompt", 0, true, "prompt");
	release("prompt");
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return state("prompt", RUNTIME_MCP_READY); }));
	struct tool_desc desc = {};
	ASSERT_EQ(runtime_tool_find(instance, "mcp_prompt_prompt__review", &desc), 0);
	cJSON *schema = cJSON_Parse(desc.input_schema);
	ASSERT_NE(schema, nullptr);
	cJSON *properties = cJSON_GetObjectItem(schema, "properties");
	cJSON *subject = cJSON_GetObjectItem(properties, "subject");
	EXPECT_TRUE(cJSON_IsObject(subject));
	cJSON *required = cJSON_GetObjectItem(schema, "required");
	EXPECT_EQ(cJSON_GetArraySize(required), 1);
	cJSON_Delete(schema);
}

TEST_F(RuntimeMcpBackgroundTest, ToolSnapshotsRemainReadableDuringATurn)
{
	server("manual", 0, false);
	ASSERT_EQ(open(), 0);
	pthread_mutex_lock(&instance->context.execution_lock);
	EXPECT_GT(runtime_tool_count(instance), 0);
	struct tool_desc desc = {};
	EXPECT_EQ(runtime_tool_info(instance, 0, &desc), 0);
	pthread_mutex_unlock(&instance->context.execution_lock);
}

TEST_F(RuntimeMcpBackgroundTest, SynchronousDiscoveryFailureIsVisible)
{
	server("invalid", 0, true, "invalid");
	release("invalid");
	ASSERT_EQ(open(false), 0);
	EXPECT_TRUE(state("invalid", RUNTIME_MCP_FAILED));
	struct runtime_mcp_status status = {};
	EXPECT_EQ(runtime_mcp_find(instance, "invalid", &status), 0);
	EXPECT_LT(status.error_code, 0);
}

TEST_F(RuntimeMcpBackgroundTest, DiscoveryListsOwnStringsAfterArenaIsDestroyed)
{
	server("lists", 0, true, "prompt");
	release("lists");
	ASSERT_EQ(open(false), 0);
	struct mcp_tool_desc *tools = nullptr;
	struct mcp_prompt_desc *prompts = nullptr;
	int tool_count = 0, prompt_count = 0;
	ASSERT_EQ(runtime_mcp_list_tools(instance, "lists", &tools, &tool_count), 0);
	ASSERT_EQ(runtime_mcp_list_prompts(instance, "lists", &prompts, &prompt_count), 0);
	ASSERT_EQ(tool_count, 1);
	ASSERT_EQ(prompt_count, 1);
	EXPECT_STREQ(tools[0].name, "echo");
	EXPECT_STREQ(tools[0].description, "Mock echo");
	EXPECT_STREQ(prompts[0].name, "review");
	EXPECT_NE(strstr(prompts[0].arguments_schema, "subject"), nullptr);
	runtime_mcp_list_free(tools);
	runtime_mcp_list_free(prompts);
}

TEST_F(RuntimeMcpBackgroundTest, PeekabooSizedSchemasDoNotDisconnectServer)
{
	server("peekaboo", 0, true, "large");
	const std::string prefix = "{\"type\":\"object\",\"description\":\"";
	const std::string suffix = "\"}";
	std::string verify = prefix + std::string(5200 - prefix.size() - suffix.size(), 'v') + suffix;
	std::string browser = prefix + std::string(5649 - prefix.size() - suffix.size(), 'b') + suffix;
	{
		std::ofstream response(directory + "/large.json");
		response << "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"verify_state\",\"inputSchema\":"
			<< verify << "},{\"name\":\"browser\",\"inputSchema\":" << browser << "}]}}\n";
	}
	release("peekaboo");
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] { return state("peekaboo", RUNTIME_MCP_READY); }));
	struct runtime_mcp_status status = {};
	ASSERT_EQ(runtime_mcp_find(instance, "peekaboo", &status), 0);
	EXPECT_EQ(status.connected, 1);
	EXPECT_EQ(status.error_code, 0);
	struct tool_desc desc = {};
	ASSERT_EQ(runtime_tool_find(instance, "mcp_peekaboo__verify_state", &desc), 0);
	EXPECT_EQ(std::string(desc.input_schema), verify);
	ASSERT_EQ(runtime_tool_find(instance, "mcp_peekaboo__browser", &desc), 0);
	EXPECT_EQ(std::string(desc.input_schema), browser);
}

TEST_F(RuntimeMcpBackgroundTest, PinnedVersionPassesThroughRuntimeConfiguration)
{
	server("pinned");
	{
		std::ofstream config(config_path, std::ios::app);
		config << "protocol_version = \"2025-06-18\"\n";
	}
	release("pinned");
	ASSERT_EQ(open(false), 0);
	EXPECT_TRUE(state("pinned", RUNTIME_MCP_READY));
	auto *client = mcp_registry_get(&instance->context.mcp, "pinned");
	ASSERT_NE(client, nullptr);
	EXPECT_STREQ(client->config.protocol_version, "2025-06-18");
	EXPECT_STREQ(client->negotiated_version, "2025-06-18");
}

TEST_F(RuntimeMcpBackgroundTest, MissingAdvertisedPromptHandlerPreservesTools)
{
	for (bool background : {false, true}) {
		SCOPED_TRACE(background);
		server("missing_prompt", 0, true, "missing_prompt");
		release("missing_prompt");
		ASSERT_EQ(open(background), 0);
		ASSERT_TRUE(wait([&] { return state("missing_prompt", RUNTIME_MCP_READY); }));
		struct runtime_mcp_status status = {};
		ASSERT_EQ(runtime_mcp_find(instance, "missing_prompt", &status), 0);
		EXPECT_EQ(status.connected, 1);
		EXPECT_EQ(status.error_code, 0);
		EXPECT_EQ(status.supports_tools, 1);
		EXPECT_EQ(status.supports_prompts, 0);
		struct tool_desc desc = {};
		EXPECT_EQ(runtime_tool_find(instance, "mcp_missing_prompt__echo", &desc), 0);
		runtime_close(instance);
		instance = nullptr;
		std::filesystem::remove(config_path);
	}
}

TEST_F(RuntimeMcpBackgroundTest, LiveServerPublishesToolsAndBecomesReady)
{
	const char *command = getenv("MORPH_TEST_MCP_COMMAND");
	if (!command || !command[0])
		GTEST_SKIP() << "Set MORPH_TEST_MCP_COMMAND to a stdio MCP server executable";
	cJSON *value = cJSON_CreateString(command);
	char *encoded = cJSON_PrintUnformatted(value);
	cJSON_Delete(value);
	ASSERT_NE(encoded, nullptr);
	{
		std::ofstream config(config_path);
		config << "[[mcp.servers]]\nname = \"live\"\ntransport = \"stdio\"\ncommand = "
			<< encoded << "\nauto_connect = true\nconnect_timeout = 90\n";
	}
	free(encoded);
	ASSERT_EQ(open(), 0);
	ASSERT_TRUE(wait([&] {
		return state("live", RUNTIME_MCP_READY) || state("live", RUNTIME_MCP_FAILED);
	}, 90000));
	struct runtime_mcp_status status = {};
	ASSERT_EQ(runtime_mcp_find(instance, "live", &status), 0);
	ASSERT_EQ(status.state, RUNTIME_MCP_READY) << morph_strerror(status.error_code);
	EXPECT_EQ(status.connected, 1);
	EXPECT_EQ(status.error_code, 0);
	int published = 0;
	for (int i = 0; i < runtime_tool_count(instance); i++) {
		struct tool_desc descriptor = {};
		ASSERT_EQ(runtime_tool_info(instance, i, &descriptor), 0);
		if (strncmp(descriptor.name, "mcp_live__", strlen("mcp_live__")) == 0)
			published++;
	}
	struct mcp_tool_desc *tools = nullptr;
	int count = 0;
	int rc = runtime_mcp_list_tools(instance, "live", &tools, &count);
	runtime_mcp_list_free(tools);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(count, published);
	RecordProperty("state", "ready");
	RecordProperty("tools", published);
	RecordProperty("supports_prompts", status.supports_prompts);
	RecordProperty("protocol", status.negotiated_version);
}
