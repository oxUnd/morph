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
Use Up/Down to select, Left/Right to page, r to refresh, and Enter to open Review. Review shows
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
| `Stop` | Before accepting a successful final answer |
| `SessionStart` | First execution in a selected session; source is startup or resume |
| `SessionEnd` | Leaving a previously executed session or orderly runtime shutdown |

Tool matchers are POSIX extended regular expressions. Native names are supported;
`exec` also matches `Bash`, and `apply_patch` matches `Edit` and `Write`. A match
against several aliases still runs the handler once. Session matchers filter
source/reason; Stop ignores matcher. Project discovery is rooted at the configured
workdir, without an ancestor-directory walk.

Handlers matching one event start concurrently. A denial does not prevent
another matching handler from starting. Results aggregate in stable source order.
Conflicting argument rewrites block the call. Pre-tool denial yields a paired
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

`permissionDecision: "ask"` requests normal HITL; without an approval handler,
the call is denied. `allow` with `updatedInput` rewrites arguments, followed
by normal permission checks. Text tool rewrites use a string `command` field.

A Stop continuation request:

```json
{"decision":"block","reason":"Run the focused tests before finishing."}
```

Morph permits two additional completion rounds within the same turn. A repeated
veto ends the turn blocked. When a trusted Stop hook is active, candidate answer
text is buffered until accepted. `continue: false` on Stop ends the attempt
without opening another model round. Runtime errors and max-iteration exits
are not successful completions and do not run Stop.

Additional model context uses `hookSpecificOutput.additionalContext` with the
matching `hookEventName`. SessionStart and UserPromptSubmit also accept plain
text context. `additionalContextLimit` is an approximate token threshold,
default 2500; oversized context is saved under the configured output directory
and replaced by a preview/path. Zero disables this per-handler threshold while
shared resource limits remain active. Stop expects JSON, not plain text.

Other nonzero exits, malformed protocol, timeout, or output overflow produce
visible `hook.failed` diagnostics and continue in compatibility mode. An
explicit supported block is distinct from a crashed checker. Inspect `/hooks
review <id>` for the last exit code, duration, and bounded stderr.

## Limits and compatibility

Supported handler fields are `type`, `command`, `timeout`, `statusMessage`, and
`additionalContextLimit`. Timeout is seconds, defaults to 600, and accepts
1–600. SessionEnd defaults to 1 and accepts 1–3; orderly shutdown enforces a
one-second cleanup budget. Shared execution time is bounded to 600 seconds per
turn. Input is capped at 1 MiB, stdout/stderr at 64 KiB each, and injected
context at 32 KiB per turn. Output overflow is an error, never a truncated
policy decision. Cancellation terminates hook process groups; background
descendants are also cleaned up on completion.

The child environment retains basic shell/locale/home/temp variables. Model
API-key variables are not inherited. `MORPH_HOOK_ACTIVE=1` suppresses recursive
hooks in nested Morph processes. Reviewed hook commands run with host privileges;
they do not inherit the tool exec sandbox.

This release does not support PermissionRequest, compaction/sub-agent/Interrupt
hooks, async handlers, MCP handlers, managed hooks, Windows overrides, or
Codex's entire common-output field matrix. Such configured handlers are shown
as unsupported/invalid and cannot be trusted as active hooks. Regex syntax,
resource limits, session transition timing, PostToolUse feedback composition,
and same-turn Stop continuation are explicit compatibility differences.
Review management is provided in the CLI and the public runtime facade;
there is no new FastCGI administration endpoint or mobile review UI in this
release. All frontends share the execution path and lifecycle events.

## Example

From the repository root, copy `examples/hooks/hooks.json` into
`.morph/hooks.json`, configure/build the project, then complete the review flow.
The example Stop command runs `examples/hooks/verify_stop.py`, which invokes
the focused arena tests and requests a revision on failure. It is not enabled
by this repository change.
