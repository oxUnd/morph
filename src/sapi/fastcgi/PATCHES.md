# morph-fastcgi runtime integration

The former weak-symbol integration described by this file is retired.
`morph-fastcgi` now owns the shared public runtime through
`runtime_open()` and `runtime_execute_turn()`.

This provides the same model, tool, Skill, extension, sub-agent, scheduled
task, dynamic-tool, and MCP bootstrap used by the CLI. Per-turn FastCGI
adapters bind authenticated user identity, memory visibility, events, HITL,
`ask_user`, operation approval, and the action queue without agent-side
patches.
