# Runtime request and response protocol

The runtime is an application service. CLI uses its asynchronous client API;
GUI and embedded hosts can attach clients through the same interface. The
existing FastCGI adapter retains its synchronous compatibility facade and wire
contract. An asynchronous client submits requests and receives owned responses,
execution events and interaction requests. No terminal, window, selected tab,
composer, scrolling or rendering state belongs to the runtime protocol.

The public interfaces are [`runtime/client.h`](../src/runtime/client.h) and
[`runtime/protocol.h`](../src/runtime/protocol.h). They do not expose ReAct,
database, model, tool-registry or frontend objects. Runtime construction and
destruction retain their existing host lifecycle interface. Existing synchronous
runtime entry points remain available for compatibility.

## Objects and ownership

| Object | Responsibility |
| --- | --- |
| Runtime | Owns the client service, session execution owners, persistence and shared services. |
| Client | Owns its request identity, capabilities, subscriptions and receive queue. |
| Session | Owns its execution context, input queue, settings and turn state. |
| Turn | Owns one submitted execution, its cancellation and final result. |
| Interaction | Owns one pending question or approval and its resolution. |
| Frontend | Owns navigation, drafts, scroll positions, expansion state and visual presentation. |

There is no selected session in the client protocol. Every session operation
names its target. Changing the visible session in a frontend does not mutate an
execution context or cancel another session's work.

Each session has an independent execution owner. The current implementation
builds those owners using the existing runtime construction and execution
facilities, with process-replica initialization and the session's captured
working directory. The public parent runtime owns their lifetime. Frontends do
not construct per-session model, tool, database or worker graphs.

The runtime schedules turns on up to eight workers and domain management on up
to four workers, with a cache of up to 64 resident session engines. These are current
implementation bounds. Idle engines can be evicted and reconstructed from
persisted session state. A session's identity and history do not depend on a
particular worker thread or cached engine. Long model, MCP and management work
does not occupy the control path used by targeted turn interruption and replies.
The service admits up to 128 pending turns across its session lanes.

All request strings and JSON parameters are copied before
`runtime_client_request_copy()` returns. The caller may then release or mutate
them. A received `runtime_message` owns every pointer, including its JSON
payload, and is released with `runtime_message_destroy()` or the typed
`runtime_message_cleanup()` scope callback. Receive outputs must be empty;
passing an occupied output returns `NB_ERR_EXISTS` without replacing it.

## Message kinds

`RUNTIME_PROTOCOL_VERSION` identifies the public protocol version. The in-process
representation uses a small C envelope and cJSON domain payloads. A future wire
transport can encode the same values without changing execution semantics.

| Message | Direction | Meaning |
| --- | --- | --- |
| `RUNTIME_MESSAGE_RESPONSE` | Runtime to requesting client | Result or error for a command ID. |
| `RUNTIME_MESSAGE_EVENT` | Runtime to subscribed clients | Scoped state or output update. |
| `RUNTIME_MESSAGE_SERVER_REQUEST` | Runtime to interaction client | Question or approval that needs a reply. |

A `runtime_command` contains `id`, `method` and `params`. IDs belong to the
client's request namespace. Methods use dotted domain names. Responses retain
the command ID and method, carry an `error_code`, and own their result in
`data`. Errors use portable nbase statuses or Morph application errors.

Events carry `session_id`, `turn_id`, `sequence` and the event method. Their
`data` is the existing `morph_event` JSON envelope, preserving the dotted event
name and domain fields. Consumers should dispatch on message kind before
interpreting a payload.

Conversation IDs, turn IDs, command IDs, interaction IDs and process IDs are
different identities. A terminal process identifier must not be used as a
conversation selector. Child agent paths are interpreted in their parent
session tree.

## Submission, acceptance and completion

There are three separate boundaries:

1. `runtime_client_request_copy()` succeeds after copying and accepting the
   command for processing. It does not wait for model execution.
2. The response reports whether the requested operation succeeded. A successful
   `turn.start` response identifies the admitted turn and its initial state.
3. A `turn.completed` event reports that execution has finished. Use `turn.get`
   to retrieve the owned final snapshot and result independently of event
   consumption.

For example, submit:

```json
{
  "id": "request-42",
  "method": "turn.start",
  "params": {
    "session_id": 12,
    "input": "Inspect the build failure"
  }
}
```

The operation response includes a `session_id`, `turn_id` and initial `status`.
Subsequent messages contain streaming text, tool events and completion. Waiting
for the operation response must not prevent the client from servicing those
other messages.

## Session and turn operations

Session metadata and domain operations use explicit targets. `session.create`
accepts a name and optional model and working directory. `session.list` and
`session.get` inspect conversations. `session.update` changes metadata and
invalidates an idle cached engine so its next turn uses the updated model and
working directory. Session loading or unloading concerns runtime resources;
it is separate from opening or closing a frontend view.
Updating, deleting or unloading a session with live execution, child follow-up
work or background processes returns `NB_ERR_BUSY`. Stopping those tasks is an
explicit operation; lifecycle changes do not silently cancel them.

`turn.start` accepts these core parameters:

