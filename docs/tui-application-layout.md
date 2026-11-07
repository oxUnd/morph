# TUI application directory conventions

A TUI consumer is an application that depends on `morph-tui`. Its root owns the
Application and its lifecycle. Organize the entire executable by application
responsibility, including commands, configuration and noninteractive modes.
The framework implementation layout (`core/`, `terminal/`, `render/`, `widgets/`)
is separate from the application layout.

```text
<application>/
  main.c                       process entry and bootstrap
  application.c, .h            Application subclass, composition and lifecycle
  application_internal.h       optional implementation conveniences
  models/                      shared application data and model operations
  pages/                       page controllers, navigation and modal interactions
    <feature>/                 cohesive multi-module feature, when needed
  widgets/                     application-specific reusable components
  commands/                    local command parsing and action dispatch
  services/                    use cases, persistence and asynchronous jobs
  adapters/                    external runtime, protocol, input and media bridges
    <integration>/             interfaces grouped by external responsibility
  presentation/                formatting, styles and output policies
  assets/                      application-owned static assets, when needed
  CMakeLists.txt                explicit sources and framework dependency
  README.md                    module map, entry points and extension guide
```

Create directories only when they have content. Small examples may keep one
`main.c`; the customization sample adds `application`, `pages` and `widgets`.
A larger app adds services, adapters and models using the same conventions.
Avoid a second `ui/` subtree with another Application beneath the real app.
Do not use generic `core.c`, `init.c` or `run.c` modules as mixed ownership buckets.

## Ownership and dependencies

- `main.c` parses process arguments, prepares logging and selects an application
  operation. Application owns its state, runtime handle, execution modes, workers
  and teardown. It embeds `struct morph_tui_application` first and uses that same
  base for initialization, run/step, presentation and shutdown. Noninteractive
  modes may leave the terminal framework uninitialized while using the same app.
- Pages compose views, keep editing/navigation state and bind semantic actions.
  Custom retained pages embed `struct morph_tui_view_controller` first. Existing
  external renderers/input libraries may use the public controller adapter;
  those pages still belong to the same Application controller stack.
- Widgets embed `struct morph_tui_widget` first when using retained controls.
  App-specific output components may use public drawing/terminal APIs. Rendering,
  key handling and local interaction state belong here; business actions go
  through page bindings or services. Shared margin/padding behavior follows the
  [View box model](tui-box-model.md).
- Models carry app data and operations on that data. They do not depend on the
  Application, pages, widgets or external runtime callbacks. Services apply use
  cases to models and the opaque runtime facade; workers submit owner-thread
  events rather than mutating views.
- Adapters translate external events, protocols and input into app actions.
  Readline integration belongs in `adapters/input/`, not in a page or framework
  implementation. Configuration validation/storage belongs to services, while
  its wizard and controls belong to pages.
- Presentation owns text/Markdown/HTML formatting and output policy. Commands
  dispatch pages or services; they never start a second event loop. The app owns
  the loop, page presentation and terminal restoration.

Prepare termination while the stopped framework is still initialized: stop new
owner-thread calls, cancel/join workers and detach wakeup producers. Then destroy
controllers and the framework, flush deferred output and release the app's
remaining resources. Borrowed app/page pointers must remain alive through their
callbacks. Import only public `<morph/tui/...>` headers; the framework never
imports app/domain headers. Do not duplicate runtime initialization, turn
execution, persistence or shutdown in a frontend.

## Modules and naming

Each implementation includes its sibling header first and explicitly imports
other module interfaces it uses. `main.c` includes `application.h` first.
Application-internal conveniences own no function declarations or domain SDK
collection. Headers must compile independently as C and C++ headers.

Use an app prefix and semantic module name, for example `cli_application_run`,
`cli_feed_create`, `cli_composer_model_expand_copy` and `cli_turn_execute`. Do not
expose framework implementation layer names in application-facing TUI APIs.
Private functions/types/constants also carry their module prefix; a runtime
observer can use `cli_runtime_observer_*` to distinguish it from other observers.
Static linkage still applies to private functions. No compatibility forwarding
headers or aliases are needed for internal module moves.

List source files explicitly in CMake, grouped by the directory map. Move
interfaces, includes, tests and documentation together. Behavioral tests stay in
`tests/`; repository-specific structural checks enforce ownership across the
whole application, including commands and services.

See the [CLI application](../src/sapi/cli/README.md) for a full consumer and the
[customization sample](../src/tui/samples/customization/README.md) for a small one.
