# CLI application

The entire CLI is a `nothing-tui` application. `struct cli_application` embeds
`struct nt_application` as its first member and owns the opaque runtime,
execution mode, workers, models and presentation state. There is one application
identity; chat, selectors and modal pages share its controller stack.

Follow the shared [TUI application conventions](../../../docs/tui-application-layout.md).
The framework's internal source layers are not application directories.

```text
cli/
  main.c                       arguments, logging and process bootstrap
  application.c, .h            application state, modes, host and teardown
  application_internal.h       common implementation/output conveniences
  models/
    composer.c, .h             image labels, path expansion and editing spans
    presentation.h             output/presentation state
    transcript.c, .h            tool/reasoning event projection and owned records
    setup.h                    configuration wizard data
  pages/
    chat.c, .h                 chat controller and page-local editing state
    history, hooks, jobs, memory, resume
    model, permissions         inline mode/policy selectors in the input area
    turn_details               modal transcript viewport on the same Application
    approval, question         modal interaction pages
    setup/                     wizard and its terminal controls
  components/                  NT views for the composer, messages, reports, lists and panels
    page_layout                full-screen breadcrumb navigation, content and shortcut footer
  commands/                    options, slash actions, registry and management entry points
  services/
    command_job, event_queue   workers, owner calls and wakeup bridge
    turn, scheduler, recap     runtime use cases and state coordination
    transcript                 model updates and UI presentation coordination
    session_history            restore persisted conversation to terminal history
    management, skill_store, share_store
    configuration/             provider defaults, validation, roles and storage
  adapters/
    input/                     input state owner, GNU Readline prompts, callbacks and stream readers
    clipboard/                 native clipboard access and terminal copy requests
    runtime/                   owner-thread UI dispatch, event translation and observer
    protocol/                  runtime interaction request/reply mapping
    media/                     external images and terminal media rendering
  presentation/                presenter state, notifications, output, history, Markdown and HTML
  CMakeLists.txt
  README.md
```

Names in the tree represent sibling `.c/.h` module pairs unless marked otherwise.
Each header owns its module's declarations; commands include their own handler
factory headers rather than declaring everything in the registry. Models do not
import application state. Configuration services do not import wizard pages.

Interactive chat is a retained ViewController in alternate screen. Its page layout
contains a scrolling feed, activity/jobs/queue controls and a multiline TextField
composer. `components/message` composes Label and content-sized TextView controls;
`components/tool_call`, plan and notice bind domain state to rich text through
`components/report`. View margin/padding determine
spacing; pages and event handlers do not count terminal rows or move the cursor.
Tool events update the existing control by call ID. Streaming messages keep their
source and remeasure Markdown at the current content width. PgUp/PgDn and wheel
scroll within the feed; reaching the bottom resumes following.

Session navigation restores the complete display history without loading or
repairing the model replay window; that work belongs to the next turn. Rich
documents retain their TextView while content changes, and TextView reuses its
wrapped rows until text or width changes. Feed messages and reports declare
`NT_FILL` width so scroll layout can reuse their formatted content at the
viewport width, even for short lines. Footer token totals use a narrow query
over the stored history.

`python tests/test_cli_resume_scale_pty.py build/bin/morph 1000 5000 10000`
reports resume, input readiness and page-up timings on a synthetic Markdown and
Unicode transcript. It also checks that the oldest and newest records remain
reachable, drafts survive scrolling and resize, and replay does not add records.
The CI case uses 1,000 turns and has no machine-dependent timing threshold.

`presentation/feed` owns streaming terminal output. `presentation/feed_block`
formats blocks for child transcripts and other text consumers and routes
interactive blocks to the chat page. The page mounts NT components, whose
margin and padding determine layout. Shared banner, list, plan and notice
formatting lives in `presentation/`.
Interactive trees reuse NT's tree view. Streaming and plain reports share
`presentation/tree.c` for branch prefixes, continuation rows and column limits.
Plain and JSON modes retain their separate output contracts.

