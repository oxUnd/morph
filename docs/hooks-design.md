# Hooks design

Status: first implementation delivered. See [Hooks usage](hooks.md) for the
implemented contract and compatibility limits. This design also records future
work: managed policy, sandbox profiles, immediate in-turn host revocation,
additional lifecycle events, and complete upstream protocol parity.

Morph should support command hooks at stable agent lifecycle boundaries.
Hooks can supply context, reject a pending tool call, inspect completed work,
or request another pass before a successful answer is committed.
The runtime owns configuration, execution, cancellation, and trust. CLI,
FastCGI, and mobile hosts consume the same runtime interface.

## Reference and scope

[Codex hooks](https://learn.chatgpt.com/docs/hooks) provide lifecycle command
handlers, JSON input on stdin, tool interception, and completion checks.
The public configuration and command-hook protocol target Codex compatibility.
Internal dotted events stay private. Compatibility is delivered per supported
event, with explicit diagnostics for unsupported capabilities; this proposal
does not claim complete Codex parity.

Initial use cases:

- Load workspace notes before processing a user prompt.
- Reject a tool call that violates a project rule.
- Run a targeted formatter after an apply_patch call.
- Check verification state before allowing a successful final answer.
- Record an accepted completion check and orderly session shutdown.

MVP excludes prompt/LLM hooks, asynchronous detached commands, permission
auto-grants, plugin discovery, and automatic commits.
Existing guardrails remain independently supported.

## Existing integration points

- `src/runtime/execute.c` binds session and request state around `react_run()`
  and finalizes durable turn state through `agent_turn_finish()`.
- `src/agent/react.c` owns tool scheduling, HITL, result collection,
  compaction, and final-answer guardrails.
- `src/event/` carries renderer-neutral notifications. Event callbacks are
  not a policy interface: a hook decision needs an explicit typed result.
- `src/exec/process.h` already provides process management, separate stdout
  and stderr capture, stdin writes, sandbox binding, timeouts, and interrupts.
  Its shell-command API needs an explicit bounded stdin-and-EOF exchange
  extension for the proposed hook protocol.
- Guardrail output checks currently have their own retry policy. Hook vetoes
  must not inherit a path that eventually accepts rejected output.

## Compatibility contract

Use Codex event names, nested matcher groups, command handlers, JSON field
names, and exit-code conventions. The compatibility reference is the official
hooks documentation retrieved on 2026-10-20. Pin fixtures to that baseline;
future upstream changes require an explicit compatibility update.

| Public event | Morph integration | First release |
| --- | --- | --- |
| `UserPromptSubmit` | Bound input before first model request | Yes |
| `PreToolUse` | Validated tool before permission checks and dispatch | Yes |
| `PostToolUse` | Completed tool before next model request | Yes |
| `Stop` | Candidate answer before successful final commit | Yes |
| `SessionStart` | Session startup/resume | Yes |
| `SessionEnd` | Orderly session close | Yes |
| `PermissionRequest` | Permission decision boundary | Later |
| `PreCompact`, `PostCompact` | Context compression | Later |
| `SubagentStart`, `SubagentStop`, `Interrupt` | Agent lifecycle | Later |

Recognize later-event configuration and list it as unsupported; never silently
pretend it is active. Support `type = "command"` initially. MCP, prompt, or
agent handlers receive an explicit unsupported diagnostic. Unsupported hooks
cannot be trusted as effective policy. A validation summary must distinguish
supported, unsupported, invalid, and awaiting-review entries.

## Configuration

Discover user `~/.morph/hooks.json`, inline hooks in the selected config.toml,
and project `<workspace>/.morph/hooks.json` or `.morph/config.toml`. Project
sources are loaded only after workspace trust, independently of hook trust.
Do not read `.codex/` automatically: importing existing Codex configuration is
an explicit copy/import action and must not execute commands.

Merge active hook sources additively, retaining source provenance; a higher
layer does not replace lower-layer hook arrays. Warn if a layer uses both
representations. Resolve sources once per turn into an immutable snapshot.

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "^Bash$",
        "hooks": [
          {
            "type": "command",
            "command": "python3 /absolute/path/to/check_policy.py",
            "timeout": 3,
            "statusMessage": "Checking command policy"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "python3 /absolute/path/to/check_completion.py",
            "timeout": 10
          }
        ]
      }
    ]
  }
}
```

Equivalent inline configuration:

```toml
[features]
hooks = true

