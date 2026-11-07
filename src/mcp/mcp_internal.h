#ifndef MORPH_MCP_INTERNAL_H
#define MORPH_MCP_INTERNAL_H

#include "mcp.h"
#include "util/array.h"

#define MCP_STDIO_MAX_FRAME_BYTES (16u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/* Request strings belong to the encode arena. Clean up the header array after
 * each exchange. Reply strings are heap-owned and released with reply_cleanup. */
struct mcp_wire_request {
	int id;
	const char *json;
	morph_array_t headers;
};

struct mcp_wire_reply {
	char *json;
	char *session_id;
	long http_status;
};

struct mcp_protocol_adapter {
	const char *version;
	int per_request;
};

const struct mcp_protocol_adapter *mcp_protocol_find(const char *version);
int mcp_protocol_encode(struct mcp_client *client,
	const struct mcp_protocol_adapter *adapter, int id, const char *method,
	const char *params, struct arena *arena, struct mcp_wire_request *out);
int mcp_protocol_initialize(struct mcp_client *client);
void mcp_protocol_reset(struct mcp_client *client);
/* A validated remote MethodNotFound is returned as -ENOSYS; other remote
 * errors remain protocol errors. The optional result retains the RPC error. */
int mcp_request(struct mcp_client *client, const char *method,
	const char *params, char **result);
int mcp_request_timeout_ms(const struct mcp_client *client);

/* Schema validation/cache and mirrored HTTP headers belong to the codec. */
int mcp_protocol_cache_tool(struct mcp_client *client, const char *name,
	const cJSON *schema);
int mcp_protocol_tool_headers(struct mcp_client *client, const cJSON *params,
	struct arena *arena, morph_array_t *headers);
int mcp_protocol_add_header(struct arena *arena, morph_array_t *headers,
	const char *name, const char *value);

/* Transport primitives never interpret protocol versions or result bodies. */
int mcp_stdio_connect(struct mcp_client *client);
void mcp_stdio_disconnect(struct mcp_client *client);
int mcp_stdio_exchange(struct mcp_client *client,
	const struct mcp_wire_request *request, int timeout_ms,
	struct mcp_wire_reply *reply);
int mcp_http_connect(struct mcp_client *client);
void mcp_http_disconnect(struct mcp_client *client);
int mcp_http_exchange(struct mcp_client *client,
	const struct mcp_wire_request *request, int timeout_ms,
	struct mcp_wire_reply *reply);
void mcp_wire_reply_cleanup(struct mcp_wire_reply *reply);

/* Decode validates JSON-RPC framing and correlation, preserving remote errors. */
int mcp_protocol_decode(struct morph_log *logger, const struct mcp_wire_reply *reply,
	int id, cJSON **out);

#ifdef __cplusplus
}
#endif

#endif
