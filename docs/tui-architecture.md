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
    tui.h                    application-facing convenience umbrella
    tui/
      application.h          one Application for standalone and embedded use
      view.h, widget.h       extensible view and widget bases
      view_controller.h      extensible pages
      window.h, layout.h     window and layout contracts
      canvas.h, renderer.h   drawing and renderer contracts
      event.h                typed events
      controller.h, ...      advanced input/screen adapters
      widgets/               label, stack, list, text_field, document, state helpers
      components.h           component convenience umbrella
  core/                      application, view controllers, view tree, window, layout
  terminal/                  POSIX terminal modes and byte decoding
  render/                    cell surface, clipped canvas, drawing, frame diffs
  widgets/                   reusable interaction and presentation components
  samples/                   catalog and shared standalone build setup
    minimal/                 main.c, CMakeLists.txt, README.md
    controls/                text input, focus, and modal presentation
    customization/           custom widget, pages, bindings, and app shortcuts
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

- Include domain APIs as `<morph/tui/<object>.h>`, such as
  `<morph/tui/application.h>` and `<morph/tui/canvas.h>`. Control headers use
  `<morph/tui/widgets/list.h>`. Public include paths do not mirror implementation
  layers. Each implementation includes
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

Public names describe the object an application uses. Implementation directory
names such as `core` and `render` do not appear in public object names.

| Object | Example public name | Header |
| --- | --- | --- |
| Application | `morph_tui_application_run` | `application.h` |
| View / ViewController | `morph_tui_view_set_hidden`, `morph_tui_view_controller_present` | `view.h`, `view_controller.h` |
| Window / Layout | `morph_tui_window_set_renderer`, `morph_tui_layout` | `window.h`, `layout.h` |
| Canvas / Renderer | `morph_tui_canvas_text`, `morph_tui_renderer` | `canvas.h`, `renderer.h` |
| Widget / Control | `morph_tui_widget_create`, `morph_tui_widget_list_create` | `widget.h`, `widgets/list.h` |

Constants follow the same vocabulary, for example `MORPH_TUI_EVENT_KEY`,
`MORPH_TUI_STYLE_SELECTED`, and `MORPH_TUI_WIDGET_ACTION_CHANGED`. Low-level
list/menu/editor models explicitly include `_state`; document wrapping uses
`morph_tui_widget_text_layout`. This distinguishes reusable interaction state
from retained controls without two similarly named List constructors.

The branch intentionally renames old headers and symbols together. It does not
maintain forwarding headers or aliases for the former implementation-prefixed API.

File-local functions, private types, and private constants include both layer
and module: `core_app_dispatch`, `terminal_input_sequence_key`,
`widget_tree_refresh`, `struct widget_select_prompt_view`, and
`CORE_APP_EVENT_BUDGET`. They remain private through static linkage where
applicable. Samples use `sample_<name>_*`; executable entry points use `main`.
Rename declarations, definitions, callers, and documentation together. Private
headers such as `core/application_loop.h` stay outside the public include tree.

## Application and controller host

`<morph/tui/application.h>` exposes an explicitly owned application. Its private
libuv loop watches terminal readiness, uses an async handle for cross-thread
wakeup, and schedules controller ticks, application ticks, escape deadlines,
and resize probes with a timer. Public headers do not expose libuv types. The
application has one terminal window, an active controller stack, and deferred
presentation transitions. It does not use the default libuv loop or a singleton.

The owner thread creates, steps/runs, and destroys the application. Workers can
call `morph_tui_application_wake` and `morph_tui_application_post`. Posts transfer payload ownership
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
are checked, preventing a resize wake from rendering an old-width frame. View
controllers use the retained view, canvas, and responder machinery described
below. Legacy controller draw callbacks continue to use their existing renderers.

`MORPH_TUI_INPUT_EXTERNAL` gives readiness exclusively to an input adapter, currently
Readline for chat. `MORPH_TUI_INPUT_KEYS` gives decoding and terminal modes to the
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

