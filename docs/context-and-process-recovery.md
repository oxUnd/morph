# Context, task checkpoints, and process recovery

The runtime keeps conversation evidence, model-visible context, durable plans,
and live processes separate. A short model context is not a deletion policy for
original evidence.

## Durable plans

`plan create` and `plan update` retain their existing arguments. They also accept
`request`, `constraints`, `acceptance`, and `rejected` text. `plan checkpoint`
updates these fields without changing step status. `update` can record a step's
`evidence`, `blocker`, and `artifact`. Plans retain their stable IDs and existing
8-plan/32-step limits. Fields have bounded capacities; invalid or oversized
checkpoint fields fail without committing partial state.

The runtime binds plans to the current session. Successful tool mutations write
an atomic JSON checkpoint to `session_plans`; restarting or evicting an in-memory
session cache does not lose it. Session deletion cascades to the checkpoint.
History import includes plans, but does not grant filesystem access for imported
verification paths. Old databases gain the table through normal schema setup.

Every model request receives current task constraints and step status independently
of compaction. Detailed evidence and verification file hashes remain available
through `plan get`, while the request uses a smaller view. Reported completion is
not proof of semantic correctness. The model must still inspect acceptance
criteria, describe unmet conditions, and avoid claiming unverified success.

## Verification reports

After running a check, call `plan verify` with `plan_id` (or `plan`), `files`
(an array of input paths), `report` (the saved report path), and `outcome`
(`passed` or `failed`). The tool records SHA-256 fingerprints of inputs and the
report itself. It does not execute a check or infer success from an exit code.

Read authorization applies to every file. Up to 16 inputs plus a report are
allowed; each must be a regular file no larger than 64 MiB, and the serialized
report binding must fit 8192 bytes. Current reports are checked before model
requests and plan operations. Changed, missing, or newly inaccessible inputs
make a report `stale`. Identical current reports can be reused. This is explicit
report reuse, not automatic caching of shell commands or external service state.
Include all relevant dependencies and configuration files in `files`; omitted
dependencies and external environment changes cannot be detected.

## Process polling

`process` retains `poll`, `write`, `interrupt`, and `kill`. Poll accepts optional
`wait_ms` (integer from 0 to `[exec].max_wait_ms`, default maximum 600000) and
`cursor` (the previous result's `next_cursor`). The model chooses the wait within
that range; omitting the value defaults to 30000 ms, or a smaller configured maximum.
It returns when new output arrives, the child exits, the wait expires, or the
request is cancelled. Cancelling a low-level wait leaves the child running.
Cancelling the agent turn (Esc or Ctrl-C in the CLI) also stops processes started
by that session and turn, including background, service, and notify-only jobs.
Processes from earlier turns or other sessions retain their own lifecycle.
The initial `exec` yield checks cancellation, and stop requests escalate from
SIGTERM to SIGKILL after `[exec].kill_grace_ms`, even if the child ignores SIGTERM.

Explicit cursors identify absolute stdout/stderr byte positions and do not consume
shared output. Repeating a cursor replays the still-retained range, including any
newly appended output. Without a cursor, the original consuming incremental poll
behavior remains. Positions beyond current output fail; positions older than the
retained window receive available output with omission metadata.

Results include `next_cursor`, `wait_ms`, and `empty_poll` (a still-running poll
with no new text). Nonempty snapshots may include `stdout_path`/`stderr_path` for
redacted retained output, useful when inline output is shorter. These are rolling
windows, not unlimited transcripts: each is capped by `max_session_output`, all
files share a 64 MiB manager quota, and files are removed at runtime shutdown.
If disk materialization fails, inline output still works and no path is advertised.

## Model failures and compaction

Models retain a structured error category, HTTP status, provider code, retry delay,
and whether output started. Context overflow is identified from known provider
codes or explicit context-limit messages in HTTP 400/413 responses. Unknown errors
remain ordinary API errors. HTTP 401/403 are authentication errors; 429 is rate
limiting; 5xx is a server error. Failed streams after output begins are incomplete
responses and are not automatically replayed.

Transient requests without output use bounded retries. `Retry-After` supports
seconds or HTTP dates, is capped at one day, and waits remain cancellable.
Compaction calls are tagged `phase=compaction` in usage metadata; ordinary calls
are `phase=turn`. Cached and uncached input are recorded separately from the
estimated active request size.

Compaction failure never installs an empty placeholder checkpoint. Context-limit
recovery removes at most 32 old tool groups from a candidate summary input while
preserving user requests and prior summaries. Call/output pairing is preserved
even when steering messages intervene. Omitted evidence is explicitly marked
unavailable and is not recorded as semantically covered by the new summary.
Formal history changes only after successful summarization and replay validation.
Normal inference can request at most two overflow recoveries per turn. Three
successive compactions reducing context by less than 5% stop with an explicit
context-limit error; the existing post-compaction budget check remains stricter
when the request still cannot fit. These limits prevent endless compaction loops.

## Request organization and metrics

Stable system instructions, skill descriptions, and sorted tool definitions precede
history. Activated skill bodies, durable plan state, memory, and hook context are
request-only messages near the end, regenerated without persisting duplicate
copies to history. Legacy text-only backends receive equivalent combined text.
Skill discovery remains progressive: descriptions are listed before activation;
full bodies load on activation. No new deferred-tool search mechanism is required.

`react.context.usage` reports estimated active input tokens, tool count, and
context recoveries. Existing compaction begin/end events report before/after
sizes. Credit metadata records usage phase, cached tokens and uncached input;
process results expose empty polls. Estimates and provider-billed cumulative
usage are different measurements and must not be presented interchangeably.
