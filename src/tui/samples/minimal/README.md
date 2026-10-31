# Minimal TUI sample

`main.c` shows an application, a controller, a menu widget, deferred redraw,
resize handling, and terminal cleanup. It needs no model configuration or API key.

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

Public names follow their layer: `morph_tui_core_*`, `morph_tui_terminal_*`,
`morph_tui_render_*`, and `morph_tui_widget_*`. Header paths mirror the source
directories, such as `<morph/tui/widgets/list.h>`. File-local sample helpers use
the `sample_minimal_` prefix; `main` is the executable entry point.
