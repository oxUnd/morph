# nbase vendored snapshot

- Upstream: https://codeberg.org/oxund/nothing
- Library/API: nbase / `nb_`
- Pinned commit: `cd7efa88aa5c3bbd3825d45d3527e45d7566b716`
- Version: `0.1.0`
- Imported: 2026-11-13 as a full `git archive` snapshot of the pinned
  commit. The snapshot contains only tracked files and no nested `.git`.

This directory is a source dependency, not a subtree. Keep local changes
minimal and record every change below. Prefer sending a change upstream and
re-importing the new commit; a local patch is only acceptable while the
migration is blocked on it.

## Local patches

1. `src/platform/posix/path.c`: `realpath()` now allocates a
   system-sized buffer (`realpath(path, NULL)`) instead of writing into a
   fixed 256-byte allocation. The old loop grew the buffer only after
   `realpath()` had already written past it.
   Regression: `tests/test_fs_extra.c` (`test_canonical_long_path`).

2. `src/platform/posix/file.c`: regular-file copies stream through the
   existing atomic temporary-file publication path in 64 KiB chunks instead
   of loading the whole source with `nb_file_read_all`. The atomic writer is
   shared by `nb_file_write_all_atomic` and `nb_file_copy`, so publication,
   permissions, fsync and cleanup keep one implementation.
   Regression: `tests/test_fs_extra.c` (`test_copy_large_file`).

3. `include/nb/thread.h`: atomic storage types are plain integers driven by
   `__atomic` builtins (MSVC uses `_Interlocked*`). This keeps the public
   headers valid under strict C++17 without the C11 `_Atomic` extension while
   preserving one C/C++ ABI.

4. `include/nb/str.h`: `NB_STR_LIT` and `NB_STR_NULL` use C++ braced
   temporaries under C++ so strict C++17 no longer rejects the C99 compound
   literals.

5. `include/nb/queue.h` (new): intrusive doubly-linked queue with the
   operations morph's `util/queue.h` relied on. Header-only; exercises in
   `tests/test_containers.c` (`test_queue`).

6. `include/nb/log.h`, `src/log_file.c` (new): size-rotated file sink
   (`nb_log_file_open` / `nb_log_file_sink` / `nb_log_file_close`) so callers
   no longer need their own `FILE *` rotation code. Exercise:
   `tests/test_log_file.c`.

7. `include/nb/file.h`, `src/platform/{posix,win32}/file.c`: atomic writes
   gain `new_file_mode` (creation-only permission bits, falling back to
   `mode`/0644) and an optional `published` out-flag that becomes true as soon
   as the final name is published, even if the parent-directory sync later
   fails. Exercises: `tests/test_fs_extra.c` (`test_atomic_options`).

8. `include/nb/file.h`, `src/path.c`: `nb_path_canonical_missing` resolves the
   longest existing ancestor with realpath semantics and appends the
   not-yet-created tail. The tail rejects `..` with `NB_ERR_INVAL` instead of
   pretending lexical cleaning is filesystem resolution. Exercise:
   `tests/test_fs_extra.c` (`test_canonical`).

9. `include/nb/toml.h`, `src/toml.c`: immutable AST values expose exact source
   token spans and table-header/inline-table metadata, native typed views and
   structural equality. Line positions use a line index rather than scanning
   the entire source for each node. Config editing preserves source comments,
   quoted keys and multiline values without accessing parser internals.
   Regression: `test_toml_source_and_equivalence`.

10. `include/nb/thread.h`, platform thread implementations: native status
    results distinguish condition signals, timeout and errors; realtime and
    monotonic deadlines are explicit. Thread options preserve stack size and
    detached creation. Thread identities and successful join/detach owner
    operations let callers use opaque handles throughout their lifecycle.
    Regression: `test_thread_owners` and condition-wait tests.

11. `include/nb/signal.h`, platform signal implementations: opaque listeners
    take an explicit signal set and a callback. POSIX blocks signals before
    worker creation, receives them using sigwait, restores the original mask
    and dispositions on owner-thread stop, and keeps default-ignored signals
    pending on Darwin. Windows returns NB_ERR_UNSUPPORTED. Native atomic
    exchange supports optional cancellation flags without a separate facade.
    Regression: `tests/test_signal.c`, including resize/USR1 delivery, owner
    thread checks and mask/handler restoration.

12. Unicode properties: `src/unicode.c` and `vendor/utf8proc/` add the pinned
   utf8proc 2.12.0 / Unicode 18 backend for width, category and canonical full
   case folding. Generic bounded text helpers are nbase APIs; terminal filtering
   and tokenizer weight policies stay in morph. Tests: `tests/test_utf8.c`.

13. Native path/text primitives: `nb_path_concat` retains a separator boundary
    without absolute-right replacement; `nb_path_expand` expands the home prefix;
    `nb_path_is_within` now explicitly selects existing-only or safe missing-tail
    resolution; `nb_path_relative_under` combines canonical containment with a
    relative result. `nb_str_copy_to`, ASCII token-byte validation and
    `nb_random_hex` cover bounded/native callers. Tests: `test_native_text_and_scan`.

14. Directory snapshots: `nb_dir_scan` returns an independent `nb_dir_listing`
    of native buffer names, with explicit filtering, hidden/link and sorting
    flags. Occupied output rejection and failure preservation are tested.

