# Unified permission management

Status: implemented; acceptance evidence is in [permissions-acceptance.md](permissions-acceptance.md). Branch: `feat/unified-permissions`.
Baseline: `87a1c9b` (`refactor/fastcgi-optimization`).

## Ownership and public contract

`morph-permission` is the shared permission-management layer. It owns policy
evaluation, review routing and grant lifetime semantics. Runtime owns its
lifecycle and supplies human, model, persistence and event adapters. Tools
describe operations; filesystem and process executors enforce the resulting
access. No frontend or tool interprets application mode names.

The public policy has three independent dimensions:

| Dimension | Values | Meaning |
| --- | --- | --- |
| Access | `read_only`, `workspace`, `full` | Default business access boundary |
| Approval | `on_request`, `never` | Whether missing authority may be requested |
| Reviewer | `user`, `model` | Reviewer for ordinary requests |

Applications implement presets: Ask = workspace/on_request/user, Auto =
workspace/on_request/model, Full = full/never/user, Read Only =
read_only/on_request/user. Other combinations are valid and displayed as Custom.
The default is Ask. Named directory profiles supplement workspace access;
read-only does not inherit their write/delete access. Runtime storage, logs,
model communication and review communication are control-plane operations,
separate from model-requested business effects.

An operation can be allowed, forbidden, require review, or require a fresh human
decision. Explicit rejection and mandatory-human requirements take precedence
over cached grants. `never` rejects an operation that still requires review; it
does not silently approve it. An approval never implicitly disables the sandbox.
`config_edit` requires a human decision in restricted access; full access permits
editing the active configuration without review, including with approval `never`.
Human decisions retain the existing once/session/persistent choices. Automatic
decisions never create session or persistent authority.

## Required behavior and acceptance criteria

| ID | Requirement | Acceptance |
| --- | --- | --- |
| P01 | Independent permission layer and three-dimensional policy | Library unit tests cover every combination, invalid inputs and preset mapping; no dependency on models, config or UI. |
| P02 | One review-routing implementation | Ordinary, mandatory-human, denied, missing callback, cancellation and invalid reviewer results fail or route deterministically; all legacy entry points use it. |
| P03 | Unified filesystem authority | Canonical read/write/delete checks, directory batches, grant expiry/revocation and persistence rollback are preserved; read-only and full affect direct tools consistently. |
| P04 | Patch checks precede every mutation | Prepare all file operations before authorizing; denial or an invalid later target leaves all files unchanged. |
| P05 | Exec uses effective access | Cwd never implicitly widens write authority; read-only disallows project writes; network and host execution need explicit authority; sandbox initialization errors fail closed. |
| P06 | All model tool origins participate | Built-ins, media, MCP, executable extensions and dynamic tools pass operation or call review; unknown effects require review; unsupported native enforcement is explicitly refused in restricted access. |
| P07 | Descendants cannot gain authority | Subagents and scheduled/background turns inherit policy; changing policy cannot leave a wider running execution behind. |
| P08 | Hooks cannot bypass permission checks | Rewritten arguments are checked; deny wins; hook allow cannot override a mandatory human decision. |
| P09 | Isolated automatic reviewer | Independent model instance, visible user objective and exact action; structured allow/deny/defer with reason; no access to main-agent tools or hidden reasoning. |
| P10 | Review failures and rejection limits | Default review timeout 30 seconds; errors/defer fall back to human, otherwise fail; three consecutive model denials pause automatic progression. |
| P11 | Configuration has one effective policy | Persist access/approval/reviewer, retain path profiles, validate combinations and provide explicit migration handling for legacy settings; no two runtime policy engines. |
| P12 | Runtime policy management is transactional | Get/set through runtime facade; reject active turns/incompatible processes with EBUSY; save failure preserves active policy; switching clears transient grants and prevents stale writes in read-only. |
| P13 | Applications only compose and render | CLI input-area selection without switching/clearing the conversation, explicit policy arguments, Custom display, existing grant management and structured/FastCGI interactions work; no command bubbles or stale composer. |
| P14 | Build, regression and evidence | Shared source manifests and supported host builds include the library; ordinary and sanitizer suites, layout/global-state checks and acceptance traceability are recorded. |

## Integration

