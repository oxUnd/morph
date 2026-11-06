# Unified permissions acceptance

Design: [permissions-design.md](permissions-design.md). Branch:
`feat/unified-permissions`, based on `refactor/fastcgi-optimization` at `87a1c9b`.
Verification host: macOS 27.0.1, arm64, Apple Clang, CMake 4.4.4.
P01–P14 are implemented and accepted for the desktop core, CLI and FastCGI
on this host. Conditional external/platform checks are recorded separately.

## Requirement traceability

The tests below are executable assertions, including real child-process sandbox
checks. Model-review tests use a local HTTP mock; they inspect the actual request
and response path without depending on a live provider.

| ID | Implementation | Acceptance evidence |
| --- | --- | --- |
| P01 | `src/permission/{permission,operation,context}.h`, independent CMake target and source manifest; presets in runtime | `Permission.EveryPolicyCombinationHasDeterministicRequirements` covers all 12 combinations and precedence; preset/custom/invalid-input round trips. The library links only util, database and threads. |
| P02 | `permission_authorize()`, compatibility operation APIs and runtime callback adapters | Ordinary/model/mandatory-human routing, missing callbacks, `never`, invalid verdicts, cancellation, concurrent serialized reviews and independent decision reasons in `tests/test_permission.cpp`. |
| P03 | Canonical path and directory grants moved intact into the shared layer; sandbox reads/writes/deletes collect the same grants | `ToolContextTest`, `DirectoryPermissionTest`, read-only profile isolation, read-grant expiry and policy-switch clearing. `ExecToolTest.ReadsOutsideWorkspaceRequireScopedAuthority` and `DirectoryPermissionsFollowPathsAcrossToolsAndPrograms` verify denial, explicit upgrade, symlinks, sibling isolation and cross-tool use. Persistence rollback/revocation regressions pass. |
| P04 | `patch_apply_authorized()` validates all targets, authorizes the complete target list, then applies hunks | `ReadOnlyPatchDenialAndLaterInvalidTargetLeaveFilesUntouched` checks missing reviewer, invalid later target before any approval, and denial of the second file after the first approval. Neither file is created on failure. Existing patch move/delete/parser tests remain included. |
| P05 | Shared sandbox builder; exec derives boundaries from the original workspace, typed host/network requests and isolated scratch space | Read-only exec cannot write its project even under `/tmp`; cwd cannot widen grants; host/network requests cannot use the workspace shortcut; existing sandbox initialization, worktree, environment, cancellation and process tests pass. Model stdin is checked again for host/network and sandboxed sessions, including after turn grants expire. |
| P06 | Registry call gate for opaque/media/MCP/dynamic tools; concrete operation checks for internally approving tools; bounded extension runner | Opaque-tool denial prevents execution, copied registries retain authority, dynamic-tool allowlist/symlink regressions pass. Real executable extension cannot write under read-only despite its manifest requesting filesystem access. Native restricted calls return ENOTSUP. MCP call-boundary and FastCGI suites remain included. |
| P07 | Child registries borrow the same authority; runtime setter checks active subagents and process sessions | `OpaqueToolsAndChildRegistriesShareAuthority`, existing `SubAgentTest` and scheduled-task regressions, and `LiveBackgroundProcessBlocksPolicyChangeUntilStopped`. A live session blocks changing to read-only until stopped. |
| P08 | Hooks receive the effective sandbox; rewritten calls enter the shared gate; mandatory-human requirements precede caches | `MockLlmTest.HookRewriteReachesToolAndPostHook` verifies the reviewer sees rewritten arguments. Hook deny/ask/malformed rewrite regressions remain included. `ToolCallGrantsRespectScopeMandatoryHumanAndPolicySwitches` verifies that cached permission cannot skip mandatory review or explicit denial. |
| P09 | Independent approval-model client, optional `[model.approval]`, objective/exact action evidence, no tools, strict verdict/reason parser | `PermissionModelTest.IndependentModelReviewsExactActionWithoutTools` checks the actual mock HTTP payload and that the main model is unchanged. JSON tests reject partial, extra, unknown and trailing output. Usage is labeled `approval`; review events carry source/reason/verdict. |
| P10 | 30-second timeout, zero retries, model error/defer to human, cancellation propagation, three-denial circuit | Router timeout/defer/error/cancel tests, invalid mock-model response falling back exactly once, and `ThreeModelDenialsRequireHumanAndCannotCreatePersistentAuthority`. Automatic decisions cannot grant session/Always authority. |
| P11 | Validated `[permissions]`; conservative legacy translation with diagnostics; named profiles retained | Config/schema tests, `LegacyPermissionFlagsMigrateWithoutKeepingAnotherAuthority`, and `BootstrapLegacyMediaSwitchCannotAutoApproveMutations`. Explicit three-axis configuration wins over old host/network/HITL switches. Runtime ReAct skips its standalone legacy HITL preflight when shared authority is present. |
| P12 | Facade get/set; execution/child checks; locked TOML merge and atomic save before applying policy | Persistence preserves comments/unrelated settings and survives reload. Busy/save-failure tests preserve active policy. Switching clears transient path, command and tool-call grants; read-only uses separate persistent write/delete/tool-call grant kinds. |
| P13 | CLI picker/preset/custom commands; facade presets; existing grant-management commands and frontend adapters | `cli_permissions_pty_integration`: inline input-area selection keeps prior conversation visible and emits no alternate-screen, clear-screen or cursor-home sequences. Selection, resize, paste protection, Escape/Ctrl-C cancellation, recall, Custom, saved settings, status, one restored composer and structured JSON command results are covered at 120/80/60 columns. Existing slash recall/spacing, JSON interaction and FastCGI tests remain included. |
| P14 | Desktop source manifest, CLI layout checks, global-state audit, release and ASan/UBSan builds | Final suite totals and commands are recorded below. Builds include FastCGI and TUI samples, with no compiler warnings. |

