# Project terminal UI framework

`morph-tui` is the terminal UI framework used by the interactive CLI, setup wizard,
session picker, hooks review, history inspector, and `ask_user` selector. It depends
on `morph-util`, libuv 1.44 or newer, and POSIX terminal
APIs. It does not depend on CLI context, runtime services, model configuration,
sessions, or agent types.

The framework covers the components these screens currently need. Application
code owns domain data and actions; the framework owns terminal mechanics,
interaction state, layout helpers, and presentation primitives.

## Directory and include conventions

Framework sources are grouped by responsibility. Public headers live only in
`include/morph/`; implementation directories are not public include paths.

```text
src/tui/
  include/morph/
    tui.h                    terminal primitive convenience umbrella
    tui/
      core/                  application, controller, event, screen, layout
      terminal/              terminal session and input decoder
      render/                draw and frame submission
      widgets/               list, select, editor, card, text_view, tree
      components.h           component convenience umbrella
  core/                      application, standalone host, and layout
  terminal/                  POSIX terminal modes and byte decoding
  render/                    drawing and frame diffs
  widgets/                   reusable interaction and presentation components
  samples/                   catalog and shared standalone build setup
    minimal/                 main.c, CMakeLists.txt, README.md
  CMakeLists.txt

src/sapi/cli/
  main.c, init.c, run.c       frontend entry and orchestration
  commands/                  slash-command handlers
  setup/                     wizard, providers, roles, and configuration
  ui/
    application.c, .h        framework-to-CLI controller adapter
    event_queue.c, .h        owner-thread queue and wakeup bridge
    runtime_events.c         runtime event callback
    recap.c, .h              recap presentation state
    input/                   Readline callbacks, composer, paste, cancellation
    presentation/            feed, transcript, terminal output, media, styles
    screens/                 history, hooks, and jobs pages
```

- Include framework APIs as `<morph/tui/<layer>/<module>.h>`, for example
  `<morph/tui/widgets/list.h>`. Each implementation includes
  its own module header and the dependencies it uses directly. `tui.h` and
  `components.h` remain convenience umbrellas for consumers; they own no types
  or declarations. Application and controller headers do not include widgets.
- Keep terminal protocols in `terminal/`, output primitives in `render/`, and
  reusable controls in `widgets/`. Framework code depends on `morph-util` and
  platform libraries; runtime DTOs, command semantics, and CLI state stay in the
  frontend. Create further directories when an implemented module needs them.
- CLI headers stay beside their implementations and use project-relative
  includes such as `"sapi/cli/ui/screens/history.h"`. A file already placed in
  `screens/` does not repeat an `_ui` suffix. The general `event_queue` and
  runtime-specific `runtime_events` names distinguish the two event adapters.
- List source files explicitly in each target's `CMakeLists.txt`, grouped to
  match the directories. Update include sites and documentation in the same
  change as a move. Keep tests in the existing `tests/test_<module>` layout.
- Put each sample in `samples/<name>/` with `main.c`, `CMakeLists.txt`, and
  `README.md`. The parent directory owns the catalog, shared standalone setup,
  and explicit `add_subdirectory` entries. Each sample owns its executable
  target and documentation. See [sample conventions](../src/tui/samples/README.md).

## Symbol naming

Public functions, struct/enum tags, and constants carry their owning layer.
The plural `widgets/` directory uses the singular `widget` namespace.

| Source directory | Public prefix | Example |
| --- | --- | --- |
| `core/` | `morph_tui_core_` | `morph_tui_core_app_run` |
| `terminal/` | `morph_tui_terminal_` | `morph_tui_terminal_input_read` |
| `render/` | `morph_tui_render_` | `morph_tui_render_frame_present` |
| `widgets/` | `morph_tui_widget_` | `morph_tui_widget_list_update` |

Types follow the same rule, such as `struct morph_tui_widget_list`. Public
constants use the uppercase form, such as `MORPH_TUI_WIDGET_LIST_VIM` and
`MORPH_TUI_CORE_EVENT_KEY`. Shared widget results use `MORPH_TUI_WIDGET_*`.
Convenience umbrella guards are the only top-level namespace names.

