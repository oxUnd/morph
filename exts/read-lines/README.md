# read_lines

A `.so` extension that reads a bounded range of complete lines from a large
text file. It is intended for skills that need to place a small slice of a
large local text file into context without reading the full file.

## Build

```sh
make -C exts/read-lines
make -C exts/read-lines test
```

CMake builds the vendored nbase static library with position independent code
and links it directly into the extension. No dependencies are fetched. A
separate build directory supports ASan/UBSan without loading sanitizer code
into the Python test runner:

```sh
cmake -S exts/read-lines -B /tmp/read-lines-asan -DNB_SANITIZE=ON
cmake --build /tmp/read-lines-asan
ctest --test-dir /tmp/read-lines-asan --output-on-failure
```

The same black-box cases exercise either the shared extension ABI or a native
sanitized executable, including roots, symlinks, complete lines, byte caps,
line-number overhead, binary rejection and Unicode/JSON output.

Install locally:

```sh
mkdir -p ~/.morph/exts/read-lines
cp exts/read-lines/manifest.toml \
   exts/read-lines/read_lines.so \
   ~/.morph/exts/read-lines/
```

## Arguments

```json
{
  "path": "/absolute/path/to/words.txt",
  "start_line": 1,
  "line_count": 20,
  "include_line_numbers": false,
  "max_bytes": 65536
}
```

- `path` must be an absolute path.
- `start_line` is 1-based.
- `line_count` is capped at 1000.
- `max_bytes` defaults to 65536 and is capped at 262144.
- The extension returns only complete lines. If the next complete line would
  exceed `max_bytes`, it stops before that line and sets `truncated` to `true`.

## Path Access

By default, the extension allows files under the current working directory,
`~/.morph`, and `~/.agents`.

Set `MORPH_READ_LINES_ROOTS` to override the allowlist. Use `:` between roots:

```sh
export MORPH_READ_LINES_ROOTS="/path/to/skills:/path/to/datasets"
```

Each requested file is resolved by `nb_path_canonical` and must remain under one
of the allowed roots according to `nb_path_is_within`.

## Example Output

```json
{
  "path": "/absolute/path/to/words.txt",
  "start_line": 101,
  "line_count": 3,
  "end_line": 103,
  "next_line": 104,
  "eof": false,
  "truncated": false,
  "content": "ability\nable\nabout\n"
}
```
