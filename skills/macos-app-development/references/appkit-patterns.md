# AppKit implementation patterns

AppKit owns the UI. Translate external SwiftUI examples into AppKit components;
do not use a hosting view or controller to avoid that work.

## Window and controller ownership

- Separate application-wide services from window-local state. Keep an explicit
  strong owner for window controllers that must remain open; understand what
  releases them on close. Do not build an entire app in one delegate callback.
- A view controller manages content; a window controller manages the window.
  Create programmatic view hierarchies in `loadView` as appropriate, and use
  lifecycle callbacks according to the controller's actual visibility and reuse.
- For document apps, use the existing `NSDocument`/`NSDocumentController` model.
  Preserve open/save behavior, edited state, autosave, undo, and close prompts.
  A new window must not accidentally share another document's mutable UI state.
- For menu bar utilities, use AppKit status items and explicit popover/window
  ownership. Choose activation and Dock behavior for the product being built;
  do not turn a regular desktop app into an accessory app by accident.

## Native interaction and layout

- Route commands through target/action and the responder chain where appropriate.
  Preserve Edit menu operations, standard key equivalents, command validation,
  first responder, and keyboard focus. Prefer system controls for standard tasks.
- Use Auto Layout or the established layout system. For programmatically
  constrained subviews, configure autoresizing-mask translation correctly. Avoid
  repeatedly adding the same constraints from layout callbacks.
- Use semantic colors, scalable typography, and accessible names, roles, values,
  and actions. Check minimum window size, long text, appearance changes, keyboard
  navigation, and focus after sheets or popovers close.
- For tables, outlines, or collections, give items stable identity and reset reused
  content. Cancel or disambiguate asynchronous loads before updating a reused row.

## Files and persisted data

- Use open/save panels for user-selected files, respecting the app's entitlements.
  Persist security-scoped bookmarks when access must survive relaunch. Handle
  stale bookmarks and revoked access rather than silently saving a raw path.
- When explicitly starting security-scoped access, check success and balance it
  with `stopAccessingSecurityScopedResource()` after the operation, including
  error and cancellation paths. Keep access alive until asynchronous I/O finishes.
- Keep model persistence independent from window visibility. Make database or
  document format migrations explicit; preserve user content on failure.

## Swift concurrency and lifetime

First inspect actual compiler and target settings, including `SWIFT_VERSION`,
`SWIFT_STRICT_CONCURRENCY`, `SWIFT_DEFAULT_ACTOR_ISOLATION`, and enabled features.
The installed compiler version alone does not determine the source language mode.

- Treat UI-owned mutation as main-actor work. Keep shared mutable services behind
  an appropriate isolation boundary; pass values rather than live UI objects.
- `async` describes suspension, not guaranteed background execution. Check
  isolation inheritance before placing expensive parsing or image work in a task.
  Use a toolchain-supported execution boundary rather than unconditionally
  copying newer concurrency syntax.
- After an `await`, confirm that a result still belongs to the current selection
  or request. Actor isolation prevents data races, not every stale-result bug.
- Prefer structured child tasks for bounded parallel work. Give longer-lived
  tasks, observers, subscriptions, and timers an owner and a cleanup point. A
  task retaining a controller can prevent its `deinit` from ever cancelling it.
- Avoid blocking async work with semaphore waits. For unsafe sendability or
  isolation exceptions, establish a concrete synchronization invariant; a compiler
  warning disappearing is not evidence of correctness.

## Sources

- [UIKit and AppKit architecture](https://developer.apple.com/documentation/technologyoverviews/uikit-appkit),
  [NSWindowController](https://developer.apple.com/documentation/appkit/nswindowcontroller),
  and [NSViewController](https://developer.apple.com/documentation/appkit/nsviewcontroller)
  for framework ownership and lifecycle.
- [macOS sandbox file access](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox)
  and [security-scoped access](https://developer.apple.com/documentation/foundation/url/startaccessingsecurityscopedresource())
  for access lifetime and bookmarks.
- [Swift concurrency](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/concurrency/)
  for tasks, isolation, and cancellation; inspect installed compiler support.
