#ifndef MCP_H
#define MCP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <nb/err.h>
#include <stddef.h>
#include <stdint.h>
#include <nb/thread.h>
#include <sys/types.h>

#include "http/request.h"
#include <nb/cleanup.h>
#include "cJSON.h"
#include <nb/map.h>
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
struct nb_log;

struct mcp_tool_desc {
	const char *name;
	const char *title;
	const char *description;
	const char *input_schema;
	const char *output_schema;
	const char *metadata;
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
	struct nb_log *logger;
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
struct mcp_client;
struct mcp_listener;

/* Borrowed host callbacks. Request results start empty and transfer an owned
 * JSON object on success. Context belongs to the active caller, never a model.
 * Callbacks must not recursively invoke this client's transport. */
typedef int (*mcp_client_request_fn)(const char *server, const char *method, const cJSON *params,
				     const void *context, cJSON **result, void *user_data);
typedef void (*mcp_client_notification_fn)(const char *server, const char *method,
					   const cJSON *params, const void *context,
					   void *user_data);

struct mcp_client_handlers {
	mcp_client_request_fn elicitation;
	mcp_client_request_fn sampling;
	int sampling_tools;
	mcp_client_request_fn roots;
	mcp_client_notification_fn notification;
	void *user_data;
};

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
	const struct nb_cancel *shutdown_cancel;
	const nb_atomic_int *catalog_cancelled;
	nb_atomic_int refreshing_catalog;
	const struct mcp_protocol_adapter *protocol;
	nb_map *tool_schemas;
	char negotiated_version[32];
	char server_name[MCP_NAME_MAX];
	char server_version[64];
	char *instructions;
	struct mcp_client_handlers handlers;
	const void *request_context;
	int64_t interaction_wait_ms;
	int active_request_id;
	int peer_cancelled;
	/* server capabilities */
	int supports_tools;
	int supports_resources;
	int supports_prompts;
	int supports_catalog_notifications;
	nb_atomic_int tools_list_changed;
	nb_atomic_int resources_list_changed;
	nb_atomic_int prompts_list_changed;
	int subscription_id;
	cJSON *subscription_filters;
	cJSON *subscription_accepted;
	nb_atomic_int subscription_acknowledged;
	nb_atomic_int subscription_ended;
	struct mcp_client *notification_target;
	struct mcp_listener *listener;
	/* stdio transport state */
	struct nb_process *child;
	pid_t server_pid;
	int stdin_fd;
	int stdout_fd;
	nb_buf read_buffer;
	/* http transport state */
	nb_http_client *http_client;
	int http_status;
	char *session_id;
	char log_level[16];
	/* JSON-RPC id counter */
	int next_req_id;
	/* thread safety */
	nb_mutex *lock;
};

/* Registry of all MCP server clients */
struct mcp_registry {
	struct nb_log *logger;
	struct mcp_client *servers[MCP_MAX_SERVERS];
	int count;
	nb_map *by_name;
};

/* ----- Registry ----- */

void mcp_registry_init(struct nb_log *logger, struct mcp_registry *reg);
int mcp_registry_add(struct mcp_registry *reg, const struct mcp_server_config *cfg);
/* Borrowed client until registry cleanup; callers exclude concurrent cleanup. */
struct mcp_client *mcp_registry_borrow(struct mcp_registry *reg, const char *name);
int mcp_registry_count(struct mcp_registry *reg);
void mcp_registry_cleanup(struct mcp_registry *reg);
int mcp_tool_instructions_append(nb_buf *text, void *registry);
int mcp_refresh_catalog(struct mcp_registry *registry, struct tool_registry *tools);

/* ----- Lifecycle ----- */

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
/* Arrays/objects are independent cJSON owners; outputs must start NULL. */
int mcp_list_resource_templates_copy(struct mcp_client *client, cJSON **out);
int mcp_complete_copy(struct mcp_client *client, const cJSON *params, cJSON **out);
int mcp_resource_subscribe(struct mcp_client *client, const char *uri);
int mcp_resource_unsubscribe(struct mcp_client *client, const char *uri);
int mcp_set_log_level(struct mcp_client *client, const char *level);
/* Modern notification subscriptions use explicit filters. Legacy HTTP opens
 * its optional GET stream; legacy stdio already carries notifications. */
int mcp_listen_notifications(struct mcp_client *client, const cJSON *filters);
int mcp_listen_catalog_changes(struct mcp_client *client);
void mcp_stop_notifications(struct mcp_client *client);
/* Drain available stdio notifications at an application boundary. */
int mcp_poll_notifications(struct mcp_client *client, int timeout_ms);

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
/* *out_url starts NULL; occupied: NB_ERR_EXISTS. Success creates malloc-owned text;
 * failure preserves the output. */
int mcp_http_request_url_create(const struct mcp_server_config *cfg, char **out_url);

/* ----- morph tool registry integration ----- */

int mcp_register_server_tools(struct mcp_client *client,
			      struct tool_registry *reg);
int mcp_register_server_resources(struct mcp_client *client,
				  struct tool_registry *reg);
int mcp_register_server_prompts(struct mcp_client *client,
				struct tool_registry *reg);

#ifdef __cplusplus
}
#endif

#endif
