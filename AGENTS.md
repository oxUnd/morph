# morph Agent Guide

## Build & Test
- CMake ≥ 3.20. Build: `cmake -S . -B build && cmake --build build`; clean: `rm -rf build`
- Tests ON by default. All: `cd build && ctest --output-on-failure`; one: `ctest -R Cleanup`; direct: `./morph-tests --gtest_filter=Cleanup.*`
- ASAN: add `-DENABLE_ASAN=ON`; FastCGI front-end: add `-DBUILD_FASTCGI=ON`

## Architecture
- **ReAct** `src/agent/react.c`: Thought → Action → Observation → Guardrail → Final, via OpenAI Function Calling (not text parsing).
- **Models**: `llm` (text chat), `image_gen`, `video_gen`, each configured independently.
- **Runtime facade** `src/runtime/`: process-level owner. Frontends hold an opaque `struct runtime *` and enter through `runtime_execute_turn()`; they never reimplement init/exec/persistence/shutdown. See `docs/runtime-architecture.md`.
- **TUI** `src/tui/`: implementation sources stay in `core/`, `terminal/`, `render/`, and `widgets/`. Public APIs name domain objects (`morph_tui_application_*`, `morph_tui_view_*`, `morph_tui_canvas_*`) rather than implementation layers; controls use `morph_tui_widget_*`. Include `<morph/tui/application.h>`, `<morph/tui/view.h>`, or `<morph/tui/widgets/list.h>`; consumers may use `<morph/tui.h>`. Keep implementation headers private and convenience umbrellas out of framework implementations. Application is one extensible object for init/deinit, run/step, messages, embedding, and main; there is no separate public App/runtime object. Custom types embed public base structs first and override optional method tables. Mutable view operations belong to the UI owner thread; workers post model messages. The entire CLI follows the TUI application layout under `src/sapi/cli/`; its root Application owns modes and lifecycle, with pages, widgets, models, services, commands, presentation and adapters. See `docs/tui-application-layout.md`. Samples live in `src/tui/samples/<name>/` with `main.c`, `CMakeLists.txt`, and `README.md`; `customization/` demonstrates custom components, pages, and interactions. See `docs/tui-architecture.md`.
- **Permissions** `src/permission/`: shared access/approval/reviewer policy, grants and review routing. Runtime owns the context, human/model adapters and application presets; frontends use `runtime_permission_get_policy()` / `_set_policy()`. Concrete tools describe operations; opaque tools use the registry gate. Descendants share authority. See `docs/permissions-design.md` and `docs/permissions-acceptance.md`.
- **Tools** `src/agent/tools/`:
  - read-only: credits, memory, memory_preference, file_read/list/info, img_info, plan, activate_skill, ask_user
  - media: img_gen, img_qa, img_inpaint, img_compose, img_annotate, img_resize, img_convert, vid_gen
  - workspace: apply_patch, checkpoint, config_edit, tasks, request_permissions
  - shell: `exec`, `background` — `exec` AST-parses a command into its program set and enters the shared permission layer. Bounded workspace commands run directly; missing authority is reviewed (allow/session/always/deny). It returns a session for long-running work that `background` manages. `background` supports `list`, `poll`, `wait`, `write`, `interrupt`, `kill`; waits default to 30 seconds, and `wait` collects through intermediate output until exit. Async jobs emit `process.started` / `process.completed`, remain visible in the CLI task area, and are inspected through `/jobs`. Finite jobs are collected before finalizing unless `notify_only` was explicitly requested; persistent servers use `service`. Replaces the former `bash_exec`.
  - dynamic tools: tool_create/promote/delete/history/diff/rollback
  - multi-agent: spawn_agent, send_message, followup_task, interrupt_agent, list_agents, wait_agent. Runtime owns `agent_control`; Ultra enables proactive delegation. Child skills/guardrails and billing are independently bound; weighted budgets persist for the session tree. CLI `/subagents` switches independent transcripts, follow-up and interruption; `/agents` prints snapshots. Scoped child builds use `MORPH_AGENT_SCRATCH`. See `docs/multi-agent.md`.
  - img_inpaint = bbox+label region i2i; img_compose = arrow+label cross-image fusion; both consume the img_annotate JSON verbatim.
