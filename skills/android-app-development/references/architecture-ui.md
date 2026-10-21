# Android architecture and UI

Use the existing app's patterns first. For new work, make data ownership and
state transitions explicit without imposing a large architecture on a small app.

## State and data boundaries

- Keep one authoritative owner for each state value. Expose read-only UI state
  and send user actions back to its owner. A screen ViewModel is a useful default;
  a domain/use-case layer is justified by reuse or complexity, not by convention.
- Put network and persistent data access behind the project's data interfaces.
  Keep the UI from coordinating transactions or constructing service clients.
  Constructor injection is sufficient when a DI framework would add little value.
- A ViewModel survives configuration changes, not process death. Use
  `SavedStateHandle` or saved instance state for small reconstructible UI inputs,
  `rememberSaveable` for suitable Compose state, and durable storage for user data.
  Save identifiers and queries rather than entire data graphs.
- Specify refresh, retry, and duplicate-submission behavior. For search and reused
  rows, cancel obsolete requests or reject stale responses before rendering them.
  Account for concurrent edits or writes when the feature permits them.
- Keep migrations explicit and preserve user data. Do not replace a failing
  production migration with destructive database recreation.

## Concurrency and lifetime

- A `suspend` function can still block the main thread. Make blocking data work
  main-safe at its boundary; inject dispatchers when deterministic testing needs it.
- Use structured concurrency and scopes whose lifetime matches the work. Avoid
  `GlobalScope` for app behavior. Do not swallow coroutine cancellation as a
  normal network error or retry it indefinitely.
- Collect UI flows with lifecycle awareness: `collectAsStateWithLifecycle` for
  Compose, or `repeatOnLifecycle` with the Fragment view's lifecycle for View UI.
  A Fragment may outlive its view; clear view bindings and view-owned callbacks.
- Work that must survive leaving a screen needs an explicit longer-lived owner.
  Work that must survive process exit may need a platform scheduler such as
  WorkManager; an application coroutine scope alone is not durable scheduling.

## Compose path

- Keep composables declarative and safe to recompose. Hoist state to the owner
  that needs it; keep screen-level orchestration out of reusable components.
- Start effects from event handlers or the appropriate effect API. Choose keys
  according to when work should restart; release listeners with lifecycle or
  disposal handling. Do not launch requests in the composable body.
- Give lazy-list items stable identity. Before optimizing recomposition, measure
  the affected flow; do not blanket-annotate types as stable or immutable when
  their actual mutation and equality behavior violates those contracts.

## Views path

- Respect Activity/Fragment ownership and restoration. Use the app's existing
  navigation system; do not introduce a competing back stack for a new screen.
- For recycled list content, reset all visible state during binding. Identify
  items consistently and cancel or disambiguate image loads for recycled rows.
- Tie view observation and binding to the view lifecycle, not just the Fragment.

## Platform behavior and UI checks

- Assign inset ownership deliberately. Account for app bars, cutouts, system
  navigation, and IME without stacking padding that handles the same inset twice.
  Verify the target SDK's behavior rather than copying a version-specific workaround.
- Use window dimensions and configuration for adaptive layouts instead of a fixed
  phone canvas. Preserve system Back behavior and test return from deep links.
- Use Material components and semantic styling consistent with the app. Verify
  large text, TalkBack labels and focus order, dark mode, keyboard input, and
  reachable actions when the IME is open.
- For relevant changes, exercise rotation or resize, background and return,
  process recreation, denied permissions, empty data, slow or failed requests,
  and retry. Choose cases based on the feature; do not turn every patch into an audit.

## Official sources

- [Architecture recommendations](https://developer.android.com/topic/architecture/recommendations)
  for state ownership, UI/data separation, and proportionate layering.
- [Save UI states](https://developer.android.com/topic/libraries/architecture/saving-states)
  for the distinction between recreation, saved state, and persistence.
- [Coroutine practices](https://developer.android.com/kotlin/coroutines/coroutines-best-practices)
  for main safety, lifetime, dispatchers, and cancellation.
- [Compose effects](https://developer.android.com/develop/ui/compose/side-effects)
  for controlled work and cleanup in composition.
- [Edge-to-edge Views](https://developer.android.com/develop/ui/views/layout/edge-to-edge)
  and [Google's edge-to-edge skill](https://github.com/android/skills/blob/main/system/edge-to-edge/SKILL.md)
  for inset investigation. Check the actual toolkit and SDK before applying examples.
