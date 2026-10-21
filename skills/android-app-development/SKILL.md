---
name: android-app-development
description: Build, modify, debug, and test Android applications. Activate for Android app features, Kotlin or Java Android code, Gradle Android projects, layouts, Jetpack Compose, emulator or device debugging, and APK or AAB builds. Excludes unrelated JVM server and desktop applications.
---

# Android App Development

Deliver working Android application changes in the user's project. Use the
available file, patch, exec, and process tools; inspect tool schemas before use.
Do not assume Android Studio, an SDK, an emulator, or device access is available.

## Task routing and delivery loop

Read only the references needed for the current task. Resolve relative paths
against this skill's directory from the active skill context, not the app's
working directory.

| Task | Reference |
| --- | --- |
| Visible UI changes, screenshot review, visual regressions | [UI inspection](references/ui-review.md) |
| State, navigation, Compose, Views, persistence, lifecycle bugs | [Architecture and UI](references/architecture-ui.md) |
| New project, Gradle failures, device checks, crashes, release issues | [Build and debug](references/build-debug.md) |

1. Inspect the app and toolchain; identify the module, variant, and affected flow.
2. Define observable success: the user action, resulting UI or data, and relevant
   failure case. For a bug, reproduce it or capture the exact diagnostic first.
3. Implement a small complete slice through UI, state, and data as needed. Keep
   the existing architecture; add layers or dependencies only for a concrete need.
4. Build, run relevant tests, and exercise that flow when execution is available.
   Use failures as evidence for the next change, then repeat affected checks.
5. Report what works, commands and results, artifact paths, and unverified behavior.

For unfamiliar APIs, SDK behavior changes, or toolchain upgrades, consult current
official documentation and project dependencies. If an official Android CLI is
already available, inspect its help before using its documentation or project
generation commands. Do not assume the historical SDK `android` utility is the
same tool, install extra skills automatically, or depend on unavailable MCP tools.

## Project and toolchain

- Inspect repository instructions, modules, the manifest, Gradle wrapper,
  version catalog, SDK levels, and existing UI and architecture conventions.
  Preserve the project's Kotlin or Java language and Compose or View UI stack.
- For a new native app, prefer Kotlin and Jetpack Compose. Choose Views when
  required by the user, integration, or dependencies. Do not migrate an existing
  UI framework as a side effect of a feature request.
- Use the checked-in Gradle wrapper. Check the installed JDK and Android SDK
  against the project's Android Gradle Plugin and Gradle requirements before
  changing versions. Do not guess version combinations or upgrade the toolchain
  merely to implement an unrelated feature.
- If starting from scratch, create a complete buildable project, including
  settings, module configuration, manifest, resources, and an application entry
  point. Use trusted tooling to generate wrapper files rather than fabricating
  a wrapper JAR. Keep machine-specific SDK paths out of committed files.

## Implementation

- Keep rendering separate from state and data access, following existing
  patterns. Model loading, empty, success, and failure states where applicable.
- Respect Activity, Fragment, and composition lifecycles. Scope asynchronous
  work appropriately; keep network, disk, and expensive work off the UI thread.
  Preserve state across recreation when the feature requires it.
- Declare only needed manifest components and permissions. Request runtime
  permissions at the point of use and handle denial. Check component export
  settings, intent inputs, and URI access when adding external entry points.
- Use resources for user-facing text, dimensions, themes, and assets. Account
  for system bars, insets, font scaling, accessibility labels, and the screen
  sizes and input methods relevant to the app.
- Do not embed service secrets in the APK. Use platform storage and credential
  facilities appropriate to the data; avoid logging credentials or personal data.

## Build and verify

- Discover module and variant names before selecting commands. For an `app`
  module with a debug variant, typical checks are
  `./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`.
  Adapt these tasks to the actual project and run checks relevant to the change.
- When a device or emulator is available, inspect `adb devices`, select an
  explicit serial if needed, install the debug APK, and exercise the changed
  flow. Check logcat for crashes and inspect the rendered UI for visual changes.
  Run relevant instrumentation tests when available.
- If execution is blocked by missing SDKs, network access, or devices, report
  the exact blocker and what was verified. Never equate compilation with a
  successful device run.
- Report the changes, verification results, and actual artifact paths. Build
  release APKs or AABs when requested; use the user's signing configuration and
  keep keys and passwords out of source and logs. Publishing is a separate action
  that requires the user's instruction.

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

Reviewed 2026-10-21. This is Morph-specific guidance, not a vendored upstream
skill. [Google Android skills](https://github.com/android/skills) informs task
selection; the references link official sources for implementation details.
Follow installed versions and current official documentation over dated examples.