- **Plan** `src/agent/plan.c` → `plan` and lightweight `update_plan` tools; child registries are session-local.
- **Checkpoints** `src/runtime/checkpoint.c`, `src/persistence/checkpoint_store.c`: each primary turn saves a persistent checkpoint before its prompt enters history. `apply_patch` captures preimages after authorization and seals actual results even on partial failure. `/rewind` restores code, conversation, or both; the model's `checkpoint` tool restores code only. Preserve external changes through conflict checks, never infer undo scope from a Git diff. Shell, other tools, and sub-agents are outside file tracking. See `docs/checkpointing.md`.
- **MCP** `src/mcp/`: stdio + Streamable HTTP; auto-registers remote tools/resources/prompts. Config `[[mcp.servers]]`.
- **Skills** `src/skill/`: hot-loadable `SKILL.md` + YAML frontmatter, from `~/.morph/skills/` and `~/.agents/skills/`; examples in `skills/`.
- **Exts** `src/ext/`, `src/ipc/`: sandboxed subprocesses (JSON-RPC) with TOML manifest (`entry`, `permissions`, `args_schema`); examples in `exts/`.
- **Context compression** `src/agent/compress.c`, triggered at `summarize_threshold_ratio` (0.8).
- **Sandbox** `src/sandbox/`: Linux seccomp-BPF denylist (default ALLOW, denies escape-relevant syscalls) + Landlock + rlimits; macOS Seatbelt via `sandbox_init`.
- **Events** `src/event/`: unified dotted names across startup/MCP/ReAct/tools/HITL/tasks. Renderers in `src/sapi/cli/adapters/runtime/events.c` and `src/sapi/fastcgi/event_sink.c`. See `docs/event-system.md`.
- **Scheduled tasks** `src/runtime/task_*.c`. See `docs/scheduled-tasks.md`.
- **Dynamic JS tools**: embedded QuickJS in `src/js_runner/`. See `docs/quickjs-tools.md`.

## Permission Semantics Checks
- When adding or changing tool authorization, check the combined semantics of
  access, approval, reviewer, operation flags and cached grants. Verify Ask,
  Auto, Full and Read Only, including unavailable approval adapters.
- Full is `full/never/user`: operations already authorized by full access must
  not accidentally require approval and then fail because approval is `never`.
  Audit unconditional `require_user` / `require_review` flags and legacy local
  approval gates. `config_edit` previously hit exactly this conflict.
- Preserve explicit forbidden operations and intentional mandatory-human
  requirements; document and test such exceptions. Do not fix a tool's flag
  mismatch by globally bypassing shared policy checks in Full mode.
- Add regressions for successful Full execution without an approval callback,
  restricted-mode approval/denial, and denial leaving files unchanged. Verify
  policy rejection separately from operating-system filesystem errors.

## CLI Status Symbols

- Terminal status glyph values are defined only in `src/tui/include/morph/tui/terminal/status.h`;
  CLI aliases and styles live in `src/sapi/cli/presentation/style.h`. All CLI
  commands, pages, tool feeds, background jobs, subagents, plans, notifications,
  diagnostics, setup flows and shared terminal spinners reuse those definitions.
  Do not hardcode alternative status glyphs or small
  `•` / `◦` step bullets in application sources.
- Use `◯` (dim gray) for queued/pending, `◉` (cyan) for running,
  `●` (green) for successful completion, `✖` (bold red) for errors/failures,
  and `?` (bold yellow) for questions/approval awaiting user input. Running
  activity may animate without changing marker width or layout. The `Working`
  row and background-job summary start with `◉`, alternating normal/dim cyan
  every 500 ms on the same monotonic-clock phase. Blink only transient live
  indicators; keep completed history static and no-color/plain output steady.
  Update dots in place without erasing the composer or replaying tool output.
- Align every top-level status marker in the same leftmost column, including
  `Working`, background summaries, tools, plans and completion receipts. Represent
  parent/child relationships with explicit `├` / `└` branches and `│` continuation
  lines where needed; indentation alone must never imply a relationship.
- Use `–` (dim gray) for cancelled/interrupted/skipped work and `⚠` (yellow)
  for warnings. `◇` is reserved for neutral system information, not outcomes.
  A plain message/section bullet may use `●` without claiming success.
- Timeouts are failures (`✖`, with `TIMEOUT` text), never successful completion.
  Historical calls without a recorded result are pending/unknown (`◯`), never
  successful. Disabled features are inactive (`◯`), not failures.
- Shape and explicit status text must remain meaningful without color. Preserve
  `--no-color`, plain/JSON event contracts, terminal sanitization and display-width
  clipping. UI radio/checkbox controls, Markdown bullets, password masks and
  user/model content are separate from application status markers.