File-local functions, private types, and private constants include both layer
and module: `core_app_dispatch`, `terminal_input_sequence_key`,
`widget_tree_refresh`, `struct widget_select_prompt_view`, and
`CORE_APP_EVENT_BUDGET`. They remain private through static linkage where
applicable. Samples use `sample_<name>_*`; executable entry points use `main`.
Rename declarations, definitions, callers, and documentation together. Old
unqualified API names and forwarding aliases are not maintained on this branch.

## Application and controller host

`<morph/tui/core/application.h>` exposes an explicitly owned application. Its private
libuv loop watches terminal readiness, uses an async handle for cross-thread
wakeup, and schedules controller ticks, application ticks, escape deadlines,
and resize probes with a timer. Public headers do not expose libuv types. The
application has one terminal window, an active controller stack, and deferred
presentation transitions. It does not use the default libuv loop or a singleton.

The owner thread creates, steps/runs, and destroys the application. Workers can
call `morph_tui_core_app_wake` and `morph_tui_core_app_post`. Posts transfer payload ownership
only on success, preserve order, and release each payload once after delivery or
shutdown. An async wake is a notification, never a message count; queued messages
remain distinct when libuv coalesces wakeups. Dispatch uses a bounded batch before
yielding to other event sources. Producers must stop or detach before destruction.

Controllers provide event, layout, draw, appear, disappear, and close callbacks.
Events return handled/unhandled or a negative error. Display/layout invalidation
is separate. Presentation copies the controller descriptor and defers changes
until callbacks finish. Dismissal targets the controller active at the time of
the request, so duplicate dismissal cannot close the underlying page. Close runs
once, after removal, and may free the page model. Reentrant stepping is rejected.

Each step commits pending transitions, resolves terminal dimensions, lays out and
draws invalid content, runs one libuv iteration, then commits and renders changes.
Invalidation coalesces across a batch. Before each render the current dimensions
are checked, preventing a resize wake from rendering an old-width frame. Existing
page draw callbacks still use their current renderers; canvas composition, a
retained view hierarchy, and responder routing are subsequent migration layers.

`MORPH_TUI_CORE_INPUT_EXTERNAL` gives readiness exclusively to an input adapter, currently
Readline for chat. `MORPH_TUI_CORE_INPUT_KEYS` gives decoding and terminal modes to the
application. Appear/disappear callbacks transfer input ownership. Only one adapter
reads the terminal, and key decoding does not consume input for the next page.
The application owns raw/paste/alternate-screen transitions and restores original
termios and descriptor flags on destruction. In particular, it reverses the
nonblocking flag set by `uv_poll_init`: stdin and stdout may share an open file
description, and nonblocking stdio can truncate a large terminal frame.

The CLI root controller adapts the existing composer, footer, live feed, and
transcript viewer. History, hooks, resume, and jobs are presented on the same
application rather than running nested polling loops. Page snapshots outlive
command dispatch and are released on close. Opening a panel suspends Readline;
closing it restores the composer. Runtime messages continue to update state,
while sanitized scrollback output is buffered until the panel leaves. Command
completion and session-selection receipts run after leaving alternate screen.

The CLI attaches its existing owner queue to application wakeups and detaches it
under the queue mutex before destroying the application. Signal handling stays
with the existing sigwait listener, which sets flags and wakes the application;
libuv does not install competing signal handlers. JSON and plain modes retain
their existing input/output contracts.

Standalone startup review, setup, and legacy synchronous tool prompts continue
to use the screen polling API below. Interactive runtime requests currently close
an open navigation panel before invoking that prompt adapter. Replacing those
blocking prompt adapters, adding responder/view/canvas layers, and replacing
Readline remain explicit follow-up work. Deferred panel output is currently an
in-memory buffer; bounded spill storage belongs with the transcript migration.

## Layers and current consumers

