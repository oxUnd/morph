# Current Morph Configuration

Read this for setup, model settings, thinking, recaps, process waits, host execution,
database storage, hooks, rewind, memory/context budgets, or config changes.
The matching-version manual remains the full reference. In a source checkout,
check `config.toml.example` and `src/config/schema.c` before proposing unfamiliar keys.

## First Run

Run `morph`, or `morph -c <new-config-path>`, in an interactive terminal with a
missing config. Use `morph setup` or `morph setup -c <path>` to reconfigure an
existing installation. There is no separate `--setup` flag. Setup follows Role →
Overview → Save, using inline, append-mode choices instead of a full-screen UI.
Completed answers remain in scrollback. Small terminals use compact choices.

- Choose a role first. Video creator requires all four model sections: text,
  vision, image, and video. Photographer requires text and vision; Photo editor
  and Designer require text, vision, and image. Other roles require text.
  Required models are prefilled and cannot be disabled; credentials can be completed later.
- Accept defaults directly when keys are ready, or select a specific model to
  edit it. Every editing page identifies Text, Vision, Image or Video, the provider
  and current model. Connect accounts and save collects only missing accounts;
  matching missing provider credentials are shared across default models.
  Save for later skips connecting. Secret input is not echoed into the transcript.
  Setup writes only one nonempty credential field per model: `api_key` or
  `api_key_env`. Reconfiguration removes the unused counterpart.
- Setup adds Exa for all roles, Playwright for development/research, macOS
  Computer Use for development and visual roles on macOS, and a disconnected
  Figma desktop entry for designers. Existing MCP definitions are preserved and
  deduplicated. Missing local launchers leave their defaults disconnected;
  packages may download on first connection. See `docs/setup-mcp.md`.
- Setup writes legacy media, read-only and host settings. The runtime uses the
  explicit `[permissions]` policy, or maps these legacy settings to Ask. Check
  `/permissions status` to see the active policy.
- Model settings expose Account, a Thinking choice for the DeepSeek adapter, and
  Advanced settings. Advanced contains the model/endpoint, URL, credentials,
  context window, and output limit. Unknown/custom endpoints need their required
  fields filled in before continuing.
- The DeepSeek text preset enables thinking with high effort. The vision preset disables thinking.
  On means more time to reason; Off favors quicker replies. Text and Vision choices
  are independent. Switching providers resets provider-specific setup choices.
- Model capabilities beyond the selected role requirements are optional and
  separately configurable. The final review can reopen any capability for changes.
- Arrows/Tab move, Enter selects, Esc goes back, and Ctrl+C exits. Cancelling a
  selection or edit preserves the previous accepted value. Cancelling setup writes
  no config. Explicit reconfiguration preserves other settings and backs up the
  old config before replacement. Unchanged fields, comments and table formatting
  are preserved. Existing prompt sources can be kept or replaced
  with a new role snapshot without changing the original files.
- Save and exit saves the config and always exits without opening the agent.
  Run Morph again to load the saved configuration. With missing credentials the action is
  Save for later; Morph prints each needed export once and exits after saving.
  Relaunch after setting those variables. Do not promise an API connection test:
  a Ready account means a configured/exported key is present, not verified by the provider.

## Model Roles and Adapters

| Section | Workload | Available adapters |
| --- | --- | --- |
| `[model.text]` | Conversation, planning, tool selection, text answers | `openai-chat-compatible`, `openai-responses`, `deepseek` |
| `[model.vision]` | Image understanding/OCR through `img_qa` | Chat adapters; the selected model must support vision |
| `[model.image]` | Image generation and editing | `openai-images`, `volcengine-images` |
| `[model.video]` | Video generation | `volcengine-videos` |

`provider` identifies the service; `model` identifies its model or deployment;
`api_base` is the service's base URL; `adapter` selects request/response behavior.
A custom proxy may require an explicit adapter even when its provider label differs.

When omitted, a chat adapter is inferred as `deepseek` for provider `deepseek`,
and `openai-chat-compatible` otherwise. `openai-responses` must be explicitly
selected; it supports native freeform custom tools. Do not apply DeepSeek message
rules to OpenAI-compatible Chat or Responses requests.

`context_limit` budgets input/context; `max_tokens` limits generated output.
They are not interchangeable. Setup enforces an output limit below the context
window and applies known ceilings for exact preset model IDs. Changing a model
resets its editable token defaults; unknown deployments do not inherit a known
model's advertised limits just because their names share a prefix.

For text/vision, the runtime defaults are `max_tokens = 4096`,
`timeout_seconds = 300`, and `retry_count = 3`. Setup may write different explicit
model defaults. Inspect the active section instead of replacing it with generic
values. `retry_count` counts retries after the first request and accepts 0–10.

## DeepSeek Thinking

A complete text configuration using the DeepSeek adapter:

