# File reading

`file_read` reads regular UTF-8 files with a fixed input buffer and bounded
output. It preserves source text, including CRLF and a missing final newline.
A leading UTF-8 BOM is omitted. Directory and special-file reads fail; opening a
FIFO does not wait for a writer. Workspace authorization applies to every call.

## Arguments

- `file_path`: required path, relative to the tool's working directory or absolute.
- `offset`: zero-based line index, default `0`.
- `limit`: maximum returned lines, default `1000`, range `1..1000000`.
- `max_lines`: legacy alias for `limit`; takes precedence when both are present.
- `max_bytes`: output content budget, default `51200`, range `4..262144`.
- `byte_offset`: seek directly to a source byte position instead of scanning to a
  line index. Mutually exclusive with `offset`. Must be on a UTF-8 boundary.
- `expected_version`: optional version from a previous response. A mismatch fails
  with a stale-file error; restart the read with a new version.

Numeric arguments must be integers. Offsets are limited to JSON's exactly
representable integer range (`0..9007199254740991`). Line offsets require scanning
preceding lines, with fixed memory and cancellation checks between input chunks.
Byte offsets seek directly and are preferable for continued reads of large files.

## Text results

Existing fields `file_path`, `size`, `binary`, `content`, `returned_lines`,
`total_lines`, and `truncated` remain. The following contracts apply:

- `content` contains source text only, without injected line numbers or markers.
- `total_lines` is a number only when EOF was reached and the absolute line count
  is known. Otherwise it is `null`; the reader does not scan the tail to count it.
  Empty files have zero lines; a trailing newline does not add a phantom line.
- `line_start` and `line_end` describe the zero-based, end-exclusive source line
  range. They are `null` when a nonzero byte offset makes the absolute line origin
  unknown. `returned_lines` includes a final partial line or line fragment.
- `truncated` means unread source bytes remain. `truncation_reason` is
  `line_limit`, `max_bytes`, `line_length`, or an empty string at EOF.
- `partial_line` means this page ends inside a source line and more bytes remain.
  `starts_mid_line` identifies a continuation that begins inside a line.
- `next_byte_offset` is the exact source byte cursor for continuation, or `null`
  at EOF. `next_offset` is provided only at a known absolute line boundary.
- `encoding` identifies UTF-8 text; `version` identifies the opened file using
  device, inode, size, and nanosecond modification/change timestamps.

A source line is returned in fragments of at most 16384 bytes (plus a newline
when it fits). Fragments always end at a complete UTF-8 character. No suffix is
silently discarded. Continue with `byte_offset` to read the remainder, including
when the output byte budget ends inside a line. Concatenating successive content
fields reconstructs the source text, excluding an initial UTF-8 BOM.

Example continuation:

```json
{"file_path":"src/main.c","limit":100}
```

If the result is truncated, use its cursor and version:

```json
{"file_path":"src/main.c","byte_offset":4096,"expected_version":"<version from response>"}
```

The cursor above is illustrative: always use the returned `next_byte_offset`.
Each read checks the file metadata again before returning. A detected concurrent
change fails rather than returning a successful page. Version checks detect
metadata changes; they do not provide an atomic filesystem snapshot. Reads can be
cancelled through the turn's existing runtime cancellation control.

## Binary and unsupported encodings

A NUL or invalid UTF-8 sequence encountered while reading the selected text
produces `binary: true`, a bounded ASCII hex `preview`, and an explanatory `note`;
no text content is returned. UTF-16/32 BOMs are recognized and reported explicitly
as unsupported text encodings. Convert these files to UTF-8 before reading.
Detection covers inspected bytes, not unread portions of a file. Binary results
have `total_lines: null`, `returned_lines: 0`, and `truncated: true` because their
source content is not returned in full.

Memory use is bounded independently of source file size: fixed read-ahead,
at most 256 KiB of content, and bounded JSON serialization overhead. Reaching a
page limit stops reading; very long source lines are never buffered in full.
