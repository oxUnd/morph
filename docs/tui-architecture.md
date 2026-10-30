# Project terminal UI framework

`morph-tui` is the small terminal UI framework used by the setup wizard, session picker, hooks
review, history inspector, and `ask_user` selector. It depends only on `morph-util` and POSIX terminal
APIs. It does not depend on CLI context, runtime services, model configuration,
sessions, or agent types.

The framework covers the components these screens currently need. Application
code owns domain data and actions; the framework owns terminal mechanics,
interaction state, layout helpers, and presentation primitives.

## Layers and current consumers

| Layer / component | Responsibility | Production consumers |
| --- | --- | --- |
| Terminal session | Save/restore termios, cursor, alternate screen, bracketed paste | All modal screens through `morph_tui_screen` |
| Input decoder | Stateful CSI/SS3 decoding, escape deadlines, UTF-8 byte input | All modal screens |
| Screen host | Typed key/resize/tick/EOF events, paste context, dirty redraw, update/draw loop | Hooks, session picker, and history use `run`; inline setup and selects use `poll` |
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

Public functions, struct/enum tags, and constants use the `morph_tui_` and
`MORPH_TUI_` namespaces. Consumers link `morph-tui` to inherit the public include
directories and include `<morph/tui.h>`, `<morph/tui/components.h>`, or
`<morph/tui/tree.h>`. Headers live under `src/tui/include/`; the former `tui_*`
names and `tui/` include paths are no longer provided.

The public low-level API is `morph/tui.h`; layout, components, and screen hosting
are in `morph/tui/components.h`; hierarchical views use `morph/tui/tree.h`. Implementations are divided into `terminal.c`,
`input.c`, `screen.c`, `layout.c`, `list.c`, `select.c`, `select_prompt.c`, `editor.c`, `card.c`, `text_view.c`,
`draw.c`, and `frame.c`.

## Interaction and screen lifecycle

A screen is zero-initialized, opened once, and closed on every exit path. Opening
saves terminal state, applies the selected terminal modes, queries dimensions,
and schedules the first draw. Closing an inactive screen is harmless. Streams
belong to the caller and must remain open until after closing. Sessions must not
overlap on one terminal.

`morph_tui_screen_run` drives this cycle:

1. Draw when dirty. The draw callback receives the current viewport.
2. Poll a key, resize, tick, or EOF event without reading ahead across screens.
3. Dispatch the event to the application update callback.
4. Redraw, continue, or return for cleanup according to the callback result.

Resize marks the host dirty automatically. Update returns `MORPH_TUI_SCREEN_CONTINUE`,
`MORPH_TUI_SCREEN_REDRAW`, `MORPH_TUI_SCREEN_CLOSE`, or a negative error. Draw returns zero
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
- Lists borrow item data. `morph_tui_list_layout` clamps the selection when data or
  viewport size changes and keeps it visible. Continuous and page-aligned
  windows are supported. Shortcut flags are explicit, so text fields do not
  accidentally inherit menu bindings. `morph_tui_scroll_update` clamps a document's
  offset to its last complete viewport.
- Menus return `MORPH_TUI_SUBMIT`, `MORPH_TUI_BACK`, or `MORPH_TUI_CANCEL` separately from selection
  changes. They ignore pasted navigation and confirmation. Hook trust decisions
  remain in `runtime_hooks_manage`, including snapshot/digest validation and
  conservative default choices.
- Editors initialize once and clean up on all exits. Their byte limit includes
  NUL. Submission and validity are separate: a required setup field must pass
  `morph_tui_editor_valid`; a search may be empty. Incomplete UTF-8 bytes can accumulate,
  but they cannot pass validity checks. Pasted control keys cannot submit, clear,
  or navigate the editor. Secret presentation uses a fixed mask, and cleanup
  wipes owned input storage. The application owns its final copy of a value.
- Selects initialize once with item count, mode, and selection limits, then
  clean up on all exits. Single mode confirms the focused item. Multi mode
  toggles checks with Space and validates the count before Enter submits.
  Arrow keys, Home/End, page keys, Tab, j/k, and digits move focus. Pasted keys
  cannot toggle, submit, or cancel. `morph_tui_select_run` owns inline rendering
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

`morph_tui_cell_append` accepts untrusted text and a trusted ANSI style. Initialize its
command buffer with `morph_buf_init`; appends preserve errors on that buffer.
`morph_tui_region_cell` additionally clips to an allocated layout region. The shared
`morph_tui_output_write` rejects a failed buffer before any terminal output, then writes
and flushes the composed bytes. A construction error cannot expose a partial
hooks screen. Actual I/O failure can still write a partial screen and must end
the current interaction so its terminal modes can be restored.

`morph_tui_frame_present` accepts newline-separated, already sanitized and clipped
rows with trusted styles. It builds the complete diff in memory and updates the
previous-frame cache only after successful output. Identical frames emit no
bytes. A resize, output-target change, explicit `morph_tui_frame_invalidate`, or output
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

`tests/test_tui.cpp` covers terminal restoration, incremental/overlong/interrupted
input, clipping, frame invalidation and I/O failure, geometry, list changes,
menu paste isolation, UTF-8 editing, bounded/secret input, document wrapping,
screen resize/paste events, and the update/draw loop on real PTYs.

The existing setup, sessions, hooks, startup hook review, and history PTY suites exercise
the production consumers, including resize without keyboard input, defaults,
confirmation, paste, search, scrolling, cancellation, stale hook snapshots,
and restoring the chat composer after closing a modal screen.

## Tree component

`morph/tui/tree.h` supports both retained trees and incremental reports. Initialize a
`morph_tui_tree`, add roots with `MORPH_TUI_TREE_ROOT` and children with their parent index,
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
`morph_tui_tree_draw` fills an allocated rectangle, highlights the selected row with
caller-supplied styles, and erases unused rows. It appends commands without I/O.
`morph_tui_tree_render` produces plain, newline-separated rows for transcript output;
its `first` and `rows` parameters address visible nodes. Every row respects the
requested display width, including CJK text and very narrow terminals. Skill
names and descriptions use bounded columns; long text receives an ellipsis.

`morph_tui_tree_row`, `morph_tui_tree_prefix`, and `morph_tui_tree_branch` serve incremental reports
whose later siblings are not yet available. CLI adapters retain domain formatting
(JSON limits, status colors, timestamps and receipts), while all branch geometry
comes from this component. Help, extensions, permissions, tasks, sync, sessions,
JSON/tool details, tool previews, artifacts, MCP startup, and approval directories
use these paths. Patch gutters and approval card borders are separate components,
not hierarchical views. JSON event envelopes and noninteractive command behavior
retain their existing dispatch paths.