The CLI embeds an Application subclass. Its `prepare_termination` hook cancels
queued and future synchronous owner calls, cancels and joins the command worker,
then detaches the owner queue under its mutex. The Application remains alive
throughout that join. Pending output is retained and drained after terminal
restoration; shutdown cannot open another interactive prompt. Termination signals
stop the loop immediately, including while a modal page is visible. Signal
handling stays with the existing sigwait listener, which sets flags and wakes the application;
libuv does not install competing signal handlers. JSON and plain modes retain
their existing input/output contracts.

Standalone startup review, setup, and legacy synchronous tool prompts continue
to use the screen polling API below. Interactive runtime requests currently close
an open navigation panel before invoking that prompt adapter. Replacing those
blocking prompt adapters and replacing Readline remain explicit follow-up work. Deferred panel output is currently an
in-memory buffer; bounded spill storage belongs with the transcript migration.

## Retained application interface

New applications can include `<morph/tui.h>`, create a root view controller
specification, and call `morph_tui_application_main`. Individual domain headers
remain available for precise dependencies. Ordinary
pages implement `build` and, when needed, `action`; they do not implement screen
drawing, byte decoding, signal handlers, resize arithmetic, or terminal cleanup.
The same Application exposes init/deinit, run/step, messaging, window access,
and advanced controller adapters. The libuv loop is private; applications and
CLI integrations never obtain a separate App or runtime pointer.

### Extensible bases

C subclasses embed their base as the first member. Method tables are borrowed
and must outlive the instance. Omitted optional methods retain base behavior.
There is no type-registration singleton or global application object.

| Base | Extension contract | Allocation and lifetime |
| --- | --- | --- |
| `morph_tui_application` | Optional launch, event, prepare-termination, and termination methods | Caller-owned init/deinit or factory-owned create/destroy; one state per initialized Application |
| `morph_tui_view_controller` | Build, action, event, appear, disappear, destroy | Specification selects instance size; host owns accepted instances |
| `morph_tui_view` | Optional measure, layout, canvas draw, event, destroy | Builder allocates subclass instances in the controller scope |
| `morph_tui_widget` | Embeds View and adds tags, action bindings, action emission | Built-in and custom widgets use the same factory and dispatch path |
| `morph_tui_layout` | Measure and arrange methods | Borrowed by a view; normally embedded in a custom container |
| `morph_tui_renderer` | Present a composed surface; optional invalidate | Caller-owned Renderer installed on the default window |

View and widget callbacks receive the base pointer; casting to a struct whose
first member is that base recovers subclass state. Factories accept
`sizeof(instance)` so the framework can allocate the complete subclass.
Custom containers can inspect children and call public measure/arrange methods.
A custom Renderer receives the same cell surface as the terminal
backend; it can serialize rows and inspect the desired cursor for capture or
another output target. The default window delegates composition to views and
presentation to the installed Renderer.

### Construction and ownership

A build callback receives a scoped builder. The first view with no parent is
the root; every other view must name a parent from the same controller. Builders
record the first error, and subsequent constructors stop. Call
`morph_tui_builder_fail` to include errors from application-specific build
steps. A failed build never presents a partial interface.

The controller owns all view instances and their hierarchy. View instance/state
storage uses an arena; growing child arrays and replaceable strings have
explicit cleanup. Children are destroyed before their parents, exactly once.
Controller `destroy` follows view teardown and must not dereference released
views. It also runs for build/presentation failure after scope initialization.
Argument or allocation failures before scope initialization leave caller data
untouched. Specification `data` is borrowed; callers retain its ownership unless
their own controller explicitly moves or releases it.

Label text and ordinary list items are copied. Data-source lists borrow their
callback and context, fetch visible rows only, and copy action text for the full
callback duration. A callback can replace its sender's items without invalidating
its action payload. An explicit widget action binding overrides the controller's
action method; its context remains caller-owned.

