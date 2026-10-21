# UIKit implementation patterns

UIKit owns the application UI and navigation. SwiftUI samples are conceptual
references only; implement the feature with UIKit without a hosting bridge.

## Scene, controller, and state ownership

- Separate application services from per-scene UI state. In a scene-based app,
  connect the window to its `UIWindowScene`, assign its root controller, retain
  it through the scene owner, and make it visible. Do not select an arbitrary
  globally connected scene to present a sheet or find a window.
- Keep the existing navigation architecture. Use UIKit containers for navigation,
  tabs, and split views; a coordinator is optional when routing complexity calls
  for one. Avoid replacing a small screen task with an architecture migration.
- For custom containment, use the full add-child, add-view, constrain, and
  `didMove(toParent:)` sequence. On removal, notify `willMove(toParent: nil)`,
  remove constraints and view, then call `removeFromParent()`.
- Model screen states and separate rendering from data operations. Avoid duplicate
  loading from multiple lifecycle callbacks. A screen becoming visible again is
  not always a request to discard edits and reload everything.
- Preserve persistent user data separately from UI restoration. For scene
  restoration, store enough identifiers and navigation context to reconstruct
  the scene, then handle missing or changed content gracefully.

## Layout, interaction, and lists

- Use safe areas and appropriate layout guides, semantic colors, and text styles.
  Set autoresizing-mask translation correctly for programmatic constraints. Keep
  constraints stable rather than adding new duplicates in every layout pass.
- Use the current view or window geometry and traits instead of a fixed screen
  size. Handle resizing, rotation, and supported iPad presentation; anchor
  popovers to the appropriate source view or bar item.
- Use keyboard layout support available to the deployment target or correctly
  convert keyboard geometry. Keep focused inputs and submission actions reachable.
- In tables and collections, use stable model identifiers. Reset reused cells,
  cancel or disambiguate image loads, and avoid using an obsolete index path as
  proof that an asynchronous result still belongs in the cell.
- Provide accessibility labels, traits, values, and focus behavior. Test larger
  text and longer strings; prefer semantic text styles and system control behavior
  over a screen built from fixed frames and custom gesture targets.

## Data, permissions, and lifecycle

- Keep persistence and networking behind the app's data interfaces. Make retries,
  cancellation, and duplicate submissions explicit; preserve existing storage
  formats and migrations. Do not wipe user data to make a migration pass.
- Handle permission denial and cancellation without trapping the user. Add
  required privacy usage descriptions for the specific APIs being used. Inspect
  applicable privacy manifest requirements for new APIs or SDK dependencies.
- Background transitions are not unlimited execution time. Persist important
  changes promptly and use platform background mechanisms only for eligible
  work. Protect sensitive content in snapshots when the feature requires it.
- For external documents, preserve access for the entire coordinated operation;
  use `UIDocument` or explicit security-scoped access and file coordination as
  appropriate. A saved raw file path is not persistent access authorization.

## Swift concurrency and lifetime

Inspect language mode, strict concurrency, default isolation, and enabled compiler
features before interpreting an actor diagnostic. Keep UIKit mutations on the
main actor, and isolate shared mutable services according to their ownership.

`Task` may inherit isolation, and `async` does not guarantee CPU work leaves the
main actor. Use execution boundaries supported by the installed compiler for
expensive work. Prefer structured concurrency and give unstructured tasks an
explicit owner and cancellation policy. After suspension, verify that a result
still belongs to the visible screen, account, or current request.

Clean up observations, timers, subscriptions, and callbacks at the appropriate
lifetime boundary. A task retaining a controller can prevent `deinit` from ever
running. Do not use unchecked sendability or blanket actor annotations merely to
remove diagnostics; establish the state ownership and synchronization invariant.

## Sources

- [UIKit and AppKit architecture](https://developer.apple.com/documentation/technologyoverviews/uikit-appkit)
  and [custom controller containment](https://developer.apple.com/documentation/uikit/creating-a-custom-container-view-controller).
- [App lifecycle](https://developer.apple.com/documentation/uikit/managing-your-app-s-life-cycle),
  [state restoration](https://developer.apple.com/documentation/uikit/restoring-your-app-s-state),
  and [background preparation](https://developer.apple.com/documentation/uikit/preparing-your-ui-to-run-in-the-background).
- [External document access](https://developer.apple.com/documentation/uikit/uidocumentpickerviewcontroller)
  for access scope and coordination.
- [Swift concurrency](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/concurrency/)
  for tasks, isolation, and cancellation; check installed compiler support.
