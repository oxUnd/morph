# Unified Event System

The runtime sends structured events to the CLI and FastCGI. Consumers dispatch
on `type` and the dotted `name`, then read the event-specific fields in `data`.
The optional `message` is display text. This lets the CLI update a tool row and
FastCGI publish an SSE record from the same call and result.

## Event contract

A `morph_event_cb` borrows the event for the duration of the call. A consumer
that queues work or retains a payload must copy it. The JSON serializer emits
`type`, `name` and `data` (an empty object when absent). It includes `phase`,
`message` and the top-level `turn_id` when supplied. Errors use portable nbase
statuses and Morph application codes. [C interface](../src/event/event.h) ·
[JSON serialization](../src/event/event.c)

## Event Names

The following groups list common events. Lifecycle hooks also emit hook-specific events described in [Lifecycle hooks](hooks.md).

Startup:

```text
startup.begin
startup.config
startup.database
startup.models
startup.hooks
startup.tools
startup.mcp
startup.session
startup.ready
startup.failed
```

MCP:

```text
mcp.connecting
mcp.connected
mcp.discovering
mcp.registering
mcp.ready
mcp.failed
mcp.disconnected
```

ReAct:

```text
react.turn.begin
react.thinking
react.content.delta
react.content.end
react.thought.delta
react.thought.end
react.reasoning.delta
react.action
react.observation
react.reflection
react.guardrail.unavailable
react.final
react.final.delta
react.final.retry
react.user.steer
react.agent.mail
react.hook.continue
react.context.usage
react.compaction.begin
react.compaction.completed
react.compaction.failed
react.agents.wait.begin
react.agents.wait.end
react.output.truncated
history.tool_output.warning
react.turn.end
react.cancelled
react.timed_out
react.max_iterations
react.failed
```

`react.agents.wait.begin` and `react.agents.wait.end` bracket the runtime's
automatic wait for active descendants before finalizing a response. They are
ReAct events, separate from explicit `wait_agent` tool calls. The end event is
also emitted when waiting exits for mail, user input, cancellation or an error,
so frontends can remove the waiting status on every exit path.

`react.output.truncated` is a background event with phase `warning` when a model
returns `finish_reason=length`. It carries `max_tokens`, `output_tokens`, and
`reasoning_tokens`. Partial text is retained as content, but the turn ends with
`llm_error` / `output_token_limit`; incomplete tool calls are not executed or retried.

`history.tool_output.warning` is emitted once at turn end when at least three
collected tool results were truncated and they comprise at least a third of the results.
Its data distinguishes `tool_results`, `truncated_results`,
`history_truncated_results`, and `output_truncated_results`. A result cut at both
layers counts once in `truncated_results`. History reload and re-preparation do
not count as new results. CLI warnings persist in normal output; JSON event mode
preserves the event envelope and counters.

`react.content.delta` carries native assistant response text, separately from
`react.reasoning.delta`. Its final role is known when the response completes:
text before tool calls ends with `react.content.end`; an answer ends with
`react.final`. Frontends can render content immediately without treating it
as reasoning or waiting for the terminal event. Native tool preambles persist
as `Content` trace steps; the low-level output callback uses
`REACT_STEP_CONTENT`. CLI labels preambles `content` in plain output, while
Android stores `CONTENT` and `REASONING` separately and renders assistant
content as streaming Markdown. Legacy thought events remain supported.

Stop hooks can request a continuation. Output guardrails with a blocking policy may withhold streaming previews. `react.hook.continue` commits the completed
response and presents the hook-generated continuation prompt before the next model
request. Its `data.text` is the prompt, also stored as a user history item with
`source: "hook"` and `hook_event_name: "Stop"`. The root `turn_id` stays unchanged;
`react.final` and `react.turn.end` still identify the eventual terminal completion.

`react.agent.mail` is emitted after a mailbox delivery is persisted and acknowledged.
Its `text` contains the received transport envelopes. It is an internal coordination
update, not human steering. Interactive clients group it with a successful
mailbox wait when available and preserve the original payload in details.

`react.user.steer` is emitted when a queued prompt is accepted into the current
loop. Its `text` is the full model input; optional `display_text` preserves the
original user-facing input (for example, image labels without expanded file
paths). Display metadata is not substituted into model history. The CLI commits
that user message to the feed at this event, not when it is first queued.

Tool:

```text
tool.call
tool.running
tool.stream.delta
tool.result
tool.failed
tool.cancelled
```

HITL:

```text
hitl.request
hitl.approved
hitl.denied
hitl.always
```

Artifact:

```text
artifact.ready
```

Background work:

```text
background.started
background.progress
background.ready
background.wait.begin
background.wait.end
background.completed
background.failed
```

CLI commands:

```text
command.started
command.completed
command.failed
```

`command.started` includes the parsed command and arguments. Terminal command
events include the captured human-readable output; failed events also include
`error_code` and `error`. In structured CLI mode, command output is never
written directly to stdout.

Structured CLI interactions:

```text
interaction.request
interaction.invalid_response
interaction.resolved
interaction.cancelled
```

When `--events json` is active, an operation that requires user input emits an
`interaction.request` NDJSON event instead of reading from a terminal. Its
`data` object contains a unique `request_id`, a stable `kind`, and a
kind-specific `request` payload. The client must write exactly one matching
response line to stdin:

```json
{"type":"interaction.response","request_id":"interaction-1","data":{"answers":["blue"]}}
```

The client can cancel the pending request with:

```json
{"type":"interaction.cancel","request_id":"interaction-1"}
```

Malformed input, a mismatched request ID, or a semantically invalid answer
emits `interaction.invalid_response`; the same request remains pending. EOF and
explicit cancellation emit `interaction.cancelled`. A valid response emits
`interaction.resolved` before the agent continues.

