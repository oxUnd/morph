# morph Agent Guide

## Build & Test
- CMake ≥ 3.20. Build: `cmake -S . -B build && cmake --build build`; clean: `rm -rf build`
- Tests ON by default. All: `cd build && ctest --output-on-failure`; one: `ctest -R test_arena`; direct: `./morph-tests --gtest_filter=TestArena*`
- ASAN: add `-DENABLE_ASAN=ON`; FastCGI front-end: add `-DBUILD_FASTCGI=ON`

## Architecture
- **ReAct** `src/agent/react.c`: Thought → Action → Observation → Guardrail → Final, via OpenAI Function Calling (not text parsing).
- **Models**: `llm` (text chat), `image_gen`, `video_gen`, each configured independently.
- **Runtime facade** `src/runtime/`: process-level owner. Frontends hold an opaque `struct runtime *` and enter through `runtime_execute_turn()`; they never reimplement init/exec/persistence/shutdown. See `docs/runtime-architecture.md`.
- **Tools** `src/agent/tools/`:
  - read-only: credits, memory, memory_preference, file_read/list/info, img_info, plan, activate_skill, agent_status, ask_user
  - media: img_gen, img_qa, img_inpaint, img_compose, img_annotate, img_resize, img_convert, vid_gen
  - workspace: apply_patch, config_edit, tasks, request_permissions
  - shell: `exec`, `process` — `exec` AST-parses a command into its program set, approves per program (allow/session/always/deny), and returns a session for long-running work that `process` manages. `process` supports `list`, `poll`, `wait`, `write`, `interrupt`, `kill`; waits default to 30 seconds, and `wait` collects through intermediate output until exit. Async jobs emit `process.started` / `process.completed`, remain visible in the CLI task area, and are inspected through `/jobs`. Finite jobs are collected before finalizing unless `notify_only` was explicitly requested; persistent servers use `service`. Replaces the former `bash_exec`.
  - dynamic tools: tool_create/promote/delete/history/diff/rollback
  - sub-agent: delegate, fanout
  - img_inpaint = bbox+label region i2i; img_compose = arrow+label cross-image fusion; both consume the img_annotate JSON verbatim.
- **Plan** `src/agent/plan.c` → `plan` tool.
- **MCP** `src/mcp/`: stdio + Streamable HTTP; auto-registers remote tools/resources/prompts. Config `[[mcp.servers]]`.
- **Skills** `src/skill/`: hot-loadable `SKILL.md` + YAML frontmatter, from `~/.morph/skills/` and `~/.agents/skills/`; examples in `skills/`.
- **Exts** `src/ext/`, `src/ipc/`: sandboxed subprocesses (JSON-RPC) with TOML manifest (`entry`, `permissions`, `args_schema`); examples in `exts/`.
- **Context compression** `src/agent/compress.c`, triggered at `summarize_threshold_ratio` (0.8).
- **Sandbox** `src/sandbox/`: Linux seccomp-BPF denylist (default ALLOW, denies escape-relevant syscalls) + Landlock + rlimits; macOS Seatbelt via `sandbox_init`.
- **Events** `src/event/`: unified dotted names across startup/MCP/ReAct/tools/HITL/tasks. Renderers in `src/sapi/cli/events.c` and `src/sapi/fastcgi/event_sink.c`. See `docs/event-system.md`.
- **Scheduled tasks** `src/runtime/task_*.c`. See `docs/scheduled-tasks.md`.
- **Dynamic JS tools**: embedded QuickJS in `src/js_runner/`. See `docs/quickjs-tools.md`.

## CLI Slash Command Presentation
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
- Completed turns (including their duration row) and completed command groups
  each own one trailing blank row on the normal background. Submitted command
  lines add no leading blank row. Editable composer padding is transient and
  must not supply history spacing. Check completed turn -> command -> receipt
  -> next command at 120, 80 and 60 columns, including wrapped receipts.
- Add one blank row between a submitted command and its first visible result.
  Defer that row until a result exists: a command without output (including a
  cancelled navigation panel) leaves only one trailing blank row. Normalize
  result headings' leading newlines so they do not add a second result gap.