## Reproduction

The baseline completed before source changes: 2019 registered tests, no failures.
Its optional PTY checks initially skipped because Python dependencies were absent.
For acceptance, `pexpect` and `pyte` were installed in an isolated QA environment;
the permission test also uses Python 3.11+ `tomllib`.

```sh
cmake -S . -B build -DBUILD_FASTCGI=ON -DBUILD_TUI_SAMPLES=ON \
  -DENABLE_GLOBAL_STATE_AUDIT=ON -DPython3_EXECUTABLE="$QA_PYTHON"
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON \
  -DBUILD_FASTCGI=ON -DBUILD_TUI_SAMPLES=ON \
  -DENABLE_GLOBAL_STATE_AUDIT=ON -DPython3_EXECUTABLE="$QA_PYTHON"
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure -j3
```

`QA_PYTHON` is the absolute path to the QA environment's Python executable.
`ENABLE_ASAN` enables both AddressSanitizer and UndefinedBehaviorSanitizer.

| Build/check | Result |
| --- | --- |
| Release full CTest | 2047 registered; 2042 executed passed, 5 conditional skips; zero failures, 257.37 seconds. |
| Debug ASan/UBSan full CTest | 2047 registered; 2042 executed passed, 5 conditional skips; zero failures, 585.50 seconds. No sanitizer reports. |
| Final deltas | Release: 214 selected checks, zero failures, 2 conditional skips (34.31 seconds). ASan/UBSan: 120 selected checks, zero failures, 2 conditional skips (34.73 seconds), plus all 102 configuration checks passed (5.02 seconds). These cover the final reviewer, callback-string, stdin and configuration changes. |
| Allocator/concurrent spawn stress | Release and ASan/UBSan both passed separately with `MALLOC_PERTURB_=165`; the full suites' conditional skip was therefore followed by executed checks. |
| CLI application layout + global-state audit | Included and passed in both full suites. Global-state audit: Release 195.62 seconds; sanitizer build 287.95 seconds. |
| Compiler diagnostics + whitespace | Zero warnings in both build logs; `git diff --check` clean. |

The missing-model adapter case returns a configuration error for human fallback;
it is distinct from cancellation. Final adapter checks also require complete JSON
evidence allocation and terminate callback reasons. Process input checks cover
both host and sandboxed sessions after a turn boundary. These final targeted
regressions were rebuilt and executed after the full runs;
no whole-suite result is used as evidence for a later untested change.

Session logs are available at:

- `/tmp/morph-permission-normal-acceptance.log`
- `/tmp/morph-permission-sanitizer-acceptance.log`
- `/tmp/morph-permission-normal-final-delta.log`
- `/tmp/morph-permission-sanitizer-final-delta.log`
- `/tmp/morph-permission-sanitizer-config-final.log`
- `/tmp/morph-permission-allocator.log` and `allocator-asan.log` with the same prefix

The committed tests and commands above are the reproducible evidence after
these temporary logs expire.

## Scope and conditional checks

