# MCP compatibility and capacity

Morph supports MCP `2026-07-28` over stdio and Streamable HTTP, with adapters for
initialization-based revisions `2025-11-25`, `2025-06-18` and `2025-03-26`.
The `2024-11-05` adapter is available over stdio only; legacy HTTP+SSE is not
implemented. The runtime implements form/URL elicitation, sampling (including
tool use), and workspace roots. Capabilities are generated from installed host
handlers and the selected revision; optional extensions are negotiated separately.

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
pipe and HTTP mechanics, not version negotiation. They dispatch incoming
requests independently from matching responses, including string peer IDs and
peer IDs that collide with an outstanding client request. HTTP consumes SSE
incrementally and posts legacy interaction replies on a separate connection.
`mcp_dispatch.c` owns incoming JSON-RPC and host callbacks;
`mcp_subscription.c` owns notification channels; `mcp_content.c` preserves
binary results as private host artifacts.

## Version selection

The default is `protocol_version = "auto"`:

1. Send a modern `server/discover` request with per-request metadata. A valid
   response advertising `2026-07-28` selects that revision; no initialization or
   session ID is used. If discovery advertises only older revisions, select the
   newest one supported by both Morph and the transport, then initialize it.
   The order of the server's version list does not affect this preference.
2. A recognized modern protocol error never selects a legacy adapter. Discovery
   without a mutually supported revision fails explicitly. Auth failures, HTTP
   5xx, malformed successful responses and HTTP transport failures also do not
   trigger downgrade.
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

Modern requests include the protocol version, client identity and actual host
capabilities in `params._meta`. Other request metadata is retained.
HTTP also carries the matching protocol, method, name/URI and annotated parameter
headers. Legacy requests keep their handshake-based envelope and session headers.

Responses must be correlated JSON-RPC 2.0 objects. Recognized remote errors remain
available for protocol decisions. Modern `resultType` may be absent or `complete`;
`input_required` fulfills the server's `inputRequests`, retains opaque
`requestState`, and resumes `tools/call`, `resources/read` or `prompts/get` with
correlated `inputResponses`. State-only continuations have cancellation and an
overall active-time budget. Unknown result kinds fail explicitly. A transport
failure never causes an implicit replay of a tool call.

## Host interactions

Elicitation uses the runtime's existing user-interaction channel. Forms support
the restricted primitive schemas, enum labels and multi-select enums, defaults,
optional fields, validation and answer review/editing. Accept, decline and cancel
remain distinct. URL mode presents the URL for the user to open and confirm;
it does not collect credentials into a form response. An unavailable frontend
cancels the interaction. Full tool access does not answer forms on the user's
behalf: these are requests for actual user input, not additional tool-policy gates.

Sampling uses a host-selected configured model, cancellation and usage accounting.
The user reviews the input before generation and the answer before disclosure to
the server. Server-provided tools are passed as generation definitions; Morph
does not execute them as host tools. Model preferences never select credentials
or an arbitrary provider. Provider/model media support still applies. Workspace
roots are file URIs, with Unicode/space escaping and native Windows drive paths.

Initialized/discovered server instructions accompany enabled MCP tools in model
requests, including new turns and independently filtered child registries.

## Notifications and lifecycle

Progress, logging, catalog changes and resource updates reach the host notification
callback. Catalog changes stage a fresh registry and publish it between turns;
running tools retain their execution bindings. Disabled settings, configured
read-only flags and timeouts survive publication.

Modern `subscriptions/listen` uses explicit filters, acknowledgment ordering and
subscription IDs. Its HTTP stream runs alongside ordinary requests. Legacy
clients can subscribe/unsubscribe resources and open the optional HTTP GET stream.
Incoming cancellation is correlated; outgoing cancellation identifies the request
being abandoned. Human interaction time is excluded from request/tool deadlines.

Legacy HTTP sessions are retained through calls and terminated with best-effort
DELETE on shutdown. Legacy SSE resumes with GET and `Last-Event-ID`, respecting
the server's retry delay, without replaying the original POST. Modern HTTP uses
its own stateless rules: it sends no session ID, GET, DELETE or Last-Event-ID.

## Discovery and payload sizes

Discovery descriptor arrays own their heap strings and have matching typed free functions. Tool schemas, prompt argument
schemas, titles, descriptions, resource URIs and MIME types are copied in full.
There is no fixed MCP schema-size limit. The runtime list APIs deep-copy these
strings into a single allocation, released with `runtime_mcp_list_free()`.

`tools/list`, `resources/list`, `resources/templates/list` and `prompts/list` follow opaque `nextCursor`
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
escaping and dynamic allocation. Resource text and stdio receive buffers grow
dynamically. Stdio frames have an explicit 16 MiB bound; exceeding a bound,
allocation failure or integer overflow is an error, never silent truncation.

HTTP bearer headers and MCP session IDs use dynamic storage. Tool results retain
`structuredContent`, `_meta` and all original content blocks. When text is
flattened into `content` for the existing tool interface, the original blocks
are retained in `contentBlocks`. Empty or non-text-only content is valid. This
preserves images, audio and embedded binary resources as private file attachments,
with their bytes, MIME types and paths available to the host, file viewer and
vision tools. Large base64 strings are not copied into model text. Resource links
remain links and do not cause an automatic local-file or network read. Error
results retain structured details and an explicit failed outcome.

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
- Requests have a 30-second active transport budget; human interaction pauses
  that budget. `connect_timeout` controls startup/discovery. The existing generic
  tool timeout also excludes time spent answering required user questions.
- The JSON parser retains its nesting limit (1,000). Memory capacity and the
  integer representation of list counts also bound accepted data.
- Binary results use the host's 32-artifact-per-result capacity. Exceeding it
  fails explicitly. Legacy SSE recovery has at most three GET attempts within
  the request budget and honors the server's retry delay.
- HTTP authentication uses configured bearer tokens. OAuth browser discovery and
  registration, the original 2024 HTTP+SSE transport, and separately negotiated
  extensions are outside this client profile. Unknown extension result types are
  rejected. No server-private host tool or approval-persistence contract is
  inferred from a protocol date.

Protocol references: [version negotiation](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning),
[HTTP binding](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http),
[pagination](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2025-06-18/server/utilities/pagination.mdx)
and [tool schemas and content](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2025-06-18/schema.ts).

## Live discovery check

To verify a local stdio server without invoking any of its tools:

```sh
MORPH_TEST_MCP_COMMAND=/absolute/path/to/server-wrapper \
  build/bin/morph-tests --gtest_filter='McpLiveServerTest.*'
```

The opt-in test negotiates the protocol, registers tools/resources/prompts and
re-reads the tool catalog after discovery. It is skipped in normal test runs.
For servers with arguments or custom environment settings, point it at a wrapper
that supplies them.
