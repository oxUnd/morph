# morph Agent Guide

## Build & Test

- CMake ≥ 3.20. Build: `cmake -S . -B build && cmake --build build`; clean: `rm -rf build`
- Tests ON by default. From the repository root, run all with
  `ctest --test-dir build --output-on-failure`; select one suite with
  `ctest --test-dir build -R Cleanup --output-on-failure`, or run
  `build/bin/morph-tests --gtest_filter='Cleanup.*'` directly.
- ASan + UBSan: add `-DENABLE_ASAN=ON`; FastCGI: add `-DBUILD_FASTCGI=ON`.
- Convention checks require Python 3. PTY regressions also require `pexpect`
  and `pyte` in the interpreter configured through
  `-DPython3_EXECUTABLE=/path/to/venv/bin/python`.
  See the [build instructions](README.md#build) for environment setup.
- For C-family source changes, run `source_control_flow_conventions`,
  `nbase_usage_conventions` and relevant behavior tests. CLI code and related
  tests also follow the [CLI guide](src/sapi/cli/AGENTS.md).
- Confirm required tests are registered (`ctest --test-dir build -N`) and
  actually run. A skipped test, missing registration or a filter matching no
  tests does not validate a change. Resolve missing dependencies where possible;
  report any remaining validation gaps and their reasons.

## Architecture
- **ReAct** `src/agent/react.c`: Thought → Action → Observation → Guardrail → Final, via OpenAI Function Calling (not text parsing).
- **Models**: configure text chat, vision understanding, image generation and video generation independently under `[model.text]`, `[model.vision]`, `[model.image]` and `[model.video]`.
- **Runtime service** `src/runtime/`: process-level owner. Frontends use opaque runtime/client handles and submit explicit session requests through `runtime_client_request_copy()`. Responses, events and interaction requests arrive on the client channel; each frontend owns navigation and presentation. `runtime_execute_turn()` remains the synchronous compatibility engine. Frontends never reimplement init/exec/persistence/shutdown. See `docs/runtime-architecture.md` and `docs/runtime-request-response.md`.
- **TUI**: The CLI uses the companion `nothing-tui` framework through its public
  `<nothing/tui/...>` headers. The application in `src/sapi/cli/` embeds
  `struct nt_application` and owns execution modes and lifecycle. Keep native
  controllers in `pages/`, reusable state-driven views in `components/` and
  CLI output formatting in `presentation/`. Workers post model events to the
  UI owner thread. Before changing CLI code or related tests, read the
  [CLI guide](src/sapi/cli/AGENTS.md). See also
  [the application layout](docs/tui-application-layout.md).
- **Permissions** `src/permission/`: shared access/approval/reviewer policy, grants and review routing. Runtime owns the context, human/model adapters and application presets; frontends use `runtime_permission_get_policy()` / `_set_policy()`. Concrete tools describe operations; opaque tools use the registry gate. Descendants share authority. See `docs/permissions-design.md` and `docs/permissions-acceptance.md`.
- **Tools** `src/agent/tools/`:
  - inspection: credits, memory, file_read/list/info, img_info; interaction and session state: memory_preference, memory_update, plan, update_plan, activate_skill, ask_user
  - media: img_gen, img_qa, img_inpaint, img_compose, img_annotate, img_resize, img_convert, vid_gen
  - workspace: apply_patch, checkpoint, config_edit, tasks, request_permissions
  - shell: `exec`, `background` — `exec` AST-parses a command into its program set and enters the shared permission layer. Bounded workspace commands run directly; missing authority is reviewed (allow/session/always/deny). It returns a session for long-running work that `background` manages. `background` supports `list`, `poll`, `wait`, `write`, `interrupt`, `kill`; waits default to 30 seconds, and `wait` collects through intermediate output until exit. Async jobs emit `process.started` / `process.completed`, remain visible in the CLI task area, and are inspected through `/jobs`. Finite jobs are collected before finalizing unless `notify_only` was explicitly requested; persistent servers use `service`. Replaces the former `bash_exec`.
  - dynamic tools: tool_create/promote/delete/history/diff/rollback
  - multi-agent: spawn_agent, send_message, followup_task, interrupt_agent, list_agents, read_agent, wait_agent. Runtime owns `agent_control`; Ultra enables proactive delegation. Child skills/guardrails and billing are independently bound; optional weighted budgets account for usage across the session tree. CLI `/subagents` switches independent transcripts, follow-up and interruption; interactive `/agents` opens the agent browser; noninteractive mode prints snapshots. Scoped child builds use `MORPH_AGENT_SCRATCH`. See `docs/multi-agent.md`.
  - img_inpaint = bbox+label region i2i; img_compose = arrow+label cross-image fusion; both consume the img_annotate JSON verbatim.
- **Plan** `src/agent/plan.c` → `plan` and lightweight `update_plan` tools; child registries are session-local.
- **Persistence** `src/persistence/orm/`: all persistent models and all database
  operations live here, using nothing-orm. Other directories call public model
  APIs and own application orchestration only. Keep SQL, ORM descriptors,
  connection leases, migrations, transactions, imports and backup operations
  inside this directory. Use structured ORM CRUD/query/schema/index/trigger
  APIs throughout production code, including this directory; do not assemble
  SQL or call raw query/statement/script entry points. Native trigger bodies
  and default expressions are immutable ORM metadata. Raw fixtures are test-only.
  Never expose native database handles or include
  `norm/*` outside the model implementation. Test fixtures also use this boundary;
  `orm_boundary_conventions` enforces it.
- **Checkpoints** `src/runtime/checkpoint.c`, `src/persistence/orm/checkpoint_store.c`: each primary turn saves a persistent checkpoint before its prompt enters history. `apply_patch` captures preimages after authorization and seals actual results even on partial failure. `/rewind` restores code, conversation, or both; the model's `checkpoint` tool restores code only. Preserve external changes through conflict checks, never infer undo scope from a Git diff. Shell, other tools, and sub-agents are outside file tracking. See `docs/checkpointing.md`.
- **MCP** `src/mcp/`: stdio + Streamable HTTP; auto-registers remote tools/resources/prompts. Config `[[mcp.servers]]`.
- **Skills** `src/skill/`: startup discovery and on-demand activation of `SKILL.md` files with YAML frontmatter. Project skills in `.agents/skills/` take precedence over user and bundled skills. See [skill management](docs/cli-management.md).
- **Exts** `src/ext/`, `src/ipc/`: executable extensions use sandboxed subprocesses and JSON-RPC; native extensions use an in-process ABI. TOML manifests declare `entry`, `type`, capabilities and `input_schema`; examples in `exts/`. Read [extension development](docs/extension-development.md) and use the bundled `morph-ext-development` skill when creating or debugging an extension. Native extensions may resolve nbase functions from a compatible host instead of linking another copy; verify the target host's exports and follow the platform-specific link instructions in that guide.
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

## Libraries (all static, from CMake link targets)
```
nbase::nbase (pinned nothing revision; nb containers, errors, log, files/paths,
              Unicode, encoding/hash, time, cancellation, threads, processes)
morph-cjson, morph-regex (independent third-party targets)
morph-db → morph-session → morph-persistence → morph-credits
morph-app-data (runtime/resources), morph-bpe (agent/bpe)
morph-command-analysis (permission/command_analysis; tree-sitter-bash)
morph-media (media/image; stb), morph-terminal (terminal/text; nbase)
morph-http (nbase HTTP policy/cancellation helpers) → morph-models
morph-event, morph-exec (→ morph-sandbox), morph-render, morph-ipc
morph-permission (policy, operations, grants; nbase, db, command analysis)
morph-agent (react, context, compress, tokenizer, tool, plan)
morph-tools, morph-skill, morph-mcp, morph-config
morph-runtime ← morph-agent, config, credits, event, mcp, session, skill, sync, tools
nothing-tui ← nbase, libuv, GNU Readline, morph-markdown-render
morph-cli ← nothing-tui, morph-terminal, morph-runtime → morph (executable)
```
Declare the dependencies actually consumed by a target. There is no shared
`morph-util` facade. Entrypoint: `src/sapi/cli/main.c` → logging, config,
open runtime, `cli_application_run()`.

Treat nbase as a third-party dependency: include its public headers with
`#include <nb/xxx.h>`, never quoted includes or vendor-relative header paths.

## Vendor
`cmake/FetchNbase.cmake` pins a post-v0.1.0 nothing revision and applies the
executable-path patch on Cygwin. Morph, nothing-tui and nothing-orm share `nbase::nbase`.
Morph enables its HTTP backend (libcurl >= 7.85 with asynchronous DNS) and uses
`nb/http.h` and `nb/sse.h`.
The nbase Unicode backend vendors utf8proc 2.12.0 (Unicode 18.0.0). cJSON and
stb remain dedicated dependencies. TOML uses `nb/toml.h`. The pinned nbase
main library is MIT licensed; retain its license when distributing it.
Use `nb_utf8_width_codepoint` for individual code point widths.

## UTF-8
Include `nb/utf8.h` and use its bounded decoder, encoder, validation, character
width and category APIs. Do not add a local UTF-8 decoder or Unicode facade.
`nb_utf8_validate` returns `NB_UTF8_OK` on success and can report the failing
byte offset; `nb_utf8_count` returns `SIZE_MAX` on invalid input. `nb_utf8_read`
provides the bounded one-code-point operation used by text consumers.
Unicode widths/category/casefold come from nbase's pinned utf8proc backend.
ANSI filtering, terminal sanitization, display clipping and ellipsis policies
for agent summaries and CLI output use `src/terminal/text.h`. CLI report formatting
lives in `src/sapi/cli/presentation/`. Language-block token weights belong to
`agent/tokenizer.c`. See `docs/unicode.md`.

## Core Data Structures (include `<nb/*.h>`, link `nbase::nbase`)
Prefer these over hand-rolled equivalents.
- **Scope cleanup** `nb/cleanup.h` — required GCC/Clang `NB_CLEANUP(fn)` for local
  owners; each resource must have one clearly defined cleanup owner.
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
- Core and CLI build prerequisites, companion source checkouts and version requirements are listed in the [build instructions](README.md#build).
- Fetched: nothing-orm (SQLite), cmark-gfm (Markdown), QuickJS, wasm3, BLAKE3,
  tree-sitter and GoogleTest v1.14.0; versions are pinned in CMake.
- The CLI requires GNU Readline ≥ 8.0. libseccomp is optional for the Linux sandbox.

## Configuration
- Default `~/.morph/config.toml` (`mkdir -p ~/.morph && cp config.toml.example ~/.morph/config.toml`); override with `-c`/`--config`.
- Use `api_key_env` for credentials and never hardcode API keys. Setup also supports a private user config with `api_key`.
- Logs `~/.morph/log/agent.log`; output `~/.morph/output`; `MORPH_DEBUG=1` enables debug logging and curl diagnostics.

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

## C Coding Conventions
- Leave one blank line after an `if`, `for`, `while`, `do ... while`, or
  `switch` when another statement follows at the same level. Apply this to
  both braced and unbraced bodies. Keep `else` attached to its `if` and
  the trailing `while` attached to its `do` body.
- Group statements by purpose. Keep related operations together and leave a
  blank line when the purpose changes.
- Keep all `#include` directives at the head of the file, before declarations
  and definitions. Preserve header guards, required configuration macros,
  language linkage and platform-specific conditional includes.
- No `//` comments — `/* */` only.
- Source strings in English for errors, logs, status, UI, model instructions; keep non-English only as input data/fixtures/examples. Localization lives in a shared i18n layer, not business logic.
- Prefer `sizeof(var)` for an existing object and `sizeof(*ptr)` when allocating
  its pointee. Use `sizeof(type)` for type metadata, such as TUI `instance_size`,
  or when no suitable object expression exists.
- Use `<limits.h>` for system limits: paths → `PATH_MAX`; stdio → `BUFSIZ`.
  Do **not** use `NAME_MAX` for logical names (tool/skill names); use app
  constants such as `TOOL_NAME_MAX`.
- String copies use the destination's byte capacity. `sizeof(dst)` gives that
  capacity only for an actual array, never for a pointer or array parameter.
  Pointer destinations require an explicit capacity. Prefer checked APIs such
  as `nb_str_copy_to`; handle insufficient capacity explicitly. If using
  `strncpy`, require nonzero capacity, copy at most `capacity - 1` bytes and
  explicitly NUL-terminate. Handle truncation explicitly; do not silently
  truncate paths or logical identifiers.
- Use `NB_ERR_*` portable statuses or application `NB_ERR_USER` codes; use explicit `return code;` for error returns.
- Memory: ordinary heap ownership with required `NB_CLEANUP(fn)` for local
  owners. Use `nb_buf`/`nb_vec` for growth. Every object has typed cleanup/destroy.
  No bulk allocation contexts. Transfers use success-only `*_take(T **)` or infallible
  typed steal; failure retains the source. Do not shallow-copy owned fields.
  Raw allocations need NULL checks and cleanup on every path.
- Multi-statement macros wrapped in `do { } while (0)`.
- Naming: functions `snake_case`, types `struct foo`, macros `UPPER_CASE`.
- Do not introduce mutable objects with static storage: file-scope variables,
  function-local statics or thread-local variables. Immutable tables, strings
  and function tables are allowed; qualify the object itself, for example
  `const char *const names[]`, not `const char *names[]`. Third-party internal
  state is excluded. See [state ownership](docs/state-ownership.md) for the
  optional `-DENABLE_GLOBAL_STATE_AUDIT=ON` AST check and its verification steps.
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
- Gitignored: `config.toml` (API keys — use `config.toml.example`), `.morph/` (runtime data).
- macOS sandbox uses `sandbox_init` with an SBPL allowlist (no `sandbox-exec` wrapper); Linux uses seccomp-BPF denylist + Landlock.
