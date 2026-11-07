# State ownership

Project-owned C and C++ code must not define mutable objects with static storage
(file-scope variables, function-local statics, or thread-local variables).
Read-only tables, strings and function tables may remain `const`. Qualify the
object itself: `const char *const names[]` is immutable; `const char *names[]`
still contains writable pointers. Third-party libraries retain their own internal
state and are outside this rule.

## Owners and borrowed contexts

Allocation follows the ownership boundary of each resource. Objects use ordinary
heap allocations and own their buffers and fields. There is no allocator context
passed through business APIs. `morph_buf`, `morph_array`, and `morph_strmap` manage
growth; an object's typed cleanup releases each owned field.

GCC/Clang scope cleanup is required. Include `util/cleanup.h` and declare local
owners with `MORPH_CLEANUP(function)`, initialized to `NULL` for pointers or `{0}`
for values. Unsupported compilers fail the build rather than silently omitting
cleanup. A pointer variable passes `T **` to its cleanup callback; an embedded
value passes `T *`. Use type-correct adapters, never a `void **` cast. Cleanup
supports empty and partially initialized valid objects and clears released fields.

Ownership contracts are explicit:

- Ordinary input pointers are borrowed. Document the borrow's end, including
  mutation and asynchronous boundaries; `const` alone does not enforce lifetime.
- Create/clone outputs go into empty owner slots. Success returns an independent
  owner; failure leaves the output empty and releases partial construction.
- A fallible `*_take` receives the owner's address: `T **` for a pointer owner,
  `T *` for a value owner. It consumes only on success, clearing the pointer or
  resetting the value after acceptance; failure retains the original owner. Complete all
  fallible preparation before committing the transfer.
- An infallible typed `*_steal(T **source)` returns the old pointer and clears the
  source. Use it when returning a local owner. Do not steal into a fallible
  receiver: pass the source address to the receiver's take API instead.
- Owning structures are not copied by ordinary assignment. A typed move operation
  commits the transfer and resets the source; clone creates independent storage.
  Container cleanup frees storage, while the containing typed owner cleans up
  owned elements first.

The API name describes the returned value or the state retained after the call:

| Form | Contract |
| --- | --- |
| `*_borrow` | Lends an object/view or retains a borrowed dependency. The header names its owner and invalidation boundary. Never release a borrowed result. |
| `*_copy`, `*_clone` | Creates independent owned data from borrowed input. The input remains with its caller. |
| `*_create`, `*_init` | Constructs a caller-owned object in an empty output slot or initialized value. |
| `*_add_borrow` | Adds an object to a registry/controller and lends its address; the containing owner destroys it. |
| `*_take` | Accepts an owner by address, clearing it only after success. Rejection preserves it. |
| `*_move`, `*_steal`, `*_detach` | Explicit transfer operations. Move replaces the destination, steal cannot fail, and a fallible detach preserves its source on failure. |
| `*_retain`, `*_acquire` / `*_release` | Holds and releases a shared reference. It extends lifetime without copying the underlying object. |
| `*_cleanup`, `*_destroy` | Releases an owned resource. Typed cleanup supports empty valid owners. |

Ordinary pointer inputs are borrowed for the call unless the header explicitly
specifies retention. Scalars, caller-buffer formatting, and immutable
process-lifetime classifiers such as `*_name` keep their ordinary names. Standard
`*_dup`/`*_alloc` names already denote allocations. The legacy UTF-8 adapters keep
their documented compatibility names. Third-party declarations and APIs are not
renamed; adapters state the original allocation/free pairing.

For example, this expression lends the buffer text only while creating an
independent parsed result:

```c
return tool_result_success_json_text_copy(result, morph_buf_cstr_borrow(&buf));
```

The buffer still belongs to its scope. An explicit transfer instead passes the
owner's address:

```c
cJSON *data MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
if (!data)
        return -ENOMEM;
return tool_result_success_take(result, &data);
```

The result clears `data` only after acceptance. On failure, scope cleanup still
releases it. Naming does not add a borrow checker to C: lifecycle checks, typed
owners, synchronization, and regression tests enforce the implementation.

Cross-thread queues commit enqueue and source clearing under the queue lock,
then publish/notify. Once accepted, the queue owns consumption, cancellation and
shutdown cleanup; a later execution failure does not return ownership to the
producer. Borrowed worker dependencies outlive every worker join.

Execution and transport operations may return owned diagnostics with a negative
status, for example an HTTP partial response or an extension error reply. Their
headers explicitly require cleanup on both success and failure. These operations
still reject an occupied destination before starting work. Constructor/copy
failure contracts remain separate from execution-status reporting.

