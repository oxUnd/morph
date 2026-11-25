# CLI tool feed

Interactive chat runs in alternate screen as a retained TUI ViewController.
The page layout contains a scrolling feed, background activity, queued messages
and a multiline TextField composer. The framework owns geometry, clipping,
terminal modes and frame presentation. PgUp/PgDn and the mouse wheel scroll the
feed. While reading earlier messages, click **↓ Back to bottom** above the
composer or press Esc to jump to the latest message and resume following new
output. Scrolling back down also resumes following. The draft keeps its cursor
position.

## Components and state

`components/message` composes a marker Label and content-sized document. Submitted
user messages own their outer margin and inner padding/background. Reasoning uses
muted italic text. Assistant messages retain source Markdown and remeasure it at
the current content width; trusted renderer SGR is converted into structured
text spans. Preview and completion update the same component.

`components/tool_call` extends the CLI report view and renders tool state
in its NT text view.
The transcript reducer retains tool state; call IDs locate existing controls.
Running, streamed and completed events update that control in place. The heading
contains status, tool name, target, outcome and completed elapsed time. Compact
output follows the last three logical stream lines, wrapped at the component's
content width. Ctrl+T opens the original transcript panel, preserving the draft
underneath. Tool output is sanitized and cannot move the cursor.

Plan and notice components compose the same base controls. Plans retain explicit
tree branches and per-step status. Background jobs remain visible outside an
active turn. Queued messages use a List; Esc expedites queued steering, or
interrupts the active turn when nothing is queued. Ctrl+C cancels the turn;
Ctrl+D exits from an empty composer.

Component spacing comes from actual four-edge View options. The framework
measures content after removing margin/padding, allocates outer slots, and clips
drawing. Sibling margins add. A background covers the frame and padding and
leaves the outer margin on the normal background. No feed component computes
its position by counting previously printed newlines.

## Status and editing

The CLI defines status glyphs and colors in `src/sapi/cli/presentation/style.h`.
The markers are pending `◯`, running `◉`, success `●`, failed `✖`,
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

Clipboard shortcuts work alongside the terminal's own copy/paste bindings:

| Action | Shortcuts |
| --- | --- |
| Copy visible selection | Cmd+C, Ctrl+Shift+C, Ctrl+Insert, or right-click |
| Paste text, falling back to an image | Cmd+V, Ctrl+Shift+V, Shift+Insert, Ctrl+V |
| Paste an image explicitly | Ctrl+Cmd+V |
| Undo an edit or an entire paste | Cmd+Z, Ctrl+Z, Ctrl+_ |

Ctrl+C copies an active selection; otherwise it clears the draft and cancels the
active turn. Completing a mouse drag also synchronizes the selection to the
clipboard, so terminals that consume Cmd+C themselves can use that selection.
Terminal-managed paste still arrives as bracketed text and never submits pasted
newlines. Forwarded paste shortcuts read UTF-8 text from the host clipboard with
a five-second deadline and a 1 MiB limit. Remote clients should use their
terminal's paste binding to send their local clipboard.

The chat requests disambiguated modified keys through the
[CSI u keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/),
restoring the previous keyboard mode when leaving. Legacy terminals keep their
normal input behavior. CSI u and xterm modifyOtherKeys shortcuts are decoded
when forwarded; shortcuts consumed by the terminal remain terminal-owned.
Standalone modifier press/repeat/release reports preserve the text selection,
so pressing Cmd before Cmd+C does not discard the range being copied.

The original welcome banner, adaptive footer, message styles and work rows are
the visual baseline. Short feeds follow document flow; the composer reaches the
bottom only as the feed grows. Once allocated, transient feed space is retained
so a shrinking preview cannot pull a bottom-positioned draft upward. Composer
and message padding is painted by the View framework, never simulated with
blank output rows. Ultra decorates framework-provided padding canvases only.

Modal pages share the root Application. Underlying feed updates and the unsent
draft survive navigation. Session restoration rebuilds message/tool controls from
canonical stored records, without executing tools or generating a new turn.

Recaps appear on the normal background as a gray `↳ Recap:` line, with only
`Recap:` in bold and the summary immediately after it. Wrapped rows align with
the start of the summary. An optional `Next:` row stays under that same text
column. The component reflows on resize and owns a blank row above and below;
background completion preserves the editable draft and cursor.

`/mcp`, `/mcp list` and `/mcp status` produce a retained tree report. Service
status markers share the feed's left edge; transport, negotiated server/protocol,
capabilities, elapsed time and errors use explicit child branches. Tools,
resources and prompts use the same component, including resource URIs. Reports
own copied display snapshots and reflow their content on resize; right padding
belongs to the View options. Tree prefixes come from the framework tree control,
and long values wrap with continuation branches rather than table columns.

## Retained media

The Canvas records immutable image assets and ancestor-clipped placements.
The renderer owns Kitty, iTerm and Sixel transmission, placement, crop and cleanup
inside the synchronized frame. Widgets never write graphics escape sequences.
The welcome logo and direct image/video previews use Image controls. A rich
document composes TextView and Image controls from trusted Markdown renderer
output, preserving inline/table images and formula bitmap positions. Hyperlinks
remain structured span metadata and are emitted by the frame renderer.
Unsupported terminals and no-color mode retain textual paths/captions.
Image attachment and clipboard ingestion still use the existing composer model.

## Compatibility adapters

`presentation/feed_block`, `presentation/transcript` append functions and the old live-feed
renderer remain available to stateless document renderers, child transcript
views, replay fixtures and compatibility tests. Their text-output newline policy
is not the retained View box model. New primary interactive content uses the
retained controls above. Plain and JSON event modes keep their existing contracts.

The legacy Markdown write adapter observes terminating newlines and normalizes
the final boundary before duration output. The retained feed instead allocates
spacing from View margins; final duration is a separate retained receipt.
Inline selectors mount native views in the chat page. Closing a selector restores
the draft, viewport and input focus. [CLI implementation](../src/sapi/cli/README.md)

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