Live MCP tests require a configured external server. Live Mac automation requires
an unlocked desktop and existing privacy consent; no consent was modified during
acceptance. The invalid-byte filename test depends on filesystem support. The
allocator stress test is conditional on its requested allocator environment.
Any such skips in the final results are listed explicitly below, not counted as
executed checks.

| Conditional test in each full run | Outcome |
| --- | --- |
| `RuntimeMcpBackgroundTest.LiveServerPublishesToolsAndBecomesReady` | Skipped: live server not configured. |
| `McpLiveServerTest.DiscoveryKeepsUsableCapabilities` | Skipped: live server not configured. |
| `FileListTest.RejectsInvalidUtf8NamesWithoutChangingTheirIdentity` | Skipped: host filesystem cannot preserve the fixture's invalid-byte filename. |
| `ExecToolTest.LiveHostAutomationReadsFrontmostApplication` | Skipped: unlocked Mac/privacy-consent opt-in was not enabled. |
| `ProcessTest.ConcurrentSpawnSurvivesSessionArrayGrowth` | Skipped in full runs; subsequently executed and passed under the requested environment in both builds. |

Linux execution and Valgrind were unavailable on this host: Docker's daemon was
not running and Valgrind was not installed. Linux backend behavior is therefore
not claimed as a runtime test result. Mobile frontends pin a separate core
revision; this branch does not upgrade or claim to validate those release pins.
Remote MCP service internals are outside the local OS sandbox; authority is
enforced at the remote call boundary. Other running morph processes retain their
own loaded policy; saved defaults affect this runtime and subsequent starts.

The legacy media regression was updated to require review under the new policy.
The reasoning PTY test now waits for the actual composer-ready signal instead
of a fixed startup delay. Intermediate runs made during iteration are diagnostic
only; full regression runs use fixed binaries, with the last reviewer-only delta
rebuilt and verified separately in both configurations.

## Input-area picker follow-up

`/permissions` now presents a transient selector at the suspended composer's
position on the main terminal buffer. It uses the existing Application controller
stack with key input and bracketed paste, without alternate-screen mode, screen
clearing or cursor-home rendering. The previous conversation stays visible;
dismissal erases the selector's own rows before restoring one composer. The
active preset is marked as current; Custom retains the three-axis status.

Verification on the same host:

- Release: all 95 related presentation, controller lifecycle, permission,
  slash-command, cancellation and approval checks passed (34.78 seconds).
- ASan/UBSan: the same 95 checks passed (58.22 seconds), without sanitizer reports.
- Final Release picker/layout checks: both passed (27.88 seconds).
- PTY checks cover 120/80/60 columns, live width changes, visible prior chat,
  current/Custom display, paste protection, Escape/Ctrl-C cancellation, plain
  command spacing, recall, persistence, structured output and composer cleanup.
  Raw output is checked for absence of screen switches, whole-screen clearing
  and cursor-home sequences.
- Existing local approval-card changes were preserved; their regression
  participated in this verification. No permission policy or grant logic changed.

Logs: `/tmp/morph-permissions-inline-regression.log`,
`/tmp/morph-permissions-inline-asan-test.log` and
`/tmp/morph-permissions-inline-final.log`. Builds reported no compiler warnings.


## Permission status footer follow-up

The composer footer now includes the applied policy (`perm: ask`, `auto`, `full`
or `read_only`). At standard widths, a Custom policy also shows its
access/approval/reviewer values. The footer reads the runtime facade on each
render and retains its last successful policy snapshot while the runtime is
busy. Policy changes therefore appear immediately, independently of the usage
polling interval, and active execution does not temporarily hide the mode.

Permission and usage fields take precedence over optional branding and paths
when the terminal is narrow. The existing footer row count and input cursor
restoration remain unchanged.

Verification:

- Release: all 132 related CLI presentation, terminal, UI lifecycle, MCP, approval,
  cancellation, slash-command and permission checks passed (58.27 seconds).
- ASan/UBSan: the same 132 checks passed (62.04 seconds), without sanitizer reports.
- Permission PTY coverage checks all four presets, Custom dimensions, immediate
  switching, cancellation, retained state during an active model request and
  preservation of the editable draft. It covers 120/80/60 columns and compact
  40/20-column layouts.
- The general CLI PTY regression confirms that narrow status bars retain context
  and credit information, and unchanged idle footers do not repaint the draft.
- Both builds reported no compiler warnings; whitespace checks passed.

Logs: `/tmp/morph-permission-footer-regression.log` and
`/tmp/morph-permission-footer-asan-test.log`. Existing local approval-card and
other unrelated changes were preserved.
