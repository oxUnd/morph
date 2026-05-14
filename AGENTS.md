# morph Agent Guide

## Build & Test
- Build: `cmake -S . -B build && cmake --build build`
- Build with tests: `cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build`
- Run all tests: `cd build && ctest --output-on-failure`
- Clean build: `rm -rf build`

## Architecture Overview
- **ReAct loop**: Thought → Action → Observation → Final. Core in `src/agent/react.c`.
- **3 model backends**: `llm` (text chat), `image_gen`, `video_gen` — each configured independently in config.toml.
- **Exts**: hot-pluggable extensions via sandbox; live in `~/.morph/exts/`. Manifest format: TOML with `entry`, `permissions`, `args_schema`. Demo at `exts/demo-translate/`.
- **Tools**: built-in (text_gen, text_qa, img_gen, img_edit, img_info, vid_gen) under `src/agent/tools/`.
- **IPC**: JSON-RPC in `src/ipc/`.
- **Context compression**: built-in in `src/agent/compress.c`, triggered at `summarize_threshold_ratio` (default 0.8).
- **Sandbox**: safe ext execution in `src/sandbox/`.

## Library Dependency Chain
All libraries are static. The dependency order is:
```
morph-toml (vendor/toml.c)
  ↓
morph-util (arena, log, file, cJSON) ← base lib, used by all others
  ↓
morph-db (database: SQLite) ──→ morph-session
morph-http (client, SSE: libcurl) ──→ morph-models (llm, image_gen, video_gen)
  ↓
morph-agent (react, context, compress, tokenizer, tool)
  ↓
morph-tools (text_gen, text_qa, img_gen, img_edit, img_info, vid_gen)
  ↓
morph-sandbox ──→ morph-ext
morph-config (TOML-based) ──→ morph-cli (main CLI binary)
morph-render (markdown via md4c, image, video)
morph-ipc (jsonrpc)
```
Entrypoint: `src/main.c` → initializes logging, HTTP, config, then runs CLI via `cli_run()`.

## Vendor
Third-party code bundled in `vendor/`: cJSON.c/h (JSON parsing), stb_image.h (image loading), toml.c/h (TOML parsing). These are compiled as part of the project, **not** fetched separately.

## Dependencies
- **Required**: SQLite3, libcurl
- **Fetched by CMake**: md4c (via FetchContent from GitHub)
- **Optional (auto-detected)**: readline (for better CLI input; brew-installed paths searched on macOS)
- **For tests**: GoogleTest (FetchContent), Threads

## Configuration
- Config file via `-c` / `--config` flag. Example: `config.toml.example`
- API keys read from env vars (`api_key_env` field)
- Logs: `~/.morph/log/agent.log`
- Providers: openai, volcengine, deepseek
- Output dir defaults to `~/.morph/output`

## Conventions
- C11 (strict), C++17 (tests)
- Debug flags: `-g -O0 -DDEBUG`; Release: `-O2 -DNDEBUG`
- Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (C only, some vendor files suppressed)
- `.gitignore` excludes `build/`, `config.toml` (secrets), `vendor/md4c/`, `.morph/`
