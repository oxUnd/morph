# CLI application

The entire CLI is a `morph-tui` application. `struct cli_application` embeds
`struct morph_tui_application` as its first member and owns the opaque runtime,
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
    setup.h                    configuration wizard data
  pages/
    chat.c, .h                 chat controller and page-local editing state
    history, hooks, jobs, resume
    approval, question         modal interaction pages
    setup/                     wizard and its terminal controls
  widgets/                     feed, transcript, banner, list and shell styling
  commands/                    options, slash actions, registry and management entry points
  services/
    command_job, event_queue   workers, owner calls and wakeup bridge
    turn, scheduler, recap     runtime use cases and state coordination
    session_history            restore persisted conversation to terminal history
    management, skill_store, share_store
    configuration/             provider defaults, validation, roles and storage
  adapters/
    input/                     Readline, callbacks, keymaps, clipboard and reader
    runtime/                   event translation and startup observer
    protocol/                  runtime interaction request/reply mapping
    media/                     external images and terminal media rendering
  presentation/                output, presenter, history, diagnostics, Markdown and HTML
  CMakeLists.txt
  README.md
```

Names in the tree represent sibling `.c/.h` module pairs unless marked otherwise.
Each header owns its module's declarations; commands include their own handler
factory headers rather than declaring everything in the registry. Models do not
import application state. Configuration services do not import wizard pages.

## Execution and teardown

`main` initializes `cli_application`, invokes interactive, single-turn or JSON
execution, then shuts the same application down. `cli_application_tui` returns
its initialized base or NULL for a mode without a terminal host. It never
allocates another framework application.

For interactive chat, Application initializes the worker, creates the chat page,
initializes its embedded TUI base and presents the chat controller. Readline is
an external-input adapter; modal pages use key input on the same controller
stack. The Application owns initialization, run and deinitialization. Pages
supply controllers/options and handle page events.

The prepare-termination hook rejects new owner calls, cancels and joins an active
worker and detaches its event queue before the framework destroys controllers
and terminal state. Deferred chat output is flushed after terminal restoration;
the chat page and job remain alive until this completes. Plain/JSON modes reuse
the app's runtime, command services and output policies without opening a TUI.

## Extending the app

- Add a page in `pages/`, expose its open/factory function in its sibling header
  and dispatch it from a command or existing page. Use retained ViewControllers
  for new view trees; public controller adapters support existing renderers.
- Add a reusable app component in `widgets/`; keep its state and drawing local.
  The [customization demo](../../tui/samples/customization/README.md) shows Widget,
  ViewController and Application extension, semantic actions and shared models.
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
