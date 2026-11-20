# Extension development

This guide describes the current tool-extension interface. It covers executable
extensions (`type = "exec"`) and native extensions (`type = "so"`), nbase API
discovery, linking, packaging, and verification. The bundled
`morph-ext-development` skill uses this guide; build and install rules include
the same file as its offline `references/development.md`.

Native extensions can build independently using the exported ext SDK; they do
not need the Morph source tree or its private configuration layout. Repository
examples below use `build` for the host build directory. Read `AGENTS.md` before
changing host repository files.

## Choose the extension type

| Type | Execution contract | Dependencies and permissions |
| --- | --- | --- |
| `exec` | A fresh subprocess reads one JSON-RPC request on stdin and writes one response on stdout. | Build/link its own dependencies. Manifest capabilities are intersected with runtime authority and enforced by the subprocess sandbox. |
| `so` | Morph loads the library and calls `ext_run_v2` (or legacy `ext_run`) in its process. | Link dependencies into the module, or import compatible host exports. Native tools require effective host-execution access; the runtime rejects them when subprocess sandbox limits would be required. |

Choose `exec` for process isolation or a language/runtime outside the native
ABI. Choose `so` for an in-process C ABI when the target permits host execution.
The file suffix does not select the contract: a macOS bundle named `probe.so`
is a native extension when the manifest specifies `type = "so"`.

Existing packages to inspect:

- `exts/read-lines/`: native nbase usage, an independently linked CMake build,
  and black-box ABI tests. Its JSON parser is extension-local legacy code;
  use a JSON library when creating a new tool with structured arguments.
- `exts/demo-upper/`: small native ABI example. It only uses nbase error
  constants, which need headers but no nbase function implementation.
- `exts/context-probe/`: context-aware native example with configuration,
  paths, invocation identity, logging and progress.
- `exts/demo-translate/`: executable JSON-RPC transport example, with fixed
  demonstration responses rather than real model calls.
- `exts/locate/`: executable extension with a CMake nbase dependency and
  platform frameworks.
- `exts/file-write/`: an embedded tool with `tool_context` registration.
  Its legacy `ext_run` returns unsupported; it is not a standalone native
  extension template.

Guardrail extensions use a separate entrypoint and lifecycle. For those,
inspect `docs/guardrails.md` and `exts/demo-guardrail-pii/` as well.

## Export and use the public ext SDK

The `morph-ext-development` skill ships a complete versioned SDK snapshot and an
offline scaffolder. From the activated skill directory:

```sh
python3 scripts/scaffold.py init /path/to/my-ext --name my_ext --sdk-version 1.1.0
make -C /path/to/my-ext test all
python3 scripts/scaffold.py check /path/to/my-ext
```

For an existing plugin use `scripts/scaffold.py sdk /path/to/ext --sdk-version
1.1.0`; add `--replace` only when deliberately replacing its SDK. Only the skill
package is required, not a Morph installation/source/build tree. The starter
builds offline with a compiler and make. SDK files live in `assets/sdk/<version>/`
inside the skill; the generated `sdk/manifest.json` pins the exact release, ABI,
dependency provenance and SHA-256 hashes. Public nbase type/status headers are
included; calling nbase functions still needs its implementation linked.

SDK releases use SemVer independently of Morph application releases. Published
snapshots are immutable: compatible fixes increment patch, additive APIs increment
minor, and binary/layout breaks increment both SDK major and context ABI. Runtime
ABI, table size and callback checks remain necessary. Existing projects select an
exact SDK update rather than inheriting a newer package version automatically.

The public headers are `include/morph/ext.h` and `include/morph/ext_types.h`.
They are also installed with Morph. Host maintainers can export these headers
from a configured build (this is not a prerequisite for plugin developers):

```sh
cmake --install build --component ext-sdk --prefix "$PWD/exported-sdk"
```

This copies headers without building or linking Morph. Distribute the resulting
`include/morph/` directory, or vendor those headers in an extension package.
Compile with `-I/path/to/exported-sdk/include` and the separately supplied nbase
public headers/libraries. No `src/` include path or Morph private library is
needed. SDK 1.1 retains context ABI 1 and appends `get_config_string` to the host
table; use `EXT_HOST_HAS_CALLBACK(host, get_config_string)` before accessing it.
New callbacks are appended, and existing public layouts/enum values are stable.

