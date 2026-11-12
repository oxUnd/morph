# Asynchronous commands

`exec` runs a command with approval and sandboxing. Short commands return their
result directly. Commands still running after `yield_time_ms` return a process
session; `background=true` skips that initial wait. Returning from the tool does
not mean the command has finished.

## Task area and inspection

The interactive CLI summarizes running background commands in one live row:

```text
◉ 2 background jobs running · /jobs to view
```

While collecting a command's result, it expands only the command being waited on:

```text
◉ Waiting for background job (3m 10s · Esc to interrupt wait) · 2 running · /jobs to view
  └ cmake --build build -j8
```

The running marker pulses between normal and dim cyan every 500 ms, in sync
with the marker before `Working`. It keeps pulsing while the agent waits and
after a turn ends if background jobs remain, without changing line width or
repainting the composer.

The elapsed time is the command's running duration. Narrow terminals shorten the
hints while retaining `Esc` and `/jobs`. Commands are clipped to display width and
terminal controls are sanitized. Logs and the model-supplied `exec.description`
remain in `/jobs`; polling never creates another job. Completion removes the live
row immediately and leaves one history receipt, including the description and
exit status. Results remain available through `/jobs`.

In the interactive composer, `Esc` during an explicit `background` wait or the
runtime's final result collection ends the current wait/turn while retaining its
background commands. The draft remains editable. `Ctrl+C` still cancels the turn
and stops its commands. With queued prompts, `Esc` retains its existing behavior
of applying those prompts. Stop an individual job through `/jobs stop <id>` or the
job panel's `k` action.

`/jobs` opens the current conversation's live job list. Up/Down selects a command,
Enter opens its command, working directory, state, exit code, and output. Output
supports Up/Down, PageUp/PageDown, Home and End. `k` stops the selected process
group; `r` refreshes. Escape returns to the list, then closes the panel without a
close receipt. A pending permission/interaction dialog takes precedence over the
panel. The list refreshes once per second and preserves the selected process ID.

`/jobs <id>` opens a specific result. IDs may be complete `proc_...` IDs or unique
prefixes of their random suffix. `/jobs stop <id>` requests termination. These
commands are local actions available during an active turn. Non-interactive
queries return JSON; JSON command mode retains the command event envelope.

## Waiting and completion

The `background` tool replaces the former `process` tool. `process` is no longer
registered; historical transcript rows retain the original name and display
`process [deprecated]` so the old name cannot be mistaken for a current API.
A stale invocation fails with an explicit deprecation message pointing to
`background`. Update tool-name configuration to `background`; an existing
`disabled_tools = ["process"]` still disables `background` and logs a deprecation
notice, preserving the explicit restriction during migration.
Session IDs, result fields, storage and `process.started` / `process.completed`
events retain their existing contracts.

The `background` tool supports:

| Action | Behavior |
| --- | --- |
| `list` | List this conversation's current-runtime jobs; no process ID required |
| `poll` | Return on new output, exit, cancellation, or deadline |
| `wait` | Collect through intermediate output until exit, cancellation, or deadline |
| `write` | Write stdin, then return the current snapshot |
| `interrupt` | Signal the process group, then return its current snapshot |
| `kill` | Request process-group termination, then return its current snapshot |

For `poll` and `wait`, the model chooses an integer `wait_ms` in the inclusive
range `0` to `[exec].max_wait_ms`. The configured maximum defaults to 600000 ms
(10 minutes); omitting `wait_ms` still uses 30000 ms, or the configured maximum
if it is smaller. The tool schema and description advertise the actual range.
Results report `wait_ms` and `max_wait_ms`. This is a collection budget, not the
command timeout: a still-running result should be waited on again using the same
session ID. Invalid types, negative, fractional and out-of-range budgets are
rejected before any write or signal is applied. Explicit `wait_ms=0` performs a
nonblocking read. Pass the previous `next_cursor` as
`cursor` for replayable incremental output. UI inspection and completion notices
do not advance this cursor. Cancelling a wait leaves the process running;
stopping a process is a separate action.

For example, `[exec] max_wait_ms = 1200000` permits the model to choose waits up
to 20 minutes. The background tool's own timeout reserves time beyond that configured
maximum so it does not prematurely end a valid long wait. Waits remain cancellable.

Before accepting a final answer, the agent collects unobserved finite commands
started by its current turn. It waits in the runtime without additional model
calls or shell sleeps, then supplies their actual output and exit states for a
final assessment. New user input or cancellation interrupts this wait. Commands
from other turns are not silently adopted as new work.

Use `service=true` for persistent servers/watchers. They remain tracked but do
not hold the final answer. `notify_only=true` also permits a finite command to
outlive the turn, and should be used only when the user explicitly requests
starting work and receiving a completion notice later. Neither flag means the
command succeeded. `background=true` alone does not waive result collection.

## Ownership and retention

The runtime facade provides process list/read/stop operations; frontends do not
access tool internals. Process metadata is tagged with its conversation and turn.
Cross-conversation reads and controls are rejected. Completion callbacks run
outside the process-manager lock and are joined during shutdown. Final durations
are measured at process exit and do not increase on later reads.

The runtime retains state and a bounded, redacted completion snapshot in the
`process_jobs` table. Current-runtime details can read the memory output window
(`exec.max_session_output`, default 1 MiB per stream); model responses and saved
completion snapshots use `exec.max_inline_output` (default 32 KiB).

Independent spool files capture redacted output from offset zero under a shared
64 MiB quota. Read relevant file ranges to recover omitted inline bytes.
`*_path_bytes` and `*_path_truncated` report how much was captured. The gap from
`*_path_bytes` to `*_retained_from`, when `*_unrecoverable_bytes` is nonzero, is
unavailable in both storage paths and requires a narrower rerun. `next_cursor`
advances past inline omissions and cannot recover them. Spool files are temporary
and removed at shutdown; they are not unlimited log archival.

After restarting Morph, completed results remain inspectable. Records that were
still running in a different runtime show `unavailable`; Morph does not infer
that an old PID is alive or reattach it. Stored spool paths are not reused.
Reading a task list or refreshing its UI does not invoke the model.