| Layer / component | Responsibility | Production consumers |
| --- | --- | --- |
| Application | libuv scheduling, controller presentation, input ownership, invalidation, teardown | Interactive chat, hooks, sessions, history, jobs |
| Terminal session | Save/restore termios, cursor, alternate screen, bracketed paste | All modal screens through `morph_tui_core_screen` |
| Input decoder | Stateful CSI/SS3 decoding, escape deadlines, UTF-8 byte input | All modal screens |
| Screen host | Typed key/resize/tick/EOF events, paste context, dirty redraw, update/draw loop | Standalone modal screens use `run`; inline setup and selects use `poll` |
| Rectangular layout | Insets, horizontal/vertical region allocation, clipping | Hooks cells/columns, session margins, and history header/body/footer sizing |
| List | Selection, visibility, paging, wrap/clamp, Home/End, configurable shortcuts | Setup choices, sessions, hook selection/decisions, history selection |
| Menu | List interaction plus submit/back/cancel and paste isolation | Setup menus, session picker, and hook decisions |
| Select | Single/multiple selection, checked state, limits, inline rendering, custom input, paste isolation | CLI `ask_user` choices |
| Card | Width-aware borders and sanitized, padded content rows | Setup provider cards |
| Editor | Cursor editing, UTF-8 backspace/movement, bounded input, paste, masking, validation | Setup fields/API keys and history search |
| Text viewer | Borrowed document, wrapped line spans, viewport scrolling | History details and request inspector |
| Tree | Owned hierarchy, expansion, navigation, bounded rows and viewport drawing | Skill list; all CLI report and streaming tree geometry through adapters |
| Drawing | Compose safe cells in memory and validate before output | Hooks and sessions, with application-specific styles |
| Frame renderer | Reusable row-diff output, cache invalidation, no-op suppression | History |

Consumers link `morph-tui` to inherit its include directories. Public headers
mirror the implementation layers and use the symbol prefixes above.

## Interaction and screen lifecycle

A screen is zero-initialized, opened once, and closed on every exit path. Opening
saves terminal state, applies the selected terminal modes, queries dimensions,
and schedules the first draw. Closing an inactive screen is harmless. Streams
belong to the caller and must remain open until after closing. Sessions must not
overlap on one terminal.

`morph_tui_core_screen_run` drives this cycle:

1. Draw when dirty. The draw callback receives the current viewport.
2. Poll a key, resize, tick, or EOF event without reading ahead across screens.
3. Dispatch the event to the application update callback.
4. Redraw, continue, or return for cleanup according to the callback result.

Resize marks the host dirty automatically. Update returns `MORPH_TUI_CORE_SCREEN_CONTINUE`,
`MORPH_TUI_CORE_SCREEN_REDRAW`, `MORPH_TUI_CORE_SCREEN_CLOSE`, or a negative error. Draw returns zero
or a negative error. The host returns on EOF or callback failure; the caller
still closes the screen. A modal screen's exit key does not consume bytes meant
for the restored Readline composer.

Setup uses the same screen host's polling API because its inline prompt owns
only a small set of lines while completed answers stay in scrollback. Its
application-specific wizard flow and active-line bookkeeping stay local. Hooks, sessions,
and history use the callback loop directly.

Signal handlers remain application-owned. Install them before opening, record a
signal in a `volatile sig_atomic_t`, and optionally set `screen.interrupted` to
that flag. The loop returns `-EINTR`; close the screen before restoring handlers
or re-raising. This keeps signal policy out of the rendering library.

Focus follows the existing screens' modes: setup dispatches to its current menu
or editor; history routes search input to the editor and normal input to its
list or document viewport; hooks routes list/review/batch input to the active
selection. The host transports events, while the screen explicitly chooses the
active component. Domain actions run only after a component submits.

## Component contracts

- Rectangles have a one-based terminal origin and nonnegative dimensions;
  region-relative cells use zero-based offsets. Splits consume the available
  region and clamp to it, including empty regions.