View construction and mutation run only on the application owner thread.
Setters reject worker calls with `-EPERM`. Workers post owned messages through
the host; messages should carry model data, not borrowed pointers to views that
may have closed. Producers must finish before application teardown.

### Layout, focus, and actions

View coordinates are zero-based and relative to their parent. This differs
from the one-based legacy terminal rectangles; terminal coordinates are confined
to output adapters. Views use intrinsic or fixed dimensions, minimum/maximum
sizes, padding, and growth weights. Stack containers measure children and then
arrange them horizontally or vertically, distribute remaining space subject to
maximums, and support stretch/start/center/end cross-axis alignment. Hidden
children consume no space. Tiny viewports produce bounded or empty regions.
`morph_tui_view_content_rect` returns the local rectangle after padding. Layout
and input callbacks use its dimensions for wrapping and scrolling; the draw
callback receives a canvas of the same size. Custom controls should use this
shared geometry rather than deriving a viewport from the outer frame.

The window tracks the active controller; each controller retains its focus.
Tab and Shift-Tab traverse focusable views in tree order, skipping hidden
ancestors. An unhandled input proceeds from the focused view through its parents
to the controller, the window handler, and the Application delegate before
default page commands. Custom parent views can
handle child input using the same optional event method.

View-controller input distinguishes command keys, complete UTF-8 text, and pasted
text. UTF-8 sanitizer state belongs to each controller/input target. A bracketed
paste retains its original target and never reaches page shortcuts; controls
receive its boundaries and text without executing pasted navigation or submit.
The low-level host and legacy adapters retain their byte-key contract.

List owns selection, viewport, drawing, and ACTIVATE/CHANGED actions. TextField
reuses Editor for bounded editing, supplies display and cursor state, and emits
CHANGED/SUBMIT. The document control owns sanitized multiline text, wrapping,
scrolling, and clipping. Reapplying the same hidden state, focusability/focus,
list selection, or Label/TextView text does not schedule a frame.
Label text changes request layout only when their
display width changes. Refreshing a list data source always requests display
because its borrowed contents may have changed; layout is required only when
the count changes. `tick_ms` is an optional controller
business timer, disabled by default.

Escape and Ctrl-C/Ctrl-D close a page after its responders decline the event.
An optional printable `quit_key` follows the same path, so a focused text field
can accept `q` as text. Modal controllers use deferred presentation; the parent
retains its view tree and focus while hidden and resumes them on dismissal.

### Drawing and lifecycle

Invalidations coalesce in the host. Layout runs before composition, followed by
frame comparison and output. Canvas draws in local coordinates with inherited
clipping and typed styles. Surface cells track wide-character continuations and
combining marks; input text is sanitized through `util/utf8.h`. Clearing and
composing a full visible surface keeps overlap/resize behavior deterministic;
row diffs suppress unchanged terminal rows. This uses the project's Unicode
width policy rather than claiming full emoji-grapheme shaping.

Application `init` establishes streams, window, and signal policy, then calls
its optional launch method exactly once. The default root page is built after
launch, so views can use initialized application model data. `run` and `step`
operate on that same object; state lasts until `deinit`. `create`/`destroy` are
conveniences for a heap-owned default instance. `main` combines init, root
presentation, run, deinit, and exit-status reporting. It rejects an already
initialized instance without destroying the host.

Standalone Application defaults to stdin/stdout, alternate screen, decoded keys,
bracketed paste, and the existing signal listener bridged to app wakeups. Signals
need no application polling tick. The host retains its resize-probe fallback.
Application delegates receive termination signals as SIGNAL events with the
signal number in `code`; consuming a wake does not suppress signal handling.
The optional `prepare_termination` hook runs with the stopped Application still
initialized so subclasses can join producers before destruction. Terminal and descriptor
restoration precedes the final Application termination hook
and error reporting. `application_run` returns framework results; `application_main` reports errors
and converts them into process exit status.

