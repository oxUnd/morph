# morph Agent Guide

## Build & Test
- CMake ≥ 3.20 required
- Build: `cmake -S . -B build && cmake --build build`
- Tests are ON by default (`BUILD_TESTS` defaults to ON); no flag needed
- Run all tests: `cd build && ctest --output-on-failure`
- Run a single test: `cd build && ctest -R test_arena --output-on-failure`
- Run test binary directly: `cd build && ./morph-tests --gtest_filter=TestArena*`
- ASAN build: `cmake -S . -B build -DENABLE_ASAN=ON && cmake --build build`
- Clean build: `rm -rf build`

## Architecture Overview
- **ReAct loop**: Thought → Action → Observation → Guardrail → Final. Core in `src/agent/react.c`. Uses OpenAI Function Calling (not text parsing).
- **3 model backends**: `llm` (text chat), `image_gen`, `video_gen` — each configured independently in config.toml.
- **Tools**: 14 built-in tools under `src/agent/tools/`: text_gen, text_qa, img_gen, img_edit, img_info, img_resize, img_convert, vid_gen, file_read, file_list, file_info, bash_exec, skill_activate.
- **Skills**: hot-loadable instruction packs (`SKILL.md` with YAML frontmatter). Discovery from `~/.morph/skills/` and `~/.agents/skills/`. Code in `src/skill/`. Examples in `skills/`.
- **Exts**: hot-pluggable extensions via sandbox; live in `~/.morph/exts/`. Manifest format: TOML with `entry`, `permissions`, `args_schema`. Demo at `exts/demo-translate/`, `exts/demo-upper/`.
- **IPC**: JSON-RPC over stdin/stdout for ext subprocesses. Code in `src/ipc/`.
- **Context compression**: hierarchical fallback in `src/agent/compress.c`, triggered at `summarize_threshold_ratio` (default 0.8).
- **Sandbox**: seccomp+rlimit (Linux), sandbox-exec (macOS). Code in `src/sandbox/`.

## Library Dependency Chain
All libraries are static. Derived from actual CMake link targets:
```
morph-toml (vendor/toml.c — compiled with -include vendor_toml_compat.h to suppress warnings)
  ↓
morph-util (arena, log, file, cJSON, base64, utf8, spin) ← base lib, cJSON compiled in
  ↓
morph-db (SQLite) ──→ morph-session
morph-http (client, SSE: libcurl) ──→ morph-models (llm, image_gen, video_gen)
  ↓
morph-agent (react, context, compress, tokenizer, tool) ← links Threads
  ↓
morph-tools ← links morph-agent, morph-models, morph-http, morph-util, morph-skill, morph-sandbox
  ↓
morph-skill ← links morph-util, morph-agent
morph-sandbox ──→ morph-ext
morph-render (markdown via md4c, image, video)
morph-config (TOML-based) ──→ morph-cli (main CLI lib)
morph-ipc (jsonrpc)
```
Entrypoint: `src/main.c` → initializes logging, HTTP, config, then runs CLI via `cli_run()`.

## Vendor
Bundled in `vendor/`: cJSON.c/h, stb_image.h, stb_image_write.h, stb_image_resize2.h, toml.c/h. Compiled as part of the project, **not** fetched separately.
md4c is **fetched** by CMake FetchContent (not in vendor/). stb_image_write/resize2 have heavy warning suppressions in `src/agent/tools/CMakeLists.txt`.

## Dependencies
- **Required**: SQLite3, libcurl, CMake ≥ 3.20
- **Fetched by CMake**: md4c (v0.5.3 via FetchContent), GoogleTest (v1.14.0 via FetchContent)
- **Optional (auto-detected)**: readline (searched in /opt/homebrew, /usr/local, /usr; falls back to fgets)
- **Optional**: libseccomp (Linux sandbox)

## Configuration
- Config file via `-c` / `--config` flag. Example: `config.toml.example`
- API keys read from env vars (`api_key_env` field) — never hardcode in config
- Logs: `~/.morph/log/agent.log`
- Output dir defaults to `~/.morph/output`
- Debug: `MORPH_DEBUG=1` prints every HTTP request/response

## C Coding Conventions (from REQUIREMENTS.md §6.11)
These differ from typical C defaults and must be followed:
- **No `//` comments** — C-style `/* */` only
- **`sizeof(var)`** not `sizeof(type)`
- **Error codes**: negative errno (`-EINVAL`, `-ENOMEM`)
- **Cleanup**: `goto out;` pattern, no early returns with leak
- **Memory**: `xmalloc`/`xfree` wrappers — failure aborts, no NULL checks needed
- **Multi-statement macros**: wrapped in `do { } while (0)`
- **Naming**: functions `snake_case`, types `struct foo`, macros `UPPER_CASE`
- **Warnings**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` — CI must pass with 0 warnings
- Tab indent (8 chars); soft limit 80 cols, hard limit 100

## Test Conventions
- Tests are C++17 (GoogleTest) linking C static libs
- Test files in `tests/` named `test_<module>.cpp`
- Integration tests use mock LLM (local HTTP server returning fixed SSE)
- Memory testing: Valgrind + ASan + UBSan expected clean

## Gotchas
- `vendor/toml.c` requires `-include vendor_toml_compat.h` to compile without errors — handled in CMake
- `img_resize.c` and `img_convert.c` need extensive warning suppressions for stb headers (already in CMake)
- `config.toml` is gitignored (contains API keys); use `config.toml.example` as template
- `vendor/md4c/` is gitignored (fetched at build time)
- `.morph/` is gitignored (runtime data dir)
