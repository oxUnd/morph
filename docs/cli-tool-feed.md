# CLI tool feed

Interactive tool output uses append-only blocks. The first row contains the
tool name (`Exec` for `exec`), its command or target, and inline elapsed time.
Tool names are never replaced by action verbs. Shell commands use the shared terminal-safe
syntax highlighter. Status remains visible in a leading marker: green `●` for success, cyan `◉`
for running, amber `●` plus `TIMEOUT` for timeouts, and red `⊗` plus `FAILED`
for failures. Shapes and explicit labels also distinguish states without color.
There is no separate summary/command row. Background process results retain a
running marker and label until the process finishes.

Command receipts, plans, approvals and MCP status share the large state markers:
`●` for completed or active, `◉` for running, `◯` for pending or available,
and `⊗` for failed. Selection lists retain filled versus hollow circles;
disabled, cancelled and system states retain their distinct symbols. Queue
headings use the pending marker. Password masks and Markdown list bullets
belong to their content and are not status markers.

The compact preview contains at most three physical output lines. The first uses
`  └ ` and continuation lines align beneath its text. Additional lines are counted
and shown as `+ N lines (ctrl+o to expand)`. Text payloads from files and MCP content
blocks use the same limit. Long lines are clipped to the terminal width. Ctrl+O
retains complete arguments, output, errors, reasoning and patch diffs in the
existing details view. Plain and JSON event output keep their existing contracts.

While a tool streams, the live block follows its last three output lines instead
of showing only `Running…`. Tabs and terminal controls use the same safe text
path as completed previews. The full stream remains available in Ctrl+O details.

Elapsed-time updates run once per second, with a steady running marker and muted
`Working` label. Incoming data requests a coalesced update after 80 ms. Idle
polling refreshes changed footer data without erasing the composer; an unchanged
footer emits no bytes. The footer uses bold and dim text rather than separate
colors for the model and directory.

Live layout uses the actual terminal width. Below 20 columns, a clipped
`Widen terminal` row replaces the live block until the window grows. Compact
headers and previews use a one-column ellipsis, including narrow Unicode text.
Normal conversation output continues to use native terminal scrollback.

Nonempty `NO_COLOR`, `TERM=dumb`, `--no-color` and redirected stdout disable
colors. Non-TTY stdin/stdout and dumb terminals use the existing line-oriented
input path and plain event presenter, without Readline repainting, graphical
output, or interactive key hints. JSON events retain their separate path.

Markdown normalization preserves literal fenced-code whitespace, repeated blank
lines, and fence-like source lines; only a matching, sufficiently long fence
without trailing source text closes a block.

These refinements apply the lifecycle, redraw and width guidance from
[tui-design](https://github.com/diegomarino/tui-design/blob/main/skills/tui-design/SKILL.md)
and the prompt/state separation in
[AI Coding Agents Terminal UI](https://github.com/vasilyu1983/AI-Agents-public/blob/main/frameworks/shared-skills/skills/ai-coding-agents-terminal-ui/SKILL.md).

`transcript.c` owns tool state and formats the feed. `terminal.c` owns only the
transient active block and its row accounting. On completion it clears that
block before appending the final block to scrollback.

Content and tool blocks share one blank row. The output writer tracks the
committed text's trailing newlines, including Markdown output. Live and completed
tools reuse an existing Markdown paragraph gap; code and table frames receive
the missing separator. Transient frame row counts follow the same spacing so
redraws preserve the composer position. Interior Markdown blank lines stay intact.

## Final spacing ownership

The Markdown SDK ends a normal paragraph with two newlines and a framed code
block with one. Previously `presentation_final()` unconditionally appended
another newline and `cli_terminal_turn_end()` prefixed the duration row with yet
another. A paragraph therefore left three blank rows before the duration.

The Markdown write adapter now observes the actual rendered tail without
changing the renderer's bytes. Only `cli_markdown_finish_final()` establishes
the final boundary: two terminating newlines, or one blank row before duration.
The successful duration row adds no leading separator. Other turn endings retain
their existing spacing. Paragraphs, lists, tables, fenced code and trailing input
newlines are checked through the real PTY event loop; interior paragraph/code
blank lines remain intact. User input, reasoning and answer styling are unchanged.

The committed duration row ends the turn with one blank normal-background row.
Command results own the same trailing gap. A command's first visible result is
preceded by one blank row, supplied lazily by the shared output writer; existing
leading result newlines do not add another row. Commands without output leave
only their trailing blank row. Submitted command lines add no leading gap, so a
command follows either kind of completed block consistently. Composer
padding belongs to the editable input and is cleared on submission; it must never
serve as the history separator. Finalizing work reserves the same gap before
commit so the composer stays on its row when background cleanup finishes.

## Visual review

Build and replay the deterministic concept fixture in a terminal:

```sh
cmake --build build
build/bin/morph-cli-pty-driver --replay-events tests/fixtures/cli_tool_feed.jsonl
```

Press Enter to close the replay. The fixture includes success, timeout, reasoning
and an active command. It performs no commands or network requests. The PTY
transcript test checks its visible rows at 100, 60, 40, 20 and 12 columns, as well as normal
CLI streaming, details, scrolling and draft restoration. Terminal font, palette
and line height remain controlled by the user's terminal.