Storage visitors lend a row only for the callback duration and finalize their
statement on every exit. Permission snapshots duplicate paths while holding the
authority lock and own those paths until typed cleanup. Memory and output
snapshots likewise own every returned field independently of the source. cJSON
values use `morph_json_cleanup`; printed cJSON text uses
`morph_json_text_cleanup`/`cJSON_free`. `morph_json_string_take(char **)` consumes
a malloc-owned string only after successful construction and preserves it on
failure. Third-party allocation/free pairs remain matched.

| State | Owner and lifetime |
| --- | --- |
| Logging | CLI `main` or `fcgi_bridge` owns a `morph_log`; runtime components and background jobs borrow its address. Stop writers before `log_shutdown`. A null logger disables logging and diagnostics. Error macros never write to stdout or stderr. |
| HTTP cancellation and tracing | Every call receives `http_control`. Models carry their default control; tool requests bind a local model copy to the request control. ReAct keeps provider errors and partial-output metadata in its request context instead of writing them back to shared model configuration. Borrowed cancellation tokens, callbacks and logger must remain alive until the call returns. |
| Tool execution | `tool_result.runtime` borrows the current `tool_runtime_context`. Clearing a result preserves the binding; cleanup removes it. Streaming, user questions and runtime queries use that binding. |
| Subagents | The runtime owns tasks and child contexts. Each task captures its submission depth; concurrent tasks do not increment shared recursion state. Cancelling a runtime cancels its own active children. |
| FastCGI | The server owns its bridge pool and authentication cache. Requests and turn jobs explicitly carry the bridge and auth pointers. Destroy services after joining request workers. Output roots are captured when a pool opens. |
| CLI output | Each `cli_application` embeds its TUI Application base and owns color/structured-output settings, capture destination, prompt lock, composer, completion cache and callback closures. |
| Readline adapters | Instance-owned libffi closures supply userdata to callbacks whose native ABI has none. Each run deeply clones the keymap and restores native callbacks before freeing closures. Readline itself still supports one active terminal editor per process. |
| Signals | The frontend owns one `morph_signal_listener`. Start it before application workers (or call `morph_signals_block` before creating them). It consumes blocked signals on its thread and wakes the owning UI. Stop it before freeing the frontend. Children reset their signal mask before exec; MCP spawn explicitly supplies an empty mask. |
| Sync serialization | A lock in the source's `.morph-sync/run.lock` serializes syncs for that source, including across processes. Different sources do not share a mutex. |

Dependencies are explicit constructor arguments or fields of an owning context.
Do not replace removed variables with singleton getters, TLS, environment-variable
storage, or mutable function-local statics. Environment variables are startup
configuration, not a place to store active request state.

Use native `return code;` and `rc = code;` so control flow and cleanup remain
visible. `morph_error_from_errno(errno)` captures a syscall's errno before
cleanup; zero maps to `-EIO`. `MORPH_TRACE_ERROR(logger, code)` is only a source
location adapter for the ordinary typed `morph_error_trace` function. It has no
return statement, assignment, temporary variables or build-dependent branches.
Both arguments evaluate once in every build, including on success. Logger
expressions must be free of business side effects; argument evaluation order is
ordinary C function-call order, not a sequencing guarantee.

Tracing returns the code unchanged, preserves errno and uses the explicitly
owned logger at `LOG_DEBUG`. `MORPH_DEBUG=1` enables these file diagnostics in
both Debug and Release frontends. Null loggers disable diagnostics; no macro or
tracing function writes to stdout/stderr. Worker contexts carry their logger
explicitly, so simultaneous runtimes cannot redirect each other's diagnostics.
Tests compile the same C probes with and without DEBUG and compare evaluation,
return values, assignment expressions, local-name safety and errno conversion.

Negative return values can also represent expected states, such as a busy
runtime or a rejected protocol probe. Callers decide whether these require
retry, fallback or a user-visible error. Frontends render actionable failures
through their normal output paths; CLI startup failures before UI initialization
use explicit stderr messages. Diagnostic macros do not render UI or decide
whether a returned error is fatal.

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
macro-generated definitions, TLS, local statics, and `const` C++ records with
`mutable` members (including nested records, arrays and base classes). Its self-tests also verify
that immutable data and third-party definitions are excluded. Runtime tests
exercise independent loggers, output capture, native callbacks, cancellation,
signal cleanup and bridge pools; PTY tests cover editing, window resizing,
interrupts, slash commands and shutdown.

Also check the fallback CLI branch with Readline explicitly disabled:

```sh
cmake -S . -B cmake-build-no-readline -DENABLE_READLINE=OFF \
  -DENABLE_GLOBAL_STATE_AUDIT=ON -DBUILD_FASTCGI=ON
cmake --build cmake-build-no-readline
ctest --test-dir cmake-build-no-readline --output-on-failure
```