[[hooks.PreToolUse]]
matcher = "^Bash$"

[[hooks.PreToolUse.hooks]]
type = "command"
command = "python3 /absolute/path/to/check_policy.py"
timeout = 3
statusMessage = "Checking command policy"

[[hooks.Stop]]

[[hooks.Stop.hooks]]
type = "command"
command = "python3 /absolute/path/to/check_completion.py"
timeout = 10
```

Timeout is in seconds. Support `additionalContextLimit` with upstream units
and per-event applicability verified against baseline fixtures; do not reuse
Morph byte limits under that name. Regex matchers apply to tool names/aliases
or event-specific trigger fields. `exec` matches native `exec` and alias `Bash`;
`apply_patch` matches its native name plus `Edit` and `Write`. Report the native
tool name in input. MCP matches use actual registered names, with explicit
aliases where necessary. Match a handler once even if multiple aliases match.
An absent matcher matches every invocation of that event; `Stop` ignores it.
Validate regex syntax and bound matcher length/input size.

Morph-only budgets and hardening options belong in `[hook_runtime]`, outside
the portable `[hooks]` tree. No mandatory custom ID, argv field, protocol
version, or per-handler `on_error` is added to portable configurations.

## Command protocol

Run the reviewed static `command` through the configured shell with workspace
cwd and no PTY. Prompt/tool values travel only through stdin JSON; never
interpolate them into the command. Extend process management with stdin EOF
and bounded simultaneous pipe draining. User shell syntax is supported because
it is part of the reviewed command itself.

Input uses the documented common fields and event-specific fields, without
wrapping them in Morph's old `data` envelope:

```json
{
  "session_id": "42",
  "transcript_path": null,
  "cwd": "/workspace/project",
  "hook_event_name": "PreToolUse",
  "model": "configured-model",
  "turn_id": "turn-123",
  "permission_mode": "default",
  "tool_name": "exec",
  "tool_input": {"command": "cmake --build build"},
  "tool_use_id": "call-1"
}
```

Map permission modes only where Morph has equivalent semantics. Document
unsupported mappings; never report a bypass mode unless actually active.
`Stop` includes `last_assistant_message` and `stop_hook_active`.
Use null for unavailable transcript paths rather than inventing a file format.
Keep Morph metadata out of portable fields; optional extensions use a dedicated
namespace. Publish schemas and baseline input/output fixtures for each event.

A tool denial uses the upstream output shape:

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "Protected directory."
  }
}
```

A completion check asks for continuation using:

```json
{"decision":"block","reason":"Run the focused tests before finishing."}
```

Support exit 2 plus stderr as event-specific blocking feedback; exit 0 parses
that event's permitted JSON or text output. Other exit codes, timeouts, malformed
output, and unsupported output combinations are hook errors, reported visibly.
Default compatibility mode reports errors and continues rather than converting
a crashed policy script into a deliberate denial. An optional Morph strict
policy can fail closed, clearly shown in review and configuration inspection.

Implement event-specific output decoding rather than accepting a universal
`decision`: include `additionalContext`, `systemMessage`, the legacy pre-tool
block shape, and post-tool feedback/replacement behavior where documented.
`Stop` expects JSON at exit 0; plain text is not automatically context for every
event. `continue: false` and other common fields have event-specific validity.

Support `PreToolUse.updatedInput` only with the documented allow decision and
valid replacement arguments. Revalidate rewritten input and run all ordinary
permissions on the final call; hook allow never grants new capabilities.
If multiple concurrent hooks propose different rewrites, stop dispatch with an
explicit conflict diagnostic; never choose a nondeterministic winner. This is
an explicit Morph conflict rule pending upstream fixture verification.

## Review and trust

Review is required in the first release for every non-managed hook, including
user hooks. Loading a config file is not consent to execute its commands.
The `/hooks` entry point opens a browser with these views/actions:

