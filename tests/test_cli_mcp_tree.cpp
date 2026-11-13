#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/presentation/mcp_tree.h"
#include "sapi/cli/presentation/style.h"
#include "runtime/runtime.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
}

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

static std::string mcp_tree_plain(const char *source)
{
	char *plain NB_CLEANUP(nb_charp_cleanup) =
		morph_terminal_strip_ansi_dup(source, std::strlen(source), nullptr);
	return plain ? plain : "";
}

static std::vector<std::string> mcp_tree_lines(const std::string &text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		lines.emplace_back(text.substr(start, end - start));
		start = end + 1;
	}
	return lines;
}

static std::string mcp_tree_unwrap(const std::string &text)
{
	std::string result;
	for (const auto &line : mcp_tree_lines(text)) {
		size_t offset = 0;
		while (offset < line.size()) {
			unsigned cp = 0;
			size_t bytes = 0;
			if (!nb_utf8_read(line.data() + offset, line.size() - offset, &cp, &bytes))
				break;
			if (cp != ' ' && cp != 0x2502u && cp != 0x251cu && cp != 0x2514u)
				break;
			offset += bytes;
		}
		result.append(line, offset, std::string::npos);
	}
	return result;
}

static void mcp_tree_expect_width(const std::string &output, int columns)
{
	for (const auto &line : mcp_tree_lines(output)) {
		EXPECT_LE(nb_utf8_width_cstr(line.c_str()), static_cast<size_t>(columns))
			<< columns << ": " << line;
		EXPECT_EQ(nb_utf8_validate(nb_str_from_cstr(line.c_str()), NULL), NB_UTF8_OK)
			<< line;
	}
}

TEST(CliMcpTreeTest, EmptyReportHasARealTreeChildAndNoTablePadding)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	ASSERT_EQ(cli_mcp_tree_render(&output, nullptr, 0, 60, 0), 0);
	std::string plain = nb_buf_cstr(&output);
	EXPECT_NE(plain.find("MCP servers (0)"), std::string::npos);
	EXPECT_NE(plain.find("└ "), std::string::npos);
	EXPECT_NE(plain.find("none"), std::string::npos);
	EXPECT_EQ(plain.find('\033'), std::string::npos);
	mcp_tree_expect_width(plain, 60);
}

TEST(CliMcpTreeTest, ServerPeersUseCanonicalMarkersAtTheLeftmostColumn)
{
	std::vector<struct runtime_mcp_status> servers(8);
	const enum runtime_mcp_state states[] = {
		RUNTIME_MCP_DISCONNECTED, RUNTIME_MCP_CONNECTING,
		RUNTIME_MCP_DISCOVERING, RUNTIME_MCP_REGISTERING,
		RUNTIME_MCP_READY, RUNTIME_MCP_FAILED,
		RUNTIME_MCP_FAILED, RUNTIME_MCP_FAILED};
	const char *markers[] = {CLI_FEED_PENDING, CLI_FEED_RUNNING, CLI_FEED_RUNNING,
		CLI_FEED_RUNNING, CLI_FEED_DONE, CLI_FEED_FAILED,
		CLI_FEED_CANCELLED, CLI_FEED_FAILED};
	const char *state_names[] = {"disconnected", "connecting", "discovering",
		"registering", "ready", "failed", "cancelled", "failed"};
	for (size_t i = 0; i < servers.size(); i++) {
		std::string name = "fixture_" + std::to_string(i);
		std::strcpy(servers[i].config.name, name.c_str());
		servers[i].config.transport =
			i % 2 ? MCP_TRANSPORT_STREAMABLE_HTTP : MCP_TRANSPORT_STDIO;
		servers[i].state = states[i];
	}
	servers[5].error_code = NB_ERR_IO;
	servers[6].error_code = NB_ERR_CANCELLED;
	servers[7].error_code = NB_ERR_TIMEOUT;
	for (int width : {120, 80, 60}) {
		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
		ASSERT_EQ(cli_mcp_tree_render(&output, servers.data(), servers.size(),
			width, 0), 0);
		std::string plain = nb_buf_cstr(&output);
		auto lines = mcp_tree_lines(plain);
		EXPECT_NE(plain.find("MCP servers (8)"), std::string::npos);
		for (size_t i = 0; i < servers.size(); i++) {
			std::string name = "fixture_" + std::to_string(i);
			bool found = false;
			for (const auto &line : lines) {
				if (line.find(name) == std::string::npos)
					continue;
				found = true;
				EXPECT_EQ(line.find(std::string(markers[i]) + " " + name), 0u);
				EXPECT_NE(line.find(i % 2 ? "[http]" : "[stdio]"),
					std::string::npos);
				EXPECT_NE(line.find(state_names[i]), std::string::npos) << line;
			}
			EXPECT_TRUE(found) << name << '\n' << plain;
		}
		EXPECT_NE(plain.find("TIMEOUT"), std::string::npos);
		mcp_tree_expect_width(plain, width);
	}
}

