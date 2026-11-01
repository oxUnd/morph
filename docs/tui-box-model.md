# TUI box model

Retained Views and Widgets share one box model. Built-in controls, custom
components, containers, and root pages use the same rules. Dimensions are
terminal cells; coordinates are zero-based.

## Allocation, frame, and content

```text
outer allocation / slot (includes margin)
+------------------------------------------------+
| margin: space outside the view                 |
|   +----------------------------------------+   |
|   | frame: view size, including padding    |   |
|   |   +--------------------------------+   |   |
|   |   | padding: space inside the view |   |   |
|   |   |   +------------------------+   |   |   |
|   |   |   | content / draw canvas  |   |   |   |
|   |   |   +------------------------+   |   |   |
|   |   +--------------------------------+   |   |
|   +----------------------------------------+   |
+------------------------------------------------+
```

The outlines show geometry, not automatically rendered borders.

| Region | Meaning | API contract |
| --- | --- | --- |
| Outer allocation | Space assigned by the parent, including margin | `morph_tui_view_measure` returns the preferred outer size; `morph_tui_view_arrange` receives the assigned outer slot |
| Margin | Space between the slot boundary and the view | Excluded from the stored frame and never drawn by that view |
| Frame | The view rectangle after removing margin | `morph_tui_view_frame` returns coordinates relative to the parent frame |
| Padding | Space between the frame boundary and content | Included in frame size; excluded from the draw canvas |
| Content | Area available to the component | `morph_tui_view_content_rect` returns coordinates local to the frame; drawing starts at `(0, 0)` in the content canvas |

The view leaves its margin and padding outside its draw canvas. Their appearance
comes from the enclosing surface or parent; configuring insets does not paint a
background or border.

## Configure edges

Include `<morph/tui/view.h>`. Both `margin` and `padding` are
`struct morph_tui_view_insets`, whose fields are `top`, `right`, `bottom`, and
`left`. Each field defaults to zero and must be nonnegative.

```c
const struct morph_tui_view_options box = {
	.margin = {.top = 1, .bottom = 1},
	.padding = {.left = 2, .right = 2},
};
```

Pass these options to any View/Widget constructor. For equal edges, use
`morph_tui_view_insets_all(1)` when assigning at runtime, or
`.padding = {1, 1, 1, 1}` in a static initializer. The previous scalar
`.padding = 1` must be replaced with an inset initializer.

Existing views can change their insets on the application owner thread:

```c
int rc = morph_tui_view_set_margin(view,
	(struct morph_tui_view_insets){.top = 1, .bottom = 1});
if (rc)
	return rc;
return morph_tui_view_set_padding(view, morph_tui_view_insets_all(1));
```

Changed values invalidate layout and display; repeated values do not schedule a
frame. Worker calls return `-EPERM`. Negative or overflowing edges return
`-EINVAL` without changing the existing options.

## Size and layout rules

`width`, `height`, and their minimum/maximum constraints describe the frame:
they include padding and exclude margin. Zero fixed dimensions mean intrinsic
size; zero maximum dimensions mean no maximum.

For an unconstrained intrinsic measurement:

```text
frame width  = content width  + padding.left + padding.right
frame height = content height + padding.top  + padding.bottom
outer width  = frame width   + margin.left  + margin.right
outer height = frame height  + margin.top   + margin.bottom
```

The framework applies fixed/minimum/maximum frame sizes before adding margins.
The parent then assigns actual space. Stack containers account for margins when
distributing `grow` space, respecting maximums, aligning children, and clipping
to available space. A root receives the viewport as its outer slot and applies
its own margin once.

Sibling margins add; they do not collapse. In a vertical stack with enough space,
the distance between adjacent frames is:

```text
previous.margin.bottom + stack spacing + next.margin.top
```

The horizontal equivalent uses right and left margins. Hidden children consume
no space, including margins or stack spacing. If the allocation is too small,
frame and content extents clamp to zero; insets never produce negative sizes.
An unrepresentable measured size returns `-EOVERFLOW`.

For example, assign an outer slot `(0, 0, 40, 9)` with:

```c
const struct morph_tui_view_options box = {
	.margin = {.top = 1, .right = 2, .bottom = 1, .left = 3},
	.padding = {.top = 1, .right = 2, .bottom = 1, .left = 2},
};
```

The stored frame is `(3, 1, 35, 7)`. The content rectangle local to that frame is
`(2, 1, 31, 5)`. The draw callback receives a `31 × 5` canvas with origin
`(0, 0)`; its first cell maps to `(5, 2)` in the parent's coordinates.

## Custom components and layouts

The View base performs inset accounting. Custom implementations follow these
boundaries:

- A `measure` callback receives available content width and returns intrinsic
  content size. Do not add padding or margin; the framework adds them.
- A `layout` or layout-manager `arrange` callback uses
  `morph_tui_view_content_rect(container)` to obtain the usable local rectangle.
  Place child outer slots relative to its `x` and `y` offsets.
- Call `morph_tui_view_measure(child, available_width, &size)` when planning child
  space. The returned size already includes that child's margin.
- Pass the allocated outer slot directly to `morph_tui_view_arrange(child, slot)`.
  Do not subtract the child's margin first; arrange applies it exactly once.
- A `draw` callback draws at content-local coordinates. Do not add padding to
  canvas coordinates. Parent and terminal clipping are already inherited.
- Wrapping, paging, and scrolling use content dimensions rather than outer frame
  dimensions. This keeps the visible viewport consistent with drawing.

See the [customization sample](../src/tui/samples/customization/README.md) for
pages combining vertical margins with horizontal padding.

## CLI chat spacing

The current CLI composer adapter uses the same distinction for submitted chat
boxes: colored inner padding belongs to the box; one normal-background blank row
above and below it provides outer separation. Adjacent feed blocks reuse an
existing separator rather than inserting another blank row.

Reasoning previews, committed reasoning, answers, and tool output start beyond
the box's bottom margin. Streaming-to-committed transitions preserve their
position. Local slash commands retain their separate plain-line and result-gap
rules described in [TUI architecture](tui-architecture.md).

## Validation

`tests/test_tui_views.cpp` covers asymmetric insets, root and child geometry,
custom allocation, growth and maximum sizes, alignment, hidden children,
runtime setters, owner-thread checks, and bounded empty frames.
`tests/test_cli_reasoning_pty.py` and `tests/test_cli_composer_cleanup_pty.py`
check unpainted chat margins, wrapped/multiline input, color modes, and stable
reasoning positions. Answer tests in `tests/test_cli_presentation.cpp` verify
that streaming and non-streaming answers reuse the submitted input separator.