- List event, matcher, source, command summary, and effective status.
- Inspect full command, timeout, cwd, aliases, context limits, and runtime policy.
- Review a new definition or the diff from a previously trusted definition.
- Trust the exact displayed definition, disable/enable it, or revoke trust.
- Inspect bounded recent outcomes and run configuration checks without execution.

Exact subcommand spelling is a Morph UI choice, not an upstream compatibility
claim. Suggested CLI equivalents are `/hooks review <id>`, `/hooks trust <id>`,
`/hooks disable <id>`, `/hooks enable <id>`, and `/hooks revoke <id>`.
Review never runs the script. Trust is an explicit host-user action; a model
cannot approve its own hook through config_edit, apply_patch, exec, or tool calls.
Structured CLI/FastCGI expose the same review payload and host-originated action.

Persist trust outside project-controlled files, scoped by user and canonical
source/workspace identity. Bind the review to a SHA-256 digest of canonical
configuration, event, matcher, command, timeout, limits, and relevant runtime
policy. Derive a stable display ID independently of that digest. Changed
execution-affecting definitions enter `changed / needs_review`; new ones enter
`needs_review`. Disabled state and trust are separate: enable does not grant
trust, revoke immediately prevents future launches. A formatting-only change
does not invalidate a canonical semantic digest.

Review actions carry expected digest and source generation. Re-read and compare
before persisting trust; reject stale actions. Recheck effective trust before
launch so revocation takes effect even within a turn; disable/revoke can cancel
an active child through explicit host action. Trust does not attest mutable
script contents or dependencies; show resolved paths and this limitation.
Never label script contents as verified by a definition hash.

Unreviewed/changed hooks are skipped with a visible startup summary and a link
or instruction to `/hooks`. Noninteractive and scheduled execution never waits
for review. Admin-managed sources, if introduced, require a real host policy
provider and cannot be created by a project setting or model-editable config.
MVP has no implicit managed source and no bypass-trust switch.

## Control flow and completion

```text
bind request/session
  -> UserPromptSubmit -> existing input checks -> model
  -> validate tool -> PreToolUse -> existing permissions/HITL -> dispatch
  -> collect result -> existing tool-output checks -> PostToolUse -> model
  -> existing final checks -> Stop
       revise -> append attributed feedback -> model
       continue -> commit final
  -> persist turn -> release request bindings
```

A tool denial creates a correctly paired tool response using the original
call ID; the model can choose another action. It cannot grant permission or
change persistent grants. Denied calls in a batch do not cancel independent
allowed calls. Existing program-level exec approval remains authoritative.

Revision runs inside the same turn and existing iteration/time budget. At most
two additional hook revision rounds are allowed by default. Every new candidate
is checked again. If vetoes persist at the limit, finish with a blocked outcome
and reason; never silently accept the candidate. Guardrail and hook counters
are separate, with the overall turn deadline bounding both.

Streaming needs an explicit contract: when any before-finish hook is active,
buffer potential final text until accepted. Preserve normal streaming when no
such hooks exist. Tool preambles can be released when the response resolves
to tool calls. Rejected candidates never emit `react.final`, persist as a
successful assistant answer, or appear as accepted output. Audit forced-final
paths, including repeated tool failures, so only genuine error terminal paths
skip before-finish checks.

Injected additional context is attributed with hook ID/event, appended at the
correct message boundary, and retained in the turn trace for replay. It preserves
the original user prompt. Post-tool replacement feedback keeps the original
result in the audit trace even when the model sees replacement text. Use the
model adapter's existing
instruction-message conventions without claiming higher authority than system
policy. Hook scripts perform deterministic checks; revision feedback is a
request for model work, not proof the model completed it.

## Execution policy and trust

Hooks execute local code with the configured host user's capabilities. They
use an explicit sandbox profile bounded by the host's policy; enabling a hook
does not add filesystem or network grants. Inherit a minimal environment
(PATH, locale, selected home/temp variables) and explicitly configured env
names. Do not pass model API-key variables by default.

All non-managed sources follow the review flow above. Model-facing config edits
cannot write trust state or invoke review approval. Host reload discovers edits
but never trusts them. The trust store is host-owned and excluded from the
agent sandbox write grants; CLI commands run with an explicit host action
identity rather than a model-authored textual command.

