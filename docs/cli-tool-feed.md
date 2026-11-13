# CLI tool feed

Interactive chat runs in alternate screen as a retained TUI ViewController.
The root Stack contains a scrolling feed, background activity, queued messages
and a multiline TextField composer. The framework owns geometry, clipping,
terminal modes and frame presentation. PgUp/PgDn and wheel scroll the feed;
scrolling to the bottom resumes following new content.

## Components and state

`widgets/message` composes a marker Label and content-sized TextView. Submitted
user messages own their outer margin and inner padding/background. Reasoning uses
muted italic text. Assistant messages retain source Markdown and remeasure it at
the current content width; trusted renderer SGR is converted into structured
text spans. Preview and completion update the same component.

`widgets/tool_call` composes a Stack, heading Label and content-sized TextView.
The transcript reducer retains tool state; call IDs locate existing controls.
Running, streamed and completed events update that control in place. The heading
contains status, tool name, target, outcome and completed elapsed time. Compact
output follows the last three logical stream lines, wrapped at the component's
content width. Ctrl+T toggles arguments, complete output, result and error in the
current turn's controls. Tool output is sanitized and cannot move the cursor.

Plan and notice components compose the same base controls. Plans retain explicit
tree branches and per-step status. Background jobs remain visible outside an
active turn. Queued messages use a List; Esc expedites queued steering, or
interrupts the active turn when nothing is queued. Ctrl+C cancels the turn;
Ctrl+D exits from an empty composer.

Component spacing comes from Stack spacing and actual four-edge View options. The framework
measures content after removing margin/padding, allocates outer slots, and clips
drawing. Sibling margins add. A background covers the frame and padding and
leaves the outer margin on the normal background. No feed component computes
its position by counting previously printed newlines.

## Status and editing

Glyph values are defined once in `src/util/terminal_status.h`, with CLI aliases
in `presentation/style.h`: pending `◯`, running `◉`, success `●`, failed `✖`,
question `?`, cancelled `–`, warning `⚠`, and neutral information `◇`. Failures
and questions use bold status colors. Historical calls without results stay
pending. Timeouts explicitly include `TIMEOUT`.

Working and background-job indicators share a monotonic 500 ms cyan/dim phase.
Completed history remains static. No-color output does not animate brightness.
A shrinking tool or a batch of completions changes feed layout without erasing
or reconstructing the editor. Resize remeasures both the feed and composer.

The TextField owns UTF-8 editing and multiline wrapping. Enter submits; Ctrl+J,
Shift+Enter and pasted newlines insert a newline. Up/Down recall submitted input.
Tab completes slash commands. Clipboard image insertion and dragged image-path
conversion reuse composer services. The footer retains permission, context,
credits and background naming state.

Modal pages share the root Application. Underlying feed updates and the unsent
draft survive navigation. Session restoration rebuilds message/tool controls from
canonical stored records, without executing tools or generating a new turn.

## Media boundary

The retained Canvas currently supports text cells and structured styles. Feed
media are represented by textual captions/paths; native Kitty image/video planes
require a dedicated retained media component with viewport and lifetime handling.
Legacy graphics writers are prevented from painting over the retained frame.
Image attachment and clipboard ingestion still use the existing composer model.

## Compatibility adapters

`widgets/feed_block`, `widgets/transcript` append functions and the old live-feed
renderer remain available to stateless document renderers, child transcript
views, replay fixtures and compatibility tests. Their text-output newline policy
is not the retained View box model. New primary interactive content uses the
retained controls above. Plain and JSON event modes keep their existing contracts.

The legacy Markdown write adapter observes terminating newlines and normalizes
the final boundary before duration output. The retained feed instead allocates
spacing from View margins; final duration is a separate retained receipt.
Composer replacement panels still use a shared row-rendering adapter on the same
Application. In retained chat they preserve the current alternate-screen session;
closing them restores the root tree, viewport and editor.

The deterministic compatibility fixture remains available for reviewing the
stateless renderer without commands or network requests:

```sh
cmake --build build
build/bin/morph-cli-pty-driver --replay-events tests/fixtures/cli_tool_feed.jsonl
```

`cli_retained_chat_pty_integration` exercises the actual retained chat at 120,
80 and 60 columns and the production no-color entry point. It covers UTF-8 paste,
completion and recall, live tool updates, details, draft anchoring, scrolling,
resize, panel return and terminal restoration. Framework tests separately check
content sizing, structured styles, padding backgrounds and dynamic view cleanup.