- Lists borrow item data. `morph_tui_widget_list_layout` clamps the selection when data or
  viewport size changes and keeps it visible. Continuous and page-aligned
  windows are supported. Shortcut flags are explicit, so text fields do not
  accidentally inherit menu bindings. `morph_tui_widget_scroll_update` clamps a document's
  offset to its last complete viewport.
- Menus return `MORPH_TUI_WIDGET_SUBMIT`, `MORPH_TUI_WIDGET_BACK`, or `MORPH_TUI_WIDGET_CANCEL` separately from selection
  changes. They ignore pasted navigation and confirmation. Hook trust decisions
  remain in `runtime_hooks_manage`, including snapshot/digest validation and
  conservative default choices.
- Editors initialize once and clean up on all exits. Their byte limit includes
  NUL. Submission and validity are separate: a required setup field must pass
  `morph_tui_widget_editor_valid`; a search may be empty. Incomplete UTF-8 bytes can accumulate,
  but they cannot pass validity checks. Pasted control keys cannot submit, clear,
  or navigate the editor. Secret presentation uses a fixed mask, and cleanup
  wipes owned input storage. The application owns its final copy of a value.
- Selects initialize once with item count, mode, and selection limits, then
  clean up on all exits. Single mode confirms the focused item. Multi mode
  toggles checks with Space and validates the count before Enter submits.
  Arrow keys, Home/End, page keys, Tab, j/k, and digits move focus. Pasted keys
  cannot toggle, submit, or cancel. `morph_tui_widget_select_run` owns inline rendering
  and optionally adds an Other item using the shared editor. In multi mode,
  custom text counts as one answer and can accompany checked suggestions;
  Enter saves it back to the list and `e` edits it again. Esc in the editor
  returns to the list; Esc in the list cancels the turn. The CLI owner loop
  preserves the composer draft while the selector owns terminal input.
- Card functions append one row without a newline or style; the screen supplies
  placement and colors. Content is sanitized and clipped by display width.
- A text viewer borrows sanitized UTF-8 text until the next wrap or cleanup.
  Rewrap after document changes or a width change. Wrapped lines are byte spans,
  allowing the application to choose presentation without copying each line.

## Rendering contracts

`morph_tui_render_draw_cell_append` accepts untrusted text and a trusted ANSI style. Initialize its
command buffer with `morph_buf_init`; appends preserve errors on that buffer.
`morph_tui_core_layout_cell` additionally clips to an allocated layout region. The shared
`morph_tui_render_draw_write` rejects a failed buffer before any terminal output, then writes
and flushes the composed bytes. A construction error cannot expose a partial
hooks screen. Actual I/O failure can still write a partial screen and must end
the current interaction so its terminal modes can be restored.

`morph_tui_render_frame_present` accepts newline-separated, already sanitized and clipped
rows with trusted styles. It builds the complete diff in memory and updates the
previous-frame cache only after successful output. Identical frames emit no
bytes. A resize, output-target change, explicit `morph_tui_render_frame_invalidate`, or output
failure forces a full repaint. External writes to its owned screen require
invalidation before the next present. Cleanup releases both cached buffers.

The chat terminal, Readline composer, owner-thread event queue, streaming media,
and transcript coordination remain separate CLI facilities. They have persistent
streaming lifecycles and runtime-specific data. The modal framework neither owns
nor duplicates those services.

## Design references

These are design influences, not new dependencies or copied upstream code:

