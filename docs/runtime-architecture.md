# Runtime architecture

`morph-runtime` is the process-level owner of the agent system. Frontends own
only an opaque `struct runtime *`; they never own the database, models, ReAct
context, tool registry, MCP registry, memory workers, credits, or session
state.

```text
 CLI / Android / future hosts
          |
          | runtime_options + runtime_request
          v
 +-------------------- public runtime facade --------------------+
 | lifecycle | sessions | turns | tasks | sync | MCP | snapshots |
 +-----------------------------+---------------------------------+
                               |
                    one private runtime_context
                               |
       +-----------+-----------+-----------+-----------+
       |           |           |           |           |
    config/db   models      tools/MCP   sessions    workers
       |           |           |           |           |
       +-----------+-----------+-----------+-----------+
                               |
                    runtime_close(), reverse order
```

## Ownership rules

1. `runtime_open()` constructs the complete dependency graph or returns an
   error after cleaning the partial graph.
2. `runtime_close()` stops workers, drains memory work, disconnects MCP, and
   destroys tools, models, database state, and locks in reverse order.
3. A frontend may keep copied DTOs and snapshots. Immutable config and string
   views may be borrowed for immediate inspection; mutable runtime-owned
   objects are not exposed through the public facade.
4. Every turn enters through `runtime_execute_turn()`. The runtime binds the
   requested session, memory, plan, dynamic-tool, usage, credit, and event
   context before execution and restores transient bindings afterward.
5. Session switching, scheduled execution, cancellation, and shutdown use the
   same runtime lock and lifecycle regardless of frontend.

## Request and result

`runtime_request` is the per-turn request context: session identity, model and
stored input, rendering/event callbacks, approval callbacks, memory policy, and
turn flags. `runtime_result` is the immediate execution result. Durable or
diagnostic state is read through copied runtime snapshots such as
`runtime_turn_status`, never through the internal ReAct object.

## Source layout

```text
runtime/
  lifecycle.c       construction, shutdown, and turn entry point
  execute.c         serialized request execution
  engine.c          turn orchestration and per-turn state
  turn_scope.c      per-request binding and restoration
  context.c         private runtime context definition
  context_owner.c   private dependency graph ownership
  bootstrap.c       model and built-in tool construction
  extensions.c      desktop extension discovery
  mcp.c             MCP client lifecycle
  mcp_background.c  concurrent startup discovery and safe tool publication
  mcp_service.c     MCP DTO and operation facade
  registry_service.c platform tool and MCP registration facade
  session_service.c session ownership and selection facade
  session_title.c   isolated background title generation and conditional persistence
  recap.c           isolated catch-up requests and stale-result validation
  session.c         session lifecycle and switching
  permissions_service.c persistent permission grant listing and revocation
  task_service.c    scheduled-task and notification facade
  task_controller.c scheduled-task execution and worker control
  task_worker.c     background scheduled execution lifecycle
  scheduler.c       due-task scanning and claim loop
  tasks.h           public scheduled-task DTO/callback facade
  services.c        turn/session/tool/memory/credit snapshots and operations
  cancel.c          cancellation plumbing
  output.c          output and rendering callbacks
  usage.c           token and credit accounting
  lock.c            runtime lock helpers
  sync.c            sync worker facade
  runtime.h         public runtime facade
  runtime_internal.h private shared declarations
  request.h         runtime_request definition
  result.h          runtime_result definition
```

CLI and Android may render results differently and provide platform callbacks,
but neither frontend reimplements core initialization, execution, persistence,
credits, memory, tool registration, or shutdown.

`morph-runtime` links `morph-agent`, `morph-config`, `morph-credits`,
`morph-event`, `morph-mcp`, `morph-session`, `morph-skill`, `morph-sync`, and
`morph-tools`. Frontends link `morph-runtime` and never reach into its private
context. See `src/runtime/CMakeLists.txt` and `src/runtime/sources.list` for the
authoritative file list. The `sources.cmake` wrapper exposes that list to CMake.