15. Copy/removal: `nb_file_copy_options.nofollow_source` rejects source links,
    including during the final source open. Both POSIX and Windows copies stream
    in 64 KiB chunks through the shared atomic publication writer. Windows opens
    with `FILE_FLAG_OPEN_REPARSE_POINT` and checks handle attributes when links
    are forbidden. `new_file_mode` sets creation-only regular-file permissions
    when source-mode preservation is disabled; existing target modes remain
    unchanged. Regression covers private/read-only creation, existing mode and
    source-mode precedence, including Windows synthetic mode expectations.
    `nb_file_remove_all` takes an explicit maximum depth (zero
    means unlimited); the skill importer passes its domain bound of 128.
    Tests: `test_native_text_and_scan` and `test_copy_symlinks`.

16. `include/nb/time.h`, `src/time.c`: saturating monotonic deadlines and
    nonnegative remaining durations centralize deadline arithmetic. Condition
    waiters use native relative intervals or monotonic absolute deadlines;
    realtime adjustment remains solely in the platform condition primitive.
    Regression: `test_monotonic_deadlines`.

17. `include/nb/err.h`, `src/err.c`: `nb_status` spans the full signed-int
    ABI range, so explicit domain/user codes remain valid in C and C++ under
    UBSan. Portable errno mapping preserves the captured OS diagnostic and
    distinguishes `EACCES` (`NB_ERR_PERMISSION`) from `EPERM`
    (`NB_ERR_NOT_PERMITTED`); errno zero on an error path maps to I/O failure.
    Static domain descriptions and explicit-logger traces need no registry.
    Regression: `tests/test_core.c` and strict C++17/UBSan integration.

18. `include/nb/process.h`, platform process implementations: stdio streams
    support pipe/inherit/null/borrowed-fd modes; PATH lookup uses the child
    environment. POSIX supports PTY/new-session/process-group setup, resets
    inherited signal masks/dispositions and runs an explicit post-fork prepare
    hook. Windows uses native handles and Job Objects. Stream descriptors can
    transfer to an external reactor. `nb_process_exchange` concurrently sends
    input, drains both outputs beyond retained limits, closes stdin at EOF,
    handles cancellation/deadlines, terminates the group on failure and reaps.
    The Windows bounded stdin writer cancels its worker I/O at deadline/cancel
    and joins before releasing borrowed input. Tests: `tests/test_process.c`,
    including bidirectional large input, truncation, timeout and stream modes;
    native POSIX and Wine runs.

19. `include/nb/file.h`, `src/file.c` and platform I/O implementations:
    `nb_pipe_open` creates
    close-on-exec binary pipes, and `nb_fd_write_until` writes with cancellation
    and a monotonic deadline while preserving borrowed fd flags. POSIX and
    Windows provide platform-native implementations; process stdin reuses the
    same bounded-write contract instead of application-local pipe loops.
    Regression: `tests/test_process.c`.

20. Portable owner cleanup: library implementation helpers explicitly clean
    local owners rather than relying on `NB_CLEANUP`, whose attribute is
    unavailable in MSVC. Typed cleanup helpers remain public conveniences.
    Morph itself requires automatic scope cleanup and checks GCC/Clang cleanup
    capability during its CMake configure step; this is an application build
    requirement, not a hidden nbase implementation dependency.

21. Bounded complete reads: `nb_file_read_all` takes `max_bytes` (zero means
    unlimited). Both platform readers reject an oversized stat before reserve,
    then enforce the limit on every chunk and probe at most one extra byte.
    Failures preserve the output owner. `tests/test_regressions.c` covers exact
    limits and grows the file from the allocator callback after stat, ensuring
    the read-time bound still rejects it without replacing the output.

22. Temporary owners: `nb_file_temp_in` can transfer a non-inheritable,
    read/write binary descriptor through an initially empty `fd_out`. Windows
    permits independent readers while that descriptor remains owned. Failure
    preserves both output owners and captures the OS status before cleanup.
    `nb_file_temp_dir` accepts an explicit parent and filename prefix so
    same-filesystem staging keeps the application's hidden names. Tests cover
    binary descriptor I/O, simultaneous path reads, private directory mode,
    occupied outputs, missing parents and invalid prefix components.

23. Recursive-copy policy: `nb_file_copy_options` includes a typed filter,
    maximum depth, directory mode and creation-only regular-file mode. The
    callback receives depth, stat data and the captured symlink target; the
    same target is used for copying. Skipping a directory skips its subtree.
    Regular sources are opened without following a replacement symlink.
    New destination trees are removed on failure; merging an existing tree
    retains the documented incremental-copy behavior. Morph now supplies
    package metadata and allowed-link policies as callbacks. Regressions cover
    filtered subtrees, bounds, failure cleanup, modes and source-link changes.

## License

The pinned upstream commit does not contain a license file for the library
itself. `tests/toml-test/LICENSE` covers only the TOML test corpus and does
not extend to nbase. Distribution as a morph dependency requires an explicit
license from upstream; see the migration assessment,
[section 9](https://github.com/oxUnd/morph/blob/feat/nothing-migration/docs/nothing-migration-assessment.zh-CN.md#9-当前待解决事项).
