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
passed through business APIs. `nb_buf`, `nb_vec`, and opaque `nb_map` manage
growth; an object's typed cleanup releases each owned field.

GCC/Clang scope cleanup is required. Include `nb/cleanup.h` and declare local
owners with `NB_CLEANUP(function)`, initialized to `NULL` for pointers or `{0}`
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
| `*_move`, `*_steal`, `*_detach` | Explicit transfer operations. Application move operations replace the destination; native `nb_buf_move` swaps both handles. Steal cannot fail. Native detach returns allocation capacity needed for `nb_free`. |
| `*_retain`, `*_acquire` / `*_release` | Holds and releases a shared reference. It extends lifetime without copying the underlying object. |
| `*_cleanup`, `*_destroy` | Releases an owned resource. Typed cleanup supports empty valid owners. |

Ordinary pointer inputs are borrowed for the call unless the header explicitly
specifies retention. Scalars, caller-buffer formatting, and immutable
process-lifetime classifiers such as `*_name` keep their ordinary names. Standard
`*_dup`/`*_alloc` names already denote allocations. Third-party declarations and APIs are not
renamed; adapters state the original allocation/free pairing.

For example, this expression lends the buffer text only while creating an
independent parsed result:

```c
return tool_result_success_json_text_copy(result, nb_buf_cstr(&buf));
```

The buffer still belongs to its scope. An explicit transfer instead passes the
owner's address:

```c
/* json_owner_cleanup is this owner's typed cJSON_Delete callback. */
cJSON *data NB_CLEANUP(json_owner_cleanup) = cJSON_CreateObject();
if (!data)
        return NB_ERR_NOMEM;
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
values use a local typed callback that calls `cJSON_Delete`; printed cJSON text uses
`cJSON_free`. JSON construction uses cJSON directly, and its containing domain
owner commits ownership only after all fallible preparation succeeds. Third-party allocation/free pairs remain matched.

| State | Owner and lifetime |
| --- | --- |
| Logging | CLI `main` or `fcgi_bridge` owns an `nb_log *`; runtime components and background jobs borrow its address. Stop writers before `nb_log_free`. A null logger disables logging and diagnostics. Error macros never write to stdout or stderr. |
| HTTP cancellation and tracing | Every call receives `http_control`. Models carry their default control; tool requests bind a local model copy to the request control. ReAct keeps provider errors and partial-output metadata in its request context instead of writing them back to shared model configuration. Borrowed cancellation tokens, callbacks and logger must remain alive until the call returns. |
| Tool execution | `tool_result.runtime` borrows the current `tool_runtime_context`. Clearing a result preserves the binding; cleanup removes it. Streaming, user questions and runtime queries use that binding. |
| Subagents | The runtime owns tasks and child contexts. Each task captures its submission depth; concurrent tasks do not increment shared recursion state. Cancelling a runtime cancels its own active children. |
| FastCGI | The server owns its bridge pool and authentication cache. Requests and turn jobs explicitly carry the bridge and auth pointers. Destroy services after joining request workers. Output roots are captured when a pool opens. |
| CLI output | Each `cli_application` embeds its TUI Application base and owns color/structured-output settings, capture destination, prompt lock, composer, completion cache and callback closures. |
| Readline adapters | Instance-owned libffi closures supply userdata to callbacks whose native ABI has none. Each run deeply clones the keymap and restores native callbacks before freeing closures. Readline itself still supports one active terminal editor per process. |
| Signals | The frontend owns one `nb_signal_listener *`. Start it before application workers so they inherit the blocked signal mask. It consumes blocked signals on its thread and wakes the owning UI. Stop it before freeing the frontend. Children reset their signal mask before exec; Native process spawn restores the child signal mask before exec. |
| Sync serialization | A lock in the source's `.morph-sync/run.lock` serializes syncs for that source, including across processes. Different sources do not share a mutex. |

Dependencies are explicit constructor arguments or fields of an owning context.
Do not replace removed variables with singleton getters, TLS, environment-variable
storage, or mutable function-local statics. Environment variables are startup
configuration, not a place to store active request state.

Use explicit `return code;` and `rc = code;` so control flow and cleanup remain
visible. Capture an OS failure immediately with `nb_status_from_errno(errno, &err)`;
the portable code is independent of the operating-system errno, which remains in
`nb_err.os_errno`. Application failure descriptions are the immutable
`NB_ERR_USER` catalog in `src/errors.h`.

`nb_err_trace` accepts the owned logger, error code, optional description table
and source location. It returns the code unchanged, preserves errno and records
at `NB_LOG_DEBUG`. It does not hide return/assignment or render UI. Null loggers
disable diagnostics; simultaneous runtimes cannot redirect each other's logs.
The frontend owns the rotated file logger until every borrowing worker joins.
`MORPH_DEBUG=1` selects debug-level file diagnostics in Debug and Release builds.

Native containers use nbase allocation/free pairs. `nb_buf_move` swaps complete
handles: the source then owns the destination's former storage. `nb_buf_detach`
returns the backing allocation and its capacity; release it with `nb_free(ptr,
capacity)`. Do not pass that allocation to a malloc-owning application or third-
party API. When an existing domain boundary requires a malloc owner, create an
explicit checked copy and destroy the temporary native buffer. Binary copies
include the full length rather than using a C-string duplicate.

`nb_vec` owns its backing storage, not resources held by elements. Pushes produce
uninitialized slots; construct each slot before it becomes observable. Destroy
removed elements before `nb_vec_remove`, and never retain slot pointers across
reserve/push. `nb_dir_listing` owns a snapshot of native name buffers and remains
valid independently of later directory changes. Its destroy operation handles
empty/partial valid owners.

`sandbox_config` owns its canonical temporary-root snapshot and path arrays.
Prepare it before spawning or changing the child environment; repeated preparation
keeps the existing snapshot. Borrowed names remain valid until
`sandbox_config_cleanup`, so clearing HOME or changing TMPDIR cannot widen the
already prepared sandbox. Explicit config transfer clears the source owner.

TOML values and length-aware strings borrow their `nb_toml`; free the document
after every retained value/view is consumed or copied. Native thread, mutex and
condition-variable handles are heap owners; initialization can fail and shutdown
must wake waiters and join workers before releasing synchronization handles.
`nb_cancel_reset` requires exclusive ownership after readers stop.

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
