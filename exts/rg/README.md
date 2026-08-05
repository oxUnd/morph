# rg

An `exec` extension that exposes common ripgrep searches as a structured tool.
It invokes `rg` with an argument vector rather than a shell command, and caps
captured output to keep large searches from filling the agent context.

## Requirements

- Python 3.8 or newer
- `rg` available in `PATH`

## Install

```sh
mkdir -p ~/.morph/exts/rg
cp exts/rg/manifest.toml exts/rg/rg.py ~/.morph/exts/rg/
chmod +x ~/.morph/exts/rg/rg.py
```

## Arguments

`pattern` is required. `paths` defaults to `["."]`. The extension supports
smart/sensitive/insensitive case matching, content/file/count output modes,
globs, fixed-string and whole-word searches, hidden files, symlink following,
context lines, and per-file match limits.

The default combined stdout/stderr limit is 256 KiB and may be raised to 1
MiB with `max_output_bytes`.

## Manual test

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"run","params":{"pattern":"ext_run","paths":["src/ext"]}}' | \
  exts/rg/rg.py
```

An `rg` exit code of 0 means at least one match, 1 means no matches, and 2
usually means a search error. All three are returned as a normal tool result.

## Permission

`permissions = 4` (`EXT_PERM_EXEC`) lets the wrapper start `rg`.
`allowed_paths = ["."]` grants read access to the process working directory on
platforms where the extension filesystem sandbox is active.
