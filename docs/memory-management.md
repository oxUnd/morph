# Memory lifetimes and media limits

ReAct separates allocations by lifetime:

- `iteration_arena` holds model request/response data, stream buffers and tool
  invocation slots. It resets before the next iteration and when the turn ends.
- `message_arena` holds the active model conversation. Compaction builds a new
  conversation first, then releases the old arena. A failed rebuild preserves
  the old conversation. The arena also resets when the turn ends.
- `turn_arena` holds execution records needed by trace persistence, guardrails
  and memory consolidation. Records allocate only their actual attachments.
- `session_arena` holds session-scoped conversation data.

Appending a trace record uses a tail pointer. Completed traces remain available
until the context is reused or destroyed. Trace text can therefore still grow
with the amount of work performed.

Tool registries use a growable entry array. Each entry owns an arena sized for
its descriptor strings. `tool_desc` is a set of borrowed string views; callers
must copy strings they need beyond a registry mutation or destruction. Entry
pointers returned by `tool_lookup` may change after registration or removal.
Child registries own their descriptor copies independently of the parent.

`morph_json_print` registers a serialized heap buffer with an arena cleanup
handler without duplicating that buffer into arena storage. `morph_json_take_string`
transfers a malloc-allocated string to cJSON, including failure cleanup. The
project uses cJSON's default allocator.

## Media limits

The following compile-time limits apply to local media processing:

| Resource | Limit |
| --- | --- |
| Local file encoded as Base64 or decoded as an image | 64 MiB |
| Total reference URI bytes in one video-generation request | 128 MiB |
| Image width or height | 32,768 |
| Image pixel count | 33,554,432 (32 × 1024 × 1024) |

Dimensions and pixel products are checked before output-buffer allocation.
Image metadata is checked before decoding. Full-resolution source pixels are
released after resizing, before encoding the resized image.

File Base64 encoding reads chunks into the final encoded buffer. Video reference
data URIs transfer directly to the JSON tree. Inline-media API requests still
require a complete encoded JSON body in memory; HTTP upload itself is buffered.
Remote reference URLs are passed to the provider without downloading their media.

## Measurements

Local macOS arm64 measurements on 2026-09-21 used `/usr/bin/time -l` maximum RSS.
These are controlled workloads, not guarantees for arbitrary user tasks.

| Workload | Before lifetime fixes | After iteration cleanup | After remaining optimizations |
| --- | ---: | ---: | ---: |
| 35 small tools, 700 mock model calls, tool returns `ok` | 1,074 MiB | 80 MiB | 8.2 MiB |

Constructing a request with one 64 MiB local video reference decreased from
approximately 489 MiB to 179 MiB peak RSS. Both runs attempted submission to a
closed local port, isolating request construction from provider behavior.

Regression tests cover attachment retention, compaction cleanup, descriptor
ownership across registry growth/removal, JSON lifetime, Base64 chunk boundaries,
media limits and the submitted video-reference JSON shape.