- Error and question marker styles include bold in the shared definitions.
  Do not substitute mathematical circled crosses, combining-circle questions,
  emoji, checkmarks or differently sized circle variants for the status set.
- Run `cli_application_layout_conventions` (including the status-literal audit)
  and relevant presentation/PTY regressions after changing status presentation.

## CLI Slash Command Presentation
- Shortcut labels use title-case modifiers (`Ctrl`, `Shift`, `Alt`, `Cmd`,
  `Meta`), `+` separators, and uppercase letter keys: `Ctrl+J`,
  `Ctrl+Cmd+V`, `Shift+Tab`. Named keys keep their normal display form,
  such as `Enter`, `Esc`, and `Tab`. Apply this to UI hints and docs.
- CLI application module ownership and naming follow
  [src/sapi/cli/README.md](src/sapi/cli/README.md) and
  [the shared application layout](docs/tui-application-layout.md). The complete
  CLI is the application; its root `application.c/.h` owns the embedded TUI base
  and all execution modes. Keep page controllers in `pages/`, reusable app
  components in `widgets/`, models in `models/`, use cases in `services/`,
  external integrations in `adapters/`, command actions in `commands/` and
  formatting in `presentation/`. Every implementation includes its sibling
  header first and imports other interfaces directly. `application_internal.h`
  owns no function declarations or domain SDK include collection. Run
  `cli_application_layout_conventions` alongside CLI/TUI behavior regressions
  after moving modules, adding interfaces or changing private names.
- TUI View options use four-edge `morph_tui_view_insets` for margin and padding.
  Measure returns the outer size including margin; arrange accepts an outer slot
  and applies margin once. Size limits include padding and exclude margin.
  Custom layouts use `morph_tui_view_content_rect` and pass allocated outer slots
  directly to child arrange calls. Sibling margins add without collapsing.
  See [the box model](docs/tui-box-model.md) for geometry and extension contracts.
- Submitted chat boxes own one normal-background row above and below their
  colored padding. Reasoning, answers, and tools share that outer separator;
  streaming previews and committed output keep the same position. Keep local
  slash-command spacing governed by the separate rules below.
- Treat slash commands as local UI/actions, separate from chat user messages.
  Do not commit submitted slash commands as `› /command` chat bubbles or retain
  their editable background in scrollback. Keep each submitted command visible
  as a separate purple command line on the normal background, followed by its
  results. Include arguments and aliases as submitted, with terminal controls
  sanitized. Readline recall history is separate and may retain the submitted
  command for Up/Down recall.
- Navigation commands (`/hooks`, `/resume`, `/history`, including aliases) open
  their panel after committing the plain command line, without a command bubble.
  Closing/cancelling a panel retains that command line and restores one editable
  composer and its footer; do not print a close receipt.
- Queries print their command/result heading and results in the normal output
  area. State changes print a concise success receipt. Neither uses user-message
  padding or the composer's background. Failures retain an explicit error.
- Preserve the composer's vertical position while streaming output or refreshing
  live tools/background jobs. A shrinking preview, wait transition or batch of
  completion events must not pull a bottom-positioned composer upward. Reclaim
  transient rows before history writes and let committed output consume the
  reservation; keep any remaining space on the normal background above the live
  feed. Check every published PTY frame with an unsent draft, including wrapped
  input and 120/80/60-column layouts.
- Completed turns (including their duration row) and completed command groups
  each own one trailing blank row on the normal background. Submitted command
  lines add no leading blank row. Editable composer padding is transient and
  must not supply history spacing. Check completed turn -> command -> receipt
  -> next command at 120, 80 and 60 columns, including wrapped receipts.
- Add one blank row between a submitted command and its first visible result.
  Defer that row until a result exists: a command without output (including a
  cancelled navigation panel) leaves only one trailing blank row. Normalize
  result headings' leading newlines so they do not add a second result gap.
- Unrecognized commands print an error and clear the editable composer, both
  idle and during an active turn. Keep them in Readline history for Up/Down
  recall. Do not forward slash commands as model steering text: while a turn is
  active, known commands that cannot run yet report that they must wait and
  keep the input for retry; explicitly supported commands such as `/recap`
  retain their existing behavior.
- Keep JSON command events and non-interactive output contracts unchanged.
  Verify panel cancellation, queries, receipts, unknown-command input clearing,
  command recall, and the absence of stale/duplicate composer backgrounds in PTY
  tests whenever command submission or display routing changes.