The agent, runtime, persistence, and util modules each own a `sources.list`
manifest. Desktop CMake, Android CMake, and the iOS core build read the same
manifests so splitting a core implementation into new files does not leave
mobile builds behind. Keep each manifest to one relative source path per line.
Platform adapters and optional tools remain selected by each frontend.

## Context budgets

The agent supplies its tokenizer to `model_estimate_input_tokens()` through a
counter callback. The model layer reuses the selected adapter's message and tool
serializers, so budgeting follows Chat Completions or Responses input without a
dependency from models back to agent tokenization. Runtime/project prefixes use
the same preparation helper for estimates and actual requests. Fields absent
from the wire, including tool titles and output schemas, consume no budget.

These counts remain local estimates, not provider-reported usage. The trigger is
the smaller of the configured threshold and context capacity minus output and
protocol reserves. Tokenizer differences and provider framing still require that
protocol margin. Compaction logs fixed input overhead and the resulting history
budget. BPE splitting processes complete text in bounded batches; the batch size
does not cap the text counted.

Tool results are UTF-8 repaired and redacted before their complete stored text,
including any omission marker, is checked against the tokenizer limit. Retention
refreshes cached counts before selecting history, keeps request/answer pairs and
complete tool exchanges, and shares unused user retention capacity with recent
history. These rules apply to active in-turn compaction as well as persisted
history; original transcript storage remains separate.

## Startup diagnostics

Hosts may provide caller-owned `runtime_options.startup_error` storage. On a
configuration failure, `runtime_open()` fills the actual source path and its
validation detail (line, column, key, and reason) before cleaning up the partial
runtime. The output remains valid when the runtime handle is null. It is cleared
on each valid open attempt, including successful retries. Secondary sources such
as global/project `hooks.json` and project TOML retain their own file names.

`startup.failed` events also carry `error_code`, and, when available, `file`,
`line`, `column`, and `key`; the event message contains the specific reason.
CLI diagnostics use this output rather than attributing every initialization
parse error to the selected main TOML file. JSON syntax errors include a position
without echoing configuration contents or secrets.

## Background MCP startup

`runtime_options.auto_connect_mcp` enables configured servers whose
`auto_connect` is true. Hosts can additionally set `background_connect_mcp = 1`
to return from `runtime_open()` without waiting for those servers. Each server
has its own worker, so an unavailable service does not delay another service.
Zero preserves synchronous startup for existing hosts, including FastCGI and
mobile integrations. The interactive CLI opts in; one-shot and event-stream
CLI modes retain synchronous startup so their first request has its MCP tools.

Workers discover tools, resources, and prompts in a private registry. They
publish capabilities under the runtime execution lock between turns. A turn
already in progress keeps its existing tool set; newly ready capabilities are
available to subsequent turns. Failed discovery publishes no partial tool set;
publication errors roll back additions. Waiting for a running turn to finish
is a separate `registering` state and does not consume the connection deadline.

`runtime_mcp_info()` and `runtime_mcp_find()` expose `state`, `error_code`, and
monotonic `elapsed_ms`, in addition to the existing connection metadata. States
are disconnected, connecting, discovering, registering, ready, and failed.
The interactive footer shows `MCP ready/total`, `...` while work is pending,
and `!` when a server failed. `/mcp` shows per-server state, elapsed time, and
errors. `/mcp connect NAME` retries failed initialization;
`/mcp disconnect NAME` also cancels an in-progress startup worker.

While a server is initializing, explicit connect, discovery, and list requests
return `-EAGAIN` rather than sharing its transport. `connect_timeout > 0`
bounds background connection plus discovery; zero retains the transport's
30-second per-request timeout without an overall startup deadline. This setting
does not shorten subsequent tool executions. Stdio reads use an absolute
request deadline, support responses up to 16 MiB, preserve buffered messages,
and skip notifications or responses for other request IDs.

