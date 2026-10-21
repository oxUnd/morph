# Android build and debug

## Discover before selecting commands

Inspect `settings.gradle*`, module `build.gradle*`, `gradle-wrapper.properties`,
`gradle.properties`, `libs.versions.toml`, manifest overlays, and CI commands.
Check `java -version`, `./gradlew --version`, SDK configuration, and connected
devices. Avoid printing a full environment or files containing credentials.

For a new app, use available trusted scaffolding and inspect its output. A
template is a starting point: confirm application ID, namespace, launch Activity,
themes, resources, wrapper, and a compatible plugin/JDK/SDK combination. Verify
version-specific APIs and dependency coordinates in official documentation.

These examples assume an `app` module and `debug` variant. Discover the actual
tasks and replace module and variant names where needed:

```sh
./gradlew projects
./gradlew :app:tasks --all
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
adb devices -l
```

Run the project's configured static checks rather than adding a new lint or
formatting stack for an ordinary feature. Keep the original exit status when
capturing logs; an output formatter must not turn a failed build into success.

## Diagnose from evidence

| Symptom | First evidence | Response |
| --- | --- | --- |
| Gradle or plugin cannot start | JDK, wrapper, plugin version, first root exception | Match the compatibility requirements; avoid unrelated upgrades. |
| SDK or dependency resolution fails | Missing package or coordinate, repository and network error | Fix that prerequisite; do not rewrite application logic. |
| Debug works, release fails | Release variant log, shrinker output, obfuscated stack and mapping | Reproduce in release; narrow reflection or serialization keep rules. |
| Crash or ANR | Exact device/OS, reproduction, stack trace or thread trace | Find the failing app frame or blocked main-thread work. |
| State lost after recreation | Saved-state path and reload behavior | Distinguish configuration survival from durable persistence. |
| Jank or memory growth | Repeatable flow and profiler evidence | Measure CPU, frames, or allocations before choosing an optimization. |

Do not begin with deleting caches, disabling shrinking, ignoring lint failures,
or clearing application data. These actions can erase evidence or hide defects.
After a fix, rerun the command or interaction that demonstrated the failure.

## Run on a selected device

Use the actual APK, application ID, and serial. Shell variables below must be
set from discovered project/device values, not copied literally:

```sh
adb -s "$device_serial" install -r "$apk_path"
adb -s "$device_serial" shell am start -W -n "$launch_component"
adb -s "$device_serial" exec-out screencap -p > "$screenshot_path"
```

Confirm install and launch results, then exercise the feature with available
automation or interaction tools. Capture relevant logcat around the failure;
do not clear global device logs or reset a user's device merely for convenience.
`adb` provides device control, not automatic proof that a UI flow was exercised.

Use unit tests for state/data logic and regression cases; Compose tests or
Espresso for app interactions, and UI Automator for system UI when needed.
For a configured instrumented suite, a typical task is
`./gradlew :app:connectedDebugAndroidTest`. Select an appropriate test target and
device; do not add every kind of test to a small change.

For performance requests, compare equivalent builds, devices, data, and flows.
Use available Android profiling tools and retain the trace or report. Report
measurements rather than unsupported claims that a change is faster.

## Artifact handoff

Discover outputs under the relevant module's build directory. Identify the exact
APK or AAB and variant; a successful debug APK build does not verify release
shrinking, signing, or store acceptance. For a release-related change, also test
the corresponding release behavior when the toolchain and signing allow it.
Give the user a concise distinction between built, installed, exercised, and
blocked checks, with the actual log/report paths for material failures.

## Sources and optional specialist lookup

- [Android architecture testing recommendations](https://developer.android.com/topic/architecture/recommendations)
  inform selecting tests for the affected layer.
- [Google Android skills](https://github.com/android/skills) contains task-specific
  guidance for tooling, testing, migrations, and R8. Consult the matching upstream
  resource when needed and available; do not automatically install its entire
  catalog or adopt toolchain migrations outside the user's task.