## Libraries (all static, from CMake link targets)
```
nbase::nbase (vendor/nothing; nb containers, errors, log, files/paths,
              Unicode, encoding/hash, time, cancellation, threads, processes)
morph-cjson, morph-regex (independent third-party targets)
morph-db → morph-session → morph-persistence → morph-credits
morph-app-data (runtime/resources), morph-bpe (agent/bpe)
morph-command-analysis (permission/command_analysis; tree-sitter-bash)
morph-media (media/image; stb), morph-terminal (tui/terminal text/size/activity)
morph-http (libcurl, SSE) → morph-models
morph-event, morph-exec (→ morph-sandbox), morph-render, morph-ipc
morph-permission (policy, operations, grants; nbase, db, command analysis)
morph-agent (react, context, compress, tokenizer, tool, plan)
morph-tools, morph-skill, morph-mcp, morph-config
morph-runtime ← morph-agent, config, credits, event, mcp, session, skill, sync, tools
morph-tui ← nbase, morph-terminal, morph-media, libuv
morph-cli → morph (executable)
```
Declare the dependencies actually consumed by a target. There is no shared
`morph-util` facade. Entrypoint: `src/sapi/cli/main.c` → logging, HTTP, config,
open runtime, `cli_application_run()`.

## Vendor
`vendor/nothing/` is a pinned nbase source snapshot with recorded local changes
in `vendor/nothing/README.morph.md`; do not fetch a floating version at build time.
The nbase Unicode backend vendors utf8proc 2.12.0 (Unicode 18.0.0) under
`vendor/nothing/vendor/utf8proc/`. cJSON and stb remain dedicated dependencies;
TOML uses `nb/toml.h`. The nbase main-library license remains a distribution
prerequisite until upstream provides an explicit grant.

## UTF-8
Include `nb/utf8.h` and use its bounded decoder, encoder, validation, character
width and category APIs. Do not add a local UTF-8 decoder or Unicode facade.
`nb_utf8_validate` returns `NB_UTF8_OK` on success and can report the failing
byte offset; `nb_utf8_count` returns `SIZE_MAX` on invalid input. `nb_utf8_read`
provides the bounded one-code-point operation used by text consumers.
Unicode widths/category/casefold come from nbase's pinned utf8proc backend.
ANSI filtering, terminal sanitization, display clipping and ellipsis policies
belong to `<morph/tui/terminal/text.h>`; language-block token weights belong to
`agent/tokenizer.c`. See `docs/unicode.md`.

## Core Data Structures (include `nb/*.h`, link `nbase::nbase`)
Prefer these over hand-rolled equivalents.
- **Scope cleanup** `nb/cleanup.h` — required GCC/Clang `NB_CLEANUP(fn)`.
  Initialize local owners to `NULL`/`{0}`. Callbacks are typed. Heap objects own
  their fields. Application fallible `*_take` APIs clear the source only after
  success; infallible typed steal transfers without allocating. See
  `docs/state-ownership.md`.
- **`nb_buf`** `nb/buf.h` — initialize/destroy; append/append_cstr/append_char,
  printf, cstr/str, reserve, consume/compact, next_line. Mutations return bool
  and set a sticky error; check `nb_buf_ok`. `nb_buf_move` **swaps** handles.
  `nb_buf_detach` requires retaining capacity for `nb_free`; never give detached
  storage to an API that releases an ordinary malloc owner.
- **`nb_vec`** `nb/vec.h` — initialize with element size; push/push_n/at,
  reserve/insert/remove/pop/clear/destroy. Growth invalidates slot pointers;
  new slots are uninitialized and elements need their own cleanup.
- **`nb_map *`** `nb/map.h` — opaque map owner; put/get/has/del and free.
  A configured value destructor handles only values still retained at clear/free;
  displaced/deleted values are returned to their caller.
- **`nb_str`** `nb/str.h` — borrowed `{ptr, len}`; `NB_STR_LIT`, from_cstr,
  from_parts, trim/find/compare. Comparisons use lexical ordering. A view need
  not be NUL-terminated; copy through explicit owning or caller-buffer APIs.
- **`nb_queue_t`** `nb/queue.h` — intrusive queue with stable node addresses.
  The containing owner determines each node's lifetime.

## Dependencies
- Required: SQLite3, libcurl, libuv ≥ 1.44, CMake ≥ 3.20.
- Fetched: cmark-gfm (Markdown), QuickJS, wasm3, BLAKE3, tree-sitter and GoogleTest v1.14.0; versions are pinned in CMake.
- Optional: readline (auto-detected; falls back to fgets); libseccomp (Linux sandbox).

