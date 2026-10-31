# Controls TUI sample

This sample combines a text field, a list, a status label, and a modal page.
Controllers embed the public ViewController base and only build their views
and handle semantic actions. Input routing, drawing, layout, focus, cursor,
resize, terminal modes, and cleanup belong to the framework.

From the repository root:

```sh
cmake -S src/tui/samples -B /tmp/morph-tui-samples
cmake --build /tmp/morph-tui-samples --target morph-tui-controls
/tmp/morph-tui-samples/bin/morph-tui-controls
```

The main build also provides `morph-tui-controls` when `BUILD_TUI_SAMPLES=ON`.
Run in a terminal; no model configuration or API key is required.

- The text field initially has focus. Typing `q` inserts text.
- Tab and Shift-Tab move focus between the text field and list.
- List navigation uses Up/Down or j/k. Enter activates a row; Enter in the
  field submits its text. Both open a modal page containing a copied label.
- Escape closes the modal and restores the original control's focus and state.
- Escape or Ctrl-C closes the root page. `q` closes it when the list has focus.
- Pasted text cannot execute page shortcuts, navigate the list, or submit.

See [the framework guide](../../../../docs/tui-architecture.md) for the
Application, ViewController, View/Widget, Layout, and Renderer extension
contracts. File-local helpers use the `sample_controls_` prefix.
