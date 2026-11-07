#ifndef MORPH_MCP_INTERNAL_H
#define MORPH_MCP_INTERNAL_H

#include "mcp.h"
#include "util/array.h"

#define MCP_STDIO_MAX_FRAME_BYTES (16u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/* Requests own their JSON and each header until request_cleanup. Replies own
 * their strings until reply_cleanup. Both cleanup functions accept zero state. */
struct mcp_wire_request {
	int id;
	char *json;
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

const struct mcp_protocol_adapter *mcp_protocol_borrow(const char *version);
int mcp_protocol_encode(struct mcp_client *client, const struct mcp_protocol_adapter *adapter,
			int id, const char *method, const char *params,
			struct mcp_wire_request *out);
int mcp_protocol_initialize(struct mcp_client *client);
void mcp_protocol_reset(struct mcp_client *client);
/* result is optional; *result starts NULL or -EEXIST preserves it. Any returned
 * text is malloc-owned (free), including diagnostics on a negative status.
 * Validated remote MethodNotFound returns -ENOSYS; other remote errors remain
 * protocol errors. Transport failure leaves the optional result unchanged. */
int mcp_request(struct mcp_client *client, const char *method,
	const char *params, char **result);
int mcp_request_timeout_ms(const struct mcp_client *client);

/* Schema validation/cache and mirrored HTTP headers belong to the codec.
 * Header helpers append independently owned strings to an initialized char*
 * array. On failure, existing elements are preserved without partial appends. */
int mcp_protocol_cache_tool_copy(struct mcp_client *client, const char *name, const cJSON *schema);
int mcp_protocol_tool_headers_copy(struct mcp_client *client, const cJSON *params,
				   morph_array_t *headers);
int mcp_protocol_add_header_copy(morph_array_t *headers, const char *name, const char *value);

/* Transport primitives never interpret protocol versions or result bodies.
 * Replies start without owned strings; occupied: -EEXIST. Success creates an
 * owner for reply_cleanup; failure preserves the caller's output. */
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
void mcp_wire_request_cleanup(struct mcp_wire_request *request);
void mcp_wire_reply_cleanup(struct mcp_wire_reply *reply);

/* Borrow reply. *out starts NULL; occupied: -EEXIST. Creates independent JSON
 * for cJSON_Delete. Valid correlated remote errors return a negative status AND
 * owned JSON diagnostics. Other failures preserve *out. */
int mcp_protocol_decode(struct morph_log *logger, const struct mcp_wire_reply *reply,
	int id, cJSON **out);

#ifdef __cplusplus
}
#endif

#endif