TEST(CliMcpTreeTest, ConnectedMetadataAndCapabilitiesRemainExplicitChildren)
{
	std::vector<struct runtime_mcp_status> servers(1);
	auto &server = servers.front();
	std::strcpy(server.config.name, "fixture_ready");
	server.config.transport = MCP_TRANSPORT_STDIO;
	server.connected = 1;
	server.state = RUNTIME_MCP_READY;
	server.supports_tools = 1;
	server.supports_resources = 0;
	server.supports_prompts = 1;
	server.elapsed_ms = 1234;
	std::strcpy(server.server_name, "Fixture server");
	std::strcpy(server.server_version, "2.4.0");
	std::strcpy(server.negotiated_version, "2025-06-18");
	for (int width : {120, 80, 60}) {
		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
		ASSERT_EQ(cli_mcp_tree_render(&output, servers.data(), 1, width, 0), 0);
		std::string plain = nb_buf_cstr(&output);
		EXPECT_NE(plain.find("Fixture server"), std::string::npos);
		EXPECT_NE(plain.find("2.4.0"), std::string::npos);
		EXPECT_NE(plain.find("2025-06-18"), std::string::npos);
		EXPECT_NE(plain.find("1234"), std::string::npos);
		EXPECT_NE(plain.find("Capabilities"), std::string::npos);
		EXPECT_NE(plain.find("Tools: yes"), std::string::npos);
		EXPECT_NE(plain.find("Resources: no"), std::string::npos);
		EXPECT_NE(plain.find("Prompts: yes"), std::string::npos);
		EXPECT_NE(plain.find("├ "), std::string::npos);
		EXPECT_NE(plain.find("└ "), std::string::npos);
		EXPECT_NE(plain.find("│ "), std::string::npos);
		for (const auto &line : mcp_tree_lines(plain)) {
			if (line.find("Tools:") != std::string::npos ||
			    line.find("Resources:") != std::string::npos ||
			    line.find("Prompts:") != std::string::npos)
				EXPECT_NE(line.find("│ "), std::string::npos) << line;
		}
		mcp_tree_expect_width(plain, width);
	}
}

TEST(CliMcpTreeTest, UnicodeValuesWrapWithTreeContinuationsWithoutLosingMetadata)
{
	std::vector<struct runtime_mcp_status> servers(1);
	auto &server = servers.front();
	std::strcpy(server.config.name, "unicode_server");
	server.connected = 1;
	server.state = RUNTIME_MCP_READY;
	std::string value;
	for (int i = 0; i < 12; i++)
		value += "中文🙂";
	ASSERT_LT(value.size(), sizeof(server.server_name));
	std::strcpy(server.server_name, value.c_str());
	std::strcpy(server.server_version, "unicode-version");
	std::strcpy(server.negotiated_version, "2025-06-18");
	for (int width : {120, 80, 60}) {
		for (int styled : {0, 1}) {
			nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
			ASSERT_EQ(cli_mcp_tree_render(&output, servers.data(), 1,
				width, styled), 0);
			std::string plain = mcp_tree_plain(nb_buf_cstr(&output));
			EXPECT_NE(mcp_tree_unwrap(plain).find(value), std::string::npos) << plain;
			EXPECT_EQ(plain.find("…"), std::string::npos) << plain;
			mcp_tree_expect_width(plain, width);
			if (!styled)
				EXPECT_EQ(std::string(nb_buf_cstr(&output)).find('\033'),
					  std::string::npos);
		}
	}
}

TEST(CliMcpTreeTest, StylingDoesNotChangeTreeTextAndErrorsKeepBoldFailureStyle)
{
	std::vector<struct runtime_mcp_status> servers(1);
	auto &server = servers.front();
	std::strcpy(server.config.name, "failed_server");
	server.state = RUNTIME_MCP_FAILED;
	server.error_code = NB_ERR_IO;
	nb_buf plain NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf styled NB_CLEANUP(nb_buf_cleanup) = {0};
	ASSERT_EQ(cli_mcp_tree_render(&plain, servers.data(), 1, 80, 0), 0);
	ASSERT_EQ(cli_mcp_tree_render(&styled, servers.data(), 1, 80, 1), 0);
	std::string colored = nb_buf_cstr(&styled);
	EXPECT_EQ(mcp_tree_plain(colored.c_str()), nb_buf_cstr(&plain));
	size_t marker = colored.find(CLI_FEED_FAILED);
	ASSERT_NE(marker, std::string::npos);
	EXPECT_NE(colored.substr(0, marker).find(ANSI_BOLD), std::string::npos);
	EXPECT_NE(colored.substr(0, marker).find(ANSI_RED), std::string::npos);
	EXPECT_NE(
		mcp_tree_plain(colored.c_str())
			.find(nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, NB_ERR_IO)),
		std::string::npos);
}

