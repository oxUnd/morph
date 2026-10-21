---
name: macos-app-development
description: Build, modify, debug, and test native macOS applications using AppKit with Swift or Objective-C. Activate for Mac app windows, menus, desktop interactions, Xcode macOS targets, app bundles, and packaging. SwiftUI is prohibited. Excludes iOS-only apps and unrelated command-line tools.
---

# macOS App Development

Deliver working native macOS application changes. Swift is allowed; use AppKit
for the application UI. Use available file, patch, exec, and process tools, and
inspect tool schemas before use. Do not assume Xcode or GUI access is available.

## Task routing and delivery loop

Read only the matching references. Resolve paths against this skill's directory
from the active skill context, not the application working directory.

| Task | Reference |
| --- | --- |
| Visible UI changes, screenshot review, visual regressions | [UI inspection](references/ui-review.md) |
| Windows, menus, layout, documents, file access, Swift concurrency | [AppKit implementation](references/appkit-patterns.md) |
| Project creation, build failures, tests, runtime diagnostics, packaging | [Build and debug](references/build-debug.md) |

1. Inspect the app, toolchain, and deployment constraints. Define the concrete
   interaction or bug reproduction that will demonstrate success.
2. Implement a complete feature slice in the existing architecture. For new
   apps, establish a launchable AppKit window before building further features.
3. Build the app target, test changed behavior, then inspect the running UI when
   GUI access exists. Fix observed failures and repeat the affected checks.
4. Report results with the commands, artifact locations, and checks still blocked.

Use available tools directly. Do not assume Xcode MCP, third-party CLIs, or other
skills are installed. External research may clarify APIs; this skill's AppKit
constraint still applies to samples and suggested dependencies.

## Required UI framework

- Do not introduce SwiftUI, `import SwiftUI`, SwiftUI views or app lifecycle,
  `NSHostingView`, `NSHostingController`, or SwiftUI-backed dependencies for UI.
  Do not use SwiftUI as a temporary scaffold, preview, or fallback.
- Do not substitute Catalyst, a web wrapper, or a cross-platform UI framework for
  the requested AppKit app unless the user explicitly changes the requirement.
  Foundation, Observation, Combine, Core Data, and other non-UI frameworks are
  not SwiftUI merely because SwiftUI examples use them.
- Use `NSApplication`, an application delegate, `NSWindowController`,
  `NSViewController`, and AppKit views as appropriate. Programmatic layout,
  storyboards, and XIBs are allowed; follow the project's established approach.
- If existing code uses SwiftUI, implement new or replaced UI with AppKit.
  Identify the affected integration points and keep unrelated migration work
  outside the task. If the requested change cannot work without a broader
  migration, explain the dependency and clarify its scope before expanding it.

## Project and implementation

- Inspect repository instructions, Xcode project or workspace, schemes, targets,
  dependencies, deployment target, entitlements, signing, and UI conventions.
  Preserve existing Swift or Objective-C code unless the task needs a change.
  Prefer Swift with AppKit for a new native app unless the user specifies otherwise.
- For a new app, produce a buildable application target and bundle, with an
  AppKit entry point, window setup, menus, resources, and bundle metadata.
  A Swift package executable alone is not evidence of a packaged `.app`.
- Follow macOS desktop behavior: menu commands, keyboard shortcuts, first
  responder and focus handling, window resizing, and multiple windows or document
  lifecycle when relevant. Use Auto Layout and support accessibility and the
  appearance modes the app exposes.
- Keep AppKit updates on the main thread or main actor. Run expensive work off
  the UI thread and handle cancellation, errors, object lifetime, and closure
  capture cycles according to the project's concurrency model.
- For sandboxed apps, request only needed entitlements and use file panels and
  security-scoped access where required. Store credentials in Keychain and keep
  signing secrets out of source and logs.
- Verify unfamiliar APIs and deployment availability against the installed SDK
  or official Apple documentation; do not invent APIs or silently raise the
  deployment target.
- Before changing Swift concurrency code, inspect the language mode, strict
  concurrency, default actor isolation, and enabled upcoming features. An
  `async` function or `Task` does not by itself move CPU work off the main actor.
  Do not silence ownership problems with blanket `@MainActor`,
  `@unchecked Sendable`, or disabled diagnostics.

## Build and verify

- Check `xcode-select -p` and `xcodebuild -version`. Discover schemes and targets
  with `xcodebuild -list` using the actual project or workspace path.
- Build the relevant macOS scheme with an explicit destination and a known
  derived-data directory. Run relevant tests with `xcodebuild test` when the
  scheme supports them. Use `swift build` or `swift test` for package components
  when appropriate; these do not replace building the application target.
- Inspect changed source and dependencies for prohibited SwiftUI usage. Report
  pre-existing usage separately; do not claim full compliance if it remains.
- When GUI access is available, launch the built app and verify the changed
  interaction, window layout, focus, menus, and shortcuts. Inspect runtime logs
  for failures. Report a GUI check as blocked if it could not be performed.
- Report actual build, test, and app bundle paths. Distinguish a local build
  from a signed, notarized distribution. Use existing signing configuration for
  requested distribution work; do not change team identities, provision new
  credentials, notarize, upload, or publish without the user's instruction.

## Required visual verification

For changes to visible UI, read and follow [UI inspection](references/ui-review.md).
Run the affected flow, capture fresh screenshots, and actually analyze the pixels
with `img_qa` or available vision tools. Review the whole screen and detailed
regions, corroborate controls with available UI/accessibility hierarchy, and
reinspect after fixes. In Morph, use `img_qa` with `max_dim: 1024` for UI detail;
the current default of 360 can hide small text and spacing defects.

This is part of completing a UI change whenever runtime and vision access exist.
Report missing capture, interaction, or vision capability as a blocked check;
never infer visual acceptance from a build, screenshot path, or UI tree alone.
Backend-only changes do not require unrelated screen review.

## Research basis

Reviewed 2026-10-21. Apple platform documentation is the API authority.
[Axiom](https://github.com/CharlesWiltgen/Axiom) was reviewed for task routing and
diagnostics, and [Swift Concurrency Skill](https://github.com/AvdLee/Swift-Concurrency-Agent-Skill)
for settings-aware triage. This skill is independently written for Morph and
does not require those packages or adopt their SwiftUI workflows.
