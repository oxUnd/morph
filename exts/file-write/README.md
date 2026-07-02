# file_write

Mobile-focused file mutation tool for environments where shell execution is
not available. Android and iOS compile this source directly into the embedded
core and register it as the `file_write` tool.

## Build And Test

```sh
make -C exts/file-write test
```

## Arguments

```json
{
  "op": "overwrite",
  "path": "notes/out.txt",
  "content": "hello\n",
  "encoding": "utf8",
  "create_parent_dirs": true,
  "overwrite": false
}
```

Supported operations:

- `write`: create a new file; fails if the target exists.
- `overwrite`: atomically replace full file content.
- `append`: append content, creating the file if needed.
- `patch`: atomically replace a byte range in an existing regular file.
- `mkdir`: create a directory and parents.
- `copy`: copy a regular file from `path` to `dst_path`.
- `rename`: move a regular file from `path` to `dst_path`.
- `delete`: delete a regular file or empty directory.

`encoding` may be `utf8` or `base64`. Decoded content is capped at 10 MiB.
For `patch`, provide `offset` and optional `length`; `length` defaults to 0,
which inserts content at `offset`.

## Size Limits

- `write`, `overwrite`, `append`, and `patch` accept at most 10 MiB of decoded
  `content` per call.
- Larger files can be built with repeated `append` calls, subject to available
  disk space and frontend/tool-call limits.
- `copy` streams file data and does not apply the 10 MiB content cap.
- `patch` streams the existing file, but the replacement `content` is still
  capped at 10 MiB per call.

## Safety

Relative write paths resolve under `output_dir`. Paths outside `output_dir`
must be approved through the host `tool_context`. Recursive delete, chmod,
chown, ACL, xattr, symlink mutation, and metadata preservation are intentionally
out of scope.