`host->get_config_string(host_data, path)` returns a borrowed string from the
effective per-call configuration snapshot. Supported paths are
`model.{text,vision,image,video}.{provider,adapter,model,api_base,api_key,api_key_env,extra_body_json}`.
Unknown paths return NULL; a configured empty string remains empty. Views remain
valid only until the native call returns. Do not free, retain or log credentials.
Private config, manifest and model option/result pointers remain opaque in the
SDK; their internal layouts are not a distributable ABI.

## Find the exact nbase API

nbase is an independent library from the `nothing` repository. Morph pins it
in `cmake/FetchNbase.cmake`, currently to a post-v0.1.0 revision at commit
`fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3`. Match the headers and ABI to the
Morph executable you intend to load into; do not substitute the latest upstream
headers for an older host.

A configured default build normally places the checkout in
`build/_deps/nbase-src`. Find the actual directory, including local overrides,
from the CMake cache:

```sh
rg '^nbase_SOURCE_DIR:STATIC=' build/CMakeCache.txt
```

If the build does not exist, follow the root README's prerequisites, then:

```sh
cmake -S . -B build
cmake --build build --target morph
```

Replace `build/_deps/nbase-src` below with the discovered source directory.

| Source | What to read |
| --- | --- |
| `include/nb/*.h` | Authoritative signatures, object layout, return values, ownership, and lifetime comments. |
| `docs/api-conventions.md` | Borrowed views, owned values, success-only transfers, and failure behavior. |
| `examples/README.md` and `examples/<module>.c` | Small usage examples, including buffers, files, maps, UTF-8, and TOML. |
| `tests/test_*.c` | Edge cases and observable behavior; search for the exact function name. |
| `README.md` | Module inventory, supported platforms, build and package options. |

For example:

```sh
rg -n 'nb_file_read_all|nb_path_is_within' build/_deps/nbase-src/include/nb/file.h
rg -n 'nb_utf8_(read|validate|width)' build/_deps/nbase-src/include/nb/utf8.h
rg -n 'nb_buf_detach' build/_deps/nbase-src/docs build/_deps/nbase-src/tests
```

The pinned sources are also available online:

