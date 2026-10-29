# Asynchronous commands

`exec` runs a command with approval and sandboxing. Short commands return their
result directly. Commands still running after `yield_time_ms` return a process
session; `background=true` skips that initial wait. Returning from the tool does
not mean the command has finished.

## Task area and inspection

The interactive CLI keeps background commands above the work/status row. Each
command has its own identity; polling never creates another task:

```text
◉ Build project
  └─ cmake --build build -j8                         42s
```

The optional parent is the model-supplied `exec.description`, in the current
response language. The child is the actual command. Without a description, the
command uses one line. Commands are clipped to the available display width and
control sequences are sanitized. At most three jobs appear, or one in short
terminals, followed by an overflow count. Logs stay out of the task area.

Successful jobs remain briefly and then disappear. Failed, timed-out, signaled,
and killed jobs remain until their terminal result is inspected. Each completion
also produces one history receipt. Jobs remain visible while the model works on
other steps and after a turn has ended.

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

The `process` tool supports:

| Action | Behavior |
| --- | --- |
| `list` | List this conversation's current-runtime jobs; no process ID required |
| `poll` | Return on new output, exit, cancellation, or deadline |
| `wait` | Collect through intermediate output until exit, cancellation, or deadline |
| `write` | Write stdin, then return the current snapshot |
| `interrupt` | Signal the process group, then return its current snapshot |
| `kill` | Request process-group termination, then return its current snapshot |

`poll` and `wait` default to `wait_ms=30000`, with a maximum of 60000. Explicit
`wait_ms=0` performs a nonblocking read. Pass the previous `next_cursor` as
`cursor` for replayable incremental output. UI inspection and completion notices
do not advance this cursor. Cancelling a wait leaves the process running;
stopping a process is a separate action.

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
`process_jobs` table. Current-runtime details can read the whole retained output
window (`exec.max_session_output`, default 1 MiB per stream); model responses and
saved completion snapshots use `exec.max_inline_output` (default 32 KiB).
Truncation is identified explicitly. This is bounded retention, not unlimited
log archival. Output spool files remain temporary and are removed at shutdown.

After restarting Morph, completed results remain inspectable. Records that were
still running in a different runtime show `unavailable`; Morph does not infer
that an old PID is alive or reattach it. Stored spool paths are not reused.
Reading a task list or refreshing its UI does not invoke the model.