| Upstream | Relevant idea | Adaptation |
| --- | --- | --- |
| [libtermkey input API](https://www.leonerd.org.uk/code/libtermkey/doc/termkey_getkey.3.html) | Stateful decoding separated from descriptor I/O | Pure `feed`/`expire` operations plus a polling adapter; incomplete sequences survive polling and interruption |
| [Ratatui rendering](https://ratatui.rs/concepts/rendering/under-the-hood/) | Compose, compare buffers, then output | Row diffs with explicit invalidation and successful-output cache commits |
| [Bubble Tea model](https://github.com/charmbracelet/bubbletea#the-model) | Application state updates separated from view generation | Typed events and update/draw callbacks with explicit redraw/close results |
| [FTXUI modules](https://github.com/ArthurSonzogni/FTXUI#short-gallery) | Separate rendering, layout, and interaction | Independent geometry, interaction state, and drawing modules |

The current framework uses explicit C state components rather than a retained
widget tree. There is one active component per current screen mode, so focus
routing remains explicit and does not need a global focus registry.

## Validation

`tests/test_tui_application.cpp` covers deferred presentation, lifecycle order,
duplicate dismissals, owner-thread enforcement, reentrancy, cross-thread message
order and ownership, timer/invalidation phases, partial startup, output errors,
resize geometry, incremental keys, and terminal restoration. The application and
terminal primitives also run together under AddressSanitizer and UBSan.

`tests/test_cli_application_pty.py` covers background completion while a panel is
open, deferred output, modal paste isolation, resize, input handoff to Readline,
and shutdown through the production CLI loop.

`tests/test_tui.cpp` covers terminal restoration, incremental/overlong/interrupted
input, clipping, frame invalidation and I/O failure, geometry, list changes,
menu paste isolation, UTF-8 editing, bounded/secret input, document wrapping,
screen resize/paste events, and the update/draw loop on real PTYs.

The existing setup, sessions, hooks, startup hook review, and history PTY suites exercise
the production consumers, including resize without keyboard input, defaults,
confirmation, paste, search, scrolling, cancellation, stale hook snapshots,
and restoring the chat composer after closing a modal screen.

## Tree component

`morph/tui/widgets/tree.h` supports both retained trees and incremental reports. Initialize a
`morph_tui_widget_tree`, add roots with `MORPH_TUI_WIDGET_TREE_ROOT` and children with their parent index,
then render or draw it. Labels and descriptions are copied into its arena and
sanitized; callers can release their input immediately. Indices remain stable
when storage grows. Adding a node with an invalid parent records a sticky error.
Cleanup releases the complete model. Initialize each instance once per lifetime.

Traversal follows sibling insertion order, including children added after later
roots. It uses an iterative traversal rather than recursive C calls. Nodes start
expanded. Up/Down, Home/End and Page Up/Down use list navigation; Right expands a
parent or enters its first child; Left collapses a parent or selects its parent.
Enter submits, Escape/Ctrl-C cancel, and bracketed paste cannot navigate or submit. Selection and
scroll position live in the component, and resize clamps them to the new page.
`morph_tui_widget_tree_draw` fills an allocated rectangle, highlights the selected row with
caller-supplied styles, and erases unused rows. It appends commands without I/O.
`morph_tui_widget_tree_render` produces plain, newline-separated rows for transcript output;
its `first` and `rows` parameters address visible nodes. Every row respects the
requested display width, including CJK text and very narrow terminals. Skill
names and descriptions use bounded columns; long text receives an ellipsis.

`morph_tui_widget_tree_row`, `morph_tui_widget_tree_prefix`, and `morph_tui_widget_tree_branch` serve incremental reports
whose later siblings are not yet available. CLI adapters retain domain formatting
(JSON limits, status colors, timestamps and receipts), while all branch geometry
comes from this component. Help, extensions, permissions, tasks, sync, sessions,
JSON/tool details, tool previews, artifacts, MCP startup, and approval directories
use these paths. Patch gutters and approval card borders are separate components,
not hierarchical views. JSON event envelopes and noninteractive command behavior
retain their existing dispatch paths.

## Minimal sample

`src/tui/samples/minimal/main.c` demonstrates application setup, controller events,
a menu widget, layout, drawing, resize, and terminal restoration. Build it
independently with `cmake -S src/tui/samples -B /tmp/morph-tui-samples`, then
`cmake --build /tmp/morph-tui-samples` and run
`/tmp/morph-tui-samples/bin/morph-tui-minimal`. Its standalone build uses only
libuv, POSIX APIs, and the required utility modules from the shared manifest;
it performs no dependency downloads. The main build enables the same executable
with `-DBUILD_TUI_SAMPLES=ON`. See [sample instructions](../src/tui/samples/minimal/README.md).