With background startup enabled, MCP event callbacks run on worker threads and
can overlap. Hosts must marshal callbacks onto their UI/event queue and keep
callback data alive until `runtime_close()` returns. Events include
`data.background = true` and preserve `mcp.connecting`, `mcp.connected`,
`mcp.discovering`, `mcp.ready`, and `mcp.failed`; `mcp.registering` reports waiting
for safe publication. Do not close the runtime from its worker callback.
Android and iOS hosts can opt in and render the same states using their native
UI; they do not need to implement connection workers or tool registration.
`runtime_close()` cancels and joins all startup workers before releasing MCP
clients, tools, and event sinks. HTTP interruption uses the existing curl
progress callback; stdio interruption is checked at most every 100 ms while
waiting for responses. Child-process termination is bounded and reaped.

## Background session titles

A frontend can set `runtime_request.generate_session_title` on the first user
turn of an automatically created session. Explicit names and resumed sessions
leave it unset. The CLI uses this instead of truncating the first prompt into
a session name.

The runtime snapshots the text model, session identity, original name, billing
configuration, and a UTF-8-safe prompt bounded to 960 bytes. An isolated worker
uses the same model/provider with no tools, no output callbacks, a 30-second
request timeout, and no retries. Only a valid JSON title is accepted; it is
normalized to one line and limited to 36 Unicode characters. Requests for the
same session are deduplicated while running, and at most four jobs run at once.

Each worker owns its model, arena, cancellation token, and database connection.
The result updates metadata only if both the original display ID and name still
match. A duplicate title receives a stable display-ID suffix within the length
limit. Title updates preserve activity timestamps and never enter model history
or the transcript. Usage is billed to the original session and user rather than
the foreground turn. Failures leave the original name intact and are silent.

Manual rename and deletion cancel pending work, including a rename to the same
name. Switching sessions does not redirect or cancel the original request.
Shutdown cancels and joins workers before releasing runtime resources. Session
snapshots and name accessors read persisted titles so hosts see background
updates without receiving extra chat messages.

`session.title` background events bracket each request with `begin` and `end`
phases, including failure and cancellation. They use the runtime's event sink
on the worker thread; hosts opting into naming must marshal events to their
UI thread. The CLI shows `Naming session...` in the footer while work is pending,
without replacing foreground activity or printing metadata in the conversation.

## Conversation recaps

Recap is independent of compaction. `runtime_recap_request()` snapshots the
selected session's recent user messages and final answers and starts an isolated
request using the configured text model. The prompt is bounded to 32 KiB,
including instructions. Long messages retain both their beginning and end.
Tools, reasoning, and previous recaps are excluded. Model output must contain
`summary` (up to 700 Unicode characters) and nullable `next_action` (up to 200).
An invented next step is explicitly forbidden by the prompt.

The runtime owns the worker, model, arena, cancellation, and billing resources.
Requests have no tools, a 30-second timeout, and no model-level retries. Only
one request runs at a time. `session.recap` events carry begin/end phases,
session identity, history position, request revision, trigger type, and either
the structured result or an error code. Hosts must marshal the runtime event
sink from the worker thread and call `runtime_recap_is_current()` before
displaying an end event. Foreground turns and session navigation cancel work
and advance the revision; history position also detects writes from another
database connection. A late event cannot replace a newer request or enter the
wrong session. Shutdown cancels and joins the worker. Recaps do not write chat
messages, model history, or compaction records.

The CLI exposes `/recap`, maintains a lower-priority pending indicator, and
renders a sanitized, width-aware `Recap` block in scrollback. It displays `Next`
only when the model supplies one. Manual failures are shown; automatic failures
retry once after 30 seconds and otherwise stay silent. The automatic scheduler
requires 3 completed turns, 2 new turns since the last displayed recap, and 30
minutes after both focus loss and the last finished turn. It uses terminal focus
reports, not guessed keyboard inactivity. Focus return cancels only automatic
requests. The UI retains recap progress for its 32 most recently recapped
sessions across navigation. Set `general.auto_recap=false` to disable automatic
requests. Terminals without focus reporting retain manual `/recap` support.

Runtime state and callback lifetimes are described in [State ownership](state-ownership.md).
