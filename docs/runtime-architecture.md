# Runtime architecture

`morph-runtime` is the process-level owner of the agent system. Frontends own
only an opaque `struct runtime *`; they never own the database, models, ReAct
context, tool registry, MCP registry, memory workers, credits, or session
state.

```text
 CLI / Android / future hosts
          |
          | runtime_options + runtime_request
          v
 +-------------------- public runtime facade --------------------+
 | lifecycle | sessions | turns | tasks | sync | MCP | snapshots |
 +-----------------------------+---------------------------------+
                               |
                    one private runtime_context
                               |
       +-----------+-----------+-----------+-----------+
       |           |           |           |           |
    config/db   models      tools/MCP   sessions    workers
       |           |           |           |           |
       +-----------+-----------+-----------+-----------+
                               |
                    runtime_close(), reverse order
```

## Ownership rules

1. `runtime_open()` constructs the complete dependency graph or returns an
   error after cleaning the partial graph.
2. `runtime_close()` stops workers, drains memory work, disconnects MCP, and
   destroys tools, models, database state, and locks in reverse order.
3. A frontend may keep copied DTOs and snapshots. Immutable config and string
   views may be borrowed for immediate inspection; mutable runtime-owned
   objects are not exposed through the public facade.
4. Every turn enters through `runtime_execute_turn()`. The runtime binds the
   requested session, memory, plan, dynamic-tool, usage, credit, and event
   context before execution and restores transient bindings afterward.
5. Session switching, scheduled execution, cancellation, and shutdown use the
   same runtime lock and lifecycle regardless of frontend.

## Request and result

`runtime_request` is the per-turn request context: session identity, model and
stored input, rendering/event callbacks, approval callbacks, memory policy, and
turn flags. `runtime_result` is the immediate execution result. Durable or
diagnostic state is read through copied runtime snapshots such as
`runtime_turn_status`, never through the internal ReAct object.

## Source layout

```text
runtime/
  lifecycle.c       construction, shutdown, and turn entry point
  execute.c         serialized request execution
  turn_scope.c      per-request binding and restoration
  context_owner.c   private dependency graph ownership
  bootstrap.c       model and built-in tool construction
  extensions.c      desktop extension discovery
  mcp_service.c     MCP DTO and operation facade
  registry_service.c platform tool and MCP registration facade
  session_service.c session ownership and selection facade
  task_service.c    scheduled-task and notification facade
  task_controller.c scheduled-task execution and worker control
  task_worker.c     background scheduled execution lifecycle
  tasks.h           public scheduled-task DTO/callback facade
  services.c        turn/session/tool/memory/credit snapshots and operations
  sync.c            sync worker facade
```

CLI and Android may render results differently and provide platform callbacks,
but neither frontend reimplements core initialization, execution, persistence,
credits, memory, tool registration, or shutdown.
