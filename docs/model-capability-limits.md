# Model capability and resource limits

Limits must select an explicitly bounded view or reject an operation before
claiming success. They must not silently change instructions, tool identities,
schemas, task lists, or user answers. Model-visible previews, transport limits,
display clipping, and persistence limits are separate contracts.

## Reference implementation

The review used OpenAI Codex commit
[`c3936b4b8a66b590b1ee4edd2276314899c87e8a`](https://github.com/openai/codex/tree/c3936b4b8a66b590b1ee4edd2276314899c87e8a).
Relevant implementation paths are:

- `codex-rs/utils/output-truncation/src/lib.rs`: shared truncation policy,
  explicit omission notices, and structured replacement of oversized results.
- `codex-rs/core/src/tools/context.rs`: serialized output includes metadata
  and markers in its budget; history does not blindly truncate JSON strings.
- `codex-rs/core/src/unified_exec/`: bounded process output is distinct from
  model response budgets.
- `codex-rs/core/src/agent/`: spawn reservations are released independently
  of retained task identity and history.
- `codex-rs/utils/image/src/lib.rs`: image detail determines a preparation
  policy instead of forcing every image into a tiny thumbnail.
- `codex-rs/config/src/config_toml.rs`: project instruction budgets are
  explicit configuration, with a 32 KiB default.

Morph uses these principles with its own C ownership, runtime, tool-result,
permission, and persistence contracts.

## Model requests and output

Tool-call names and provider IDs are owned dynamic strings throughout parsing,
cloning, execution, replay, and subsequent requests. Local identity remains
separate from provider response-item identity.

Environment-provided API credentials can use dynamically owned storage beyond
the inline configuration capacity. Clones own independent copies and request
views borrow them. Authorization headers are built dynamically, so a long
gateway token is not silently shortened before transmission.

Responses streams must reach a recognized terminal status. Output-token
exhaustion preserves available partial text, reports an incomplete response,
and does not execute incomplete tool calls. Failed responses, other incomplete
statuses, and missing terminal events are errors.

Context selection reserves the complete configured output budget plus the
protocol margin, even when output exceeds half the window. The request path
checks that budget before transmission. Input counting is an estimate;
provider tokenizers can differ. Keep a protocol margin and use explicit
context-error recovery rather than presenting local counts as provider facts.
Long unbroken text uses an indexed BPE merge heap instead of repeatedly
rescanning the whole input. It preserves merge ranks and leftmost tie-breaking
without imposing a new character cutoff on counting.

## Tool results and recovery

The worker retains a complete tool-result envelope. The model/history path
applies `context.tool_result_max_tokens` (default 8000) with explicit omission
metadata and valid JSON. The persisted original is not first cut at 256 KiB.
Narrow a query or read another page when a preview is insufficient.

`file_read` and `file_list` retain their bounded pages and continuation
contracts. Display previews may clip more aggressively without changing the
model result or stored data.

Process output retains a bounded memory window and a separate redacted spool
under a 64 MiB runtime quota. Results identify the captured prefix and any gap
unavailable in both storage paths. An advancing poll cursor is not a mechanism
for recovering bytes omitted from an inline preview.

Dynamic-tool schemas and descriptions use dynamic ownership. Dynamic source
and capture budgets remain configurable. A capture overflow is reported as
an output limit, before attempting to parse a partial JSON-RPC response.
Return file paths instead of embedding large datasets in tool results.
The runner's fetch and process APIs use that configured capture budget too;
fetch supports validated headers, including Range for servers that support it.

## Tasks, skills, memory, and interaction

Subagents retain stable task records independently of the eight active-task
slots. Completion and failure release running capacity. A fanout larger than
eight is rejected before starting; the schema exposes that bound. Depth
remains limited to two. These are execution bounds, not lifetime task quotas.

The skill registry grows dynamically and keeps borrowed entry addresses
stable. Skill metadata uses Unicode-aware validation and explicit errors for
oversized or malformed fields. A rejected metadata value is never silently
shortened into a different skill name or description.

Memory injection selects complete records using the configured Unicode
character budget. Omitted records are identified and remain retrievable with
the `memory` tool, including explicit preferences. Automatically extracted
facts are not discarded merely because their value exceeds 200 bytes.
Memory pages preserve UTF-8 boundaries and expose a continuation offset. Their
token allowance includes the finalized JSON envelope, escaping, and compatibility
fields, so pagination metadata survives the subsequent history budget.

Questions and user answers must preserve their full text. Choice counts and
interaction transport budgets are explicit, validated limits. CLI command
parsing must retain all arguments, and scheduled-task display titles must
not act as the execution prompt's storage limit.

Model approval receives the complete user objective and uses the configured
review model output budget. If the complete request cannot fit, review defers
through the normal failure path rather than reviewing only a prefix.
LLM guardrail prompts use dynamic buffers.

## Retained resource protections

These explicit limits protect resources and protocol integrity:

| Surface | Default or fixed bound | Overflow behavior |
| --- | --- | --- |
| Project guidance | `context.project_doc_max_bytes`, default 32 KiB | Whole file omitted with path and `file_read` recovery instruction |
| Prompt templates | 256 KiB/file, 1 MiB combined source and rendered output | Explicit error |
| MCP stdio | 16 MiB/frame | Explicit error and disconnect |
| FastCGI request | 4 MiB | HTTP 413 |
| Hook transport | 1 MiB input and each output stream | Explicit error |
| Hook inline context | 32 KiB/dispatch, plus per-handler token policy | Private full-context file and visible recovery reference |
| Interactive question | 64 choices; 1 MiB combined answer text | Explicit error, including optional timeout fallback |
| Memory result page | At most 4000 Unicode characters and the configured tool token budget | Smaller complete page with continuation offset |
| Skill metadata | Name 64 and description 1024 Unicode characters; 1 MiB frontmatter | Explicit error |
| Dynamic-tool source | Configurable, default 256 KiB | Explicit error |
| Dynamic-tool capture | Configurable, default 1 MiB | Explicit error; save large data to a file |
| Tracked file snapshot | 16 MiB/file | Capture or pre-write postimage validation fails explicitly |
| Captured checkpoint data | 128 MiB before/after data | Write validation or seal fails explicitly |
| Checkpoint restore plan | 256 MiB including current compensation copies | Explicit error |
| Patch input | 64 KiB/call | Explicit error; split into complete patches |
| Image decode | 64 MiB encoded file, 32 million pixels | Explicit error |

Image generation dimensions are validated by the configured adapter. Image
understanding defaults to a 2048-pixel high-detail view, supports original
detail within the shared image safety bounds, and inherits the configured
vision output-token budget.