Move shared operation types out of agent ownership into the permission API,
retaining source-compatible include forwarding where useful. Reuse existing
path canonicalization and permission-grant persistence rather than implementing
another filesystem or database abstraction. A tool context references one
permission context; ReAct and copied child registries carry that same authority.
The ordinary tool-call gate handles opaque tools; tools that can describe paths
or process requirements perform concrete operation checks instead of a second
generic prompt. No internal-approval flag exempts an operation from the policy.

Exec derives its sandbox from effective access plus the grants for its owner.
Explicit host and network requests are typed operation fields. Full uses host
execution; read-only removes default writable project, metadata and tool-cache
roots while retaining isolated scratch space. Existing process sessions retain
their spawn-time boundaries, so incompatible live sessions prevent switching.
Trusted temporary roots come from `permission/trusted_paths`: fixed platform
roots plus a validated custom directory that cannot contain the original home.
Sandbox configuration owns one canonical root snapshot captured during parent
preparation, including any explicit scratch override. Environment filtering and
child setup never recollect that policy; Landlock and SBPL read the same names.
A standalone sandbox entry captures missing state before filtering its environment.
Model-supplied stdin enters the review gate again, including sandboxed sessions
that may retain earlier filesystem grants after those grants expire.
Remote MCP authority is enforced at the call boundary, not claimed to be an
OS sandbox around a remote service. Executable extensions and dynamic tools
receive bounded process policy; restricted native extensions must not claim
enforcement that cannot be provided.

The runtime review adapter normalizes tool and operation requests into the same
review API. The model reviewer uses an independent model client, no tools, and
strict JSON verdict validation. It may reuse text-model credentials, with an
optional independent approval-model configuration. Approval events identify the
decision source and retain existing frontend protocols through adapters.

Policy changes are user-control-plane operations. They are saved atomically in
the active configuration and apply to this runtime and future starts. Other
running processes are not remotely reconfigured. Explicit settings override
legacy input only at load time; incompatible mixed settings have a useful
diagnostic. Existing legacy-only files are translated with a migration notice,
never evaluated by a second runtime authorization path.

## Verification and completion

Run baseline tests before implementation and identify pre-existing failures.
Exercise policy combinations, real filesystem/process restrictions, whole-patch
denial, unknown origins, callback absence, child inheritance, revocation,
concurrent review/cancellation, storage failures and independent mock-model
review. CLI PTY acceptance includes 120, 80 and 60 columns, cancellation,
wrapped receipts, recall and structured interaction. Include FastCGI regression.

Use normal and ASan/UBSan builds (`ENABLE_ASAN` enables both sanitizers), full
CTest, `cli_application_layout_conventions`, and the global-state audit. Record
platform-only checks honestly: a skip or unavailable execution environment is
not a pass. `permissions-acceptance.md` maps every requirement to implementation,
test evidence and outcome. Completion requires every requirement to close;
neither line coverage nor deleting an unmet requirement substitutes for it.

## Reference and migration decisions

The review used OpenAI Codex source at
[`7b54fd245de62a2d6c74257ee387abfb55731477`](https://github.com/openai/codex/tree/7b54fd245de62a2d6c74257ee387abfb55731477/codex-rs).
Its separation of sandbox policy, approval policy and reviewer informed the
three dimensions. Morph keeps existing operation/path grants and a single review
router rather than introducing a general capability language or approval tickets.
Application preset names belong to runtime; the permission library never parses
them.

Legacy network/host/HITL flags cannot express the independent dimensions without
silently broadening access. Their explicit migration is therefore conservative:
workspace/on_request/user, with a warning. An explicit `[permissions]` table
wins and emits a mixed-settings diagnostic. Named directory profiles retain
their existing meaning. This translation does not run a parallel legacy engine.

Read grants also feed process sandbox construction. Runtime library/system
locations are control-plane reads; workspace/output, named profile roots and
matching grants define business reads. Read-only retains isolated scratch space
but does not inherit profile or built-in write/delete roots. Remote resources
and prompts declared read-only retain their read-only call semantics; remote
opaque tool effects require review at the call boundary. Executable extensions
intersect their manifest with runtime limits; native extension calls are refused
when restricted execution cannot enforce those limits.