Applications embedded in a host use `external_signals`, or present a view
controller directly on an existing `morph_tui_application`. They reuse the host's
signal policy. No competing libuv signal handlers are installed. The production
`/jobs` page uses retained list/document controls on the CLI's existing app;
its CLI adapter still flushes deferred output and completes command groups after
the panel leaves. Legacy standalone screen adapters remain available for startup
and setup migration.

## Layers and current consumers

| Layer / component | Responsibility | Production consumers |
| --- | --- | --- |
| Application | libuv scheduling, controller presentation, input ownership, invalidation, teardown | Interactive chat, hooks, sessions, history, jobs |
| Terminal session | Save/restore termios, cursor, alternate screen, bracketed paste | All modal screens through `morph_tui_screen` |
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
- Lists borrow item data. `morph_tui_widget_list_state_layout` clamps the selection when data or
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
  `morph_tui_widget_editor_state_valid`; a search may be empty. Incomplete UTF-8 bytes can accumulate,
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

`morph_tui_draw_cell_append` accepts untrusted text and a trusted ANSI style. Initialize its
command buffer with `morph_buf_init`; appends preserve errors on that buffer.
`morph_tui_draw_region_cell` additionally clips to an allocated layout region. The shared
`morph_tui_draw_write` rejects a failed buffer before any terminal output, then writes
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

Legacy screen adapters compose explicit interaction-state objects and route
focus themselves. Retained pages own a view tree and use per-controller focus
and responder routing through the public View/Widget interfaces.

## Validation

`tests/test_tui_application.cpp` covers deferred presentation, lifecycle order,
duplicate dismissals, owner-thread enforcement, reentrancy, cross-thread message
order and ownership, timer/invalidation phases, partial startup, output errors,
resize geometry, incremental keys, and terminal restoration. The application and
terminal primitives also run together under AddressSanitizer and UBSan.

`tests/test_cli_application_pty.py` covers background completion while a panel is
open, deferred output, modal paste isolation, resize, input handoff to Readline,
and termination during active work at the root or in a modal, including terminal
restoration. `tests/test_cli_ui.cpp` checks that shutdown releases waiting owner
calls, rejects late requests, and retains queued output without pumping prompts.

`tests/test_tui_views.cpp` covers content geometry and padded scrolling, focus
and paste handoff, redundant-update suppression, custom subclasses and layout,
action propagation, controller scope cleanup, and worker mutation rejection.

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

## Samples

`src/tui/samples/minimal/main.c` builds a label/list/label view tree and handles
one activation action. Its application runner manages terminal and signal
lifecycle, layout, focus, redraw, rendering buffers, and cleanup.
`src/tui/samples/controls/main.c` adds a text field, Tab/Shift-Tab traversal,
UTF-8 input, paste isolation, modal presentation, and restoration of the same
focused control. Both use public framework APIs without CLI or runtime services.

The [customization demo](../src/tui/samples/customization/README.md) is the
extension tutorial: a Widget subclass draws a counter and interprets its keys;
Dashboard and Inspector subclasses compose different view trees over one model;
a bound field action, page reset command, and Application help/redraw shortcuts
show each interaction scope. Application launch initializes model resources
before pages are built, and termination releases them after page teardown.

Configure independently with `cmake -S src/tui/samples -B /tmp/morph-tui-samples`,
then `cmake --build /tmp/morph-tui-samples`. Run
`/tmp/morph-tui-samples/bin/morph-tui-minimal` or `morph-tui-controls` from that
same `bin/` directory. The standalone build uses libuv, POSIX APIs, and utility
modules from the shared manifest, without downloads. The main build enables
all targets with `-DBUILD_TUI_SAMPLES=ON`. See the
[sample catalog](../src/tui/samples/README.md).
