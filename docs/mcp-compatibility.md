# MCP compatibility and capacity

Morph supports MCP `2026-07-28` over stdio and Streamable HTTP, with adapters for
initialization-based revisions `2025-11-25`, `2025-06-18` and `2025-03-26`.
The `2024-11-05` adapter is available over stdio only; legacy HTTP+SSE is not
implemented. Optional capabilities and extensions are not implied by a protocol
version: Morph advertises no sampling, elicitation, roots or extension support.

## Adapter architecture

`mcp_client.c` implements tools, resources, prompts and registry bindings. Every
RPC enters `mcp_request()`; operation code never branches on a protocol date.

`mcp_protocol.c` owns the explicit version table, connection negotiation,
per-request metadata, JSON-RPC response validation, session policy and result
kind checks. A version selects an adapter descriptor; an unknown date is an
error. Both eras use the same request/response path. No version behavior is
copied into either transport.

`mcp_headers.c` validates and caches tool schemas and builds the modern HTTP
routing and parameter headers. It follows exact nested `properties` paths,
checks primitive types and safe integers, rejects duplicate/invalid annotations,
and applies the specified Base64 sentinel encoding. Invalid annotated HTTP tools
are excluded individually, with a warning. Strings in enum/const/default/examples
are instance data, not schema annotations. Cached schemas are released on
disconnect; direct tool calls discover schemas before constructing headers.

`mcp_stdio.c` and `mcp_http.c` exchange encoded wire messages. They own process,
pipe and HTTP mechanics, not version negotiation. HTTP selects the SSE response
matching the request ID, ignoring progress notifications and unrelated replies.

## Version selection

The default is `protocol_version = "auto"`:

1. Send a modern `server/discover` request with per-request metadata. A valid
   modern response selects `2026-07-28`; no initialization or session ID is used.
2. A recognized modern protocol error never selects a legacy adapter. Unknown
   modern versions fail explicitly. Auth failures, HTTP 5xx, malformed successful
   responses and HTTP transport failures also do not trigger downgrade.
3. Legacy evidence (including a correlated non-modern JSON-RPC rejection on HTTP
   200) selects the `initialize` handshake, offering `2025-11-25`. Its initial
   HTTP request omits the not-yet-negotiated protocol-version header.
   The selected legacy version must be in Morph's table. HTTP session IDs are
   retained only for legacy requests. `notifications/initialized` is sent only
   after a successful legacy handshake.
4. A stdio peer may exit, ignore or reject the discovery probe. Before legacy
   initialization, Morph terminates/reaps that process and launches a clean one.
   The automatic stdio probe has a five-second budget; an explicit startup
   deadline allocates at most a quarter of its request budget to the probe.
   Cancellation is checked before launching the replacement process.

Pin a known server to skip automatic era selection or to require modern support:

```toml
[[mcp.servers]]
name = "local"
transport = "stdio"
command = "my-mcp-server"
protocol_version = "2025-06-18" # Skip discovery and require this legacy revision.

[[mcp.servers]]
name = "remote"
transport = "http"
url = "https://example.com/mcp"
protocol_version = "2026-07-28" # Discover modern capabilities; never downgrade.
```

A modern pin uses the normal request/startup timeout rather than the short
legacy-detection budget. This is useful for slow-starting subprocesses. A legacy
pin rejects a different server-selected version rather than silently switching.

Modern requests always include the protocol version, client identity and an empty
client-capabilities object in `params._meta`. Other request metadata is retained.
HTTP also carries the matching protocol, method, name/URI and annotated parameter
headers. Legacy requests keep their handshake-based envelope and session headers.

Responses must be correlated JSON-RPC 2.0 objects. Recognized remote errors remain
available for protocol decisions. Modern `resultType` may be absent or `complete`;
`input_required` fails with `ENOTSUP` because Morph does not yet implement MRTR
client interactions. Unknown result kinds fail explicitly and are never treated
as successful tool execution. Tool calls are not automatically replayed.

## Discovery and payload sizes