Full-screen pages compose `components/page_layout`. Its fixed breadcrumb starts
with a clickable `morph` link back to chat, followed by the page name and the
current subpage. Ancestor links are underlined and the current location is bold.
Each page owns its return action and content state. The shared layout reserves
content space between its navigation and shortcut footer.

The full-screen `components/agents_browser` owns its split and stacked layouts,
filters, selection and detail scrolling. It indexes the borrowed snapshot once
per change so the native list can look up a visible row directly. The subagents
page refreshes snapshots, preserves the selected agent by ID and opens its
conversation.

## Execution and teardown

`main` initializes `cli_application`, invokes interactive, single-turn or JSON
execution, then shuts the same application down. `cli_application_tui_borrow` returns
its initialized base or NULL for a mode without a terminal host. It never
allocates another framework application.

During an active turn, submitted instructions queue for the next loop boundary.
Press Esc while messages are queued to interrupt the current model/tool wait and
apply them in FIFO order within the same turn. The unsent composer draft is
preserved. With no queued messages, Esc cancels the turn. During a background-job wait it
ends the wait/turn while retaining background commands; Ctrl+C always cancels
the turn and stops its commands.
Interrupted tools retain an explicit result indicating that side effects may
have occurred, so the agent can inspect state before retrying.

For interactive chat, Application initializes the worker and its embedded TUI
base, then presents the chat controller. NT owns the accepted controller. The
app borrows it to route events until the controller begins teardown.
The framework owns key decoding, alternate screen, paste, mouse/focus reporting,
resize and drawing. The TextField owns editing; the page owns submission, recall,
completion and command routing. Modal pages use the same controller stack.
Application owns initialization, run and deinitialization.

The prepare-termination hook rejects new owner calls, cancels and joins an active
worker and detaches its event queue before the framework destroys controllers
and terminal state. Each controller releases its page state as NT destroys it.
Subagent feeds and drafts belong to the session so reopening a page restores
them. Application releases the worker after the TUI loop ends. Plain/JSON modes reuse
the app's runtime, command services and output policies without opening a TUI.

## Extending the app

- Add a page in `pages/`, expose its open/factory function in its sibling header
  and dispatch it from a command or existing page using an NT view controller.
- A custom `*_view` embeds its NT superclass first and registers its matching
  `nt_view_class`. Use `nt_view_add` to construct it. Vertical page roots can
  inherit `nt_stack_view_class` to reuse measurement and layout.
- Declare local UI state on its owning view. Bind derived properties during
  construction. Flush pending state before a synchronous focus transfer.