TEST(CliMcpTreeTest, ServerTextCannotInjectTerminalControlsAndCredentialsStayPrivate)
{
	std::vector<struct runtime_mcp_status> servers(1);
	auto &server = servers.front();
	std::strcpy(server.config.name, "fixture\033[2Jname");
	server.connected = 1;
	server.state = RUNTIME_MCP_READY;
	std::strcpy(server.server_name, "Fixture\033]52;c;private\a\nserver");
	std::strcpy(server.server_version, "version\r1");
	std::strcpy(server.negotiated_version, "2025-06-18");
	std::strcpy(server.config.http_url, "https://secret.invalid/private_endpoint");
	std::strcpy(server.config.http_auth_token_env, "MCP_TOPSECRET_TOKEN");
	std::strcpy(server.config.env_keys[0], "PRIVATE_KEY");
	std::strcpy(server.config.env_vals[0], "MCP_TOPSECRET_VALUE");
	server.config.env_count = 1;
	for (int styled : {0, 1}) {
		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
		ASSERT_EQ(cli_mcp_tree_render(&output, servers.data(), 1, 60, styled), 0);
		std::string text = nb_buf_cstr(&output);
		EXPECT_EQ(text.find("\033[2J"), std::string::npos);
		EXPECT_EQ(text.find("\033]52;"), std::string::npos);
		EXPECT_EQ(text.find('\r'), std::string::npos);
		EXPECT_EQ(text.find("private_endpoint"), std::string::npos);
		EXPECT_EQ(text.find("MCP_TOPSECRET"), std::string::npos);
		EXPECT_NE(mcp_tree_plain(text.c_str()).find("fixture"), std::string::npos);
		mcp_tree_expect_width(mcp_tree_plain(text.c_str()), 60);
	}
}

TEST(CliMcpTreeTest, ResourceItemsUseSiblingBranchesAndKeepDescriptionsAndUrisWhenWrapped)
{
	const std::string description =
		"Read this document without changing its original contents or its access policy.";
	const std::string uri = "fixture://documents/long-path/" + std::string(100, 'x');
	const struct cli_mcp_tree_item items[] = {
		{"First document", description.c_str(), uri.c_str()},
		{"Second document", "Read another document", "fixture://documents/second"}};
	for (int width : {120, 80, 60}) {
		nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
		ASSERT_EQ(cli_mcp_tree_items_render(&output, "resources", "fixture",
			items, 2, width, 0), 0);
		std::string plain = nb_buf_cstr(&output);
		EXPECT_NE(plain.find("MCP resources for 'fixture' (2)"), std::string::npos);
		EXPECT_NE(plain.find("├ First document"), std::string::npos);
		EXPECT_NE(plain.find("└ Second document"), std::string::npos);
		EXPECT_NE(mcp_tree_unwrap(plain).find(description), std::string::npos) << plain;
		EXPECT_NE(mcp_tree_unwrap(plain).find(uri), std::string::npos) << plain;
		EXPECT_NE(plain.find("│ "), std::string::npos);
		EXPECT_EQ(plain.find('\033'), std::string::npos);
		mcp_tree_expect_width(plain, width);
	}
}

TEST(CliMcpTreeTest, InvalidArgumentsDoNotAppendPartialTreeOutput)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	ASSERT_EQ((nb_buf_append_cstr(&output, "retained") ? 0 : NB_ERR_NOMEM), 0);
	EXPECT_EQ(cli_mcp_tree_render(&output, nullptr, 1, 80, 0), NB_ERR_INVAL);
	EXPECT_EQ(cli_mcp_tree_render(&output, nullptr, 0, 0, 0), NB_ERR_INVAL);
	EXPECT_EQ(cli_mcp_tree_items_render(&output, "tools", "fixture", nullptr, 1, 80, 0),
		  NB_ERR_INVAL);
	EXPECT_STREQ(nb_buf_cstr(&output), "retained");
}