| Parameter | Meaning |
| --- | --- |
| `session_id` | Target conversation. |
| `input` | Model input text, copied at submission. |
| `stored_user_input` | Optional original user-facing input; defaults to `input`. |
| `turn_flags` | Optional execution and persistence flags. |
| `generate_session_title` | Opt in to isolated title generation. |

Only one primary turn can be active in a session. A second `turn.start` is
rejected while the first is admitted or running. Different sessions execute
independently.

Runtime coordinates mutating tools when session working directories overlap.
The first mutation retains a workspace claim through the turn and any active
descendant or background work. Another session waits before mutating that
workspace while unrelated model and read-only work can continue. Named turn
interruption also cancels a session waiting for that claim.

`turn.steer` names `session_id`, the expected `turn_id` and additional `input`.
It contributes to the named execution rather than creating another turn.
`turn.interrupt` also names the expected turn, so a delayed cancellation cannot
affect its replacement. A mismatched turn returns an error. Queries and
interaction replies remain available while the model or a tool is waiting.

`turn.get` returns a copied snapshot, including the turn identity, status,
`final_text`, `execution_rc` and `persistence_rc`. Model completion and successful
persistence are separate facts. Consumers must not infer success from an
operation response, nonempty partial output or a completed tool call.
Its `stream_text` contains the current uncommitted primary-agent preview. It is
cleared when that content is committed; child-agent text remains in scoped
events, and the completed answer remains available in `final_text`.

Context, history and memory operations also carry the target `session_id`:

| Methods | Result or effect |
| --- | --- |
| `context.get` | Actual actor context item/token counts, item kinds and transcript count. |
| `history.list`, `history.read` | Persisted newest-first pages with stable sequence cursors. |
| `history.diagnose` | Copied history consistency report. |
| `history.repair` | Repairs the named actor's history and reports whether it changed. |
| `history.compress` | Compresses the named actor's model context and reports removed/kept items. |
| `memory.read`, `memory.report` | Copied memory text or a structured report for that session. |
| `memory.jobs`, `memory.retry` | Inspect or retry its memory processing work. |
| `memory.clear` | Clears the explicitly requested memory scope. |
| `preference.read`, `preference.update` | Inspect or update its bound preferences. |
| `agent.budget` | Inspect the named actor's shared usage, reservations and budget limit. |

These operations resolve the named session actor rather than the compatibility
runtime's current-session projection. History repair and compression update the
same execution context used by that session's next turn.

Present parameters with invalid types or values are rejected before a
destructive action. For example, `checkpoint.restore.mode` accepts `code`,
`conversation` or `both`; a non-string mode is invalid. `memory.clear.scope` is an
integer from 0 through 3 (`all`, `facts`, `tasks`, `tips` respectively); omitting
it intentionally selects `all`. Permission/model update scopes accept their
documented scope strings, and turn flags and cancellation options retain their
numeric and boolean types. Malformed values do not silently select defaults.

## Subscriptions and recovery

Starting a turn subscribes its submitting client to the target session.
`session.subscribe` attaches another client and returns a snapshot and event
frontier. `session.unsubscribe` stops that client's delivery; it does not stop
execution. A disconnected client's accepted turns likewise continue under the
parent runtime.

Sequence numbers describe the runtime's ordered publication of session events.
They do not establish a global order between unrelated sessions. A frontend
retains its own display state and uses snapshots when it must reconstruct a
view or recover an event gap.

The subscription snapshot carries a numeric `cursor`, a `runtime_epoch`, the
retained `events` and `pending_interactions`. Events published after that
frontier arrive on the normal receive channel. A supplied stale epoch or an
expired `since_sequence` is reported with `resync_required` in the snapshot.

Session streams include child-agent events with their scoped `agent_id` in the
event data. `agent.subscribe` returns a child's retained events and frontier;
each frontend filters live events for its own visible child. Subscribing to a
different child in another client does not change the first client's stream.

The event queue is bounded. Slow consumers must receive an explicit recovery
signal rather than make runtime execution depend on UI drawing speed. Domain
state and final results remain queryable. Responses and interaction control
must remain usable when ordinary event delivery reaches its configured bound.

Overflow is reported as `runtime.resync_required`, with reason
`client_buffer_full`. Affected live subscriptions pause until refreshed. Command
capacity counts unread responses as well as work still being processed, so an
unattended request producer cannot accumulate unlimited response owners.
Targeted interruption, steering, turn inspection and interaction progress have
a reserved allowance of 16 admissions so a full normal queue cannot consume all
control capacity. The reserve is bounded as well.
The CLI refreshes its subscriptions after overflow, deduplicates replayed
sequences and restores the active preview or terminal result from the snapshot
while preserving its selected view and draft.

## Questions and approvals

Human interactions use `RUNTIME_MESSAGE_SERVER_REQUEST` with method
`interaction.request`, an interaction ID and a payload containing `kind` and
`request`. Session and turn identity remain attached to the message.

Reply using `runtime_client_reply_copy()` and the same interaction ID. Approval
results carry a `decision` advertised by the request, such as `allow`, `deny`,
`session` or `always`. Question results carry an `answers` array. Replies use a
control path independent of normal command capacity.

