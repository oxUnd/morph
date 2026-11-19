---
name: morph-ext-development
description: Scaffold, modify, build, and debug Morph native or executable tool extensions. Includes a packaged, versioned native SDK and offline scaffolding script, manifests, linking, JSON-RPC, and extension tests. Use for Morph ext development or loading failures; ordinary Morph usage and unrelated plugins are outside this skill.
---

# Morph Extension Development

Deliver a buildable extension with the existing Morph extension contract. Use
available file, patch, exec, and background tools; inspect their schemas before
use. Preserve the user's chosen language, extension type, and deployment target.

## Read the development guide

Resolve skill-relative paths from this skill's directory, supplied by the active
skill context. Read `references/development.md` for the ABI, manifests, nbase API
locations, platform link commands, ownership, and verification steps.

When using this skill directly from the Morph source checkout before staging,
read `../../docs/extension-development.md` instead. This is the canonical source;
the `morph-data` build target and installation copy it into the bundled skill's
`references/development.md`. Both locations contain the same guide.

Repository-relative paths in the guide refer to a matching Morph checkout,
not to the user's extension directory or to the installed skill directory.
They are maintainer examples, not prerequisites for SDK scaffolding. Inspect a
checkout's `AGENTS.md` when changing host repository files.

## Scaffold a native extension or pin its SDK

This skill package includes the SDK itself in `assets/sdk/<version>/`, including
the public nbase declarations required by the native ABI. It also includes native
templates and `scripts/scaffold.py`. The script reads only these packaged assets;
users need neither Morph source/build directories nor a download to create and
build the starter. Python 3.9+, make, and a C11 compiler are sufficient on macOS
and Linux. Use this helper for a new native extension, keeping other chosen
languages/execution types on their existing workflow.

Resolve the script path against this skill's directory, then run:

```sh
python3 scripts/scaffold.py versions
python3 scripts/scaffold.py init /path/to/my-ext --name my_ext --sdk-version 1.1.0
make -C /path/to/my-ext test all
```

To generate SDK files in an existing extension without changing its source:

```sh
python3 scripts/scaffold.py sdk /path/to/ext --sdk-version 1.1.0
python3 scripts/scaffold.py check /path/to/ext
```

`init` requires a new or empty destination. `sdk` preserves a different or edited
snapshot unless `--replace` explicitly selects an SDK update; it changes only
`sdk/`. The starter returns fixed JSON; add a JSON library when implementing
structured input or dynamic output. Its nbase headers declare types/constants,
not implementations; add linking when calling nbase functions.

Commit generated `sdk/manifest.json` and headers with the plugin. The manifest
pins an exact SDK release, context ABI, dependency provenance and file hashes.
SDK versions use SemVer independently of the Morph application version. Preserve
published SDK snapshots: add a new version directory for an update. Compatible
fixes increment patch, additive APIs increment minor, and breaking ABI/layout
changes increment SDK major and context ABI. Runtime compatibility still checks
ABI, `struct_size`, and required callbacks; SDK version strings alone do not prove
that a host supplies a service. Existing projects update only to an exact chosen
version, never implicitly to the newest bundled release.

## Choose the execution and linking mode

- `type = "exec"` runs a bounded subprocess using one JSON-RPC request and
  response. Link any native dependencies into that executable normally.
- `type = "so"` runs inside Morph. When targeting a known compatible host,
  it can import the host's exported nbase symbols. On macOS prefer
  `-bundle_loader` against the actual target executable to check imports at
  link time. Use the guide's self-linked CMake path for independent builds,
  standalone test processes, or hosts without the required exports.
- Use `ext_run_v2(const struct ext_context *, const char *, char **)` for native
  context and host services. Include the exported `morph/ext.h`, validate context/table ABI
  version and size, and keep borrowed views, callbacks and workers within the
  call lifetime. Existing `ext_run` modules remain supported. Use the packaged
  scaffolder to vendor the SDK; never require a Morph source tree
  or copy private config layouts. Read effective model settings through
  `get_config_string`, guarded by `EXT_HOST_HAS_CALLBACK` for older hosts.
- Match the nbase headers and ABI to the target Morph build. Read the relevant
  public header and example before using an unfamiliar API. Do not infer that
  every function in a static library exists in the host's dynamic exports.

## Implement and verify

1. Inspect the package, manifest, target OS and architecture, and selected bundled
   SDK version. For host imports, inspect the actual Morph executable's exports.
   Use the packaged scaffolder for new native extensions; source/build directories
   are only relevant when deliberately working on the host itself.
2. Implement the requested behavior and explicit input/output schemas. Keep
   JSON parsing and escaping in a real JSON library. For native extensions,
   return malloc-compatible JSON text and keep nbase-owned allocations inside
   their documented ownership boundaries.
3. Declare required subprocess sandbox capabilities and paths. Reusing nbase
   grants no extra authority. Native execution requires host access; selecting
   a different linker option does not change that requirement.
4. Build the actual artifact. For host imports, check the target's dynamic
   exports and unresolved extension symbols. Exercise the entrypoint with
   success and relevant invalid-input/resource-failure cases. A Python or
   standalone loader needs its own nbase implementation or exports; it does
   not inherit symbols from a separate Morph process.
5. Verify installation/discovery in an isolated directory where practical.
   Report artifact paths, linking mode, host/version assumptions, tests run,
   and any host execution that remains unverified. Finish finite build/test
   jobs before delivery. Use the existing install workflow when installation
   is requested; otherwise deliver the package without changing user settings.

## Existing interface boundaries

Native `ext_run_v2` borrows a typed call context, a read-only effective config
snapshot and optional manifest `[config]` settings. Its host table provides
cancellation, logging/streams, permission checks, text/vision/image/video model
calls, user questions and artifact attachment. Model services inherit the
invoking agent's usage/budget sinks and cancellation. Use these callbacks
instead of importing private runtime symbols.

Legacy `ext_run` and executable JSON-RPC remain unchanged, without context or a
reverse host-service dispatcher. Neither interface exposes session history,
database access or automatic checkpoint tracking. nbase supplies utilities,
not additional Morph services; explain remaining interface gaps explicitly.