```toml
[model.text]
provider = "deepseek"
adapter = "deepseek"
model = "deepseek-v4-flash"
api_base = "https://api.deepseek.com/v1"
api_key_env = "DEEPSEEK_API_KEY"
context_limit = 128000
max_tokens = 32768
timeout_seconds = 300
retry_count = 3
extra_body_json = '''{"thinking":{"type":"enabled"}}'''
```

These token values are a configuration example, not the model's maximum capacity.
Export `DEEPSEEK_API_KEY` before launching Morph. To turn thinking off, change
`thinking.type` to `disabled` inside that same JSON object. For an existing config,
preserve its other JSON fields; do not overwrite them with a replacement example.
If the user also requests an effort level, it belongs in the JSON object as
`reasoning_effort`, not as a top-level Morph TOML key. Use only values supported
by the selected model/provider.

The DeepSeek adapter replays available reasoning fields and handles synthetic
context summaries for thinking requests. Thinking-specific tool-choice and message
conversions are adapter-local; changing providers does not rewrite shared history.
Setup writes an explicit thinking setting for each DeepSeek model role. Inspect `extra_body_json` and the adapter before answering
whether thinking is enabled; do not infer it from a screenshot or filename.

For DeepSeek vision, use a vision-capable model such as the source preset
`deepseek-flash` under `[model.vision]`; the text preset is not automatically a
vision model. Check the matching manual/provider capabilities before changing it.

`extra_body_json` must be a JSON object encoded as a TOML string. It merges
provider-specific fields into the request but cannot replace Morph-managed fields
such as `model`, `messages`, `tools`, `stream`, or output limits. See the manual for
the selected adapter rather than copying a different provider's thinking syntax.

## Recaps, Titles, and Details

- `/recap` uses the current text model in a separate background request to summarize
  recent user messages and final answers. It excludes tools/reasoning, does not
  compress the context, and does not add a new message to model history.
- `[general] auto_recap = true` is the default. Automatic recaps require terminal
  focus reports, 30 minutes both unfocused and idle, and at least 3 completed turns.
  Between recaps at least 2 more completed turns are needed. New input/session
  navigation invalidates older requests; returning focus cancels automatic work.
- To disable only automatic recaps:

  ```toml
  [general]
  auto_recap = false
  ```

  Manual `/recap` remains available. No dedicated recap-model setting exists.
- Automatic session titles are model summaries generated in the background on
  the first turn of an automatically created session. They are not text truncation
  or periodic renaming. `/rename` controls the session name manually. No separate
  title-model setting exists.
- Thinking is shown as a one-line live preview, with an ellipsis for hidden text.
  Ctrl+T opens current-turn reasoning and tool details; Ctrl+T or Esc returns to
  the conversation. Do not describe the preview as discarded reasoning.

## Permissions, Hooks, and Rewind

Permission presets use three independent dimensions:

| Preset | Access | Approval | Reviewer |
| --- | --- | --- | --- |
| Ask | `workspace` | `on_request` | `user` |
| Auto | `workspace` | `on_request` | `model` |
| Full | `full` | `never` | `user` |
| Read Only | `read_only` | `on_request` | `user` |

Use `/permissions status` to inspect the current policy and `/permissions mode`
to select a preset. Existing authority runs directly; operations needing review
cannot proceed with approval `never` or an unavailable approval adapter. Auto
uses an isolated text client; optional `[model.approval]` overrides its connection.
Legacy host/network/HITL flags do not grant authority under the shared policy.

Hooks have separate definition and project-workspace trust. `/hooks` opens review;
`/hooks check` reports effective states. Review alone never executes a command.
Changed definitions need fresh trust. Use the exact digest-bound commands shown
by review; do not edit the database to bypass trust. Hook scripts run as the host
user. `[features] hooks = false` disables execution. See `*morph-hooks*` and
`docs/hooks.md` for source discovery and review details.

`/rewind` restores code, conversation, or both from a primary-turn checkpoint
while idle. Only primary-agent `apply_patch` changes are captured for code
restore; shell, manual and subagent writes are outside capture. External edits
cause conflicts rather than being overwritten. The model's `checkpoint` tool
restores code only. Rewind does not undo external side effects or billing.
See `*morph-checkpoints*` and `docs/checkpointing.md`.

## Storage and Memory Budgets

SQLite is the default database. PostgreSQL requires a build with
`-DENABLE_POSTGRESQL=ON`, then configuration such as:

```toml
[database]
driver = "postgresql"
url_env = "MORPH_DATABASE_URL"
tls_mode = "verify_full"
connect_timeout_ms = 5000
query_timeout_ms = 30000
```

Export the connection URL before launch; do not print credentials. The database
role needs schema migration permissions. In PostgreSQL mode, file sync omits the
legacy primary `data.db`; use database administration tools for database backup
and restore. See the README's PostgreSQL storage section for deployment limits.

