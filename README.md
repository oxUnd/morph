# morph

![demo](misc/demo.png)

A terminal-native multimodal AI agent written in pure C. Orchestrates text, image, and video generation and understanding through a ReAct loop.

## Features

- **Multimodal in one place**: text chat, image generation/editing, and video generation under a single entry point
- **ReAct engine**: automatic Thought → Action → Observation orchestration
- **Ext extensions**: hot-pluggable extensions running in a sandbox, written in any language
- **Local-first**: sessions and artifacts persisted to SQLite, replayable offline
- **Lightweight**: minimal static dependencies, fast startup

## Build

Requirements: CMake ≥ 3.16, SQLite3, libcurl. Optional: readline.

```bash
cmake -S . -B build
cmake --build build
```

Run tests:

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

## Configuration

Copy the example config and set your API key:

```bash
mkdir -p ~/.morph
cp config.toml.example ~/.morph/config.toml
export OPENAI_API_KEY=sk-...
```

Supported providers: `openai`, `volcengine`, `deepseek`.

## Usage

```bash
./build/morph
```

Optional flags:

- `-c <path>`: specify a config file
- `-s <name>`: specify a session name

## Layout

```
src/
  agent/    ReAct loop, context compression, tool dispatch
  models/   LLM / image / video backends
  tools/    Built-in tools (text_gen, img_gen, vid_gen, ...)
  ext/      Ext loading and management
  sandbox/  Sandboxed ext execution
  ipc/      JSON-RPC
  render/   Markdown / image / video terminal rendering
exts/       Example exts (manifest.toml + entry script)
vendor/     Third-party libraries (cJSON, stb_image, toml)
```

See [AGENTS.md](AGENTS.md) for conventions and [REQUIREMENTS.md](REQUIREMENTS.md) for the full spec.
