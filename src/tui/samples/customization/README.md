# Customization demo

Run this demo to see custom components, pages, and interaction routing together.
Every source file uses public TUI headers; it has no dependency on CLI adapters,
framework-private headers, model configuration, or API keys.

From the repository root:

```sh
cmake -S src/tui/samples -B /tmp/morph-tui-samples
cmake --build /tmp/morph-tui-samples --target morph-tui-customization
/tmp/morph-tui-samples/bin/morph-tui-customization
```

The main build also provides `build/bin/morph-tui-customization` with
`-DBUILD_TUI_SAMPLES=ON`.

## Try the three extension points

1. **Custom component.** The Counter initially has focus. Press `+` or Right
   to increase its value, `-` or Left to decrease it, and Space to switch the
   step between 1 and 5. It draws its own clipped progress display and emits
   semantic actions. Enter activates it and opens the Inspector.
2. **Custom page.** The Inspector reuses the same Counter in a horizontal
   layout. Change its value, Tab into the name field, press Ctrl+U to clear it,
   type a name, and press Enter to save. Escape returns with shared model changes
   applied and the original control focused. Unsaved Dashboard drafts survive
   help/modal visits unless the shared name was explicitly changed elsewhere.
3. **Custom interaction.** On the Dashboard, `r` resets the counter through a
   page event handler. `?` opens Help through the Application's fallback handler.
   Ctrl+L requests a redraw and posts a message back to the page. The name field
   has a directly bound action: Enter saves its text without a controller-wide
   dispatch branch. When a text field has focus, `r`, `q`, and `?` insert text.

Tab/Shift+Tab move focus. The Dashboard list offers the same page transitions
and reset action through list activation. Help scrolls with Up/Down. Escape
closes the active page; `q` also closes the Dashboard and Help when a text field
has not consumed it. Paste never executes controls, submit, or shortcuts.
Resize the terminal while visiting each page; layout and clipping are automatic.

Pages use vertical margins for space outside the page and horizontal padding
for space inside it. Configure edges independently through View options:

```c
const struct morph_tui_view_options inset = {
	.margin = {.top = 1, .bottom = 1},
	.padding = {.left = 1, .right = 1},
};
```

The layout includes margins in space allocation and clips drawing to the padded
content rectangle. Custom components use the same options; their draw callbacks
receive local content coordinates. `morph_tui_view_insets_all(1)` provides equal
edges, and `morph_tui_view_set_margin` / `_set_padding` update an existing view.
See the [box model guide](../../../../docs/tui-box-model.md) for measurement,
allocation, coordinate, and clipping rules.

The files below follow the shared [TUI application directory conventions](../../../../docs/tui-application-layout.md),
also used by the complete CLI application.

## Where to change the code

```text
customization/
  main.c                     application instance and root page
  application.h, .c          Application subclass, shared model, app shortcuts
  widgets/
    counter.h, .c            Widget subclass, drawing, keys, semantic actions
  pages/
    dashboard.h, .c          ViewController subclass, composition, bound action
    inspector.h, .c          another page/layout over the same shared model
    help.h, .c               modal documentation page
  CMakeLists.txt
  README.md
```

| Customization | Read first | Framework contract |
| --- | --- | --- |
| Component shape and behavior | `widgets/counter.c` | Embed `struct morph_tui_widget`; supply optional View methods; allocate with `morph_tui_widget_add_borrow` |
| Page structure and navigation | `pages/dashboard.c`, `pages/inspector.c` | Embed `struct morph_tui_view_controller`; provide build/action/event methods and an instance size |
| Component-owned input | Counter event method | Consume handled input with 1; return 0 to continue along the responder chain |
| A directly bound action | Dashboard name callback | `morph_tui_widget_set_action_borrow` overrides that control's controller action dispatch |
| Page-specific commands | Dashboard event method | Handle an unconsumed key without intercepting focused text input |
| Application-wide commands | Application event method | Receive input declined by the view tree and page; present Help or post a message |
| Shared state and appearance | Dashboard appear method | Sync explicitly changed model data while retaining drafts and focus |
| Owned resources | Application launch/terminate; Counter destroy | Model resources live through page teardown; widget buffers are released automatically with the view |

`launch` runs once during Application initialization, before root construction,
so the name buffer is ready when pages build. The same Application object owns
run/step, messages, window access, and shutdown. There is no public loop/runtime
handle to retrieve. Embedded hosts use `application_init` with `external_signals`
and later call `application_deinit`; subclasses can stop/join their workers in
`prepare_termination` while the stopped application is still initialized.

Custom factories participate in builder error collection. Page `build` callbacks
need no repeated cleanup branches: on error, the framework destroys registered
widgets and their resources. Mutating a custom widget should validate ownership,
update its state, invalidate its view, and optionally emit an action. A draw
method uses local Canvas coordinates and typed styles; it never writes ANSI or
terminal streams. Action text is borrowed for the callback duration and must be
copied if retained. Page/controller data is borrowed under the documented
lifetime rules in [the framework guide](../../../../docs/tui-architecture.md).
