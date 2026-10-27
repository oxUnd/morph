# Project terminal UI framework

`morph-tui` is the small terminal UI framework used by the setup wizard, session picker, hooks
review, and history inspector. It depends only on `morph-util` and POSIX terminal
APIs. It does not depend on CLI context, runtime services, model configuration,
sessions, or agent types.

The framework covers the components these screens currently need. Application
code owns domain data and actions; the framework owns terminal mechanics,
interaction state, layout helpers, and presentation primitives.

## Layers and current consumers

| Layer / component | Responsibility | Production consumers |
| --- | --- | --- |
| Terminal session | Save/restore termios, cursor, alternate screen, bracketed paste | All modal screens through `tui_screen` |
| Input decoder | Stateful CSI/SS3 decoding, escape deadlines, UTF-8 byte input | All modal screens |
| Screen host | Typed key/resize/tick/EOF events, paste context, dirty redraw, update/draw loop | Hooks, session picker, and history use `run`; inline setup uses `poll` |
| Rectangular layout | Insets, horizontal/vertical region allocation, clipping | Hooks cells/columns, session margins, and history header/body/footer sizing |
| List | Selection, visibility, paging, wrap/clamp, Home/End, configurable shortcuts | Setup choices, sessions, hook selection/decisions, history selection |
| Menu | List interaction plus submit/back/cancel and paste isolation | Setup menus, session picker, and hook decisions |
| Card | Width-aware borders and sanitized, padded content rows | Setup provider cards |
| Editor | Cursor editing, UTF-8 backspace/movement, bounded input, paste, masking, validation | Setup fields/API keys and history search |
| Text viewer | Borrowed document, wrapped line spans, viewport scrolling | History details and request inspector |
| Drawing | Compose safe cells in memory and validate before output | Hooks and sessions, with application-specific styles |
| Frame renderer | Reusable row-diff output, cache invalidation, no-op suppression | History |

The public low-level API is `tui/tui.h`; layout, components, and screen hosting
are in `tui/components.h`. Implementations are divided into `terminal.c`,
`input.c`, `screen.c`, `layout.c`, `list.c`, `editor.c`, `card.c`, `text_view.c`,
`draw.c`, and `frame.c`.

## Interaction and screen lifecycle

A screen is zero-initialized, opened once, and closed on every exit path. Opening
saves terminal state, applies the selected terminal modes, queries dimensions,
and schedules the first draw. Closing an inactive screen is harmless. Streams
belong to the caller and must remain open until after closing. Sessions must not
overlap on one terminal.

`tui_screen_run` drives this cycle:

1. Draw when dirty. The draw callback receives the current viewport.
2. Poll a key, resize, tick, or EOF event without reading ahead across screens.
3. Dispatch the event to the application update callback.
4. Redraw, continue, or return for cleanup according to the callback result.

Resize marks the host dirty automatically. Update returns `TUI_SCREEN_CONTINUE`,
`TUI_SCREEN_REDRAW`, `TUI_SCREEN_CLOSE`, or a negative error. Draw returns zero
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
- Lists borrow item data. `tui_list_layout` clamps the selection when data or
  viewport size changes and keeps it visible. Continuous and page-aligned
  windows are supported. Shortcut flags are explicit, so text fields do not
  accidentally inherit menu bindings. `tui_scroll_update` clamps a document's
  offset to its last complete viewport.
- Menus return `TUI_SUBMIT`, `TUI_BACK`, or `TUI_CANCEL` separately from selection
  changes. They ignore pasted navigation and confirmation. Hook trust decisions
  remain in `runtime_hooks_manage`, including snapshot/digest validation and
  conservative default choices.
- Editors initialize once and clean up on all exits. Their byte limit includes
  NUL. Submission and validity are separate: a required setup field must pass
  `tui_editor_valid`; a search may be empty. Incomplete UTF-8 bytes can accumulate,
  but they cannot pass validity checks. Pasted control keys cannot submit, clear,
  or navigate the editor. Secret presentation uses a fixed mask, and cleanup
  wipes owned input storage. The application owns its final copy of a value.
- Card functions append one row without a newline or style; the screen supplies
  placement and colors. Content is sanitized and clipped by display width.
- A text viewer borrows sanitized UTF-8 text until the next wrap or cleanup.
  Rewrap after document changes or a width change. Wrapped lines are byte spans,
  allowing the application to choose presentation without copying each line.

## Rendering contracts

`tui_cell_append` accepts untrusted text and a trusted ANSI style. Initialize its
command buffer with `morph_buf_init`; appends preserve errors on that buffer.
`tui_region_cell` additionally clips to an allocated layout region. The shared
`tui_output_write` rejects a failed buffer before any terminal output, then writes
and flushes the composed bytes. A construction error cannot expose a partial
hooks screen. Actual I/O failure can still write a partial screen and must end
the current interaction so its terminal modes can be restored.

`tui_frame_present` accepts newline-separated, already sanitized and clipped
rows with trusted styles. It builds the complete diff in memory and updates the
previous-frame cache only after successful output. Identical frames emit no
bytes. A resize, output-target change, explicit `tui_frame_invalidate`, or output
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
