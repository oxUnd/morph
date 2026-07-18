# morph-fastcgi — optional agent-side patches

The MVP build of `morph-fastcgi` is **fully self-contained**: it links
against the existing morph static libraries through their public API only
and requires **no source changes** to the agent module.  When the optional
hooks below are absent at link time, the FastCGI process degrades
gracefully (POST /turns returns 202 but the SSE stream emits an `error`
event explaining the missing hook).

For a production deployment the three patches below are recommended;
each is small and additive.

---

## §1 — runtime: execute turns through the shared runtime

Already present in upstream.  No change needed.  The FastCGI bridge uses
`runtime_execute()` so persistence, ReAct execution, cancellation state,
and event bridging follow the same lifecycle as CLI and Android.

---

## §2 — react.h: optional `on_action` hook

Lets the FastCGI action pump inject decisions (`approve` / `reject` /
`cancel` / `prompt`) into a running ReAct loop.  Recommended for human-in-
the-loop / approval workflows; **not** required for read-only sessions.

```c
/* In src/agent/react.h */
struct react_action {
    const char *type;          /* "approve" | "reject" | ... */
    const char *payload_json;
};

typedef int (*react_action_drain_fn)(void *user, struct react_action *out,
                                     int timeout_sec);

int react_set_action_drain(struct react_context *ctx,
                           react_action_drain_fn fn, void *user);
```

Implementation: at each natural breakpoint (after a tool call, before a
guardrailed action), call `drain_fn(user, &act, 5)`.  On `cancel`, exit
the loop.  ~30 lines.

---

## §3 — react.h: per-session context factory

```c
/* In src/agent/react.h */
struct session_store;
struct react_context *
react_context_create_for_session(struct session_store *store,
                                 const char *session_id,
                                 const char *user_id);
```

Wires the agent's history/canvas/MCP routing to the session referenced by
the FastCGI HTTP layer.  The fastcgi worker picks this up via the
`__attribute__((weak))` declaration in `handlers/turns.c`.

When unimplemented the symbol resolves to NULL at link time on most
toolchains; the FastCGI handler checks for that and returns a clear error
event so a missing patch is observable rather than silent.
