# macOS build and debug

## Inspect and build the application

Find the actual `.xcworkspace`, `.xcodeproj`, or project generator manifest and
follow its workflow. If XcodeGen or Tuist is already used, edit its source manifest
and regenerate; do not maintain conflicting generated project edits. If creating
a new project, use available trusted tooling without making it a hidden dependency.
Ensure source/resource membership, app bundle metadata, entitlements, and a scheme
that builds the application. Do not fabricate a successful `.app` from a package
executable and an unverified property list.

Check `xcode-select -p`, `xcodebuild -version`, and the installed SDKs. These
examples assume a workspace; use `-project` for a project without a workspace.
Set variables to discovered paths and names before executing:

```sh
xcodebuild -list -workspace "$workspace_path"
xcodebuild -showdestinations -workspace "$workspace_path" -scheme "$scheme_name"
xcodebuild -showBuildSettings -workspace "$workspace_path" -scheme "$scheme_name"
xcodebuild -workspace "$workspace_path" -scheme "$scheme_name" \
  -configuration Debug -destination 'platform=macOS' \
  -derivedDataPath "$derived_data_path" build
```

Use resolved settings such as `TARGET_BUILD_DIR` and `FULL_PRODUCT_NAME` to locate
the app; do not assume it has the same name as the scheme. Run relevant tests
with the same workspace, scheme, and destination. Add a fresh
`-resultBundlePath` for a test run when diagnostic artifacts are useful.
Preserve `xcodebuild`'s exit status if formatting or recording its output.

## Failure triage

| Symptom | Inspect before changing code |
| --- | --- |
| No scheme, missing source, or missing resource | Workspace, shared scheme, target membership, generator manifest |
| Compiler or linker failure | First actionable error, target configuration, SDK and package versions |
| Signing or entitlement failure | Team, identity, target entitlements, provisioning and distribution intent |
| Window disappears or never shows | Entry point, controller ownership, activation, main-thread setup |
| Keyboard command does nothing | First responder, target/action, command validation, menu key equivalent |
| Layout conflict | Constraint identifiers, hierarchy, autoresizing masks, repeated activation |
| File works until restart | Sandbox permission, bookmark resolution and access lifetime |
| Leak or hang | Repeated reproduction, retained objects or blocked-thread sample |

Reproduce the specific issue, make a focused correction, and rerun the failing
check. Do not start by deleting all Derived Data or package caches, killing every
Xcode process, disabling sandboxing, or suppressing concurrency checking. If a
stale build is demonstrated, clean only the affected task-owned output.

## Runtime and performance

Launch the exact built bundle, for example `open "$app_bundle_path"`. A successful
`open` command only submits the launch request. Verify that the process remains
alive and the requested window and flow work using available GUI tools or XCTest
UI automation. Preserve relevant logs or crash reports for failures.

For changed UI, inspect resize, focus, keyboard commands, and the relevant close
and reopen behavior. For file features, verify cancel, denied access, save errors,
and relaunch with the user's content preserved. Select checks for the task.

For performance work, choose evidence that matches the symptom: Time Profiler or
a thread sample for CPU/hangs, Allocations or memory graphs for growth, and UI
instrumentation for responsiveness. Inspect `xcrun xctrace help` when using CLI
profiling; compare equivalent builds and flows. Do not infer improved performance
from shorter code or report a measurement that was not collected.

## Framework constraint and delivery

Review changed source, project files, and added dependency products for SwiftUI
imports, `NSHostingView`, and `NSHostingController`. A source search is an initial
check; inspect generated UI scaffolding and newly adopted libraries too. Do not
treat the OS framework's own internal implementation as app-authored SwiftUI.

Report build and test results and the actual `.app` path. For requested
distribution, verify the appropriate archive, signing, and entitlements. Local
launch does not prove notarization or distribution readiness, and disabling code
signing does not validate the real sandboxed release behavior.

## Sources

- [AppKit](https://developer.apple.com/documentation/appkit) for application APIs.
- [Xcode result bundles](https://developer.apple.com/documentation/Xcode-Release-Notes/xcode-11-release-notes)
  document result bundle output; use the installed `xcodebuild -help` and
  `xcrun xcresulttool help` for supported command syntax and result inspection.
- [Swift Concurrency Skill](https://github.com/AvdLee/Swift-Concurrency-Agent-Skill)
  is an optional specialist reference. Verify its version-dependent suggestions
  against the actual project and compiler before applying them.
