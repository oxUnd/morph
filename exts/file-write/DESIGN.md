# file_write Implementation Design

## Status

Implemented to a production-oriented v1 for embedded Android/iOS use.

Current implementation is complete for file-level operations:

- `write`
- `overwrite`
- `append`
- `mkdir`
- `copy`
- `rename`
- `delete`

Current implementation deliberately does not include line/range patching,
recursive delete, chmod/chown, xattr/ACL preservation, symlink mutation, or
metadata preservation.

## Purpose

`file_write` exists for mobile frontends where `bash_exec` is unavailable or
intentionally disabled. It gives the agent a narrow, auditable way to mutate
files while preserving the existing morph path authorization model.

The source lives under `exts/file-write`, but Android and iOS do not load it
through the general ext loader. They compile `file_write.c` directly into the
embedded core and register it with a live `struct tool_context`.

The standalone `.so` entrypoint is intentionally disabled for real writes. Its
`ext_run` returns an error because safe operation requires an embedded
`tool_context`.

## Public Tool Interface

Tool name: `file_write`

Arguments:

```json
{
  "op": "overwrite",
  "path": "notes/out.txt",
  "dst_path": "notes/new.txt",
  "content": "hello\n",
  "encoding": "utf8",
  "create_parent_dirs": true,
  "overwrite": false
}
```

Fields:

- `op`: required operation name.
- `path`: required source or target path.
- `dst_path`: required for `copy` and `rename`.
- `content`: required for `write`, `overwrite`, and `append`.
- `encoding`: `utf8` or `base64`; defaults to `utf8`.
- `create_parent_dirs`: creates destination parents when true; defaults to true.
- `overwrite`: allows destination replacement for `copy` and `rename`.

All results are JSON. Success returns `ok: true` plus operation metadata.
Failure returns `ok: false`, `code`, and `error`.

## Operation Semantics

- `write`: creates a new regular file with `O_EXCL`; fails if it already exists.
- `overwrite`: writes to a same-directory temp file, `fsync`s it, closes it, then
  renames it over the target.
- `append`: opens the target with `O_APPEND`, creating it if needed.
- `mkdir`: creates the requested directory and parents; repeated calls are
  accepted.
- `copy`: copies only regular files from read-authorized `path` to
  write-authorized `dst_path`.
- `rename`: moves only regular files from write-authorized `path` to
  write-authorized `dst_path`.
- `delete`: deletes regular files with `unlink` or empty directories with
  `rmdir`.

Decoded `content` is capped at 10 MiB per call.

## Authorization Model

All embedded writes use the host `struct tool_context`:

- Write targets call `tool_context_authorize_path(..., TOOL_PATH_WRITE, ...)`.
- `copy` source calls `TOOL_PATH_READ`.
- `copy` destination calls `TOOL_PATH_WRITE`.
- `rename` and `delete` treat `path` as a write target.

Relative write paths resolve under `output_dir`, because this is the existing
`tool_context` behavior for `TOOL_PATH_WRITE`. Paths outside `output_dir` can
still be allowed by the frontend approval callback.

When no `tool_context` is available, the tool should not be exposed to the
agent. The standalone ext entrypoint enforces this by returning `-ENOTSUP`.

## Safety Boundaries

The v1 safety boundaries are intentional:

- No recursive delete.
- No directory copy.
- No symlink creation, replacement, or mutation.
- No chmod/chown.
- No ACL, xattr, owner, timestamp, or mode preservation.
- No shell command execution.
- No text patch operations.

The implementation uses resolved paths from `tool_context` and rejects special
files for `copy`, `rename`, and `delete`.

## Mobile Integration

Android integration status:

- `fronts/android/CMakeLists.txt` includes `exts/file-write/file_write.c`.
- `fronts/android/embed.c` includes `file_write.h`.
- `embed_init()` registers `file_write_tool_init(&ctx->tools, ctx->tctx)`.

iOS integration status:

- `fronts/ios/core/build_core.sh` includes the file-write include path.
- The same script compiles `exts/file-write/file_write.c` into `libmorphcore.a`.
- Registration is inherited through shared `fronts/android/embed.c`.

CLI integration status:

- Not registered as a default CLI tool.
- Standalone ext entrypoint returns an error to avoid bypassing path approval.

## Test Coverage

Plugin-local tests live in `exts/file-write/test_file_write.c` and are run with:

```sh
make -C exts/file-write test
```

Covered scenarios:

- create new file
- reject duplicate `write`
- overwrite content
- append content
- nested mkdir
- copy regular file
- rename regular file
- delete regular file
- delete empty directory
- write base64-decoded bytes
- deny write outside `output_dir` without approval

The tests intentionally stay inside the ext directory and are not added to the
core `tests/` target.

## Known Gaps

Future versions may add:

- line/range patch operations for text editing
- non-empty directory copy with explicit recursive opt-in
- non-empty directory delete with explicit recursive opt-in
- file metadata preservation
- frontend-visible operation previews before approval
- richer error messages mapped from `morph_strerror`

These are excluded from v1 to keep the mobile file mutation surface small and
auditable.
