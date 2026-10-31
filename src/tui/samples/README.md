# TUI samples

Each sample lives in a named directory and owns its source, executable target,
and usage instructions:

```text
samples/
  CMakeLists.txt             shared standalone setup and explicit sample list
  README.md                  catalog and directory conventions
  minimal/
    CMakeLists.txt           morph-tui-minimal executable target
    README.md                purpose, build commands, and controls
    main.c                   executable entry point and controller
  controls/                  text field, list, and modal controller
    CMakeLists.txt
    README.md
    main.c
  customization/             custom components, pages, and interaction routing
    CMakeLists.txt
    README.md
    main.c
    application.c, .h
    widgets/                 reusable sample widgets
    pages/                   sample page controllers
```

- Use lowercase `snake_case` directory names and `morph-tui-<name>` target names
  (replace underscores with hyphens in target names).
- Every sample has `main.c`, `CMakeLists.txt`, and `README.md`. Add other source
  and header files beside `main.c` when needed. Larger samples group custom
  controls in `widgets/` and pages in `pages/`; put assets in `assets/`.
- The parent build owns shared dependency setup and lists samples explicitly
  with `add_subdirectory`. Each child build defines its target and links
  `morph-tui`; it does not duplicate framework or utility sources.
- File-local helpers use `sample_<name>_*`; `main` is the entry point. Follow
  the project's C conventions and include the public TUI module headers.
- Keep generated files in the chosen build directory.

| Sample | Demonstrates |
| --- | --- |
| [minimal](minimal/README.md) | View tree, list activation, automatic layout/drawing, and application runner |
| [controls](controls/README.md) | Text input, focus traversal, Unicode, paste, and modal focus restoration |
| [customization](customization/README.md) | Custom Widget/Application subclasses, custom pages, bound actions, page/app shortcuts, and shared state |

Build the collection with CMake 3.20+, a C11 compiler, pkg-config, and libuv
1.44+. Utility sources come from the project's shared manifest. The standalone
build does not configure agent, networking, database, or media targets and does
not download dependencies.

```sh
cmake -S src/tui/samples -B /tmp/morph-tui-samples
cmake --build /tmp/morph-tui-samples
/tmp/morph-tui-samples/bin/morph-tui-minimal
```

Or enable samples in the main build:

```sh
cmake -S . -B build -DBUILD_TUI_SAMPLES=ON
cmake --build build --target morph-tui-customization
./build/bin/morph-tui-customization
```

Run samples in a terminal. See each sample's README for its controls.
