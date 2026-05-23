#ifndef MCP_H
#define MCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <pthread.h>
#include <sys/types.h>

#include "util/arena.h"
#include "agent/tool.h"

#define MCP_PROTOCOL_VERSION   "2025-06-18"
#define MCP_NAME_MAX           128
#define MCP_DESC_MAX           1024
#define MCP_SCHEMA_MAX         4096
#define MCP_URI_MAX            1024
#define MCP_CMD_MAX            64
#define MCP_CMD_ARG_MAX        256
#define MCP_ENV_MAX            16
#define MCP_ENV_VAL_MAX        512
#define MCP_MAX_SERVERS        32
#define MCP_JSON_BUF_MAX       65536
#define MCP_READ_BUF_MAX       131072

enum mcp_transport_type {
	MCP_TRANSPORT_STDIO,
	MCP_TRANSPORT_STREAMABLE_HTTP,
};

/* MCP tool descriptor returned by tools/list */
struct mcp_tool_desc {
	char name[MCP_NAME_MAX];
	char title[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char input_schema[MCP_SCHEMA_MAX];
};

/* MCP resource descriptor returned by resources/list */
struct mcp_resource_desc {
	char uri[MCP_URI_MAX];
	char name[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char mime_type[64];
};

/* MCP prompt descriptor returned by prompts/list */
struct mcp_prompt_desc {
	char name[MCP_NAME_MAX];
	char description[MCP_DESC_MAX];
	char arguments_schema[MCP_SCHEMA_MAX];
};

/* One MCP server configuration */
struct mcp_server_config {
	char name[MCP_NAME_MAX];
	enum mcp_transport_type transport;
	/* stdio */
	char command[256];
	char cmd_args[MCP_CMD_MAX][MCP_CMD_ARG_MAX];
	int cmd_args_count;
	char env_keys[MCP_ENV_MAX][64];
	char env_vals[MCP_ENV_MAX][MCP_ENV_VAL_MAX];
	int env_count;
	/* http */
	char http_url[512];
	char http_auth_token_env[64];
	/* startup */
	int auto_connect;
	int connect_timeout;
};

/* MCP client -- manages one server connection */
struct mcp_client {
	struct mcp_server_config config;
	int connected;
	int connecting;
	char negotiated_version[32];
	char server_name[MCP_NAME_MAX];
	char server_version[64];
	/* server capabilities */
	int supports_tools;
	int supports_resources;
	int supports_prompts;
	int tools_list_changed;
	/* stdio transport state */
	pid_t server_pid;
	int stdin_fd;
	int stdout_fd;
	/* http transport state */
	void *curl_handle;
	char session_id[128];
	/* JSON-RPC id counter */
	int next_req_id;
	/* thread safety */
	pthread_mutex_t lock;
};

/* Registry of all MCP server clients */
struct mcp_registry {
	struct mcp_client *servers[MCP_MAX_SERVERS];
	int count;
};

/* ----- Registry ----- */

void mcp_registry_init(struct mcp_registry *reg);
int mcp_registry_add(struct mcp_registry *reg, const struct mcp_server_config *cfg);
struct mcp_client *mcp_registry_get(struct mcp_registry *reg, const char *name);
int mcp_registry_count(struct mcp_registry *reg);
void mcp_registry_cleanup(struct mcp_registry *reg);

/* ----- Lifecycle ----- */

int mcp_connect_stdio(struct mcp_client *client);
int mcp_connect_http(struct mcp_client *client);
int mcp_initialize(struct mcp_client *client);
void mcp_disconnect(struct mcp_client *client);

/* Lazy-connect wrapper */
int mcp_ensure_connected(struct mcp_client *client);

/* ----- Tools ----- */

int mcp_list_tools(struct mcp_client *client, struct arena *arena,
		   struct mcp_tool_desc **out_tools, int *out_count);
int mcp_call_tool(struct mcp_client *client, struct arena *arena, const char *name,
		  const char *args_json, char **out_result_json);

/* ----- Resources ----- */

int mcp_list_resources(struct mcp_client *client, struct arena *arena,
		       struct mcp_resource_desc **out_res, int *out_count);
int mcp_read_resource(struct mcp_client *client, struct arena *arena,
		      const char *uri, char **out_content);

/* ----- Prompts ----- */

int mcp_list_prompts(struct mcp_client *client, struct arena *arena,
		     struct mcp_prompt_desc **out_prompts, int *out_count);
int mcp_get_prompt(struct mcp_client *client, struct arena *arena, const char *name,
		   const char *args_json, char **out_result);

/* ----- Utilities ----- */

int mcp_ping(struct mcp_client *client);

/* ----- morph tool registry integration ----- */

int mcp_register_server_tools(struct mcp_client *client,
			      struct tool_registry *reg);
int mcp_register_server_resources(struct mcp_client *client,
				  struct tool_registry *reg);
int mcp_register_server_prompts(struct mcp_client *client,
				struct tool_registry *reg);

/* ----- Config helpers ----- */

int mcp_config_to_server_config(const void *config_mcp_server,
				struct mcp_server_config *out);

#ifdef __cplusplus
}
#endif

#endif