Hook children run outside the model tool registry, preventing recursive tool
hooks. Mark their environment with `MORPH_HOOK_ACTIVE=1`; a nested Morph process
disables hooks by default. This is recursion control, not a security boundary.

Execution errors follow the documented compatibility behavior above. Budget
exhaustion launches no further commands and produces an explicit diagnostic;
strict policy can end the operation blocked. Cancellation terminates active
children. SessionEnd is best effort on orderly session close, not an after-turn
hook and not guaranteed after a crash. Persist turn outcomes through the
existing runtime independently of SessionEnd.

## Ownership and concurrency

Introduce `morph-hooks` under `src/hooks/`, linked by `morph-runtime`.
Agent code sees an injected callback and typed result, avoiding an agent to
runtime dependency. The runtime owns immutable hook definitions and process
resources. Use arena allocation, morph_buf, morph_array, and cJSON.

Initiate hook dispatch on the ReAct owner thread, before worker launch and after
worker join. Launch matching command handlers concurrently, then aggregate their
results in stable source/group/handler order. A blocking result does not prevent
other matching handlers from starting; scripts must not rely on execution order.
Workers never modify model messages, invoke frontend interactions,
or own hook state. A slow synchronous hook delays that boundary; deadlines make
the cost explicit. Wait and drain child pipes concurrently to avoid deadlocks.
On timeout/cancel, terminate the process group, escalate after a grace period,
reap children, and release pipe descriptors. Output overflow terminates the
child and reports a protocol error rather than parsing a truncated decision.

The first release supports root events only and visibly reports sub-agent
events as unsupported. Child contexts must explicitly clear the
hook callback instead of accidentally inheriting it. Phase two can add
`scope = "root" | "all"` with agent IDs, deterministic aggregation, and budgets
that cannot multiply without bound across fanout.

## Observability and management

Emit `hook.started`, `hook.completed`, `hook.blocked`, `hook.failed`, and
`hook.skipped` through the existing event channel. Include hook ID, boundary,
call correlation, duration, decision, and normalized error. Raw prompt, args,
stdout, and stderr stay out of default events/logs. Diagnostics are bounded and
available on explicit inspection. Hook events themselves never trigger hooks.

Add runtime snapshot and host-management APIs for hook definitions, review,
trust, enable/disable, reload, and recent summaries. All frontends use this
facade. Events include definition digest and review state, while sensitive
command details are returned only through explicit authorized inspection.

## Implementation sequence and acceptance

1. Add portable JSON/TOML parsing, event-specific validation, matchers/aliases,
   source discovery, immutable definitions, and shared source manifests.
2. Deliver review/trust storage, stale-review protection, and `/hooks` plus
   frontend-neutral management before enabling any command execution.
3. Implement bounded concurrent runner, stdin EOF, cancellation, wire decoders,
   and integration for input/tool/session boundaries.
4. Add Stop continuation, final buffering, rewritten-argument permissions,
   snapshots, examples, and compatibility fixtures.
5. Add remaining events and handler types behind explicit support reporting.

Test both representations against identical normalized output; additive source
merge; aliases/regex; new and changed trust; stale review; revocation; host-only
trust mutations; unsupported features; exit 0/2/other codes; malformed outputs;
context fields; rewritten input/conflicts; process cleanup and pipe bounds;
concurrent start and deterministic aggregation; normal permission enforcement;
paired tool denial; post-tool replacement audit; Stop continuation/exhaustion;
no rejected final output; persistence failures; scheduled and frontend behavior.

Stop continuation remains inside the same Morph turn with a finite revision
budget. This is a deliberate runtime difference from implementations that open
a new user turn for continuation, and must be visible in compatibility notes.
No claim of complete compatibility ships until the supported-event matrix and
fixture suite establish it. Imported examples with unsupported capabilities
must explain what cannot run instead of silently weakening the hook.

Run standard build/ctest, relevant sanitizer checks, and FastCGI build coverage
when implementation begins. This design-only revision requires documentation
review and whitespace validation.
