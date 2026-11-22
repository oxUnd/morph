# MCP client conformance

This describes the implementation and acceptance evidence for Morph's MCP client.
It covers the core client surface and declared capabilities in revisions 2026-07-28, 2025-11-25,
2025-06-18, 2025-03-26 and 2024-11-05. Optional, separately specified extensions
need their own explicit negotiation; supporting a protocol date does not imply
support for every third-party extension.

Connection success and tool discovery alone do not establish client conformance.
The negotiated revision determines transport rules; the capabilities actually
declared by a host determine which optional interactions it must fulfill.

| Area | Required behavior and acceptance evidence |
| --- | --- |
| Negotiation | Select a common revision; honor explicit pins; preserve authentication and protocol errors. |
| JSON-RPC | Correlate requests and responses independently in both directions, including string peer IDs; reply to unknown requests; ignore unknown notifications. |
| Legacy interactions | Handle nested elicitation, roots and sampling during an outstanding request on stdio and HTTP. |
| Modern interactions | Fulfill `inputRequests`, retain opaque `requestState`, and resume only at the server's explicit request. |
| Elicitation | Form and URL modes, schema validation, defaults, review, accept/decline/cancel, and interruption. Never manufacture a human decision. |
| Sampling | Host-model selection, content and tools, explicit user review, cancellation, and exact result correlation. |
| Roots | Authorized workspace roots encoded as file URIs; preserve user and session boundaries. |
| Content | Preserve text, structured content, images, audio, embedded resources, resource links, metadata and error state in a usable host representation. |
| Instructions | Include initialized/discovered server guidance with its enabled tools; preserve it through reconnect and child-session synchronization. |
| Server features | Paginated tools, resources, resource templates and prompts; resource reads, prompt retrieval, completions and logging controls. |
| Notifications | Progress, messages, list changes and resource updates; update live catalogs without mutating an executing registry. |
| Subscription lifecycle | Legacy subscriptions and modern `subscriptions/listen`, including acknowledgments, correlation, cancellation and shutdown. |
| Transport lifecycle | Incremental SSE; legacy sessions/DELETE and GET resumption with retry delays; modern stateless POST streams; process cleanup, cancellation and deadlines excluding human response time. |
| Application lifecycle | Real frontend interaction, noninteractive cancellation, persistent results and independent transcripts. |

Tests exercise both wire behavior and the application boundary. A declared
capability must have a working handler; a headless host must cancel an interaction
rather than invent an answer. A tool approval does not answer an MCP form on the
user's behalf. Completed work must be verified with deterministic fixtures before
live-server checks; desktop actions require a disposable desktop fixture.

Acceptance suites:

- `test_mcp.cpp` and `test_mcp_protocol.cpp`: correlated peer requests,
  MRTR, pagination, completion/logging/resource controls, incremental HTTP
  interaction, subscription ordering/concurrency/reconnection, catalog
  publication and legacy SSE recovery without replaying a tool call.
- `test_mcp_interaction.cpp`: revision-dependent capabilities, real host-handler
  decisions, schema/default/enum handling, URL restrictions, sampling review and
  model controls, roots, image bytes, structured errors and both model codecs.
- `test_cli_mcp_interaction_pty.py`: the production CLI, Full mode, explicit
  accept/decline/cancel, server instructions and draft preservation at
  120/80/60 columns. Existing CLI selector/cancellation/MCP tests and FastCGI
  interaction tests cover the shared frontend channel.
- `MockLlmTest.HumanQuestionsPauseTheGenericToolDeadline`: a human response
  longer than the tool deadline remains a successful question.
- Runtime shutdown cancels catalog discovery before a model call, and requests
  queued behind a shared connection remain cancellable. Catalog cancellation
  does not cancel another session's independent tool context.
- The same protocol/runtime/model suites run under AddressSanitizer and UBSan.

The optional live LCU test runs pure JavaScript after negotiation. It is not
desktop-action evidence and never requests application access. Standard empty
approval forms are covered through the wire fixtures and the production CLI.

See [compatibility and capacity](mcp-compatibility.md) for the supported transport
profile, authentication setup, optional-extension boundaries and resource limits.

Authoritative references:

- [2026-07-28 specification](https://modelcontextprotocol.io/specification/2026-07-28)
- [2026-07-28 schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)
- [2025-11-25 specification](https://modelcontextprotocol.io/specification/2025-11-25)
