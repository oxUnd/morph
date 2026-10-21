# Android UI inspection

## Evidence-driven visual review

For changes to visible UI, exercise the affected flow and inspect its rendered
screens before declaring it verified. Read this reference when implementing or
reviewing UI. Build success, launch success, screenshot creation, UI-tree queries,
and passing interaction tests each provide partial evidence; none alone verifies
visual quality. Keep the review proportional to the changed screens and risks.

### Capture and actually look

1. Identify the affected screens and expected outcomes, using the user's design,
   reference images, or the app's existing conventions as the comparison target.
   Capture the current reproduction before editing when practical.
2. Launch the exact build under test on a selected device or desktop session.
   Reach each affected state through real interactions. Wait for an observable
   ready condition with a timeout; arbitrary sleeps or a stable hierarchy alone
   do not establish that content finished loading.
3. Capture a fresh screenshot of the target window or screen. Record build,
   device/OS, viewport, appearance, font scale, state, and the action that reached
   it. Keep unrelated desktop content out of captures where possible.
4. Send the screenshot to an available image-understanding tool and read its
   findings. In Morph, use `img_qa`; `img_info`, a file path, or a successful
   screenshot command does not inspect pixels. Check the current tool schema.
   The current `img_qa` default `max_dim` is only 360: use `max_dim: 1024` for UI
   review (current maximum), with enough output tokens for concrete findings.
   Do not assume the default preserves small text or fine alignment.
5. Inspect the whole screen for hierarchy, then examine header/navigation,
   content, inputs/actions, and edges/overlays separately. For unreadable text or
   dense regions, capture an element or crop the original with an available
   image utility and inspect that region separately. Preserve the full image
   and map crop coordinates back to it. Upscaling a tiny image does not recover
   detail. If detail remains unreadable, report uncertainty and obtain better
   evidence rather than guessing.
6. Use available accessibility/UI hierarchy inspection to corroborate labels,
   bounds, enabled/selected/focused state, and scrollability. Pixels reveal
   visual defects; semantic trees help explain controls and locate actions.
   Investigate discrepancies instead of treating either source as complete.

Example Morph call (use a real captured file path and task-specific expectations):

```json
{
  "file_path": "/absolute/path/to/captured-screen.png",
  "max_dim": 1024,
  "max_tokens": 2048,
  "prompt": "Review this application screen. Expected: the form and Save action are visible and usable. Inspect navigation, content, fields, action area, and screen edges separately. Identify clipping, overlap, alignment/spacing problems, unreadable text, missing assets, weak contrast, and obscured controls. For each finding give its region, visible evidence, and impact. Separate observations from uncertainty; do not invent unreadable text or infer successful interaction from pixels."
}
```

### Inspect states and behavior

- Review text wrapping/truncation, alignment, spacing, visual hierarchy, icons,
  image sizing, contrast, safe areas/insets, and controls hidden by overlays.
  Compare the actual result against concrete requirements or reference regions,
  rather than accepting a generic verdict such as "looks good".
- Exercise the changed primary action and applicable navigation, back/cancel,
  input validation, scrolling, focus, and dismissal. Capture meaningful resulting
  states, including applicable loading, empty, error, and permission-denied UI.
  Scroll long content to inspect lower regions rather than extrapolating from
  the first viewport. Do not trigger unrelated destructive or external actions.
- Select relevant supported variants: constrained size, larger text, appearance,
  orientation, or alternate input. Do not claim every configuration was tested
  or expand a small change into an exhaustive device matrix.
- Reobserve after navigation, scrolling, resizing, or presentation changes.
  Prefer stable semantic identifiers for actions; refresh bounds before any
  coordinate fallback and confirm the resulting state rather than issuing a
  long sequence of unobserved clicks.

### Repair and close the loop

Record each concrete defect with state, region, expected/actual behavior, and
screenshot path. Fix it, rebuild/relaunch as needed, repeat the same reproduction,
and inspect a fresh screenshot of the repaired state and adjacent affected UI.
Do not reuse a pre-fix screenshot as final evidence. For an existing screenshot
suite, compare equivalent environment/data and inspect the diff; update a golden
only after explaining the intended change, never merely to make a failure pass.

Report the states/configurations actually inspected, representative screenshot
paths, interaction results, remaining defects, and blocked checks. If capture,
interaction, or vision is unavailable, complete independent checks and state
which visual or behavioral evidence is missing. An unconfigured vision model is
a blocker to image analysis, not permission to claim the screenshot was reviewed.


## Android capture and focused checks

Use the selected serial and `adb exec-out screencap -p` command in
[Build and debug](build-debug.md). Use available UI Automator hierarchy inspection,
Compose semantics, or Espresso queries alongside pixels. A semantics/UI dump
may omit custom-drawn content; inspect its screenshot directly. Prefer existing
instrumentation or interaction tools; verify the installed UI Automator version
before using newer waiting, element-capture, or multi-window APIs.

Inspect edge-to-edge/system-bar insets, keyboard shown/hidden, focused field and
primary-action visibility, back navigation, permission dialogs, scrolling and
bottom sheets. For relevant layout changes, select a supported compact viewport
and larger system font/display scale; include rotation or multi-window when
affected. For theme/asset changes, inspect supported light/dark appearances.
Restore task-changed device settings after review.

Use existing screenshot tests when present; host-rendered component previews
supplement device interaction checks. Keep device density, fonts, OS, theme and
data equivalent for meaningful image comparisons. Do not introduce a new visual
test stack merely to inspect a small change.

## Sources

- [Android screenshot testing](https://developer.android.com/training/testing/ui-tests/screenshot)
  describes reference-image comparison and controlled capture conditions.
- [UI Automator](https://developer.android.com/training/testing/other-components/ui-automator)
  documents semantic queries, state waits, and screenshots. Hierarchy stability
  does not guarantee all background work has finished.