- Unrecognized commands remain in the editable composer after the error, so the
  user can correct them. Do not forward slash commands as model steering text:
  while a turn is active, unsupported local commands report that they must wait
  and keep the input for retry; explicitly supported commands such as `/recap`
  retain their existing behavior.
- Keep JSON command events and non-interactive output contracts unchanged.
  Verify panel cancellation, queries, receipts, unknown-command correction,
  command recall, and the absence of stale/duplicate composer backgrounds in PTY
  tests whenever command submission or display routing changes.

## Libraries (all static, from CMake link targets)
```
morph-toml (vendor/tomlc17)
morph-util (arena, log, file, cJSON, base64, utf8, spin)
  ├─ morph-db (SQLite) → morph-session → morph-persistence → morph-credits
  ├─ morph-http (libcurl, SSE) → morph-models
  ├─ morph-event, morph-exec (→ morph-sandbox), morph-render, morph-ipc
  └─ morph-sandbox → morph-ext
morph-agent (react, context, compress, tokenizer, tool, plan)
morph-tools, morph-skill, morph-mcp
morph-config
morph-runtime ← morph-agent, config, credits, event, mcp, session, skill, sync, tools
morph-cli → morph (executable)
```
Entrypoint: `src/sapi/cli/main.c` → logging, HTTP, config, open runtime, `cli_run()`.

## Vendor
`vendor/`: cJSON, stb_image{,_write,_resize2}, tomlc17, utf8proc 2.12.0 — compiled in, never fetched.
md4c is fetched by CMake FetchContent. stb write/resize2 have warning suppressions in `src/agent/tools/CMakeLists.txt`.

## UTF-8
Always use `src/util/utf8.h` (`#include "util/utf8.h"` from outside `src/util/`, `"utf8.h"` inside); never hand-roll decode/encode/width. It wraps vendored `vendor/utf8proc/` (v2.12.0, Unicode 18.0.0), compiled into `morph-util` through the shared `src/util/sources.list` manifest. Only `src/util/utf8.c` may include or call the upstream API; public headers and callers use project-owned types and helpers. Decoding, encoding, validation, character width and general Unicode categories come from utf8proc. Legacy names `utf8codepoint`, `utf8len`, `utf8valid`, and `utf8dup` remain as compatibility adapters. Prefer bounded `utf8_decode_codepoint` / `utf8_encode_codepoint`, `utf8_clamp_bytes`, `utf8_sanitize_into` / `_inplace`, and `utf8_display_width` for new code. ANSI filtering, layout/clipping and tokenizer language-block policies remain application logic. The header is the source of truth.

## Core Data Structures (`src/util/`, link `morph-util`)
Prefer these over hand-rolled equivalents.
- **Arena** `arena.h` — bump allocator for scope-lived data: `arena_create/destroy/reset`, `arena_alloc` (zeroed), `arena_alloc_aligned`, `arena_strdup`.
- **`morph_buf_t`** `buf.h` — growable string builder: `morph_buf_init`/`_init_arena`, `append`/`puts`/`putc`/`printf`, `cstr`, `str`, `detach`, `cleanup`. Never `char[N]` + `snprintf` accumulation.
- **`morph_array_t`** `array.h` — generic dynamic array: `morph_array_init`/`_init_arena`, `push`/`push_n`/`pop`/`get`/`reserve`/`clear`, `foreach`, `cleanup`. `push` may realloc — don't hold element pointers across pushes.
- **`morph_strmap_t`** `strmap.h` — open-addressing string→`void *`: `init`/`cleanup`/`clear`, `set`/`get`/`contains`/`remove`/`len`.
- **`morph_str_t`** `str.h` — non-owning `{len, const char *}` view: `morph_strdup`/`strndup`, `morph_strcmp`/`strcasecmp`/`strncmp`, `morph_str_to_c`, `morph_str_chr`/`rchr`/`trim`, `MORPH_STRLIT`.
- **`morph_queue_t`** `queue.h` — intrusive doubly-linked list, header-only (`morph_queue_init`, `insert_head/tail`, `remove`, `foreach`/`foreach_safe`, `morph_queue_data`).