Discovery descriptors use arena-owned strings. Tool schemas, prompt argument
schemas, titles, descriptions, resource URIs and MIME types are copied in full.
There is no fixed MCP schema-size limit. The runtime list APIs deep-copy these
strings into a single allocation, released with `runtime_mcp_list_free()`.

`tools/list`, `resources/list` and `prompts/list` follow opaque `nextCursor`
values until the server omits the cursor, including across empty pages. Repeated
cursors and malformed list responses return a protocol error rather than looping
or publishing an incomplete list.

A server can incorrectly advertise a list capability without implementing the
corresponding method (for example, Peekaboo 4.9.7 advertises `prompts` but returns
JSON-RPC `-32601` for `prompts/list`). A validated, correlated MethodNotFound on
the first page disables only that capability and yields an empty list. Other
capabilities remain available, and runtime status reflects the corrected flags.
Authentication failures, timeouts, malformed responses, other RPC errors and
failures after pagination has begun still fail discovery. Remote RPC error codes
and messages are logged separately from local error descriptions.

Tool registration grows dynamically beyond the previous 128-entry ceiling.
Schemas may omit `properties`, as allowed for JSON Schema objects. Required
property names do not have to appear in `properties` for open schemas.

Tool and prompt arguments and resource-read parameters are serialized with JSON
escaping and dynamic allocation. Resource text and blob aggregation and stdio
receive buffers also grow dynamically. The former 64 KiB request/resource and
16 MiB stdio response ceilings have been removed. Allocation failures and integer
overflow remain errors; there is no silent payload truncation.

HTTP bearer headers and MCP session IDs use dynamic storage. Tool results retain
`structuredContent`, `_meta` and all original content blocks. When text is
flattened into `content` for the existing tool interface, the original blocks
are retained in `contentBlocks`. Empty or non-text-only content is valid. This
preserves image/audio/resource data but does not automatically turn those blocks
into native model image/audio attachments.

## Remaining boundaries

- Local configuration still has fixed capacities: 32 servers, 64 command
  arguments (255 bytes each), 16 environment overrides, 127-byte server names,
  and platform path-sized URL/environment values. These are configuration
  boundaries, not protocol guarantees. Runtime conversion rejects invalid
  argument/environment counts instead of clamping them; the configuration
  parser has its own limits.
- Names exposed to Morph's registry must fit in 511 bytes, including the server
  namespace. Oversize names fail explicitly with `ENAMETOOLONG`. Name
  sanitization can produce collisions, which fail with `EEXIST`; names are never
  silently shortened. Discovery/publication failure remains transactional, so
  invalid schemas or collisions can still prevent a server from becoming ready.
- Requests use the existing 30-second transport timeout. `connect_timeout`
  controls startup/discovery. Long-running server tools may need future timeout
  configuration independently of startup.
- The JSON parser retains its nesting limit (1,000). Memory capacity and the
  integer representation of list counts also bound accepted data.
- Legacy HTTP+SSE transport, OAuth negotiation, sampling, elicitation,
  resource templates/subscriptions, server-to-client request handling and
  automatic list-change refresh are not implemented as complete client flows.
  MRTR input handling, extension result types, response caching, live subscription
  streams and reconnection remain unimplemented. SSE support handles finite
  request responses, not general bidirectional or subscription streams.

Protocol references: [version negotiation](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning),
[HTTP binding](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http),
[pagination](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2025-06-18/server/utilities/pagination.mdx)
and [tool schemas and content](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2025-06-18/schema.ts).

## Live discovery check

To verify a local stdio server without invoking any of its tools:

```sh
MORPH_TEST_MCP_COMMAND=/absolute/path/to/server-wrapper \
  build/bin/morph-tests --gtest_filter=McpLiveServerTest.*
```

The opt-in test negotiates the protocol, registers tools/resources/prompts and
re-reads the tool catalog after discovery. It is skipped in normal test runs.
For servers with arguments or custom environment settings, point it at a wrapper
that supplies them.
