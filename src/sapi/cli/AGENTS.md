# CLI Agent Guide

Apply these rules when changing `src/sapi/cli/` or its related tests. Follow the
repository's [root guide](../../../AGENTS.md) for build, ownership and C rules.
Paths such as `components/` and `presentation/` below are relative to this directory.

## Application Layout

- CLI application module ownership and naming follow
  [README.md](README.md) and
  [the shared application layout](../../../docs/tui-application-layout.md). The complete
  CLI is the application; its root `application.c/.h` owns the embedded TUI base
  and all execution modes. Keep page controllers in `pages/`, reusable app
  components in `components/`, models in `models/`, use cases in `services/`,
  external integrations in `adapters/`, command actions in `commands/` and
  formatting in `presentation/`. Each module in `components/` builds a reusable
  NT view or a composition of NT controls. Bind view properties to their owning
  state and handle interaction through NT view events. Shared text formatters,
  stream writers and presentation policies belong in `presentation/`.
  Each presented page embeds `struct nt_view_controller` first. Keep local state
  on the page and use `cli_application_from_controller()` to access its host.
  Modal pages inherit `cli_application_panel_class` for command completion.
  Every implementation includes its sibling
  header first and imports other interfaces directly. `application_internal.h`
  owns no function declarations or domain SDK include collection. Run
  `cli_application_layout_conventions` alongside CLI/TUI behavior regressions
  after moving modules, adding interfaces or changing private names.
