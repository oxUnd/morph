# Directory listing

`file_list` browses one directory without recursion. It includes hidden and
ignored entries and returns directories first, followed by other entries, with
bytewise name ordering inside each group. Workspace authorization applies to
each request. Listing a symlink to an external directory still requires access
to its target; symlinks encountered as children are listed without following them.

## Arguments and continuation

- `dir_path`: required directory path, relative to the tool working directory or
  absolute.
- `limit`: maximum returned entries, default `1000`, range `1..10000`.
- `max_bytes`: budget for the serialized `entries` JSON array, default `51200`,
  range `2048..1048576`. The response's small metadata object is additional.
  If a single entry cannot fit, the tool reports an error asking for a larger budget.
- `after`: optional `{ "name": "...", "type": "..." }` cursor returned as
  `next_after` on a previous page. Continue with the same `dir_path`.

The existing `dir_path` and `entries` result fields remain. Each entry contains
`name` and `type`. Regular files and directories retain types `file` and `dir`;
other types are `symlink`, `fifo`, `socket`, `char`, `block`, and `unknown`.
A dangling symlink is listed as `symlink`, not as a regular file.

The result also contains:

- `total_entries`: the number of children observed during the scan, including
  entries before the cursor but excluding `.` and `..`.
- `returned_entries`: the number of entries in this page.
- `truncated`: whether eligible entries remain after the returned page.
- `next_after`: cursor for the last returned entry, or `null` at the end.
- `truncation_reason`: `limit`, `max_bytes`, or an empty string for a complete page.

Example:

```json
{"dir_path":"src","limit":100}
```

If truncated, pass the returned cursor unchanged:

```json
{"dir_path":"src","limit":100,"after":{"name":"util","type":"dir"}}
```

The cursor is based on the directories-first name ordering. Each page scans the
whole directory to select the next sorted entries. Storage is bounded by `limit`,
not directory size: a max-heap retains only the current page's smallest eligible
entries, then those entries are sorted once and converted directly to JSON.
Selection costs O(n log limit); final sorting costs O(limit log limit).
Directory changes between pages can cause entries to appear or disappear;
pagination is best effort, not a filesystem snapshot.

File types use the directory's type information when available, falling back to
`fstatat` without following symlinks. System errors, including failed directory
reads, are reported instead of returning a misleading successful partial result.
Names that cannot be represented as valid UTF-8 fail explicitly with an encoding
error rather than silently changing their identity. Allocation and error paths
release the directory stream, array, and JSON objects. Runtime cancellation is
checked during scanning and JSON construction.

For recursive project file discovery, use `rg --files` through the managed shell;
its ignore and hidden-file behavior differs from directory browsing. `find` is
useful for recursive type, size, and time predicates. Neither is a dependency of
this tool.