## Dependencies
- Required: SQLite3, libcurl, CMake ≥ 3.20.
- Fetched: md4c v0.5.3, GoogleTest v1.14.0.
- Optional: readline (auto-detected; falls back to fgets); libseccomp (Linux sandbox).

## Configuration
- Default `~/.morph/config.toml` (`mkdir -p ~/.morph && cp config.toml.example ~/.morph/config.toml`); override with `-c`/`--config`.
- API keys from env vars (`api_key_env`) — never hardcode.
- Logs `~/.morph/log/agent.log`; output `~/.morph/output`; `MORPH_DEBUG=1` logs every HTTP request/response.

## Error Handling
- `typedef int morph_err_t` (`src/util/error.h`): `0` = success, `< 0` = error; never return bare `-1`.
- Use POSIX `-errno` for system errors; return `-errno` (not a fixed code) from failing `pipe`/`fork`/`select`/`opendir` calls.
- Use `MORPH_ERR_*` for domain errors: NOT_CONFIGURED (-257), NOT_INITIALIZED (-258), API (-259), NETWORK (-260), PARSE (-261), PROTOCOL (-262), DB (-263), FORMAT (-264), PROCESSING (-265), SANDBOX (-266), LOAD (-267), LLM (-268).
- Use explicit `return code;`. Optional `MORPH_TRACE_ERROR(logger, code)` is an expression that calls a typed tracing function with the source location; write `return MORPH_TRACE_ERROR(logger, code);` when tracing is useful. It never hides control flow and evaluates both arguments once in all builds. Diagnostics use `LOG_DEBUG` file logs, never terminal output.
- Assign `rc = code;` before a cleanup jump. Use `rc = MORPH_TRACE_ERROR(logger, code);` for optional tracing. Capture syscall errors immediately with `rc = morph_error_from_errno(errno);` before cleanup. Logging contexts are explicit, never global or thread-local.
- `morph_strerror(err)` for all user/LLM-facing messages; never raw `%d`.

## `goto`
- Default: no `goto`. Use direct returns for validation and error paths without cleanup.
- Use `goto` only when several exit paths share non-trivial cleanup. Prefer helper functions or local cleanup first.
- Labels describe the action (`out_free_buffer`), never numbered. Use distinct cleanup levels in reverse acquisition order.
- A centralized-cleanup function must preserve its error code with an explicit assignment before cleanup; never fall through to success.
- Reference: [Linux kernel coding style §7](https://www.kernel.org/doc/html/latest/process/coding-style.html#centralized-exiting-of-functions).

## C Coding Conventions (REQUIREMENTS.md §6.11)
- No `//` comments — `/* */` only.
- Source strings in English for errors, logs, status, UI, model instructions; keep non-English only as input data/fixtures/examples. Localization lives in a shared i18n layer, not business logic.
- `sizeof(var)`, not `sizeof(type)`.
- Use `<limits.h>` for system limits: paths → `PATH_MAX`; stdio → `BUFSIZ`; `strncpy` uses `sizeof(dst) - 1`. Do **not** use `NAME_MAX` for logical names (tool/skill names) — use app `#define`s like `TOOL_NAME_MAX`.
- Negatives errno or `MORPH_ERR_*`; use explicit `return code;` for error returns.
- Memory: arena for scope-lived data, `morph_buf`/`morph_array` for growth; raw `malloc`/`free` needs NULL checks and cleanup on every path.
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
- Config parsed by vendored tomlc17 and validated against the embedded schema in `src/config/schema.c`.
- `img_resize.c` / `img_convert.c` need stb warning suppressions (already in CMake).
- Gitignored: `config.toml` (API keys — use `config.toml.example`), `vendor/md4c/` (fetched), `.morph/` (runtime data).
- macOS sandbox uses `sandbox_init` with an SBPL allowlist (no `sandbox-exec` wrapper); Linux uses seccomp-BPF denylist + Landlock.
