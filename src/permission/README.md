# Permission layer

This library owns the three-axis policy, operation decisions, review routing and
grant scope. It has no model, configuration, runtime or frontend dependency.
`operation.h` is the shared vocabulary; `permission.h` is the policy/router API;
`context.h` integrates canonical paths, command programs and the existing grant
store. `agent/tool_context.h` is a compatibility include.

Runtime owns one context and provides reviewer callbacks. Tools describe effects;
they do not interpret preset names. Runtime exposes get/set and preset helpers.
Filesystem/process implementations enforce effective authority; a review result
does not by itself turn off the OS sandbox.

```text
application -> runtime policy API -> permission policy/context
tool -> concrete operation or opaque-call gate -> allow/review/human/deny
                                                -> runtime reviewer callbacks
allowed effect -> native path check or bounded process execution
```

To add a tool, use concrete path/command checks when effects can be described.
Opaque tools use the registry gate. `TOOL_FLAG_INTERNAL_APPROVAL` means the tool
performs concrete checks, never that it is exempt. Remote read-only metadata is
a declaration; remote service internals are not an OS isolation boundary.

To add a reviewer, implement the synchronous `permission_review_fn` adapter.
Return a negative error for unavailable/failed review, or a validated verdict,
reason and optional defer. The router owns model-to-human fallback, mandatory
human routing and denial counting. Automatic approval cannot create session or
persistent authority. Reviewers must honor cancellation and must not recursively
request another review from the same context.

Contexts and callback state remain alive until all borrowed child registries
and active calls finish. Configure callbacks before execution. The authority
mutex protects grants and policy; the review mutex serializes reviewer callbacks
and copies optional caller-owned `review_out` before releasing the decision.
Runtime changes policy only while idle and rejects active descendants/processes.
The lock order is authority then review. Application code uses runtime APIs
rather than mutating policy fields during execution.

See [the design](../../docs/permissions-design.md) and
[acceptance evidence](../../docs/permissions-acceptance.md).