Only one valid resolution is accepted. Repeated or late replies return an
error; they are never converted into chat input. Cancellation and shutdown wake
pending interaction waiters. An unanswered required question is not consent.
Existing optional-question timeout and recommendation semantics remain in
effect.

`interaction.progress`, naming `interaction_id` or `request_id`, disables the
runtime's optional idle deadline. A frontend that handles idle timing and edit
detection locally can take over that deadline when presenting the prompt. The
CLI does this for its native selector; deferred prompts retain runtime timing.
A client explicitly assumes
interaction ownership with `session.claim_interactions` after the previous
controller disconnects. It must have the interaction capability and the same
trusted identity. Passive subscription does not steal the controller; handed-off
pending requests retain their interaction IDs.

Enabled tools that need an exclusive native terminal interaction can request
`terminal.acquire` and `terminal.release`. The host adapter yields or restores
its own terminal and acknowledges with `{"ok":true}`. These are optional host
interaction requests enabled by the client's `terminal_handoff` option; runtime
owns no terminal layout or frontend state.

Runtime permission policy remains authoritative. Ask, Auto, Full and Read Only
keep their existing access, approval and reviewer semantics. Full execution does
not require a human approval callback for operations already authorized by Full
access. A denied operation must leave its protected side effects untouched.

## Client capabilities

`runtime_client_options` binds a trusted host identity and capability mask:

- `RUNTIME_CLIENT_READ` permits inspection and subscription.
- `RUNTIME_CLIENT_EXECUTE` permits execution control.
- `RUNTIME_CLIENT_INTERACT` permits answering routed interactions.
- `RUNTIME_CLIENT_MANAGE` permits management operations.

Zero capabilities retains the embedded local-host compatibility default of all
capabilities. A network adapter must bind the authenticated identity and an
explicit capability mask; untrusted request parameters do not grant authority.
Host adapters remain responsible for their per-session visibility rules before
exposing a local runtime service across an authentication boundary.

The optional wakeup callback only signals that messages are available. It must
not synchronously reenter client or runtime APIs. Frontends drain messages on
their own owner/event thread and decide how to present them.

## Disconnect and shutdown

`runtime_client_close()` disconnects one endpoint and releases its subscriptions.
It does not close the runtime or cancel accepted turns. The host closes the
runtime explicitly when its service lifetime ends.

Runtime shutdown stops new admissions, cancels active work, wakes interaction
waiters and joins owned workers before releasing their dependencies. A client
handle may outlive `runtime_close()` to drain queued messages; it then receives
`NB_ERR_CLOSED` and can be closed normally. Closing the client remains required
to release its retained message channel resources.

`runtime_client_try_receive()` returns `NB_ERR_AGAIN` when no message is ready.
`runtime_client_receive()` waits up to its timeout, returning `NB_ERR_TIMEOUT`
when no message arrives and `NB_ERR_CLOSED` after shutdown and queue draining.
A negative timeout waits until a message or shutdown.

## Persistent execution records

[`persistence/runtime_store.h`](../src/persistence/runtime_store.h) owns typed
turn records, runtime instance ownership and session setting overrides. The
repository owns SQL and row mapping. Runtime code decides PID liveness, orphan
recovery, permission enum validation and protocol serialization.

Turn-start idempotency keys are scoped by session and trusted user. Repeating a
key returns the recorded turn instead of launching its model/tools again, also
after a restart. An abandoned instance's unfinished turn is recovered as
interrupted. A live instance sharing the same database is not declared abandoned
merely because another runtime starts.

Session permission and collaboration-mode overrides survive execution-owner
unload/reload and runtime restart. Updating unrelated session metadata does not
restore a more permissive default policy.

## Frontend integration

CLI uses the runtime client protocol, which also provides the boundary for GUI
hosts. Frontend services translate input into commands, reduce messages into
per-session models and answer interactions. A single UI owner thread performs
all rendering. Session workers never call terminal or GUI code.

The CLI can maintain one selected conversation, a session list and independent
drafts while multiple runtime sessions work. A GUI may choose tabs, windows or
split panes. Neither choice changes runtime scheduling or session identity.
One-shot, plain and legacy event-stream contracts remain frontend adapters.

## Verification

`RuntimeClientTest.*` in
[`tests/test_runtime_client.cpp`](../tests/test_runtime_client.cpp) exercises the
protocol without constructing a CLI application. A concurrent loopback HTTP
server holds one model request while another session completes, independently
of the rendering framework. Regressions cover request ownership, session
isolation, named steering/cancellation, required questions, approval denial,
Full execution and concrete patches, observer capabilities, subscription
lifetime, bounded delivery, idempotency, recovery and shutdown. `RuntimeStoreTest.*`
checks typed repository ownership and explicit orphan recovery separately.

Run the focused suite after building:

```sh
build/bin/morph-tests --gtest_filter='RuntimeClientTest.*:RuntimeStoreTest.*'
```

CLI PTY tests separately verify navigation, drafts, live background updates,
command presentation, resizing and terminal restoration. Runtime correctness
does not depend on those UI tests.
