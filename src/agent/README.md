# Agent implementation boundaries

Runtime owns the process lifecycle, models, tool registries and agent contexts.
Frontends enter through `runtime_execute_turn()`; these modules implement the
engine beneath that facade.

## ReAct output

| Module | Responsibility |
| --- | --- |
| `react.c` | One explicit turn loop, model/tool decisions, hooks and terminal outcomes. |
| `react_request.h/.c` | Private prompt/message/schema preparation and compaction budgets. |
| `react_tool_worker.h/.c` | Private joinable workers, cancellation and borrowed callback closure. |
| `react_output.h/.c` | Typed output events and synchronous callback dispatch. |
| `react_events.h/.c` | Structured event serialization, using a borrowed event sink. |
| `react_stream.h/.c` | Request-local incremental text parsing and channel separation. |

The output, event and stream modules do not depend on `react_context`. The owner
supplies the callbacks, cancellation flag and output-suppression flag. A stream
owns its buffers until `react_stream_cleanup()`; the model response owns its
separate response data. Legacy text-only responses are copied into an independently owned model
response before the stream is destroyed. Local request and response owners use
typed scope cleanup; transfers clear the source only after acceptance.

Stream callbacks borrow their text only until return. They must copy text they
retain, and cannot feed the same stream reentrantly. Sink and allocation failures
are sticky: subsequent input cannot replay an already delivered delta. Content
and reasoning preserve UTF-8 codepoint boundaries independently; incomplete
UTF-8 at completion is a protocol error. Cancellation is checked before input is
appended. Suppressed output stays buffered until output resumes or the stream
is destroyed.

Legacy final markers are recognized only at line starts, after optional spaces
or tabs. Native content retains literal marker text. Incremental scanning keeps
only possible marker prefixes pending, and skips ordinary text in batches. Delta
delivery borrows slices of reusable buffers without per-delta allocation.

## History and memory

`history.h` remains the agent history contract. Recording is in `history.c`;
`history_replay.c` owns protocol reconstruction, `history_content.c` owns secret
redaction and result budgets, `history_compaction.c` owns retained-window commits,
and `history_inspection.c` owns diagnosis and repair. Their shared helpers stay
in `history_internal.h`. Model-generated checkpoints are produced outside the
commit transaction; replay and active-request validation still precede commit.

`memory.c` owns input interpretation, advisory extraction and turn consolidation.
`memory_context.c` selects relevant context, `memory_render.c` formats the session
tree, and `memory_query.c` formats filtered and multi-session queries. Persistence
owns fact versions, generation fences, episode writes and copied record snapshots
through `memory_repository.h` and `memory_snapshot.h`; it does not format the
memory query UI. Snapshot strings remain owned until `memory_snapshot_cleanup()`.

`memory_jobs.c` owns worker/queue lifecycle and recovery decisions. The concrete
`persistence/memory_job_store` owns job SQL, conditional state changes and copied
record snapshots; background status formatting stays in `memory_render.c`.
Memory write transactions begin and finish through `memory_repository` APIs.
Generation checks guard derived writes after model requests complete, so model
latency never extends a memory write transaction.

`persistence/history_store` also owns the visible-message reads shared with recap,
retention-row token writes, repair updates and summary-source inserts. Agent code
selects token/counting and compaction policies and keeps its short savepoint
boundaries explicit; the storage layer does not run model or retry policies.

## Tools

| Interface | Responsibility |
| --- | --- |
| `tool_artifact.h` | Artifact values and their JSON representation. |
| `tool_result.h` | Result ownership, envelopes, text and attached artifacts. |
| `tool_registry.h` | Descriptors, registration, replacement and retained handles. |
| `tool_execution.h` | Permission-gated execution and business-result normalization. |
| `tool_runtime.h` | Borrowed per-call application services and stream output. |

`tool.h` remains a compatibility umbrella. New implementation code should include
the interface it uses directly. Add compilation units to `sources.list`, which
is shared by desktop and mobile builds.

A raw `tool_entry` is a registry-owned view. Its address and descriptor strings
can change during registration, replacement or removal. Callers that inspect
raw entries must hold the registry data lock; structural writes acquire the
mutation lock first. Registry cleanup requires execution owners to be drained.

`tool_registry_acquire()` instead returns an allocation-free `tool_handle`. It
retains the callback binding and copies execution capabilities under the data
lock. The handle remains valid across registry growth, replacement and removal.
Descriptors are deliberately excluded so a handle cannot accidentally expose
freed descriptor strings. The permission context is borrowed from runtime and
must outlive all handles.

Acquire into an unowned handle, do not copy ownership, and release it after the
last callback or user-data access. Releasing an empty handle is safe. Acquisition
reports the disabled state; normal dispatch rejects disabled tools. Background
cleanup can still retain disabled tools to drain existing work safely. Callbacks
run without holding registry locks, including callbacks that register or remove
tools themselves.

## Verification

`test_react_stream.cpp` exercises stream boundaries without an HTTP server or an
agent context. `test_tool.cpp` covers retained handles across growth, removal and
replacement. Existing ReAct, turn, subagent, permission and HTTP-mock tests cover
integration contracts, cancellation, timeout, history replay and compaction.
