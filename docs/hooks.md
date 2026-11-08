# Lifecycle hooks

Morph runs reviewed command hooks around prompts, tools, completion, and
session transitions. Configuration follows the supported subset of the
[Codex hook format](https://learn.chatgpt.com/docs/hooks). Hook scripts are
local programs running as the host user; review their command and dependencies
before trusting them.

## Configure

Put user hooks in `~/.morph/hooks.json` or project hooks in
`<workdir>/.morph/hooks.json`:

```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "^Bash$",
      "hooks": [{
        "type": "command",
        "command": "python3 /absolute/path/to/check_policy.py",
        "timeout": 3,
        "statusMessage": "Checking command policy"
      }]
    }]
  }
}
```

Inline configuration in the selected `config.toml` or project
`.morph/config.toml` uses the same hierarchy:

```toml
[[hooks.PreToolUse]]
matcher = "^Bash$"

[[hooks.PreToolUse.hooks]]
type = "command"
command = "python3 /absolute/path/to/check_policy.py"
timeout = 3
statusMessage = "Checking command policy"
```

The loader also discovers `hooks.json` beside the selected config file.
All active sources are merged. Prefer one representation per directory.
Definitions reload between turns and during review. Disabling the feature
in the selected runtime config skips execution:

```toml
[features]
hooks = false
```

Morph does not automatically import `.codex` files. Existing definitions can
be copied into a Morph source and reviewed. Commands referring to `.codex`
scripts still need those paths to exist. Command cwd is the configured Morph
workdir; use absolute paths if the script must work from subdirectories.

## Review and trust

Loading configuration never grants permission to run it. In an interactive
terminal, startup shows one review when hooks need a decision. Choose **Trust all
current hooks and remember**, **Reject all current hooks and remember**, or review
hooks individually. Trusting all also trusts the displayed workspace for project
hooks. Rejecting all disables the current hooks persistently. Neither action runs
hook commands. Esc leaves decisions unchanged.

`/hooks review-all`, or **a** in the `/hooks` list, opens this batch review again.
The batch review token covers every displayed definition; changes invalidate it,
and the entire decision is saved atomically.

`/hooks` opens a full-screen list with states and selected-hook details.
Use Up/Down to select, Left/Right to page, r to refresh, and Enter to open Review.
Press **e** for a lifecycle-event overview with installed, active, and pending-review
counts. Enter opens that event's handlers, including an explicit empty state;
Esc returns to the event overview, then to all hooks. Unsupported handlers show
why they cannot run. Unreviewed hooks can be disabled without granting trust.
Review shows
the complete definition, highlights changes from the trusted definition, and
shows the latest recorded execution result. PgUp/PgDn and Home/End scroll long
content; the list and review adapt when the terminal is resized.

Choose a review action with Up/Down and confirm with Enter. The initial selection
keeps the hook unchanged. Depending on its state, a hook can be trusted, revoked,
disabled, or enabled. Project hooks require a separate workspace trust decision
before definition trust. Esc returns to the list; Esc again closes it. Ctrl-C or
q closes the full-screen view. `/hooks review <id>` also opens Review directly.

Noninteractive and JSON output retain the command-based workflow:

1. Run `/hooks check` to list IDs, events, sources, and effective states.
2. For project sources, run `/hooks project`, inspect the workspace path,
   and run the printed `/hooks trust-project workspace <digest>` command.
3. Run `/hooks review <id>`. Inspect the current definition, previous trusted
   definition, cwd, and any recent execution result.
4. Run the exact `/hooks trust <id> <digest>` command printed by review.

Review and `/hooks check` do not execute commands. A digest binds the definition
you inspected. Execution-affecting changes require fresh trust; stale review
commands fail. Formatting and JSON object-key order do not change the digest.
Trust covers the definition, not mutable script contents or dependencies.

Other commands use the current digest shown in review:

```text
/hooks disable <id> <digest>
/hooks enable <id> <digest>
/hooks revoke <id> <digest>
/hooks revoke-project workspace <workspace-digest>
/hooks trust-all workspace <review-digest>
/hooks deny-all workspace <review-digest>
```

Enable does not grant trust. Disabling an unreviewed hook is supported. Pending,
changed, disabled, and unsupported hooks do not run. Unattended turns skip
unreviewed hooks without prompting. Trust is persisted in the runtime database. User hooks are scoped to their
canonical configuration source and apply across workspaces. Project hooks remain
scoped to source and canonical workdir. Unchanged trust and rejection decisions
survive restarts; changed trusted definitions require review again. Matching old
user trust records are migrated when their original workspace is opened. Revoking
trust prevents legacy records from restoring it. It is managed through host commands/runtime APIs,
not exposed as an agent tool. Treat database access as host authority.

## Supported events

| Event | When it runs |
| --- | --- |
| `UserPromptSubmit` | Before the prompt reaches the model |
| `PreToolUse` | Before permissions/HITL and tool dispatch |
| `PostToolUse` | After a completed tool result, before the next model request |
| `Stop` | After a completed assistant response, before ending the turn |
| `SessionStart` | First execution in a selected session; source is startup or resume |
| `SessionEnd` | Leaving a previously executed session or orderly runtime shutdown |
| `PreCompact` | Before manual or automatic durable-history compaction |
| `PostCompact` | After a successful durable-history compaction |
| `Interrupt` | Once when an active root turn is cancelled |

Tool matchers are POSIX extended regular expressions. Native names are supported;
`exec` also matches `Bash`, and `apply_patch` matches `Edit` and `Write`. A match
against several aliases still runs the handler once. Session matchers filter
source/reason; compaction matchers filter `manual` or `auto`. `*`, an empty matcher,
and an omitted matcher match all invocations. Stop, UserPromptSubmit, and Interrupt
ignore matcher, including regex syntax. Project discovery is rooted at the configured
workdir, without an ancestor-directory walk.

Handlers matching one event start concurrently. A denial does not prevent
another matching handler from starting. Results aggregate in stable source order.
When multiple handlers rewrite an input, the last completed valid rewrite wins.
Any supported denial takes precedence over rewrites. Pre-tool denial yields a paired
tool response, so the model can choose another action. Hook allow does not grant
filesystem/network access or bypass ordinary tool/program permissions.

## Script input and output

A command receives one JSON object on stdin and EOF. Common fields include
`session_id`, `turn_id`, `cwd`, `hook_event_name`, `model`, `permission_mode`,
and nullable `transcript_path`. Tool events include `tool_name`, `tool_input`,
and `tool_use_id`; PostToolUse also includes `tool_response` and `error_code`.
Text tools expose their raw text as `tool_input.command`. Stop includes
`last_assistant_message` and `stop_hook_active`.

Return a JSON decision on stdout, or use exit code 2 plus a reason on stderr.
A PreToolUse denial:

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "This target is protected."
  }
}
```

`permissionDecision: "ask"`, legacy `decision: "approve"`, and `allow` without
`updatedInput` are unsupported, as in Codex 0.167.0. They report a failed hook
without changing the operation. `allow` with `updatedInput` rewrites arguments,
followed by normal permission checks. Text tool rewrites use a string `command`
field. A denial requires a non-empty reason. Explicit default values
`continue: true` and `suppressOutput: false` are accepted for tool hooks.

A Stop continuation request:

```json
{"decision":"block","reason":"Run the focused tests before finishing."}
```

Stop does not buffer reasoning or assistant text. The response streams normally.
`decision: "block"` adds its non-empty reason as a hook-generated user prompt
and continues within the same turn. Completed assistant responses and hook prompts
remain in durable history and are replayed after resume. Continuation prompts
use a distinct `<hook_prompt hook_run_id="...">` wrapper for the model; raw feedback
remains available to the UI and is excluded from user-input memory extraction.
Subsequent Stop inputs have `stop_hook_active: true`. There is no separate
two-revision limit; the normal turn iteration, cancellation, and execution budgets
still apply. An empty block
reason, including an empty exit-2 stderr, fails the hook without requesting a
continuation. `continue: false` takes precedence over all continuation requests
and ends normally. Runtime errors and max-iteration exits do not run Stop.

SessionStart, PreCompact, and PostCompact honor `continue: false` and `stopReason`.
A PreCompact stop prevents compaction; a PostCompact stop leaves the completed
checkpoint intact and prevents the next model request. Successful compaction
also runs SessionStart with `source: "compact"`; its context reaches the immediate
continuation. These boundaries apply to `/compress` as well as automatic durable
history compaction. The legacy in-memory trace compressor does not emit these
events.

Interrupt runs after cancellation, even when the turn's hook budget has expired.
Its output cannot restart or prevent cancellation. It defaults to one second and
accepts one through three seconds. It does not run for idle sessions or child agents.

PostToolUse blocking feedback replaces the model-visible tool result. The original
result remains in the tool completion trace and persisted `tool_meta` under
`hook_original_tool_response`. Hook failures preserve normal tool output.

Additional model context uses `hookSpecificOutput.additionalContext` with the
matching `hookEventName`. SessionStart and UserPromptSubmit also accept plain
text context. `additionalContextLimit` is an approximate token threshold,
default 2500; oversized context is saved under the configured output directory
and replaced by a preview/path. If saving fails, the preview is still delivered.
Zero passes the full additional context, subject to transport limits. Stop expects
JSON, not plain text.

Plain text from tool and compaction hooks is ignored. Malformed JSON-looking text
is a failed hook, including for SessionStart and UserPromptSubmit; it never becomes
model context. Unsupported output fields and incorrectly typed common fields fail
the handler without applying a partial decision. `suppressOutput` has no effect
on model streaming. SessionEnd ignores stdout; Interrupt accepts only an optional
`systemMessage`.

Other nonzero exits, malformed protocol, timeout, or output overflow produce
visible `hook.failed` diagnostics and continue in compatibility mode. An
explicit supported block is distinct from a crashed checker. Inspect `/hooks
review <id>` for the last exit code, duration, and bounded stderr.

The terminal shows a transient status for hooks running longer than 300 ms.
Quiet successes leave no transcript row. `systemMessage`, failures, and blocked
outcomes are visible in both interactive and plain output; model-only context
remains out of terminal output. JSON event consumers receive `hook.progress`
alongside the existing lifecycle events, with duration and exit status on completion.

## Limits and compatibility

Supported handler fields are `type`, `command`, `timeout`, `statusMessage`, and
`additionalContextLimit`. Timeout is seconds, defaults to 600, and accepts
1–600. SessionEnd defaults to 1 and accepts 1–3; orderly shutdown enforces a
one-second cleanup budget. Shared execution time is bounded to 600 seconds per
turn. Input and each stdout/stderr stream are capped at 1 MiB. Per-handler token
previews retain a `Full hook context` reference when saving succeeds. Preview limits do not discard
a successfully decoded policy decision or input rewrite. Transport output
overflow is an explicit error, never a truncated policy decision. Cancellation
terminates hook process groups; background
descendants are also cleaned up on completion.

The child environment retains basic shell/locale/home/temp variables. Model
API-key variables are not inherited. `MORPH_HOOK_ACTIVE=1` suppresses recursive
hooks in nested Morph processes. Reviewed hook commands run with host privileges;
they do not inherit the tool exec sandbox.

This release does not support PermissionRequest, sub-agent
hooks, async handlers, MCP handlers, managed hooks, Windows overrides, or
all Codex lifecycle events. Such configured handlers are shown
as unsupported/invalid and cannot be trusted as active hooks. Regex syntax,
resource limits, context-preview formatting, and session transition timing remain
explicit compatibility differences.
Review management is provided in the CLI and the public runtime facade;
there is no new FastCGI administration endpoint or mobile review UI in this
release. All frontends share the execution path and lifecycle events.

The 2026-11-08 compatibility audit targets Codex 0.167.0, matching the installed
Codex version, at commit
[`086ccc8`](https://github.com/openai/codex/tree/086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a).
It compares the [event parsers and schemas](https://github.com/openai/codex/tree/086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a/codex-rs/hooks/src),
the [turn loop](https://github.com/openai/codex/blob/086ccc833bc9cf01024ad2dd6af7dbd421b6bf9a/codex-rs/core/src/session/turn.rs),
and upstream hook tests with the
[official hook documentation](https://learn.chatgpt.com/docs/hooks).

| Audited behavior | Morph contract |
| --- | --- |
| Stop streaming and continuation | Normal text streaming; durable same-turn continuation prompts |
| Stop `continue: false` | Normal completion; overrides other handlers' continuation requests |
| Missing block feedback | Failed handler with no control effect |
| Tool decisions and common fields | Supported decisions only; explicit default values accepted |
| Concurrent rewrites | Last completed valid rewrite wins; denials take precedence |
| Context spilling | Per-handler token threshold; storage failure retains a preview; zero keeps full context |
| Invalid JSON-looking stdout | Failed handler, never injected as plain context |
| SessionEnd and Interrupt | Advisory shutdown stdout ignored; Interrupt accepts `systemMessage` only |

## Example

From the repository root, copy `examples/hooks/hooks.json` into
`.morph/hooks.json`, configure/build the project, then complete the review flow.
The example Stop command runs `examples/hooks/verify_stop.py`, which invokes
the focused cleanup/ownership tests and requests a revision on failure. It is not enabled
by this repository change.
