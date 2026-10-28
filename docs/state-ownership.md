# State ownership

Project-owned C and C++ code must not define mutable objects with static storage
(file-scope variables, function-local statics, or thread-local variables).
Read-only tables, strings and function tables may remain `const`. Qualify the
object itself: `const char *const names[]` is immutable; `const char *names[]`
still contains writable pointers. Third-party libraries retain their own internal
state and are outside this rule.

## Owners and borrowed contexts

| State | Owner and lifetime |
| --- | --- |
| Logging | CLI `main` or `fcgi_bridge` owns a `morph_log`; runtime components borrow its address. Stop writers before `log_shutdown`. A null logger disables ordinary logging. Debug error macros write stateless diagnostics to stderr. |
| HTTP cancellation and tracing | Every call receives `http_control`. Models carry their default control; tool requests bind a local model copy to the request control. Borrowed cancellation tokens, callbacks and logger must remain alive until the call returns. |
| Tool execution | `tool_result.runtime` borrows the current `tool_runtime_context`. Clearing a result preserves the binding; cleanup removes it. Streaming, user questions and runtime queries use that binding. |
| Subagents | The runtime owns tasks and child contexts. Each task captures its submission depth; concurrent tasks do not increment shared recursion state. Cancelling a runtime cancels its own active children. |
| FastCGI | The server owns its bridge pool and authentication cache. Requests and turn jobs explicitly carry the bridge and auth pointers. Destroy services after joining request workers. Output roots are captured when a pool opens. |
| CLI output | Each `cli_context` owns color/structured-output settings, capture destination, prompt lock, composer, completion cache and callback closures. |
| Readline adapters | Instance-owned libffi closures supply userdata to callbacks whose native ABI has none. Each run deeply clones the keymap and restores native callbacks before freeing closures. Readline itself still supports one active terminal editor per process. |
| Signals | The frontend owns one `morph_signal_listener`. Start it before application workers (or call `morph_signals_block` before creating them). It consumes blocked signals on its thread and wakes the owning UI. Stop it before freeing the frontend. Children reset their signal mask before exec; MCP spawn explicitly supplies an empty mask. |
| Sync serialization | A lock in the source's `.morph-sync/run.lock` serializes syncs for that source, including across processes. Different sources do not share a mutex. |

Dependencies are explicit constructor arguments or fields of an owning context.
Do not replace removed variables with singleton getters, TLS, environment-variable
storage, or mutable function-local statics. Environment variables are startup
configuration, not a place to store active request state.

libffi supplies the native callback adapters. libcurl 7.84 or newer is required
for concurrent global initialization; each request or persistent HTTP session
holds a balanced libcurl initialization reference.

## Audit and verification

The optional AST check requires Python 3, Clang and libclang. Set
`LIBCLANG_LIBRARY` to the libclang shared library if automatic discovery fails.

```sh
cmake -S . -B build -DENABLE_GLOBAL_STATE_AUDIT=ON -DBUILD_FASTCGI=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The option enables `compile_commands.json`, `global_state_audit`, and
`global_state_audit_selftest`. The audit covers project headers and translation
units under `src`, `tests`, `exts`, and `examples`, including standalone C examples
that are outside the main build. The locate extension is parsed as Objective-C
on macOS. It uses the configured target's active preprocessor branches; run the
check on each supported platform to check platform-specific code there.

The audit rejects mutable arrays of pointers, typedef-hidden mutable objects,
macro-generated definitions, TLS and local statics. Its self-tests also verify
that immutable data and third-party definitions are excluded. Runtime tests
exercise independent loggers, output capture, native callbacks, cancellation,
signal cleanup and bridge pools; PTY tests cover editing, window resizing,
interrupts, slash commands and shutdown.
