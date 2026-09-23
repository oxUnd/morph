# morph

![demo](misc/demo.png)

A terminal-native multimodal AI agent written in pure C. Orchestrates text, image, and video generation and understanding through a ReAct loop.

中文系统介绍: [docs/archive/introduction.zh-CN.md](docs/archive/introduction.zh-CN.md)

## Features

- **Multimodal**: text chat, image generation/editing, and video generation under one entry point
- **ReAct engine**: automatic Thought → Action → Observation orchestration
- **Skills**: hot-loadable instruction packs (SKILL.md)
- **Extensions**: hot-pluggable, sandboxed, any language
- **Managed shell**: `exec` parses each command, asks approval scoped to the programs it would run, and hands long-running work to `process` sessions
- **Dynamic tools**: create session JavaScript tools on the fly with `tool_create`
- **Scheduled tasks**: recurring or one-shot agent runs deliver to a persistent inbox
- **Local-first**: sessions and artifacts persisted to SQLite, replayable offline

Sandbox, platform policy, extension manifests, and nested-macOS testing: [docs/sandbox.md](docs/sandbox.md).

## Build

Requirements: CMake ≥ 3.20, SQLite3, libcurl, libwebp, and
[mathjax-c](https://github.com/oxUnd/mathjax-c). Optional: readline.

```bash
git clone https://github.com/oxUnd/mathjax-c vendor/mathjax-c
git clone https://github.com/oxUnd/morph-markdown fronts/morph-markdown
cmake -S . -B build
cmake --build build
cmake --install build --prefix /usr/local   # optional
```

Runtime data lives in `share/morph` relative to the executable; the installed
user manual is `share/morph/morph.txt`.

Run tests:

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

The PTY regression needs `pexpect` and `pyte`; CTest skips it when unavailable:

```sh
python3 -m venv /tmp/morph-pty-venv
/tmp/morph-pty-venv/bin/pip install pexpect pyte
cmake -S . -B build -DPython3_EXECUTABLE=/tmp/morph-pty-venv/bin/python
cmake --build build
ctest --test-dir build -R cli_pty_integration --output-on-failure
```

## Configuration

Run `morph` in a terminal; a missing config triggers an interactive wizard
(↑/↓ choose, Enter select, Esc back, Ctrl-C/Ctrl-D cancel). Provider presets
fill in model and API URL. Vision, image generation, and video generation are
independent optional steps. The config is saved to `~/.morph/config.toml`
(owner-only, 0600), or the path from `-c`; existing files are never overwritten.

API keys already in environment variables are detected; you can paste a key to
save in `api_key` or name an `api_key_env` instead. Non-interactive runs
(`-p`, JSON events) do not prompt — create the config first or copy the example:

```bash
mkdir -p ~/.morph
cp config.toml.example ~/.morph/config.toml
export OPENAI_API_KEY=sk-...
```

Supported providers: `openai`, `volcengine`, `deepseek`.

## Usage

```bash
./build/bin/morph
```

- `-c <path>`: config file
- `-w <path>`: working directory
- `-p <prompt>`: run one prompt and exit
- `-s <name>`: select or create a named session (with `-p`, reuse the conversation)

With readline, the prompt stays editable while the agent runs: Enter submits a
requirement adjustment, Esc/Ctrl+C cancels, Ctrl+J or Alt+Enter inserts a
newline. Ctrl+O opens the turn's full-screen tool transcript. Pasted and typed
image paths become `[IMAGE#1]` chips (Backspace removes one); `/render <path>`
previews, `/image <path>` attaches.

## Extensions

Extensions install under `[ext].dir` (default `~/.morph/exts`):

```bash
/ext install github:owner/repo
/ext install github:owner/repo@v1.2.0
/ext install github:owner/repo//exts/foo
/ext install https://github.com/owner/repo/tree/main/exts/foo
```

Source format: `github:<owner>/<repo>[@ref][//subdir]`. A package contains
`manifest.toml` or `morph-ext.toml` with `name`, `version`, `type`, `entry`, and
optional `[build] command` (run only after confirmation unless `--yes`).

## Layout

```
src/
  agent/        ReAct loop, context compression, tool dispatch
  agent/tools/  Built-in tools (credits, memory, img_gen, vid_gen, ...)
  runtime/      Process-level owner: lifecycle, sessions, turns, tasks, MCP
  exec/         Managed process sessions behind the exec and process tools
  event/        Unified event sink shared by all frontends
  js_runner/    Embedded QuickJS runtime for dynamic tools
  persistence/  Persistent stores for memory and credit queries
  models/       LLM / image / video backends
  skill/        Skill discovery, parsing, and activation
  sync/         Session synchronisation
  sapi/         Front-ends: CLI and FastCGI
  db/           SQLite schema, sessions, permission grants
  ext/          Ext loading and management
  sandbox/      Sandboxed ext execution
  ipc/          JSON-RPC
  render/       Markdown / image / video terminal rendering
fronts/         Extra front-end libraries (morph-markdown)
exts/           Example exts (manifest.toml + entry script)
vendor/         Third-party libraries (cJSON, stb_image, toml)
```

See [AGENTS.md](AGENTS.md) for conventions and [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) for the full spec.
