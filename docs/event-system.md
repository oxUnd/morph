# Unified Event System

## Problem

Morph now has multiple front ends and partial event paths:

- CLI renders ReAct progress through `react_output_cb`.
- FastCGI converts the same callback into SSE events, sometimes by parsing
  human text such as `tool(args)`.
- Startup, MCP auto-connect, model initialization, tool registration, HITL,
  artifacts, and background work do not share one event contract.

This makes GUI and Android integrations fragile. They must either parse CLI
text or duplicate internal state logic. A unified event system is required so
CLI, GUI, Android, FastCGI, tests, and future front ends consume the same
structured facts.

## Goals

- Provide one core event contract for CLI and GUI surfaces.
- Preserve the existing CLI UX while making structured output available.
- Stop requiring front ends to parse human-readable ReAct strings.
- Cover startup, MCP, ReAct, tools, HITL, artifacts, background work, and
  errors.
- Keep existing behavior working during migration.
- Make missed event paths visible through an implementation checklist and
  tests.

## Non-Goals

- Do not build the full GUI in this change.
- Do not remove the existing `react_output_cb` immediately.
- Do not make every subsystem asynchronous as part of the initial event work.
- Do not introduce a new container, buffer, string, or map implementation.

## Hard Constraints

- Use shared foundational containers from `src/util/`:
  - `morph_buf_t` for variable-length JSON/text assembly.
  - `morph_array_t` for event queues, subscriber lists, and dynamic batches.
  - `morph_str_t` for string views when ownership is external.
  - `morph_strmap_t` for string-keyed subscriptions or event name lookup.
- Do not hand-roll growable buffers, dynamic arrays, hash maps, or UTF-8 logic.
- Use `cJSON` for structured event payloads in the first implementation.
- Event `message` is for human fallback only. Front ends must depend on
  `type`, `name`, `phase`, and `data`.
- Event names and payload fields are a front-end protocol. Rename or remove
  them only through an explicit compatibility plan.
- Background threads must not directly mutate CLI/UI state or `tool_registry`.
  If cross-thread events are needed, enqueue them through a thread-safe event
  queue and drain them from the owner thread.
- Error returns follow project convention: negative errno or `MORPH_ERR_*`.

## Event Contract

The core event object should be small and renderer-neutral:

```c
enum morph_event_type {
	MORPH_EVENT_STARTUP,
	MORPH_EVENT_REACT,
	MORPH_EVENT_TOOL,
	MORPH_EVENT_MCP,
	MORPH_EVENT_HITL,
	MORPH_EVENT_ARTIFACT,
	MORPH_EVENT_BACKGROUND,
	MORPH_EVENT_TASK,
	MORPH_EVENT_ERROR,
	MORPH_EVENT_COMMAND,
};

struct morph_event {
	enum morph_event_type type;
	const char *name;
	const char *phase;
	const char *message;
	cJSON *data;
};

typedef int (*morph_event_cb)(const struct morph_event *ev, void *user_data);
```

Recommended source layout:

```text
src/event/event.h
src/event/event.c
```

The event module should provide:

- event type name conversion;
- JSON serialization for NDJSON/SSE/trace output;
- helper emit functions that build `cJSON` payloads safely;
- optional recorder helpers for tests.

## Event Names

Event names are stable strings. Use dotted namespaces.

Startup:

```text
startup.begin
startup.component.begin
startup.component.ready
startup.component.failed
startup.ready
startup.failed
```

MCP:

```text
mcp.registered
mcp.skipped
mcp.connecting
mcp.connected
mcp.discovering
mcp.ready
mcp.timeout
mcp.failed
mcp.disconnected
```

ReAct:

```text
react.turn.begin
react.thought.delta
react.thought.end
react.reasoning.delta
react.action
react.observation
react.reflection
react.final
react.turn.end
react.cancelled
react.timed_out
react.max_iterations
react.failed
```

Tool:

```text
tool.call
tool.running
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
background.stopping
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

MCP event payload:

```json
{
  "server": "filesystem",
  "transport": "stdio",
  "auto_connect": true,
  "timeout_seconds": 3,
  "tools": 4,
  "resources": 2,
  "prompts": 0,
  "error_code": -260,
  "error": "network error"
}
```

Tool event payload:

```json
{
  "tool": "img_gen",
  "args": {"prompt": "..."},
  "tool_call_id": "call_123",
  "result": {"path": "..."},
  "error_code": -5,
  "error": "I/O error"
}
```

Task event payload:

```json
{
  "task_id": 8,
  "title": "Hourly AI news",
  "kind": "agent",
  "trigger_type": "interval",
  "status": "waiting",
  "next_run_at": 1782117600,
  "attempts": 2,
  "max_attempts": 0,
  "interval_seconds": 3600,
  "reason": "interval",
  "error_code": -5,
  "notification_id": 42,
  "notification_level": "warning",
  "notification_title": "Hourly AI news",
  "notification_body": "The search failed because ..."
}
```

`reason` is the machine-readable outcome subtype. Current values include
`completed`, `interval`, `retry`, `timeout`, `max_attempts`, `runner_error`,
`condition_not_met`, `no_runner`, and `cancelled`.

ReAct event payload:

```json
{
  "turn_id": "optional-stable-id",
  "iteration": 2,
  "text": "partial or final text",
  "state": "thinking",
  "outcome": "timeout",
  "reason": "step_timeout",
  "error_code": -110,
  "error": "Operation timed out"
}
```

Terminal ReAct events (`react.turn.end`, `react.cancelled`,
`react.timed_out`, `react.max_iterations`, and `react.failed`) include
`outcome`. Failed terminal events also include `reason`, `error_code`, and
`error`. Stable outcomes are `success`, `cancelled`, `timeout`,
`max_iterations`, `llm_error`, `tool_error`, `guardrail_denied`, and
`internal_error`.

HITL event payload:

```json
{
  "tool": "bash_exec",
  "args": {"cmd": "make test"},
  "verdict": "approved"
}
```

Artifact event payload:

```json
{
  "id": "artifact-id",
  "kind": "image",
  "mime": "image/png",
  "path": "/abs/path",
  "url": "/api/artifacts/artifact-id"
}
```

## Front-End Renderers

CLI human renderer:

- Renders startup and MCP progress as concise status lines.
- Renders ReAct thinking/tool progress through the existing spinner behavior.
- Renders final answers with markdown/media handling.

CLI structured renderer:

- Emits one JSON object per line.
- Should be enabled by an explicit option such as `--events=json`.
- Must not mix ANSI output into stdout in structured mode.

FastCGI renderer:

- Maps unified events to SSE records.
- Must stop parsing `tool(args)` strings after React emits structured tool
  events.

GUI/Android renderer:

- Consumes structured event objects.
- Uses `message` only as fallback display text.
- Should switch on stable `name` and read structured `data`.

Test recorder:

- Records events in `morph_array_t`.
- Supports assertions on event order, names, phases, and payload fields.

## Required Migration Points

The implementation is incomplete until every item below has been reviewed.

Core event module:

- `src/event/event.h`
- `src/event/event.c`
- top-level and `src/` CMake integration

CLI:

- `src/sapi/cli/cli.h`: event callback/mode fields in `struct cli_context`
- `src/sapi/cli/init.c`: startup init path
- `src/sapi/cli/init.c` and `src/sapi/cli/commands/mcp.c`: MCP init and `/mcp` command events
- `src/sapi/cli/events.c`: JSON and human event renderers
- `src/sapi/cli/core.c`: one-shot command path emits structured events
- `src/sapi/cli/scheduler.c` and `src/sapi/cli/core.c`: task scheduler and memory consolidation background events
- `src/sapi/cli/main.c`: CLI flags and startup event mode wiring

ReAct:

- `src/agent/react.h`: event callback API on `react_context`
- `src/agent/react.c`: turn begin/end
- `src/agent/react.c`: streaming thought deltas
- `src/agent/react.c`: action, tool call/running/result/failed/cancelled
- `src/agent/react.c`: observation/reflection/final
- `src/agent/react.c`: cancellation and timeout events
- `src/agent/react.c`: HITL request/verdict events
- `src/agent/react.c`: guardrail reflection events
- `src/agent/react.c`: artifact detection from structured tool results

Tools and artifacts:

- image/video tools that produce artifact paths through React artifact events
- plan and ask_user tools that already produce UI JSON

MCP:

- `src/mcp/mcp_client.c`: register functions return structured counts
- `src/sapi/cli/events.c`, `src/sapi/cli/init.c`, and `src/sapi/cli/commands/mcp.c`: registered, skipped, connecting, connected, discovering,
  ready, timeout, failed, and disconnected events

FastCGI:

- `src/sapi/fastcgi/handlers/turns.c`: consumes unified React/tool/artifact events
  and maps them to existing SSE records
- `src/sapi/fastcgi/handlers/events.c`: SSE output compatibility
- `src/sapi/fastcgi/README.md`: update event type documentation

Sub-agents/background work:

- `src/agent/sub_agent.c`: task started/completed/failed events
- `src/agent/sub_agent.c`: child ReAct contexts inherit event callbacks
- `src/agent/tools/sub_agent_tools.c`: delegate/status/fanout are surfaced
  through tool events and sub-agent background events

Memory/compression:

- `src/sapi/cli/core.c`: background memory consolidation status is surfaced
- compression remains a ReAct internal operation and is visible through the
  enclosing turn events

Docs/tests:

- `REQUIREMENTS.md`: mention unified events as multi-front-end foundation
- tests for event JSON serialization
- tests for React structured tool events
- tests for React artifact events
- CLI JSON smoke path for startup/background events
- FastCGI build coverage for the unified event bridge

## Compatibility Plan

Phase 1:

- Add event module.
- Add event callback fields.
- Emit MCP startup events.
- Keep current `react_output_cb` unchanged.

Phase 2:

- Make React emit structured events while still calling `react_output_cb`.
- CLI may keep old renderer during this phase.
- FastCGI can prefer structured events and fall back to old callback.

Phase 3:

- Move CLI renderer to unified events.
- Move FastCGI bridge fully to unified events.
- Keep `react_output_cb` as a compatibility wrapper.

Phase 4:

- Remove text parsing of `tool(args)`.
- Treat old callback as deprecated.

## Acceptance Criteria

- CLI default behavior remains usable and human-readable.
- Structured CLI mode produces valid NDJSON with no ANSI escape sequences on
  stdout.
- FastCGI SSE receives structured tool name, args, result, and artifact data
  without parsing human text.
- MCP auto-connect progress, timeout, failure, and ready states are observable.
- ReAct turn begin/end, streaming deltas, tool lifecycle, final, cancellation,
  and failure states are observable.
- Tests pass with `ctest --output-on-failure`.
- No new hand-rolled buffers, arrays, maps, or UTF-8 helpers are introduced.
- Existing public behavior remains compatible until the old callback is
  explicitly deprecated.
