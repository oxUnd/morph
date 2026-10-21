---
name: ios-app-development
description: Build, modify, debug, and test native iPhone and iPad applications using UIKit with Swift or Objective-C. Activate for iOS app features, view controllers, navigation, Xcode iOS targets, simulator or device debugging, and app archives. SwiftUI is prohibited. Excludes macOS-only apps and unrelated Swift packages.
---

# iOS App Development

Deliver working native iOS application changes. Swift is allowed; use UIKit for
the application UI. Use available file, patch, exec, and process tools, and
inspect tool schemas before use. Do not assume Xcode, simulators, or connected
devices are available.

## Task routing and delivery loop

Read only the matching references. Resolve paths against this skill's directory
from the active skill context, not the application working directory.

| Task | Reference |
| --- | --- |
| Visible UI changes, screenshot review, visual regressions | [UI inspection](references/ui-review.md) |
| UIKit screens, navigation, scenes, layout, state, Swift concurrency | [UIKit implementation](references/uikit-patterns.md) |
| Project creation, Xcode failures, simulator checks, crashes, archives | [Build and debug](references/build-debug.md) |

1. Inspect targets, dependencies, toolchain, deployment, and supported devices.
   Define success as an actual user flow or a reproducible bug and its correction.
2. Implement a complete slice using the existing architecture. For a new app,
   get a UIKit root controller launching before expanding the feature set.
3. Build, test changed behavior, and exercise the flow on an available simulator
   or device. Fix observed failures and repeat affected checks.
4. Report commands, results, artifact locations, and any unverified behavior.

Use available tools directly. Do not assume Xcode MCP, third-party simulator
automation, or other skills are installed. External examples must be translated
to UIKit when they use SwiftUI; do not introduce a hosting bridge to reuse them.

## Required UI framework

- Do not introduce SwiftUI, `import SwiftUI`, SwiftUI views, SwiftUI `App`/`Scene`
  lifecycle, `UIHostingController`, or SwiftUI-backed dependencies for UI.
  Do not use SwiftUI as a temporary scaffold, preview, or fallback.
- Do not replace the requested native UIKit app with a web wrapper or a
  cross-platform UI framework unless the user explicitly changes the requirement.
  Foundation, Observation, Combine, Core Data, and other non-UI frameworks are
  not SwiftUI merely because SwiftUI examples use them.
- Use `UIApplicationDelegate`, `UIWindowSceneDelegate` where appropriate,
  `UIWindow`, `UIViewController`, and UIKit views. Programmatic layout,
  storyboards, and XIBs are allowed; follow the project's established approach.
- If existing code uses SwiftUI, implement new or replaced UI with UIKit.
  Identify the affected integration points and keep unrelated migration work
  outside the task. If the requested change cannot work without a broader
  migration, explain the dependency and clarify its scope before expanding it.

## Project and implementation

- Inspect repository instructions, Xcode project or workspace, schemes, targets,
  dependencies, deployment target, device families, entitlements, signing, and
  existing navigation and state conventions. Preserve existing Swift or
  Objective-C code; prefer Swift with UIKit for a new native app unless specified.
- For a new app, produce a complete buildable iOS application target, including
  UIKit launch and scene configuration, root view controller, bundle metadata,
  assets, and required usage descriptions. Do not return isolated view code as
  though it were a complete app.
- Respect view controller containment, presentation, navigation, and scene
  lifecycles. Keep state and data access separate from rendering according to
  the project. Handle loading, empty, error, and permission-denied states.
- Use Auto Layout and safe areas. Handle keyboards, Dynamic Type, VoiceOver,
  localization, and supported orientations. Validate iPad layouts and multitasking
  when the target supports them; do not silently narrow supported devices.
- Keep UIKit updates on the main thread or main actor. Move expensive work off
  the UI thread and manage cancellation, background transitions, and retain cycles.
- Request permissions when needed with accurate usage descriptions. Keep
  credentials in Keychain and use appropriate file data protection. Do not embed
  service secrets or log credentials and personal data.
- Verify unfamiliar APIs and deployment availability against the installed SDK
  or official Apple documentation; do not invent APIs or silently raise the
  deployment target.
- Before changing Swift concurrency code, inspect the language mode, strict
  concurrency, default actor isolation, and enabled upcoming features. An
  `async` function or `Task` does not by itself move CPU work off the main actor.
  Do not silence ownership problems with blanket `@MainActor`,
  `@unchecked Sendable`, or disabled diagnostics.

## Build and verify

- Check `xcode-select -p` and `xcodebuild -version`. Discover schemes with
  `xcodebuild -list` using the actual project or workspace. Inspect available
  destinations and `xcrun simctl list devices available` before choosing a
  simulator; do not hardcode a device model or runtime that may not be installed.
- Build the relevant scheme for an available iOS Simulator destination with a
  known derived-data directory. Run relevant unit and UI tests with
  `xcodebuild test` when supported. A simulator build does not prove that device
  signing or an archive for distribution works.
- Inspect changed source and dependencies for prohibited SwiftUI usage. Report
  pre-existing usage separately; do not claim full compliance if it remains.
- When a simulator is available, boot the selected device, install and launch
  the built app with `simctl`, and exercise the changed flow using available UI
  automation or manual interaction tools. Inspect screenshots for visual changes
  and runtime logs for crashes. Installation alone does not verify interaction.
- Report changes, completed checks, actual artifact paths, and any missing SDK,
  simulator, signing, or device access that blocked verification.
- For requested archives or exports, use the user's signing and export settings.
  Do not change team identities, provision credentials, upload to TestFlight, or
  publish to the App Store without the user's instruction.

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
[Axiom UIKit](https://github.com/CharlesWiltgen/Axiom/blob/main/.claude-plugin/plugins/axiom/skills/axiom-uikit/SKILL.md)
was reviewed for problem-oriented routing, and
[Swift Concurrency Skill](https://github.com/AvdLee/Swift-Concurrency-Agent-Skill)
for settings-aware triage. This skill is independently written for Morph and
does not require those packages or adopt their SwiftUI workflows.
