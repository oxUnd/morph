#ifndef MCP_H
#define MCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/types.h>

#include "http/client.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/strmap.h"
#include "agent/tool.h"

#define MCP_PROTOCOL_VERSION   "2026-07-28"
#define MCP_LEGACY_PROTOCOL_VERSION "2025-11-25"
#define MCP_NAME_MAX           128
#define MCP_CMD_MAX            64
#define MCP_CMD_ARG_MAX        256
#define MCP_ENV_MAX            16
#define MCP_ENV_VAL_MAX        PATH_MAX
#define MCP_MAX_SERVERS        32

enum mcp_transport_type {
	MCP_TRANSPORT_STDIO,
	MCP_TRANSPORT_STREAMABLE_HTTP,
};

/* Discovery arrays own every string until the matching descriptor free call. */
/* MCP tool descriptor returned by tools/list */
struct morph_log;

struct mcp_tool_desc {
	const char *name;
	const char *title;
	const char *description;
	const char *input_schema;
	/* Only an explicit boolean readOnlyHint grants read-only classification. */
	int read_only;
};

/* MCP resource descriptor returned by resources/list */
struct mcp_resource_desc {
	const char *uri;
	const char *name;
	const char *description;
	const char *mime_type;
};

/* MCP prompt descriptor returned by prompts/list */
struct mcp_prompt_desc {
	const char *name;
	const char *description;
	const char *arguments_schema;
};

/* One MCP server configuration */
struct mcp_server_config {
	struct morph_log *logger;
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
	char http_url[PATH_MAX];
	char http_auth_token_env[64];
	/* Empty or "auto" probes; a version pins its adapter without downgrade. */
	char protocol_version[32];
	/* startup */
	int auto_connect;
	int connect_timeout;
};

struct mcp_protocol_adapter;

/* MCP client -- manages one server connection */
struct mcp_client {
	struct mcp_server_config config;
	int connected;
	int connecting;
	/* Registry discovery status, maintained by the runtime facade. */
	int discovery_error;
	int capabilities_registered;
	/* Optional startup interruption; owned by the connection worker. */
	int (*interrupt_check)(void *);
	void *interrupt_user_data;
	const struct mcp_protocol_adapter *protocol;
	morph_strmap_t tool_schemas;
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
	morph_buf_t read_buffer;
	/* http transport state */
	struct http_session session;
	char *session_id;
	/* JSON-RPC id counter */
	int next_req_id;
	/* thread safety */
	pthread_mutex_t lock;
};

/* Registry of all MCP server clients */
struct mcp_registry {
	struct morph_log *logger;
	struct mcp_client *servers[MCP_MAX_SERVERS];
	int count;
	morph_strmap_t by_name;
};

/* ----- Registry ----- */

void mcp_registry_init(struct morph_log *logger, struct mcp_registry *reg);
int mcp_registry_add(struct mcp_registry *reg, const struct mcp_server_config *cfg);
/* Borrowed client until registry cleanup; callers exclude concurrent cleanup. */
struct mcp_client *mcp_registry_borrow(struct mcp_registry *reg, const char *name);
int mcp_registry_count(struct mcp_registry *reg);
void mcp_registry_cleanup(struct mcp_registry *reg);

/* ----- Lifecycle ----- */

int mcp_connect_stdio(struct mcp_client *client);
int mcp_connect_http(struct mcp_client *client);
int mcp_initialize(struct mcp_client *client);
void mcp_disconnect(struct mcp_client *client);

/* Lazy-connect wrapper */
int mcp_ensure_connected(struct mcp_client *client);

/* Discovery returns independent owned arrays; each matching free releases all
 * string fields and the array. Outputs must start empty and remain so on error. */
void mcp_tool_descs_free(struct mcp_tool_desc *items, int count);
void mcp_resource_descs_free(struct mcp_resource_desc *items, int count);
void mcp_prompt_descs_free(struct mcp_prompt_desc *items, int count);

/* ----- Tools ----- */

int mcp_list_tools_copy(struct mcp_client *client, struct mcp_tool_desc **out_tools,
			int *out_count);
int mcp_call_tool(struct mcp_client *client, const char *name, const char *args_json,
		  struct tool_result *out_result);

/* ----- Resources ----- */

int mcp_list_resources_copy(struct mcp_client *client, struct mcp_resource_desc **out_res,
			    int *out_count);
int mcp_read_resource(struct mcp_client *client, const char *uri, struct tool_result *out_content);

/* ----- Prompts ----- */

int mcp_list_prompts_copy(struct mcp_client *client, struct mcp_prompt_desc **out_prompts,
			  int *out_count);
int mcp_get_prompt(struct mcp_client *client, const char *name, const char *args_json,
		   struct tool_result *out_result);

/* ----- Utilities ----- */

int mcp_ping(struct mcp_client *client);
int mcp_test_connection(const struct mcp_server_config *cfg,
			char *message, size_t message_cap);
/* Independent malloc-owned text; caller frees it. */
char *mcp_http_sse_json_create(const char *raw, size_t len);
int mcp_http_url_uses_auth_token(const char *url);
/* *out_url starts NULL; occupied: -EEXIST. Success creates malloc-owned text;
 * failure preserves the output. */
int mcp_http_request_url_create(const struct mcp_server_config *cfg, char **out_url);

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