- TUI View options use four-edge `nt_view_insets` for margin and padding.
  Measure returns the outer size including margin; arrange accepts an outer slot
  and applies margin once. Size limits include padding and exclude margin.
  Custom layouts use `nt_view_content_rect` and pass allocated outer slots
  directly to child arrange calls. Sibling margins add without collapsing.
  See [the NT layout guide](https://codeberg.org/oxund/nothing-tui/src/tag/v0.1.0/docs/guide/layout.md) for geometry and extension contracts.

## Shortcuts

- Every application shortcut hint uses the TUI theme's accent color plus bold
  for key names; descriptions use the muted theme role. Apply this consistently
  to startup help, chat activity, panels, popovers, sidebars, setup and help.
  Use standard TUI controls and native text spans through the shared
  `components/shortcut_hint` component and `presentation/shortcuts` policy.
  Never concatenate ANSI escapes or hardcode colors to highlight shortcuts in
  page, component or formatter code. Preserve plain/JSON and no-color output;
  do not restyle user/model text or tool payloads as application shortcut hints.
- Shortcut labels use title-case modifiers (`Ctrl`, `Shift`, `Alt`, `Cmd`,
  `Meta`), `+` separators, and uppercase letter keys: `Ctrl+J`,
  `Ctrl+Cmd+V`, `Shift+Tab`. Named keys keep their normal display form,
  such as `Enter`, `Esc`, and `Tab`. Apply this to UI hints and docs.
- Every new or changed application shortcut MUST register matching TUI metadata.
  Use `nt_view_bind_commands()` for command bindings; raw key handlers use
  `nt_view_set_keymap()` or the view class's `keymap` query. Register physical
  keys, modifiers, descriptions and meaningful help groups on the owning view.
  Keep mode-dependent bindings consistent with the current state and actual
  input precedence, including capture handlers that override native editing.
  Verify the shortcut appears correctly in the native `Ctrl+K` popup.

## Status Symbols

- Terminal status glyphs and styles are defined in `src/sapi/cli/presentation/style.h`. All CLI
  commands, pages, tool feeds, background jobs, subagents, plans, notifications,
  diagnostics and setup flows reuse those definitions.
  Do not hardcode alternative status glyphs or small
  `•` / `◦` step bullets in application sources.
- Use `◯` (dim gray) for queued/pending, `◉` (cyan) for running,
  `●` (green) for successful completion, `✖` (bold red) for errors/failures,
  and `?` (bold yellow) for questions/approval awaiting user input. Running
  activity may animate without changing marker width or layout. The `Working`
  activity header starts with `◉`, alternating normal/dim cyan every 500 ms.
  The bottom dock owns Back to bottom, activity, input and the single-line
  status bar as four sibling regions. Only the first activity row has a status
  marker. Subordinate agent/job/task/queue summaries and details use independent
  `CLI_ACTIVITY_CHILD_PREFIX` elbows aligned with the first letter of the header,
  without additional status markers, connected spines or nested branches.
  Keep activity outside the input-only composer. Blink only transient live
  indicators; keep completed history static and no-color/plain output steady.
  Update dots in place without erasing the composer or replaying tool output.
- Align every top-level status marker in the same leftmost column, including
  `Working`, background summaries, tools, plans and completion receipts. Represent
  parent/child relationships with explicit `├` / `└` branches and `│` continuation
  lines where needed; indentation alone must never imply a relationship.
  Activity details (queued messages and their overflow count, live
  subagents, and waited background commands) use flat gray `└` rows. Align the
  elbow with the `W` in `Working`, two columns after the top-level marker.
- Use `–` (dim gray) for cancelled/interrupted/skipped work and `⚠` (yellow)
  for warnings. `◇` is reserved for neutral system information, not outcomes.
  A plain message/section bullet may use `●` without claiming success.
- Timeouts are failures (`✖`, with `TIMEOUT` text), never successful completion.
  Historical calls without a recorded result are pending/unknown (`◯`), never
  successful. Disabled features are inactive (`◯`), not failures.
- Shape and explicit status text must remain meaningful without color. Preserve
  `--no-color`, plain/JSON event contracts, terminal sanitization and display-width
  clipping. UI radio/checkbox controls, Markdown bullets, password masks and
  user/model content are separate from application status markers.
- Error and question marker styles include bold in the shared definitions.
  Do not substitute mathematical circled crosses, combining-circle questions,
  emoji, checkmarks or differently sized circle variants for the status set.
- Run `cli_application_layout_conventions` (including the status-literal audit)
  and relevant presentation/PTY regressions after changing status presentation.

## Chat and Slash Command Presentation

- Submitted chat boxes own one normal-background row above and below their
  colored padding. Reasoning, answers, and tools share that outer separator;
  streaming previews and committed output keep the same position. Keep local
  slash-command spacing governed by the separate rules below.
- Treat slash commands as local UI/actions, separate from chat user messages.
  Do not commit submitted slash commands as `› /command` chat bubbles or retain
  their editable background in scrollback. Keep each submitted command visible
  as a separate purple command line on the normal background, followed by its
  results. Include arguments and aliases as submitted, with terminal controls
  sanitized. Readline recall history is separate and may retain the submitted
  command for Up/Down recall.
- Navigation commands (`/hooks`, `/resume`, `/history`, including aliases) open
  their panel after committing the plain command line, without a command bubble.
  Closing/cancelling a panel retains that command line and restores one editable
  composer and its footer; do not print a close receipt.
- Queries print their command/result heading and results in the normal output
  area. State changes print a concise success receipt. Neither uses user-message
  padding or the composer's background. Failures retain an explicit error.
- Preserve the composer's vertical position while streaming output or refreshing
  live tools/background jobs. A shrinking preview, wait transition or batch of
  completion events must not pull a bottom-positioned composer upward. Reclaim
  transient rows before history writes and let committed output consume the
  reservation; keep any remaining space on the normal background above the live
  feed. Check every published PTY frame with an unsent draft, including wrapped
  input and 120/80/60-column layouts.
- Completed turns (including their duration row) and completed command groups
  each own one trailing blank row on the normal background. Submitted command
  lines add no leading blank row. Editable composer padding is transient and
  must not supply history spacing. Check completed turn -> command -> receipt
  -> next command at 120, 80 and 60 columns, including wrapped receipts.
- Add one blank row between a submitted command and its first visible result.
  Defer that row until a result exists: a command without output (including a
  cancelled navigation panel) leaves only one trailing blank row. Normalize
  result headings' leading newlines so they do not add a second result gap.
- Unrecognized commands print an error and clear the editable composer, both
  idle and during an active turn. Keep them in Readline history for Up/Down
  recall. Do not forward slash commands as model steering text: while a turn is
  active, known commands that cannot run yet report that they must wait and
  keep the input for retry; explicitly supported commands such as `/recap`
  retain their existing behavior.
- Keep JSON command events and non-interactive output contracts unchanged.
  Verify panel cancellation, queries, receipts, unknown-command input clearing,
  command recall, and the absence of stale/duplicate composer backgrounds in PTY
  tests whenever command submission or display routing changes.

## Command Execution and Responsiveness

- All newly registered commands run on the command worker unless their
  `on_owner(argc, argv)` predicate explicitly selects a bounded UI action.
  Keep the policy in command registration, reuse it for aliases and distinguish
  panel opening from data processing subcommands. Do not add background-command
  name checks to `pages/chat.c`. `cli_command_dispatch()` enforces this boundary
  even when a caller bypasses chat submission.
- UI actions may mount/dismiss native pages, change focus or present already
  prepared data. They must not connect to MCP, execute a model, build an
  extension, compress history, wait for a process or perform large file work.
  Slash dispatch must not run turn preparation or scheduled tasks as hidden
  synchronous preflight on the owner.
- Keep the composer editable and the feed scrollable while a command runs.
  Esc/Ctrl+C must remain dispatchable. Background failure, timeout and startup
  failure must release command state and leave drafts intact. Do not silently
  discard an attempted submission while busy; retain it for retry.
- Use `cli_event_queue_call_owner()` for short native presentation updates and
  queued owned output/events for asynchronous delivery. Borrowed report arrays
  may cross a synchronous owner call only while the worker retains their owners.
  The worker must not change composer, permission/status cache or native view
  state directly. Never wait for that worker from an owner callback it needs.
- Runtime responses are correlated by request ID. Worker waits observe command
  cancellation, shutdown and a monotonic total deadline. MCP management goes
  through the runtime client so cancellation/shutdown can interrupt transport
  waits. Cancel only the submitting client's request; do not interrupt unrelated
  sessions or weaken access/approval/reviewer policy.
- Legacy owner control requests have a 100 ms total limit, including unrelated
  messages. Keep new slow operations asynchronous instead of increasing the
  limit or looping on timeout. A timed-out accepted mutation may still complete;
  do not announce rollback or automatically retry it.
- Before finishing a related change, run the registered
  `cli_command_responsiveness_pty_integration` at 120/80/60 columns, covering a
  backend that waits, exits, times out or is cancelled. Assert the unsent draft
  and wheel-scroll frames while the backend is still held, then assert recovery
  and clean exit. Run slash/panel/MCP/extension regressions and registry alias
  policy tests as appropriate. Keep assertions synchronized to visible results;
  successful eventual output alone does not prove responsiveness.

## Verification

- Run `cli_application_layout_conventions` after changing CLI ownership,
  interfaces, private names, module layout, status presentation or shortcuts.
- For shortcut changes, run `cli_shortcuts_pty_integration` and relevant page or
  input regressions. Verify `Ctrl+K` descriptions, groups and applicable state,
  precedence over native editing, and draft/caret restoration on close at
  120, 80 and 60 columns, including no-color output.
- For other UI changes, run the affected presentation and PTY regressions.
  Keep the frame and lifecycle checks required above; a skipped required PTY
  test does not validate the change. See the root guide for test dependencies.
