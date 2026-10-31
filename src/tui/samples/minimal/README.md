# Minimal TUI sample

`main.c` builds a view tree (labels, a vertical stack, and a list), handles an
activation action, and enters the default application runner. The controller
embeds the public ViewController base. The framework owns layout, focus,
redraw, rendering buffers, signal handling, and terminal cleanup. The sample
needs no model configuration or API key.

From the repository root, configure the samples collection with CMake 3.20+,
a C11 compiler, pkg-config, and libuv 1.44+, then build this sample's target.
The parent build owns shared dependency setup; this directory owns only the
`morph-tui-minimal` executable. No model configuration or downloads are needed.

```sh
cmake -S src/tui/samples -B /tmp/morph-tui-samples
cmake --build /tmp/morph-tui-samples --target morph-tui-minimal
/tmp/morph-tui-samples/bin/morph-tui-minimal
```

Or enable the sample in the main build:

```sh
cmake -S . -B build -DBUILD_TUI_SAMPLES=ON
cmake --build build --target morph-tui-minimal
./build/bin/morph-tui-minimal
```

Run in a terminal. Use Up/Down or j/k to move, Enter to select, and q, Escape,
or Ctrl-C to exit. Pasted navigation cannot activate controls. Resize updates
the layout; exiting restores terminal modes.

Public names describe objects: `morph_tui_application_*`, `morph_tui_view_*`,
and `morph_tui_canvas_*`; controls use `morph_tui_widget_*`. Applications can
include `<morph/tui.h>` or individual object headers. Implementation layers stay
private. File-local sample helpers use `sample_minimal_*`; `main` is the entry
point.