- Add a reusable app component in `components/` and keep its state and drawing local.
  The NT sample builds a custom control and presents pages through view controllers.
  [Customization example](https://codeberg.org/oxund/nothing-tui/src/tag/v0.1.0/samples/customization/main.c)
- Add data operations in `models/`, business actions in `services/` and an
  external integration in `adapters/<integration>/`. Keep runtime access behind
  the runtime facade. Formatting belongs to `presentation/`.
- Register a command with a handler factory in `commands/<module>.c/.h`; register
  that factory in `commands/registry.c`. Navigation dispatches the app's pages;
  it never creates a private UI loop.
- Add the source explicitly to the matching CMake group and update this map if
  adding a new responsibility. Use `cli_<module>_*` private functions/types and
  `CLI_<MODULE>_*` constants (composer model and runtime observer use their full
  semantic names).

Run `cli_application_layout_conventions` and the CLI/TUI behavior regressions
when changing boundaries. PTY coverage includes modal cancellation, termination
with an active worker, slash-command spacing, recall, resize and terminal state
restoration. Keep the slash-command presentation contract in
[AGENTS.md](../../../AGENTS.md).

## Presentation ownership

The application owns the presenter, the input adapter and the runtime lifecycle.
The presenter manages stream buffers, Markdown rendering and artifact deduplication.
The input adapter owns cancellable stream reads and the per-application Readline
callback used for a blocking prompt. Chat owns its native TextField, recall
history, completion actions and adaptive TextView footer. Feed and turn services query final-output visibility
through the presenter interface.

The event queue only copies/transports items, wakes the owner and completes or
cancels owner calls. `adapters/runtime/dispatch` coordinates input suspension and
presentation, while notification formatting belongs to `presentation`.
Application pauses event/notification delivery before mounting interactive chat
and before terminal teardown. Startup events are retained until the page appears;
shutdown events cannot fall through to the parent shell. Owner calls remain
available for startup interactions. Plain and JSON modes retain their delivery
contracts. Resume instructions are printed once after terminal restoration.
The transcript model projects tool events and historical results into owned records,
retains reasoning positions, and resolves tool status without application state,
terminal output, workspace access or a clock. The transcript service supplies the
time and coordinates presentation. Widgets render const model/tool views; live
rendering receives its width from the feed. Call-time patch previews are captured
by `presentation/tool_preview`, so replay and redraw never reread today's files.
The `turn_details` page hosts a native scroll view and a text view bound to the transcript revision. Application controls
its terminal session and input handoff just like other modal pages.

The retained feed receives geometry from the framework. Legacy output adapters
use the shared terminal-size helper, including row fallback.

`/model` (or `/m`) replaces the editable composer with the inline Normal/Ultra
selector, using the same composer component as `/permissions`. Escape and
Ctrl+C restore the composer without a receipt. Confirming a change persists the
runtime collaboration mode; `/model normal`, `/model ultra`, `/model status` and
`/model <name>` also work in noninteractive modes. Ultra keeps a purple `»`
prompt and its original padding sweep, drawn only on framework-provided padding
canvases. The TextField measures its prompt and wrapped draft at the current
content width. Prompt color is separate from the draft style; no-color output
keeps the same geometry.

`cli_chat_panel_mount` builds the inline panel in the chat view tree. The panel
returns its initial focus target and receives its completion callback once.
`components/selector` supplies native lists and labels for the model and
permission pages. The subagent picker supplies its own activity list. Closing
the panel restores the draft, footer and input focus.

`/agents [id|shortname]` opens the full-screen agent browser, with a live status
summary, a scrolling roster and the selected task preview. Enter opens the
selected child's feed within the same terminal screen. Escape returns to the
agent browser, and Escape from the browser returns to the primary conversation.
Noninteractive `/agents` retains text snapshots.
`/subagents [id|shortname]` opens a compact agent picker within the composer area.
Selecting a child replaces the visible feed with that child's full-width
conversation in an alternate terminal screen. Escape opens its full-screen
agent browser. The `morph` breadcrumb returns directly to the primary feed.
`pages/subagents` owns navigation and follow-up input,
`models/subagents` reduces ordered child events, `presentation/subagents` composes
shared feed messages and tool rows, and `services/agents` enters the runtime
facade. Snapshot/live sequence frontiers prevent duplicate replay; child events
never enter the primary reducer. Shared message presentation retains user boxes,
reasoning, Markdown and code styles rather than displaying role-labelled dumps.
The picker uses lightweight activity snapshots and draws only the rows it needs.
Enter opens a child, Tab and Left/Right select agents in the picker or switch
threads in the child feed, `f` edits a follow-up, `k` interrupts, and `d` or
Ctrl+T toggles tool details. Each child retains its own transcript, viewport,
details setting and unsent draft across navigation. Primary output continues
reducing in the background and is reduced into the retained primary feed while the child is visible;
the primary draft is preserved. Scripts receive a text snapshot from `/agents`.

## Status presentation

CLI status glyphs and styles live in `presentation/style.h`. Pending uses `◯`
(dim), running `◉` (cyan), completed `●` (green), failed `✖` (bold red),
and awaiting user input `?` (bold yellow).
Cancellation/skipping uses `–` (dim); warnings use `⚠` (yellow). Neutral system
information uses `◇`. Message bullets may use `●` without indicating success.
Tool calls, subagents, background jobs, plans, task notifications, setup receipts
and configuration diagnostics follow the same mapping. Timeouts are errors;
missing historical results stay unknown/pending. JSON and plain event payloads
retain their existing contracts. The application layout convention test rejects
hardcoded status glyphs outside their shared definitions.
