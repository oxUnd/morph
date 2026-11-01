# CLI UI module conventions

This directory adapts the terminal UI framework to CLI state, runtime events,
commands, and process policy. Generic layout, widgets, input decoding, surfaces,
and terminal sessions belong in [`src/tui`](../../../tui/).

## Responsibilities

```text
ui/
  application.c, .h       Application subclass, root controller, panel lifecycle
  event_queue.c, .h       owner queue, wakeup bridge, synchronous owner calls
  runtime_events.c, .h    runtime event callback and owner dispatch
  recap.c, .h             recap UI state and presentation
  input/
    callbacks.c, .h       instance-owned libffi callback bindings
    cancel.c, .h          signal/cancellation bridge and cancel monitor
    clipboard.c, .h       platform clipboard image adapter
    composer.c, .h        draft images and expandable input model
    keymap.c, .h          owned Readline keymap cloning and cleanup
    reader.c, .h          cancellable stdio and synchronous prompt reads
    readline.c, .h        editing, bindings, completion, footer, session setup
  interactions/
    approval.c, .h        tool/operation approval and decision mapping
    question.c, .h        ask_user interaction and answer validation
  presentation/
    banner.c, .h          welcome banner and brand presentation
    feed_style.h          shared markers, colors, gutters, Readline style escapes
    image_loader.c, .h    Markdown image loading and temporary image lifetime
    list.c, .h            CLI list/tree formatting over TUI primitives
    markdown.c, .h        Markdown output, streaming, renderer lifecycle
    media.c, .h           rendered image/video node adapter and deduplication
    presenter.c, .h       runtime-to-feed presentation and chat/command spacing
    shell_style.c, .h     command syntax coloring
    terminal.c, .h        transient feed, work rows, composer frame coordination
    transcript.c, .h      retained tool/reasoning details and transcript display
  screens/
    history.c, .h         history inspector and report presentation
    hooks.c, .h           hook browsing and trust review
    jobs.c, .h            process/jobs page and live jobs presentation
    resume.c, .h          session picker, paging, and selection completion
```

`run.c` at the frontend root selects structured, plain, or interactive execution
and performs startup orchestration. Interactive Application ownership belongs in
`ui/application.c`; Readline editing and setup belong in `ui/input/`.

Command handlers parse arguments and dispatch application actions. Page geometry,
drawing, navigation, polling, and modal completion belong in `ui/screens/`.
Markdown rendering belongs in `presentation/markdown.c`; questions and approval
decisions belong in `interactions/`. JSON interaction transport remains in the
frontend's `interaction.c`, preserving its nonterminal protocol contract.

## Interfaces and names

- Each `.c` file has a sibling `.h` that owns its exported interface. Include
  that header first so missing interface dependencies fail at the implementation.
- Include another module's header directly when using its functions. Headers
  declare their own required types and do not include `internal.h`.
- Use project-relative CLI includes, such as
  `"sapi/cli/ui/screens/resume.h"`, and public framework includes, such as
  `<morph/tui/view_controller.h>`. Framework implementation headers stay private.
- Public adapters use the CLI domain prefix: `cli_application_*`,
  `cli_event_queue_*`, `cli_question_*`, `cli_approval_*`, `cli_markdown_*`, and
  the corresponding screen names. Callback closures retain `cli_callback_*`.
- File-local functions, private types, and constants carry the owning module:
  `cli_hooks_render`, `struct cli_resume_picker`, `CLI_TERMINAL_FRAME_MS`.
  Shared opaque state types keep the names declared by their owning headers.
- `internal.h` provides common context/utilities and CLI output macros. It owns
  no UI interfaces or domain/SDK include collection. `cli.h` carries frontend
  state and entry points; embedded state models include only their data headers.
- Register source files explicitly in `src/sapi/cli/CMakeLists.txt`, grouped by
  directory. Move declarations, includes, tests, and documentation together;
  remove old files and names rather than leaving forwarding headers.

## Dependency and lifecycle boundaries

Application coordinates input, presentation, command execution, and panels.
Workers enqueue events or synchronous owner requests; they do not mutate UI
state or draw. During termination, the Application rejects/cancels owner calls,
cancels and joins the command worker, then detaches the queue and restores the
terminal before presenting remaining output.

Framework code never imports CLI, runtime, agent, database, or configuration
modules. Frontend adapters may use their domain services and public TUI APIs.
Retained pages and legacy screen/prompt adapters follow the same directory
ownership rules; this organization does not change their event or JSON contracts.

See [TUI architecture](../../../../docs/tui-architecture.md) and the
[box model](../../../../docs/tui-box-model.md) for framework contracts.

## Checks

`tests/test_cli_ui_layout.py` verifies sibling headers, first includes, explicit
cross-module UI interfaces, private naming, CMake registration, command/root
ownership, and the framework/frontend dependency direction. It runs as the
`cli_ui_layout_conventions` CTest. Header compilation checks cover C/C++ with
and without Readline. The existing CLI unit and PTY suites verify interaction,
terminal restoration, command spacing, and structured output behavior.