Stable interaction kinds and response payloads are:

- `ask_user`: the request contains `question`, `choices`, `selection_mode`,
  `min_choices`, `max_choices`, `recommended_indices`, `response_policy`, and
  `timeout_seconds`; respond with `data.answers`, an array of
  selected strings or custom answers. Suggested choices do not restrict the
  answer text; selection counts and duplicate-answer checks still apply.
  Recommendations use zero-based indices and are preselected without being
  submitted. Optional questions expire after 60 seconds by default, resolving
  with reason `timeout_recommended` or `timeout_no_input`. Required questions
  have no automatic resolution. A matching `interaction.progress` frame marks
  editing activity and disables automatic resolution for the pending question.
  Late frames remain protocol messages and never become chat input.
- `tool_approval`: the request contains `tool_name`, `arguments_json`, and
  `allowed_decisions`; respond with `data.decision`.
- `operation_approval`: the request contains the normalized operation fields,
  directory capabilities, and `allowed_decisions`; respond with
  `data.decision`.
- `command_confirmation`: destructive system commands use this kind with an
  action-specific request; respond with `data.decision` set to `confirm` or
  `cancel`.

The permitted decision strings are advertised in each request. They may
include `allow`, `session`, `always`, and `deny`. Top-level user prompts remain
plain input lines for backward compatibility; only responses to an active
interaction are JSON input frames. The default interactive CLI keeps its
existing terminal prompts and rendering.

Tasks:

```text
task.created
task.updated
task.cancelled
task.claimed
task.started
task.notification
task.completed
task.rescheduled
task.failed
task.timed_out
task.max_attempts_reached
```

## Common Phases

Use these where applicable:

- `begin`
- `delta`
- `end`
- `ready`
- `skipped`
- `timeout`
- `failed`
- `cancelled`

## Payload Requirements

All payloads should be JSON objects. Avoid arrays at the top level unless the
event itself represents a batch.

MCP lifecycle events carry the server name, transport and auto-connect setting.
Discovery results add available tool, resource and prompt counts. Failures can
carry the error status and diagnostic. [MCP emitter](../src/runtime/mcp.c)

The CLI keeps connection lifecycle events out of conversation history. Its MCP
footer summary opens a right-side floating card on click; progress and failure
details stay in that card while the composer remains editable. Explicit MCP
commands print their own result or error receipts independently of these events.

The core `artifact.ready` data includes `kind`, `path` and an optional `source`.
FastCGI registers the file for its web client and supplies the web-facing
artifact reference. [FastCGI adapter](../src/sapi/fastcgi/adapters/turn_events.c)

A tool call includes `tool`, `tool_call_id` and `args`. Text-input tools wrap
their input in `args.input`. A result event carries the result as a string.
An observation can additionally carry structured data, UI and artifacts.
Negative statuses add `error_code` and `error`.
[ReAct event serialization](../src/agent/react_events.c)

A successful turn ends with `react.turn.end`. An unsuccessful turn emits a
specific terminal event such as `react.cancelled`, followed by `react.turn.end`.
The terminal data includes `outcome` and `text`, with `reason`, error fields
and diagnostic detail when available. Consumers can finish a turn on
`react.turn.end` in either case. [Turn completion](../src/agent/react.c)

Task events identify the scheduled task and its state. A notification carries
the inbox record that clients can inspect after restarting. See the
[task event fields and examples](scheduled-tasks.md#task-events).

## Frontends and verification

- Interactive CLI workers post events to the owner thread. It updates the
  retained conversation, activity rows and tool transcript. [CLI tool feed](cli-tool-feed.md)
- `morph --events json` writes NDJSON. Use `request_id` to correlate
  interaction responses and `tool_call_id` to match tool calls and results.
- FastCGI maps runtime events to the web client's SSE names and persists
  frontend events for replay. [FastCGI API](../src/sapi/fastcgi/README.md#api-surface)
- Tests can use the event recorder to inspect serialized events in arrival
  order. [Recorder tests](../tests/test_event.cpp)

## Background command lifecycle

`process.started` and `process.completed` are `background` events independent of
`tool.result`. Returning a running process session finishes the tool invocation,
not the process. Lifecycle data contains `session_id` (the process ID),
`owner_session_id` (the conversation ID), `turn_id`, `description`, `command`,
`workdir`, `service`, `notify_only`, `status`, and `duration_ms`. Terminal states
also carry an exit code or signal where applicable and a bounded output snapshot.
The process monitor invokes completion observers outside its lock. The CLI event
queue owns terminal updates; monitor threads never draw directly.

See [asynchronous commands](async-exec.md) for waiting, retention, and UI behavior.

### Permission reviews

`permission.review` uses the existing HITL event category. Its `source` is `user`
or `model`; phases are `begin`, `end` and `failed`. Completed reviews include a
`verdict` (`allow`, `deny`, `defer`) and `reason`, plus the tool and turn identity.
Automatic reviews use a separate model client with usage phase `approval`.
Human fallback retains the existing operation/HITL callback contracts, so CLI,
JSON and FastCGI continue to present their existing approval interactions.

Multi-agent activity uses `agent.started`, `agent.updated`, and `agent.completed`.
Payloads include `agent_id`, `parent_session_id`, `description`, `status`,
`activity`, and `error_code`. CLI activity rows consume these on the UI thread;
full child tool streams remain scoped to the selected agent.

`background.wait.begin` and `background.wait.end` are additional `background`
events that bracket a blocking `background(action="wait")` or runtime collection
of finite commands. Their data contains only `session_id`; no command output or
stdin is repeated. End is emitted on success, cancellation and wait timeout.
They drive the CLI's transient waiting row; they do not rename the existing
`process.started` / `process.completed` lifecycle events.