- [Public headers](https://codeberg.org/oxund/nothing/src/commit/fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3/include/nb)
- [API ownership conventions](https://codeberg.org/oxund/nothing/src/commit/fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3/docs/api-conventions.md)
- [Examples](https://codeberg.org/oxund/nothing/src/commit/fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3/examples)

Use public includes such as `#include <nb/buf.h>`. The compiler's include search
path supplies the directory containing `nb/`; do not write build/vendor paths
into source includes or duplicate declarations with hand-written `extern`.
The headers also provide C linkage when included from C++.

| Need | Public headers |
| --- | --- |
| Growable buffers, arrays, maps, borrowed strings | `nb/buf.h`, `nb/vec.h`, `nb/map.h`, `nb/str.h` |
| Scope cleanup and allocation ownership | `nb/cleanup.h`, `nb/mem.h` |
| UTF-8 decoding, validation, Unicode width/categories | `nb/utf8.h` |
| Files, paths, directories, atomic writes | `nb/file.h` |
| TOML and INI parsing | `nb/toml.h`, `nb/ini.h` |
| Encoding, hashes, secure random bytes | `nb/encoding.h`, `nb/hash.h`, `nb/random.h` |
| Portable errors and explicit logger objects | `nb/err.h`, `nb/log.h` |
| Time, threads, cancellation, processes | `nb/time.h`, `nb/thread.h`, `nb/cancel.h`, `nb/process.h` |
| Sockets and readiness polling | `nb/socket.h`, `nb/poll.h` |

nbase functions need no Morph runtime pointer. Stateful objects such as buffers,
loggers, and processes still have their own initialization and cleanup contracts.
nbase supplies no JSON parser or Morph model client; declare additional
dependencies actually used by the extension.

## Native ABI and ownership

The loader prefers the context-aware `ext_run_v2` symbol. Export a C function
with this signature (C++ implementations need C linkage):

```c
#include <morph/ext.h>

int ext_run_v2(const struct ext_context *context,
               const char *args_json, char **result_json);
```

Existing extensions retain the legacy signature:

```c
int ext_run(const char *args_json, char **result_json);
```

- `args_json` is borrowed, NUL-terminated JSON for the call. Native calls return
  the tool result itself, without a JSON-RPC envelope.
- The host passes an empty output slot. Return an independent, NUL-terminated
  JSON string allocated with ordinary `malloc`/`strdup`; the host calls `free`,
  including when the call returns an error with a diagnostic result.
- Return `NB_OK` on success or an appropriate negative `NB_ERR_*` status.
  Capture syscall errors immediately with `nb_status_from_errno`. Do not
  return bare `-1` or assume `-errno` is a portable status.
- Keep nbase-owned storage inside its allocation/free pairing. In particular,
  `nb_buf_detach` returns storage requiring `nb_free` with the original capacity;
  do not return it directly through `result_json`. Copy the finished JSON into
  an ordinary malloc owner instead.
- Initialize owners to `NULL` or `{0}` and use typed scope cleanup. Do not
  install a global allocator/default logger or change process-wide signal state
  in the host. Finish workers before returning or unloading their code.
- The legacy native entrypoint receives no cancellation callback. The v2 host
  table supplies cooperative cancellation and a deadline. Subprocess
  time/resource limits do not sandbox either native entrypoint.

### Native context and host services

`morph/ext.h` defines an actual borrowed C object, independent of the tool's
JSON arguments/schema. Validate `abi_version` and `struct_size` on both context
and host table. Context ABI version 1 accompanies the `ext_run_v2` entrypoint.
If both symbols exist, v2 wins; the loader never calls a legacy symbol using
the new signature.

| Field | Meaning |
| --- | --- |
| `config` | Legacy opaque pointer. Independently built extensions read configuration through `host->get_config_string`, without including `config/config.h`. |
| `manifest`, `settings` | Package metadata and optional typed `[config]` manifest table. Read settings with the public `nb/toml.h` accessors. |
| `logger` | Borrowed runtime logger; `host->log` also adds extension and call identity. |
| `paths.workdir` | Current agent's working directory, including child-agent bindings. |
| `paths.output_dir` | Effective output directory, including runtime overrides. Save final artifacts here. |
| `paths.extension_dir` | Canonical package directory for bundled resources. |
| `paths.data_dir` | Persistent extension/user-specific directory under the package parent's `.data/`. Its key hashes canonical package path and user identity; sessions of that user share it. |
| `paths.temp_dir` | Private scratch directory removed when the call ends, including errors/cancellation. |
| `call` | Current user, display session, agent/depth, turn, tool call and absolute monotonic deadline in milliseconds. Zero deadline means none. Unavailable IDs are empty strings. |
| `permissions` | Access/approval/reviewer snapshot; operations still use the live shared policy. |
| `host`, `host_data` | Synchronous service callbacks bound to this invocation. Pass `host_data` as the first argument. |

Package settings can be declared without changing tool arguments:

```toml
[config]
label = "my extension"
retries = 3
```

These settings are read at discovery and require reloading after changes.
The host configuration accessor describes Morph's effective configuration.

The host table provides:

- `check_cancelled`: `NB_OK`, `NB_ERR_CANCELLED`/`NB_ERR_INTERRUPTED`, or
  `NB_ERR_TIMEOUT`. Poll during CPU work and finish every extension worker
  before returning.
- `log` / `emit_stream`: runtime logs and existing `tool.stream.delta` events,
  attributed to the current extension, turn and tool call. Use `status` for
  progress and `text` for content.
- `check_operation` / `authorize_path`: live shared permission and grant
  handling. The host binds tool/principal identity to the extension. Full does
  not add an approval requirement; explicit forbidden/mandatory-human flags
  retain their meaning. Native code still requires host execution access and
  is rejected when subprocess enforcement is needed.
- `chat` / `vision`: configured text and vision requests using option types
  from `models/llm.h`. Output goes to the supplied synchronous callback, or to
  the extension's tool stream when no callback is supplied.
- `image` / `video`: configured generation using `ext_image_request` /
  `ext_video_request` and results from `models/image_gen.h` /
  `models/video_gen.h`. Reference paths and the effective output directory
  are authorized; attach generated files explicitly when appropriate.
- Model services use request-local views with the invoking agent's
  cancellation/deadline and usage/budget sinks, without mutating shared model
  clients. HTTP success is normalized to `NB_OK`; missing configuration returns
  `MORPH_ERR_NOT_CONFIGURED`. They do not append conversation messages.
- `ask_user`: current frontend/parent-agent question callback, using the call's
  cancellation token. Free every returned malloc-owned answer and its array,
  including partial outputs on failure. An unavailable adapter returns
  `NB_ERR_UNSUPPORTED`.
- `add_artifact`: authorized file/image/video attachment with optional
  dimensions/duration. Attachments survive JSON result conversion. Paths inside
  the scratch directory are rejected because scratch is removed after return.

Context, config, settings, host table and `host_data` remain host-owned and
valid only until `ext_run_v2` returns. `result_json` keeps the legacy malloc/free
contract. Public SDK consumers must not dereference private config/manifest/model
layouts. Low-level callers use `ext_run_with_context`; calling a v2-only
module through the context-free `ext_run` wrapper returns `NB_ERR_INVAL`.

Build the example independently with:

```sh
cmake -S exts/context-probe -B build/exts/context-probe
cmake --build build/exts/context-probe
```

Override `MORPH_SDK_INCLUDE_DIR` and `NBASE_INCLUDE_DIR` for exported headers.
The example also uses the repository's cJSON source; independently packaged
extensions can carry their own JSON library. The output directory contains
`manifest.toml` and `probe.so`.
The example links its own cJSON and calls Morph through the host table.

For a minimal legacy no-argument example, create `exts/nbase-probe/probe.h`:

```c
#ifndef NBASE_PROBE_H
#define NBASE_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

int ext_run(const char *args_json, char **result_json);

#ifdef __cplusplus
}
#endif

#endif
```

And `exts/nbase-probe/probe.c`:

```c
#include "probe.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <stdlib.h>
#include <string.h>

int ext_run(const char *args_json, char **result_json)
{
	nb_buf output NB_CLEANUP(nb_buf_destroy) = {0};

	if (!args_json || !result_json)
		return NB_ERR_INVAL;

	if (*result_json)
		return NB_ERR_EXISTS;

	if (!nb_buf_append_cstr(&output, "{\"ok\":true}"))
		return nb_buf_status(&output);

	*result_json = strdup(nb_buf_cstr(&output));
	return *result_json ? NB_OK : NB_ERR_NOMEM;
}
```

This example has no argument fields and emits fixed JSON. For a real tool,
parse and validate arguments with a JSON library, escape dynamic strings with
that library, and publish matching schemas.

## Link native extensions

There are two valid dependency models:

| Model | Appropriate use |
| --- | --- |
| Import nbase functions from Morph | A native extension targeting a known host executable with compatible exported functions. The module carries no second nbase implementation. |
| Link nbase into the extension | Independent builds, standalone test runners, or hosts missing required exports. The extension declares `nbase::nbase` as its own dependency. |

Including a header supplies declarations. Calling a non-inline function also
requires an implementation, supplied by one of these models. Header-only macros,
constants, and inline helpers need no separately linked implementation unless
they call other external functions.

### macOS: import symbols from a specific host

Build against the exact Morph executable you will use. `-bundle_loader`
validates imports against it without copying nbase into the module:

```sh
mkdir -p build/exts/nbase-probe
cc -std=c11 -D_POSIX_C_SOURCE=200809L -bundle -fPIC \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
  -Ibuild/_deps/nbase-src/include \
  -Wl,-bundle_loader,build/bin/morph \
  exts/nbase-probe/probe.c -o build/exts/nbase-probe/probe.so
```

The same approach works with the existing `exts/read-lines/read_lines.c`.
Use matching architecture and deployment settings if the host was cross-built.
In a CMake `MODULE` target, the equivalent host setting is:

```cmake
target_link_options(probe PRIVATE
  "LINKER:-bundle_loader,${MORPH_EXECUTABLE}"
  "LINKER:-undefined,error")
```

Here `MORPH_EXECUTABLE` is the selected executable's absolute path. Add the
matching nbase include directory with `target_include_directories`; do not add
`target_link_libraries(... nbase::nbase)` for this dependency model.
The explicit `-undefined error` overrides CMake's permissive macOS module
defaults so imports absent from the host fail the link.

Alternatively, macOS accepts `-Wl,-undefined,dynamic_lookup`, or
`target_link_options(probe PRIVATE "LINKER:-undefined,dynamic_lookup")`.
This defers missing-symbol checks until load/binding time. Prefer
`-bundle_loader` when the intended host is available, so missing exports fail
during the build. Plain `-shared` without nbase or a host lookup option can
fail with `Undefined symbols`.

Check the actual export table and the module's imports:

```sh
xcrun dyld_info -exports build/bin/morph | rg '_nb_buf_'
nm -u build/exts/nbase-probe/probe.so
```

`U _nb_*` entries in a host-importing module are expected. Check that its
required functions exist in the host's dynamic exports; an ordinary `nm` listing
alone does not establish dynamic visibility on every platform.

### Export availability and other platforms

Morph links nbase statically. A static archive contributes needed object files,
so a Morph executable may omit whole nbase modules. Link-time optimization,
dead-code removal, visibility, and export settings can further affect what is
available. Do not treat all public nbase headers as a guaranteed host export
surface, or depend on the symbol count of one build. If an import is missing,
choose the self-linked model or explicitly arrange the intended host exports
as a separate host build change.

On Linux, shared modules normally allow unresolved imports unless linked with
`-z defs`/`--no-undefined`. The host must export the needed symbols in its
dynamic symbol table, usually via `-rdynamic`/`--export-dynamic` or CMake's
`ENABLE_EXPORTS`. Those flags do not by themselves pull every object out of a
static archive. The current Morph CMake executable target does not establish
a portable, complete nbase export contract. Inspect the intended Linux host:

```sh
nm -D --defined-only build/bin/morph | rg ' nb_buf_'
nm -D --undefined-only build/exts/nbase-probe/probe.so
```

The macOS flags above do not apply to Windows/Cygwin DLLs. Those targets need
their own export/import setup and compatible toolchain. Use an independently
linked dependency build when a verified host-import setup is unavailable.

### Link nbase into the extension

For a standalone CMake extension, set position-independent code before adding
the static dependency. From an extension project directory, a minimal build is:

```cmake
cmake_minimum_required(VERSION 3.20)
project(nbase_probe LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(MORPH_SOURCE_DIR "" CACHE PATH "Matching Morph source checkout")
include("${MORPH_SOURCE_DIR}/cmake/FetchNbase.cmake")

add_library(probe MODULE probe.c)
target_link_libraries(probe PRIVATE nbase::nbase)
target_compile_definitions(probe PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(probe PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
set_target_properties(probe PROPERTIES PREFIX "" SUFFIX ".so")
```

Then from the Morph root:

```sh
cmake -S exts/nbase-probe -B build/exts/nbase-probe-standalone \
  -DMORPH_SOURCE_DIR="$PWD" \
  -DFETCHCONTENT_SOURCE_DIR_NBASE="$PWD/build/_deps/nbase-src"
cmake --build build/exts/nbase-probe-standalone
```

The local source override is optional; without it, `FetchNbase.cmake` fetches
the pinned release. An installed matching nbase development package can instead
provide `find_package(nbase CONFIG REQUIRED)` and `nbase::nbase`. Declare
other consumed dependencies explicitly. Do not assume another process or
executable supplies them.

## Executable protocol

The host starts the manifest's `entry` without command-line arguments, sends
one JSON-RPC 2.0 request followed by a newline, then collects the response:

```json
{"jsonrpc":"2.0","id":1,"method":"run","params":{"text":"hello"}}
```

Success and error responses use the same request ID:

```json
{"jsonrpc":"2.0","id":1,"result":{"text":"HELLO"}}
```

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"missing text"}}
```

Write one JSON response to stdout, diagnostics to stderr, and terminate after
the response. stdout is a protocol channel, so banners/progress/logs must not
be mixed into it. An ordinary successful invocation should exit zero. Current
execution uses a 300-second exchange timeout and a 1 MiB stdout capture limit;
larger tool data should be saved to an authorized file with a path in the result.
The host supports cancellation of the subprocess exchange.

Executable extensions link nbase normally when they use it. Their separate
process cannot resolve functions from Morph's executable. A script needs no
nbase dependency unless it launches or loads code that uses the library.

## Manifest and discovery

For the native probe, create `exts/nbase-probe/manifest.toml`:

```toml
name = "nbase_probe"
version = "0.1.0"
description = "Verify native nbase access."
type = "so"
entry = "probe.so"
fronts = ["cli"]
categories = ["dev"]
sandbox_capabilities = []
input_schema = '{"type":"object","properties":{},"additionalProperties":false}'
output_schema = '{"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]}'
```

`entry` is relative to the installed package directory. The loader recognizes
`manifest.toml`, with `morph-ext.toml` as a fallback. `fronts` is optional;
when present it filters the allowed frontend names. Provide real object input
and output schemas so the model and tool registry know the contract. Choose a
tool name that does not collide with another registered tool.

For `exec`, set `type = "exec"`, choose an executable/script entry, and mark
the file executable. Declare only the capabilities needed by the subprocess:
`network`, `filesystem`, `exec`, `environment`, `pty`, `process_info`, `ipc`,
and `temporary_directory`. Named `filesystem` requires `allowed_paths`.
`allowed_paths` also supplies readable roots; the filesystem capability adds
write/delete authority within those roots, subject to runtime policy.
Use `allowed_env` for specific inherited variables and
`allowed_mach_services` for required macOS services. Resource fields include
`max_memory_mb`, `max_cpu_seconds`, and `max_open_files`. See `docs/sandbox.md`
for platform behavior. A capability declaration grants no authority beyond
the effective runtime policy, and does not turn native calls into sandboxed
processes.

If a package supplies `[build] command`, the source installer runs it in the
package directory using the existing confirmation/`--yes` workflow. Make that
command produce the manifest's relative `entry`; a host-importing build needs
explicit paths to its matching host and headers. Do not assume it is inside a
Morph source checkout on the user's machine.

Install an authorized local package by copying its manifest and artifact into
one directory under `[ext].dir` (default `~/.morph/exts`). For example:

```sh
mkdir -p ~/.morph/exts/nbase-probe
cp exts/nbase-probe/manifest.toml build/exts/nbase-probe/probe.so \
  ~/.morph/exts/nbase-probe/
```

Use the actual output path if built with the standalone CMake variant. Restart
Morph to discover changes, then use `/ext list` and `/ext info nbase_probe`
inside the CLI. `/ext install` accepts supported GitHub sources as described
in the root README. There is no `/ext run` command; discovery/inspection alone
does not invoke the entrypoint.

## Verification

Separate these checks and report which actually ran:

1. Build the artifact with the intended headers, architecture, dependencies,
   and host-import or self-linked model. For host imports, inspect dynamic
   exports and validate against the target executable when possible.
2. Invoke the entrypoint in an appropriate test host. Cover success, malformed
   or missing arguments, output JSON, and relevant resource/permission failures.
   Free a native result on both success and failure. A Python `ctypes` test or
   small `dlopen` runner is a different process from Morph: it needs its own
   compatible nbase symbols. Use the self-linked artifact for such standalone
   tests, or a runner explicitly linked/exporting the required implementation.
3. Check manifest parsing and discovery using an isolated install directory
   where practical. Then exercise the actual tool in Morph with the intended
   permission policy. Native rejection under restricted access differs from
   an extension's own operating-system filesystem error. Do not switch the
   user's global permission policy merely to make a native extension run.

The repository has a ready-to-run native ABI regression example:

```sh
cmake -S exts/read-lines -B build/exts/read-lines-test \
  -DFETCHCONTENT_SOURCE_DIR_NBASE="$PWD/build/_deps/nbase-src"
cmake --build build/exts/read-lines-test
ctest --test-dir build/exts/read-lines-test --output-on-failure
```

For repository C/C++ source changes, also run the relevant behavior tests and
`source_control_flow_conventions`. Add `cli_application_layout_conventions`
when touching CLI layout, ownership, interfaces, or presentation.

## Host-service boundary

Native `ext_run_v2` receives the context/services above. Legacy native calls
remain JSON input/output only. Executable extensions retain the original
single-request/response JSON-RPC protocol without native pointers, configuration
or a reverse host-service dispatcher; their argument schemas are unchanged.

Neither interface exposes a raw runtime/database owner or automatic session
history. History, memory/storage services and checkpoint integration are
separate future interfaces. Direct native writes are not automatically tracked
by `/rewind`. Independent network clients do not inherit host model accounting
or cancellation. Importing private Morph symbols is not a supported host API.

Implementation sources to consult in a matching checkout:
`src/ext/context.h`, `src/ext/ext.h`, `src/ext/ext.c`, `src/ext/manifest.c`,
`src/ipc/jsonrpc.c`, `src/sandbox/loader.c`, `src/runtime/extensions.c`, and
`src/runtime/extension_host.c`. For repository ownership
rules read `docs/state-ownership.md`; for authorization read
`docs/permissions-design.md` and `docs/sandbox.md`.
