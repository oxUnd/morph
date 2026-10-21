# iOS build and debug

## Project and destination discovery

Inspect the workspace or project, shared schemes, app/test targets, deployment
targets, bundle IDs, scene and launch configuration, capabilities, and dependencies.
For generated projects, edit the generator manifest and regenerate with the
project's workflow. Do not introduce a new generator for an unrelated feature.
For a new project, verify source and resource membership, an application product,
a UIKit entry point, a visible root controller, and a buildable scheme.

Check `xcode-select -p`, `xcodebuild -version`, and installed SDKs. Use discovered
values for these variables; choose a simulator destination supported by the app:

```sh
xcodebuild -list -workspace "$workspace_path"
xcodebuild -showdestinations -workspace "$workspace_path" -scheme "$scheme_name"
xcrun simctl list devices available
xcodebuild -workspace "$workspace_path" -scheme "$scheme_name" \
  -configuration Debug -destination "platform=iOS Simulator,id=$simulator_udid" \
  -derivedDataPath "$derived_data_path" build
```

Use `-project` instead of `-workspace` where appropriate. For configured tests,
run the relevant test plan or selected tests with the same destination and a
fresh `-resultBundlePath` when useful. Preserve the original exit status when
recording output. Inspect the first actionable failure, not just `BUILD FAILED`.
Use resolved build settings to locate the `.app` and its bundle identifier.

## Simulator verification

Boot the chosen simulator only if it is not already booted. Confirm its readiness
and then install the exact application built for that destination:

```sh
xcrun simctl bootstatus "$simulator_udid" -b
xcrun simctl install "$simulator_udid" "$app_bundle_path"
xcrun simctl launch "$simulator_udid" "$bundle_id"
xcrun simctl io "$simulator_udid" screenshot "$screenshot_path"
```

Launch success and a screenshot do not prove the flow works. Exercise navigation,
input, submit/cancel, and the relevant failure case with available interaction
tools or XCTest UI tests. `simctl` alone is not a general-purpose tap/type UI
automation tool; do not invent subcommands. If no interaction mechanism exists,
report the launch and visual checks separately from the blocked flow validation.

Use unit tests for state transitions and data logic, XCTest UI tests for changed
interactions, and the project's existing framework for other tests. Prefer
deterministic fakes and observable conditions over arbitrary sleeps. Avoid
resetting simulator contents or uninstalling user data just to obtain a green run.

## Failure triage

| Symptom | Inspect first |
| --- | --- |
| No destination or incompatible SDK | Installed runtime, app deployment target, scheme and destination list |
| Missing source, resource, or package | Target membership, generated project source, dependency resolution error |
| Blank screen after launch | App/scene configuration, window retention, root controller, launch-time crash |
| Actor or Sendable diagnostic | Language mode, default isolation, actual ownership boundary |
| Constraint warnings or hidden controls | Conflicting constraints, autoresizing masks, safe area, keyboard geometry |
| Wrong image after scrolling | Stable identity, cell reuse, request cancellation and stale responses |
| Crash, leak, or hang | Reproduction, crash stack, object graph or thread trace |
| Device or archive signing fails | Team, profiles, entitlements and selected build destination |

Do not begin by deleting all Derived Data, killing every simulator/Xcode process,
disabling diagnostics, or changing signing identities. Separate an environment
failure from an application defect and rerun the original reproduction after fixing it.

For performance requests, use available Instruments or `xctrace` tooling and
measure a repeatable flow. Memory graphs and allocation traces answer retention
questions; time profiles and thread samples answer CPU/hang questions. Preserve
evidence and do not equate simulator timing with physical-device performance.

## Framework constraint and delivery

Review changed source, project files, and added dependencies for SwiftUI imports,
`UIHostingController`, representable wrappers, and SwiftUI app/scene scaffolding.
Inspect generated files too. Do not reject Foundation or Combine simply because
examples elsewhere use them with SwiftUI, or classify Apple's internal framework
implementation as app-authored SwiftUI.

Report the exact build, tests, simulator/device checks, artifact paths, and remaining
blockers. A simulator `.app` cannot be presented as a device-installable IPA.
For requested archives, use a device/archive destination and the user's signing
configuration, then validate export separately. An archive, an exported IPA, a
TestFlight upload, and App Store publication are distinct outcomes.

## Sources

- [UIKit](https://developer.apple.com/documentation/uikit) for platform APIs.
- [Xcode result bundles](https://developer.apple.com/documentation/Xcode-Release-Notes/xcode-11-release-notes)
  describe test artifacts; check installed `xcodebuild -help`,
  `xcrun simctl help`, and `xcrun xcresulttool help` for supported command syntax.
- [Axiom UIKit](https://github.com/CharlesWiltgen/Axiom/blob/main/.claude-plugin/plugins/axiom/skills/axiom-uikit/SKILL.md)
  offers specialist topics. Its SwiftUI integration routes do not apply here.
