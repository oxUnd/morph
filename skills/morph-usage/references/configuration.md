# Current Morph Configuration

Read this for setup, model settings, thinking, recaps, process waits, host execution,
or config changes.
The matching-version manual remains the full reference. In a source checkout,
check `config.toml.example` and `src/config/schema.c` before proposing unfamiliar keys.

## First Run

Run `morph`, or `morph -c <new-config-path>`, in an interactive terminal with a
missing config. There is no separate `--setup` flag. Setup follows Account →
Preferences → Start, using inline, append-mode cards instead of a full-screen UI.
Completed answers remain in scrollback. Small terminals use compact choices.

- Choose a provider and connect an account using a hidden API key, an environment
  variable name, or Set up later. Secret input is not echoed into the transcript.
- Preferences expose Account, a Thinking choice for the DeepSeek adapter, and
  Advanced settings. Advanced contains the model/endpoint, URL, credentials,
  context window, and output limit. Unknown/custom endpoints need their required
  fields filled in before continuing.
- DeepSeek thinking defaults to Off in setup and is explicitly confirmed once.
  On means more time to reason; Off favors quicker replies. Text and Vision choices
  are independent. Switching providers resets provider-specific setup choices.
- Vision, image generation, and video generation are optional and separately
  configurable. The final review can reopen any capability for changes.
- Arrows/Tab move, Enter selects, Esc goes back, and Ctrl-C exits. Cancelling a
  selection or edit preserves the previous accepted value. Cancelling setup writes
  no config; an existing config is not overwritten.
- Start using morph saves the config. With missing credentials the action is
  Save setup for later; Morph prints the needed exports and exits after saving.
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
Setup explicitly writes On/Off, so its default differs from omitting thinking in
a hand-written config. Inspect `extra_body_json` and the adapter before answering
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
  Ctrl+O opens current-turn reasoning and tool details; Ctrl+O or Esc returns to
  the conversation. Do not describe the preview as discarded reasoning.

## Process Wait Budgets

Configure the maximum duration of an individual `process` collection call:

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

If a `process` call fails immediately with a `wait_ms` range error, inspect its
arguments and the installed version's advertised range. Older versions used a
fixed 60000 ms maximum. This validation error does not establish that the child
command failed; inspect its session status, exit code, and output.

## Explicit Host Execution

Ordinary `exec` calls run inside the child OS sandbox. A trusted development
setup can enable explicit host calls in the active user config:

```toml
[exec]
allow_host_execution = true
```

This is disabled by default. Enabling it makes `host: true` available for individual
`exec` calls; it does not change ordinary calls or executable-extension isolation.
Host calls have the current user's filesystem and network access, without the
child OS sandbox or its resource limits. Command approval, timeouts, output limits,
and process-session management still apply. Explain that scope when enabling the
option; honor authorization already given rather than asking again.

For an authorized task requiring macOS host automation, explicit tool arguments
can be:

```json
{"command":"osascript -e 'tell application \"System Events\" to get name of first application process whose frontmost is true'","host":true,"timeout_ms":10000}
```

The initial result reports `execution_mode: "host"` or `"sandbox"`; inspect the
command's `exit_code` and stderr to determine whether it succeeded. With the config
option disabled, `host: true` returns `host_execution_disabled` without execution.
Do not silently escalate a failed sandboxed command to host execution or enable
the config merely because an error occurred. A config change requires a normal
restart using the same active config path.

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
- A changed config needs a restart. `config_edit` validates the schema, backs up
  and atomically replaces the active file, and reports `restart_required`; it does
  not hot-reload startup settings or terminate Morph.