Use `[memory] max_reply_chars = 3000` for the injected memory character budget.
`max_context_chars` is a compatibility alias. The independent defaults
`capture_input_enabled`, `save_tasks_enabled`, and `learn_from_chats_enabled`
are all true; do not describe disabling one as disabling the whole subsystem.

Context compaction defaults to running between tool iterations as needed.
`[context] tool_result_max_tokens = 8000` caps each retained result, including
its truncation marker. This token budget differs from exec's output byte caps.
`protocol_reserve_tokens = 4096` reserves framing/estimation overhead separately
from the text model's output budget. Project guidance has a combined default
`project_doc_max_bytes = 32768`; omitted files remain readable with `file_read`.
Prefer narrower searches and ranged reads before increasing these budgets.

## Process Wait Budgets

Configure the maximum duration of an individual `background` collection call:

```toml
[exec]
max_wait_ms = 600000
```

For `poll` and `wait`, the model chooses an integer `wait_ms` in the inclusive
range `0` to `max_wait_ms`. The maximum defaults to 600000 ms (10 minutes).
This does not make every call wait 10 minutes: omitting `wait_ms` uses 30000 ms
(30 seconds), or the configured maximum if lower. `wait_ms = 0` reads immediately;
`wait_ms = 120000` waits up to 2 minutes. A completed process returns promptly.
The `wait` action continues collecting through intermediate output until exit
or its collection budget expires.

`max_wait_ms` accepts a positive integer up to 2147483647. For example, setting
it to 1200000 permits waits up to 20 minutes. The tool schema advertises the
configured range. Explicit values outside that range, fractions, and non-numbers
are rejected rather than clamped; validation happens before writes or signals.
Results report the effective `wait_ms` and configured `max_wait_ms`.

This collection budget is separate from the command's execution timeout
(`exec` argument `timeout_ms`, defaulted from `exec.default_timeout_ms`). Increasing
the collection maximum does not extend a command's execution timeout. Changes
require a normal Morph restart with the active config.

Finite process sessions must be collected before ending a turn. `service: true`
marks a persistent server or watcher. `notify_only: true` skips turn-end collection
only for an explicit user request to run a task and notify them later. `/jobs`
inspects retained processes and can request a stop. Shell tools can be disabled
by the runtime/build or tool configuration; do not promise availability everywhere.

If a `background` call fails immediately with a `wait_ms` range error, inspect its
arguments and the installed version's advertised range. Older versions used a
fixed 60000 ms maximum. This validation error does not establish that the child
command failed; inspect its session status, exit code, and output.

## Explicit Host Execution

Under workspace and read-only access, ordinary commands run in the child OS
sandbox. `host: true` requests access to the user's host filesystem and network
for one call. The active permission policy governs that request. Full access
already runs commands on the host. Process sessions, timeouts and output limits
apply in both modes. Use `/permissions status` to inspect the effective policy.

For an authorized task requiring macOS host automation, explicit tool arguments
can be:

```json
{"command":"osascript -e 'tell application \"System Events\" to get name of first application process whose frontmost is true'","host":true,"timeout_ms":10000}
```

The initial result reports `execution_mode: "host"` or `"sandbox"`; inspect the
command's `exit_code` and stderr to determine whether it succeeded. A rejected
permission request and an OS error after a successful launch have different causes.

For AppleScript errors, distinguish Morph's sandbox from macOS privacy consent.
`sandbox_extension_issue_file ... Operation not permitted` with `-54`, or system
logs denying `appleevent-send`, identifies a sandbox restriction. `System Events`
can also reject a sandboxed sender with `-10004`, even with permissive profile
rules. `-600` can mean the application is not running or reflect a sandbox
restriction; the number alone does not establish the cause. Confirm the command,
full stderr, and relevant system logs before attributing a failure to permissions.

Host execution does not grant or reset Automation, Accessibility, or Screen
Recording permissions. If macOS reports missing privacy consent, identify the
actual responsible terminal/application and the required Privacy & Security
setting. Do not bypass a system denial or claim that changing Morph's config
grants those permissions. See the matching manual's shell/tool section for the
full contract, and check installed-version support before suggesting this option.

## Diagnose Before Changing Settings

- Missing model/key: check the section for the failing workload, the active `-c`
  path, and whether its environment variable was exported before launch.
- HTTP 400 mentioning `reasoning_content`: inspect the adapter and thinking setup,
  then the installed version and error detail. This is a protocol failure, not a
  reason to increase retries, edit the session database, or invent missing reasoning.
- A changed config needs a restart. `config_validate` checks the active file,
  another file selected by `path`, or complete TOML supplied as `content`. It is
  read-only and does not hot-reload settings, write backups, or terminate Morph.