## Configuration
- Default `~/.morph/config.toml` (`mkdir -p ~/.morph && cp config.toml.example ~/.morph/config.toml`); override with `-c`/`--config`.
- API keys from env vars (`api_key_env`) — never hardcode.
- Logs `~/.morph/log/agent.log`; output `~/.morph/output`; `MORPH_DEBUG=1` logs every HTTP request/response.

## Error Handling
- Portable mechanism errors use `nb_status`: `NB_OK` is zero; `NB_ERR_*` values
  are negative. `-errno` is not a portable nbase status; never return bare `-1`.
- Capture syscall errno immediately using `nb_status_from_errno(errno, &err)`.
  An optional `nb_err` preserves the exact OS diagnostic in `os_errno`; cleanup
  must not replace the captured failure.
- Application errors live in `src/errors.h`, allocated with `NB_ERR_USER(n)`.
  Keep model/configuration/protocol error descriptions in that catalog rather
  than adding another generic error framework.
- Use explicit `return code;` and `rc = code;`. Optional `nb_err_trace` receives
  an explicit logger, description table and source location, returns the code
  unchanged and preserves errno. It never renders UI.
- Use `nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, code)` for
  application-facing descriptions, or `nb_status_str` for known base statuses.
  Emit diagnostics through the owned `nb_log` at `NB_LOG_DEBUG`.
- `nb_log_write(NULL, ...)` disables logging. Do not install the nbase default
  logger or change the global allocator while runtimes are executing.

## `goto`
- Do not use `goto` in project-owned C/C++/Objective-C source, including macros,
  platform-specific branches, tests, examples and extensions. Vendor code is excluded.
- Use direct returns with initialized typed scope cleanup for local resources.
  Keep error codes explicit and transfer ownership only on success.
- Split complex operations into helpers. Their owner explicitly commits or rolls
  back transactions, restores state and stops workers after the operation returns.
  Do not replace jumps with single-iteration loop wrappers or control-flow macros.
- Run `source_control_flow_conventions` to check this rule.

## C Coding Conventions (REQUIREMENTS.md §6.11)
- No `//` comments — `/* */` only.
- Source strings in English for errors, logs, status, UI, model instructions; keep non-English only as input data/fixtures/examples. Localization lives in a shared i18n layer, not business logic.
- `sizeof(var)`, not `sizeof(type)`.
- Use `<limits.h>` for system limits: paths → `PATH_MAX`; stdio → `BUFSIZ`; `strncpy` uses `sizeof(dst) - 1`. Do **not** use `NAME_MAX` for logical names (tool/skill names) — use app `#define`s like `TOOL_NAME_MAX`.
- Use `NB_ERR_*` portable statuses or application `NB_ERR_USER` codes; use explicit `return code;` for error returns.
- Memory: ordinary heap ownership with required `NB_CLEANUP(fn)` for local owners; `nb_buf`/`nb_vec` for growth. Every object has typed cleanup/destroy. No bulk allocation contexts. Transfers use success-only `*_take(T **)` or infallible typed steal; failure retains the source. Do not shallow-copy owned fields. Raw allocations need NULL checks and cleanup on every path.
- Multi-statement macros wrapped in `do { } while (0)`.
- Naming: functions `snake_case`, types `struct foo`, macros `UPPER_CASE`.
- Do not introduce global variables.
- `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; CI must pass with 0 warnings.
- Tab indent (8); soft 80 / hard 100 columns.

## Tests
- C++17 GoogleTest linking the C static libs; files `tests/test_<module>.cpp`.
- Integration tests use a mock LLM (local HTTP server returning fixed SSE).
- `test_ext_demo.c` exists but is not in the CMake test build — don't rely on it.
- Expect Valgrind + ASan + UBSan clean.

## Gotchas
- Config parsed by `nb_toml` and validated against the embedded schema in `src/config/schema.c`.
- `src/media/image.c`, `img_resize.c` and `img_convert.c` need the recorded stb warning suppressions.
- Gitignored: `config.toml` (API keys — use `config.toml.example`), `vendor/md4c/` (fetched), `.morph/` (runtime data).
- macOS sandbox uses `sandbox_init` with an SBPL allowlist (no `sandbox-exec` wrapper); Linux uses seccomp-BPF denylist + Landlock.
