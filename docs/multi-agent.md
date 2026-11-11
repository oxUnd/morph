# Multi-agent collaboration

Morph follows the Codex MultiAgentV2 collaboration model. The reference source is
OpenAI Codex commit
[`6107b9886c114702e5c1cfd1c1b54f7c48bfaf49`](https://github.com/openai/codex/tree/6107b9886c114702e5c1cfd1c1b54f7c48bfaf49/codex-rs/core/src/agent).
The relevant implementations are `control/`, `history.rs`,
`tools/handlers/multi_agents_v2/`, and `session/multi_agents.rs`.
Morph implements these semantics in C using its existing runtime, ReAct,
permission, model, and history facilities. There is no separate team or task
framework.

## Enabling Ultra-style delegation

```toml
[agents]
enabled = true
max_threads = 6
max_depth = 2
multi_agent_mode = "auto"
```

`auto` selects proactive delegation when the effective model reasoning effort
is `ultra`, matching Codex's effort-dependent mode selection. Other efforts
require explicit delegation instructions from the user, project, or skill.
`explicit` and `proactive` override automatic selection. This changes delegation
instructions, not the provider's available reasoning levels or model quality.

For a Chat Completions model that accepts Ultra:

```toml
[model.text]
# Keep your configured provider, endpoint, model and api_key_env.
extra_body_json = '''{"reasoning_effort":"ultra"}'''
```

For Responses models, the equivalent field is `reasoning.effort`:
`extra_body_json = '''{"reasoning":{"effort":"ultra"}}'''`.
Setting `multi_agent_mode = "proactive"` also enables automatic delegation for
models with other reasoning settings. Frontends opt into the capability with
`runtime_options.enable_multi_agent`; CLI and FastCGI do so.

## CLI mode selection

`/model` or `/m` opens an inline Normal/Ultra list in the composer area. Arrow
keys or digits select; Enter applies; Escape or Ctrl+C cancels. `/model normal`
and `/model ultra` apply directly, `/model status` reports the selection, and
`/model <name>` continues to switch the text model.

The selection persists `agents.multi_agent_mode` as `explicit` or `proactive`.
It does not replace provider reasoning parameters: existing model settings keep
working with either collaboration mode. Saving must succeed before the runtime
and prompt change. Active turns reject mode changes. Ultra uses a purple `»`
prompt and a short wave in the input padding. The animation runs on the UI event
loop, preserves drafts and cursor positions, and remains out of scrollback and
structured output.

Ultra turns append the agent summary to the `Working` row, including before
delegation: `Agents: 0 running`
means the model has not spawned an agent. Once work starts, rows show
canonical agent paths and current activity such as `Thinking` or `Running exec`,
with one row per active agent and a bounded `+N more` row when necessary.
Full task prompts appear in `/agents` and expanded turn details. Targeted
`/agents /root/name` output preserves complete multiline tasks and results,
including content beyond 8 KB. Completion receipts wait for the active parent
message to finish before entering history, keeping Markdown streams contiguous. Collaboration
tool records follow the Codex `multi_agents.rs` presentation: `Spawned …`,
`Sent message to …`, `Waiting for agents`, and `Finished waiting`, with a bounded
prompt preview for assignments and messages. Completion and failure leave a
concise `Finished …` / `Failed …` receipt. `/agents` works
during a turn and lists all assignments, status and shared token usage;
`/agents /root/reader` also shows the result. Agent events repaint on the UI
owner thread without replacing the user's draft. The compact area shows up to
three active agents; `/agents` exposes the complete list.

`/agents` opens a full-screen browser with live status, a scrolling agent roster
and a selected-task preview. Enter opens the selected child's independent feed
without nesting alternate screens; Escape returns to the primary conversation.
`/agents /root/reader` opens that feed directly. Text/JSON command execution keeps
the existing snapshot output.

`/subagents` replaces the composer with a compact agent picker, including the
primary thread, while retaining the main feed above it. The picker uses the
same reusable composer panel as `/model` and `/permissions`, sizes itself to
the visible entries and leaves the main conversation in place. Select an
agent with Up/Down and Enter, or open `/subagents /root/reader` directly.
Selecting a child switches the entire visible feed to an alternate terminal
screen containing that child's conversation. In a
child thread, Tab/Shift+Tab or Left/Right switches threads, Up/Down and Page
Up/Down scroll, Home/End jumps, `f` opens a directed follow-up composer and `k`
interrupts that agent. `d` or Ctrl+T toggles tool details.
Escape returns directly to the primary feed, including from tool details;
while editing a follow-up, Escape first leaves the editor. Switching to the
primary thread also returns. The primary draft survives navigation, and each
child retains its own scroll position, details setting and unsent follow-up.
New output follows automatically while the viewport is at the bottom. Scrolling
up pauses following; scrolling back to the bottom or pressing End resumes it.
Follow-ups to running agents steer them at safe
points; idle agents start another turn. Multiline bracketed paste waits for an
explicit Enter before sending.

Each child has an independent transcript reducer for user messages, reasoning,
answers and tools, with the same feed message styles, Markdown/code rendering
and compact tool rows as the main conversation. The child viewport uses the
full terminal width and height, with its own scrolling and feed state.
Primary events keep updating the primary transcript in the background; their
rendered output is deferred until the primary screen is restored. Child
preview rows never enter primary terminal scrollback.
Snapshot and live events share an atomic sequence frontier,
so opening a running thread neither duplicates nor skips streamed text. Child
events remain separate from the primary transcript. The overview uses small
activity snapshots instead of copying complete tasks/results on animation ticks.

## Delegation decisions and integration

Proactive mode provides a decision policy at every model request:

1. Identify dependencies before assigning work. Keep critical-path decisions
   with the primary agent; dispatch independent work whose inputs are ready.
2. Give each assignment an objective, sufficient context, owned files,
   acceptance checks and the evidence the primary needs back. Prefer
   `fork_turns=none` for a self-contained investigation. Assign disjoint discovery
   scopes; the primary does not repeat a full scan already assigned to a child.
3. Continue independent primary work while children run. Verify returned claims
   with focused checks. A second complete review needs a concrete unresolved
   question and a bounded assignment, including when a child delegates again.
   Finding limits are ceilings, not quotas: return fewer findings or no findings
   when the assigned checks are complete.
4. Review returned evidence, integrate the edits, resolve conflicts and run
   final verification. A child's completion is not proof of correctness.
5. Reuse idle identities and keep enough shared budget for integration.
   Send confirmed findings and blockers to the parent as they become available.
   After a wait times out, assess progress and request current conclusions
   before extending the investigation. Do not repeatedly wait without making
   a decision about the remaining work.

Use `assignment` on `spawn_agent` and idle `followup_task` calls to record a compact
contract alongside the full `message`:

```json
{
  "scope": "INI parsing and serialization",
  "parent_task": "",
  "expected_evidence": "Confirmed file:line findings and minimal reproductions",
  "stop_condition": "Report once the named checks pass or a blocker is confirmed"
}
```

Each value is at most 512 UTF-8 bytes. `parent_task` names independent, unassigned
work reserved for the caller; an empty value explicitly reserves coordination
only. The other fields must be nonempty. A running agent's contract cannot be
replaced: interrupt it first, then assign its remaining question through an idle
follow-up. An idle follow-up without a contract clears the previous contract.
Contracts are persisted, restored and included in session imports; legacy callers without
`assignment` remain supported.

Every request receives a bounded `delegation_checkpoint` after conversation
history, containing the caller's contract and up to eight active direct children
with their owners, scopes and stopping conditions. Additional children remain
available through `list_agents`. The spawn receipt also states the parent's next
action. Neither the checkpoint nor a status listing includes full child reports.

When all active direct children explicitly reserve no independent parent work,
the parent receives only coordination, planning, user-question and existing-job
control tools until it collects a child finding/blocker or the children settle.
The execution boundary also rejects stale investigation calls with
`agent_coordination_pending`; hiding schemas alone is not the enforcement. A child
message must be collected, not merely queued, to release this restriction. New
user steering or collected follow-up input can supply additional work and releases
the coordination-only restriction. Children keep their own investigation tools. Once evidence arrives,
the parent can run focused verification; arbitrary read-only scope overlap is not
inferred or locked. Legacy or independently reserved parent work remains
unrestricted, with current ownership visible in the checkpoint.

Task decomposition and dependency judgments are model decisions, not a new DAG
scheduler or a fixed minimum number of agents. Runtime enforces thread/depth
limits, independent contexts, mailbox delivery and the final-result wait. It
does not infer semantic dependencies from prose or claim to verify arbitrary
acceptance criteria. A small sequential task can correctly use zero children.
Model/provider instruction-following still determines delegation quality;
synthetic HTTP tests establish execution behavior, not a live model's judgment.

Child turns have a default soft deadline of 600 seconds, configured through
`agents.task_timeout_seconds`. Nested tasks inherit the ancestor's remaining
time. At a safe model boundary, a finish request closes discovery tools and asks
for the current verified findings, completed checks and remaining limitations.
This time bound is independent of token-budget enforcement; disabling the token
limit does not authorize unlimited investigation.

Finalization normally collects active descendant results for up to
`agents.finalize_wait_seconds` (60 seconds). If work is still running, the
controller requests current summaries and allows `agents.finalize_grace_seconds`
(30 seconds) before interrupting unfinished descendant work. Closing a branch
also closes admission for new work beneath it, and interruption covers the
entire descendant subtree. Results are collected before releasing worker-owned
resources. A zero task timeout or finalization wait disables that respective
limit; a zero grace period interrupts immediately after requesting summaries.
Cancellation and user steering remain responsive while waiting. In-process
tools must cooperate with cancellation; their borrowed resources remain alive
until they have safely drained.

## Task planning

`update_plan` accepts the Codex checklist shape: optional `explanation` and a
complete `plan` array of `{step, status}` entries. Status is `pending`,
`in_progress`, or `completed`, with at most one step in progress. It updates a
normal persistent `Task plan` entry in the existing session plan registry and
does not make an additional model request. Empty lists clear that checklist;
other named plans and the task plan's stable identity remain intact. Invalid
arguments and failed persistence leave the previous plan unchanged. Current C
storage limits are 32 steps and fewer than 256 UTF-8 bytes per step; oversized
input is rejected rather than silently cut.

Each child binds both `plan` and `update_plan` to its own registry and model.
Resumed child turns reload that registry without modifying the primary plan.
The existing `plan` tool remains available for requirements, acceptance,
verification evidence and named plans. Checklist completion is reported state,
not proof that acceptance checks passed.

Successful updates publish `plan.updated` with checklist state and explanation;
the CLI reuses its checklist presentation. Tool replies stay concise, and the
next model request receives the persisted plan through existing context
injection. Task decomposition and delegation decisions remain model policy.

## Tools

| Tool | Contract |
| --- | --- |
| `spawn_agent` | Start an independent agent with `task_name` and `message`; return its canonical path. |
| `send_message` | Queue a message without waking an idle agent. |
| `followup_task` | Start a new turn for an idle agent; deliver instructions at safe points for a running agent. |
| `interrupt_agent` | Interrupt the target and its active descendants, retaining identity and history. |
| `list_agents` | Page through compact status for the caller's subtree, or an explicit path prefix. |
| `read_agent` | Read one agent's full report through UTF-8-safe byte pages. |
| `wait_agent` | Wait for mailbox activity, cancellation, user input or a bounded timeout; return scoped progress and an explicit wake reason. |

`list_agents` never embeds completed reports. Its bounded pages contain agent
identity, status, elapsed time, activity, active-child count and result
availability. Follow `next_offset` while `has_more` is true; `total` and `active`
describe the entire selected scope. The default page contains up to eight rows
and approximately 2 KB of status data. Child calls default to their own subtree,
so another branch's completed report cannot hide their children.

`read_agent` is a Morph extension to the collaboration tool set. Supply `target`
and optionally a byte `offset` and `max_bytes` (4–4096, default 2048). Follow its
`next_offset` until `has_more` is false. If history preparation must shorten a
report page, it retains a contiguous UTF-8 prefix and advances the cursor only
past those visible bytes. Status identifies failed or interrupted work; retained
partial reports are evidence to review, not a claim of successful completion.

Normal `wait_agent` expiration returns success with `reason="timeout"` and
`timed_out=true`. Other reasons distinguish mailbox updates, pending user input,
finish requests and a root with no active agents. Timeout arguments use
milliseconds consistently with the outer tool worker, including waits longer
than the generic tool timeout. Cancellation remains an explicit cancellation
outcome, rather than a timeout or argument failure.

The primary agent is `/root`. Task names contain lowercase letters, digits, underscores and hyphens. `parser` spawned by `/root` becomes `/root/parser`. Relative targets
resolve under the sender; canonical paths support communication between branches.
Children of `/root/b` named `aa` and `bb` become `/root/b/aa` and `/root/b/bb`.
Names are unique within each parent: `/root/a/aa` and `/root/b/aa` are distinct
agents. Activity rows, completion receipts, tool rows and the thread viewer retain
the full path, including `/root`, so the parent is visible.
Only agents in the same root conversation can communicate. Descendants may spawn
within the configured depth and thread limits. `max_threads` includes the root and active child executions. Completed identities
remain addressable without consuming execution capacity. Spawning and resuming an
idle agent use the same admission limit; wait for capacity before starting more work.

`fork_turns` accepts `all` (default) or `none`. `all` copies active model history
up to the spawning tool exchange, leaving that incomplete exchange behind. The
child gets fresh identity, project context and cancellation state. `none` starts
with the supplied task and project instructions. Inherited tasks and live tool/process
handles are explicitly marked as historical context, not a new assignment. Only `none` accepts `model` and
`reasoning_effort` overrides. Model overrides use the configured text provider
and endpoint. Each execution clones its LLM client; provider request state is
never shared with the root or another agent.

Messages arrive before model requests and before accepting a final response.
An unread follow-up that races with a final response triggers another turn on
the same agent. Final answers are attributed and delivered to the parent. Every agent waits for its own active descendants and consumes their results before
accepting a final answer. This includes child agents coordinating grandchildren;
waiting excludes the caller itself. This is an execution safeguard; the model still has to integrate
and verify the findings.

## Runtime and ownership

`runtime` owns `agent_control`. The controller retains agent identities and
coordinates spawning, mail, interruption, persistence and joins. An agent does
not occupy a running worker while idle. Each child has an independent session-bound
plan registry; both `plan` and `update_plan` are rebound to that registry and its
own model clone. Restored follow-ups reload the child plan without changing the
primary plan. Each child also owns its skill catalog, activation state, configured
guardrails, checker/media model clients and mutable tool bindings. Skill
activations persist with that child's session and are restored on cold resume;
primary checkpoint rewind restores primary skill activation state as well.
Each execution owns a cloned model, retained tool
registry, ReAct context and a database connection (in-memory tests share the
connection with short connection guards). The worker drains tool workers before
publishing completion. Runtime joins agent workers before restoring request
callbacks, billing bindings or hooks. Cancellation and shutdown stop agents
before destroying their dependencies.

Tool registries retain the existing shared permission authority. A copied
registry does not grant wider permissions. Child tool calls carry their own
session, agent path, working directory, cancellation and event context. Child
billing owns its actor identity and writes through the runtime's lasting usage
observer, including idle follow-ups started from the viewer. Request-local
primary callback data is not retained by a child. New idle work is refused while
the primary drains workers and restores its request bindings; session changes
are refused while child work is active. Shutdown closes admission before joining.
Child questions do not invoke borrowed frontend question callbacks; agents can
send questions to `/root` for user interaction.

MCP discovery preserves an explicit boolean `annotations.readOnlyHint` through
registry copies and rediscovery. Missing or malformed hints do not classify a
tool as read-only. A trusted explicit `react.readonly_tools` entry takes
precedence and is applied to initial, background and repeated discovery;
revoking a server-provided hint removes only that hint's classification.
Unknown operations remain subject to the shared permissions and child write
scope. Permission diagnostics identify missing read-only classification rather
than misreporting every denied operation as a concurrent file write.

The workspace is shared, matching the collaboration tools' contract. Morph adds
an optional `write_paths` array to `spawn_agent` and to idle `followup_task`
assignments. This is a Morph ownership restriction, not a claim that the Codex
reference tool schema contains that field. Omission leaves patches and sandboxed
workspace access read-only; host execution follows the shared permission policy.
A follow-up without the field retains an existing in-memory assignment.

Claims cover canonical workspace paths, including new directories resolved from
the nearest existing ancestor. Unresolved traversal is rejected. Paths outside the workspace and
overlaps with an active writer are rejected before a worker starts. Scope
changes require an idle agent. `apply_patch` checks every write/delete/move
endpoint against the caller's assignment and active owners, including primary
edits; a patch in progress prevents a conflicting new assignment. Claims stop
blocking other agents when their execution finishes or is interrupted and
drained, and are checked again on reuse. Nested agents cannot broaden their parent's scope. A parent may lend a disjoint
subpath to a child: the child can write it while the ancestor is blocked from
that subpath, and the ancestor retains access to its other paths. Siblings cannot
reassign one another's write authority. Ending the child execution releases its
subpath back to the ancestor.

Child shell commands use the same host and network permission policy as the
parent. Full Access uses host execution by default; explicit `host=true` and
`network=true` requests enter the shared permission boundary without a separate
child-only prohibition. Host commands are not confined by `write_paths`.
For sandboxed execution, `agents.child_shell = "scoped"` grants only the current
`write_paths` plus a per-agent scratch directory, intersected with the parent's
permission ceiling. `readonly` restricts sandboxed writes to scratch only. `TMPDIR`
and `MORPH_AGENT_SCRATCH` identify the private directory; build with, for example,
`cmake -S . -B "$MORPH_AGENT_SCRATCH/build"`, then run its tests. Literal `/tmp`
paths do not refer to that private directory. The legacy macOS `/bin/bash` 3.2
here-document implementation can use global temporary storage despite `TMPDIR`;
create probe files with `printf` or Python in private scratch instead. A failed
here-document reports this next action without widening filesystem access or
silently substituting the configured shell. A broad parent
lease is withheld from shell while a descendant owns any of its subpaths, because
positive OS path grants cannot safely express that exclusion. Scoped child jobs
cannot use `service` or `notify_only`; finite background jobs must be collected
before completion releases write ownership. Ordinary descendants in the shell's
process group are killed before its completion is published. A program that
deliberately changes its process group or creates a detached session can escape
that cleanup; the current OS sandbox does not revoke an inherited path grant
when a lease ends. Child commands must therefore keep build/test descendants in
their managed process group, and persistent services belong to the primary.
The primary's newly launched shell
commands also run read-only while
there are active writers. Other mutating tools are rejected in these contexts;
use scoped `apply_patch` or scoped child builds, and let the primary run writable
integration checks after writers finish. Permissions remain an additional shared ceiling. Existing
processes, user edits and external services are not retrospectively sandboxed:
this is a collaboration ownership contract, not filesystem isolation from all
outside actors. No worktree manager or automatic merge layer is added.
Existing checkpoint semantics remain: primary-agent `apply_patch` edits are
tracked; shell and descendant edits are outside that tracking. Restoration and
permission changes reject active agents.

## Optional token budget

Token limiting is off by default. It requires explicit opt-in through the
[Codex rollout-budget configuration](https://learn.chatgpt.com/docs/config-file/config-reference):

```toml
[features.rollout_budget]
enabled = true
limit_tokens = 500000
reminder_at_remaining_tokens = [150000, 50000]
prefill_token_weight = 1.0
sampling_token_weight = 1.0
```

Enabling requires a positive `limit_tokens` and an explicitly supplied reminder
array; `[]` disables threshold reminders. Up to 32 remaining-unit thresholds are
accepted, each positive and below the limit. Order is immaterial; duplicate
thresholds produce only one reminder. Weights are finite, non-negative numbers,
including fractional values and zero, and default to 1.0.

The old `agents.max_total_tokens`, `budget_input_weight`, `budget_output_weight`,
`budget_warning_percent`, `budget_primary_reserve_percent` and
`budget_enforcement` keys are accepted for old configuration files but ignored
with a migration log warning. They never implicitly enable a limit. Existing
sessions whose usage exceeded an old limit can continue with the feature off.

Root and descendants draw from a common allocation. There is no fixed primary
reserve or automatic 70/80-percent dispatch policy. Crossing a configured
remaining-budget threshold sends attributed `BUDGET_REMINDER` mail to the root
and active children without stopping delegation. Actual usage reaching the
explicit limit stops the tree. Disabling the feature leaves accounting active
without reminders or token-based stopping.

Morph retains its root-session ledger: the allocation covers the root and all
descendants, follow-ups and future primary turns. Usage survives restart and
session import; importing retains the larger raw counters. A new session has an
independent ledger. This persistence scope is Morph's policy; the public Codex
configuration reference does not establish identical runtime scope.

Units are `(input - cached_input) * prefill_token_weight + output *
sampling_token_weight`. Missing cached usage is treated as non-cached input;
missing provider usage falls back to an estimate. Raw counters persist, and the
current weights apply to their cumulative total, rounded up once to whole units.
Changing weights therefore revalues recorded usage. Accounting is independent
from billing.

Concurrent ReAct requests reserve conservative weighted input plus maximum output
before dispatch. If maximum output does not fit, the request lowers its own
output maximum without changing model configuration. A request whose input plus
one output token cannot fit is refused locally, without exhausting or cancelling
the tree. Smaller requests can still run. Temporary in-flight reservations cause
waiting with cancellation checks, then release on every return. Reservations do
not survive restart. Provider usage can differ from estimates, and auxiliary
model calls are accounted without advance reservations, so this is not an exact
provider spending cap.

## Persistence and events

`agent_threads` retains root/child session IDs, canonical paths, model settings,
status and results. `agent_mailbox` retains attributed messages and consumption
positions. Completion state and parent notification commit together. Mailbox
acknowledgment and model-history insertion commit in the same database savepoint.
Failed history persistence leaves mail unread.

`agent_events` stores scoped child events. Consecutive streaming deltas with
identical metadata are coalesced in bounded 8192-byte batches; lifecycle, turn,
tool and stream-kind boundaries flush pending text. Every byte is retained, and
selected live streams still deliver each original delta immediately. Completion
events use immutable terminal-status snapshots; queued follow-ups publish progress
rather than falsely reporting a still-running agent as completed. Child output is forwarded only when
that agent is selected, preserving the root transcript. `agent.started`, `agent.updated` and
`agent.completed` background events expose identity, task, status and activity. Frontends use copied
`runtime_agent_*` snapshots and marshal worker events to their UI owner thread.

Restoring a conversation reloads agent metadata, history and unread mail.
Previously active agents become interrupted; no tool is replayed automatically.
An explicit `followup_task` continues from persisted history. Write claims are
not persisted: restored agents begin read-only, and an idle follow-up must
explicitly assign `write_paths` again before new edits. Session import
preserves these relations and messages while remapping session IDs; imported
running agents remain interrupted. Deleting a root session removes its internal
agent sessions. Internal agent sessions do not appear in ordinary session lists.

## Remaining Codex parity boundaries

The current source reference implements proactive delegation through model
instructions, not a mandatory DAG or fixed child count. Morph's Ultra UI selects
that orchestration policy; it does not reproduce the reasoning quality or effort
of a different provider/model.

Morph adds explicit write ownership and scoped verification to its shared
permission layer. These restrictions and the configurable weighted session-tree
allocation are Morph policies; their configuration is not a copied Codex API.
Idle identities no longer consume execution capacity, but full residency
eviction/archiving and automatic restoration are not implemented. Child questions
are routed through the primary rather than opening a second frontend approval
dialog. Model quality and semantic result integration remain model-dependent.

## Reproducible task evaluation

`tests/eval_multi_agents.py` runs two small C projects in Normal and Ultra mode.
One has independent modules; the other requires the primary to establish a
shared header before delegation. Every run uses the production CLI, isolated
workspace/configuration/database, actual file edits and compilation, and an
independent external verifier with 22 boundary checks. Reports include elapsed time,
agent overlap, token usage, failed tools, repeated reads, result delivery and
external acceptance. Protected fixtures are checked for modification, and the
external verifier uses fixed headers and explicit conditions rather than
candidate-controlled assertions. Candidate compilation and execution run through
`morph-agent-eval-runner` with a clean environment, private writable directories,
disabled networking, timeouts and process-group cleanup.

```sh
python3 tests/eval_multi_agents.py --driver build/bin/morph \
  --output /tmp/morph-agent-task-evaluation.json
```

Build both `morph` and `morph-agent-eval-runner` before running the evaluator.
The default HTTP fixture is deterministic: it verifies execution and integration,
and cannot establish live-model delegation quality or speedup. Opt into real
inference using an existing model profile and its `api_key_env` in the invoking
process; credentials are passed only through the child environment:

```sh
python3 tests/eval_multi_agents.py --driver build/bin/morph \
  --provider-config ~/.morph/config.toml \
  --token-budget 500000 --timeout 600 \
  --output /tmp/morph-agent-live-evaluation.json
```

Live runs retain the profile's explicit context/output limits, timeout, retry and
thinking settings; effective non-secret model parameters and the trial allocation
are recorded. `task_passed` reports external correctness separately from
`orchestration_passed`. An Ultra orchestration pass requires child model activity,
productive child patches, consumed results and primary verification evidence.
The dependency case additionally checks that the primary's header-contract
receipt precedes child implementation calls in persisted order. No observed
delegation is reported as `not_observed`, not as successful multi-agent work.
Copied fork history is excluded from metrics using turn ownership; ambiguous and
imported ordering remains unknown. Reported concurrency uses each identity's
last recorded execution interval, not a complete follow-up timeline.

`--mode ultra --require-delegation` runs a separate explicit-delegation probe.
It asks for two disjoint workers and primary integration, to check execution
independently from the model's proactive decision. Its timing and token usage
are not comparable to a policy-only run with different instructions.

The report excludes credentials, source, prompts and full replies. Keep several
live samples per mode before comparing performance. Historical sessions can be
analyzed without inference or database writes:

```sh
python3 tests/eval_multi_agents.py --session-db ~/.morph/data.db \
  --session-id 336 --output /tmp/morph-agent-session-evaluation.json
```

This reports the full root/descendant tree. Repeated reads can be justified;
mailbox acknowledgment establishes delivery, not semantic integration. Existing
session timestamps also cover resumed turns and should not be compared directly
with a fresh run's monotonic elapsed time.

## Replacing the old implementation

The old `sub_agent.c`, configured `agent_<name>` tools, `delegate`, `fanout` and
`agent_status` are removed. `[[agent.sub_agents]]` is rejected with a migration
message. Historical `sub_agent_tasks` and `sub_agent_events` tables remain for
reading/importing old conversations; they do not drive new execution.

Acceptance is covered by `AgentControlTest.*`, the collaboration session-import
regression, configuration tests, runtime/permission/history regressions and the
CLI layout/control-flow checks. HTTP-backed tests cover spawning, safe history
forking, follow-up, interruption, namespaces, model overrides, mailbox recovery,
permissions, events and root result integration. Additional tests cover shared-budget aggregation and
preflight rejection, ownership overlap and traversal, scope release/restoration,
and patch ownership checks plus host/network permission inheritance for children.
`cli_agents_pty_integration` and `cli_subagents_pty_integration` exercise spawn,
live tool activity, independent transcript switching, follow-up, interruption,
large UTF-8 results and draft preservation at 120, 80 and 60 columns. Runtime
tests cover child skill/guardrail isolation, cold restoration, actor binding,
drain admission, opt-in weighted budgets/reservations and actual scoped CMake/CTest
verification. `multi_agent_task_evaluation` registers the deterministic task runs
with CTest when Python 3.11 or newer is available.
