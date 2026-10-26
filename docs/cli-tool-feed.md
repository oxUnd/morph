# CLI tool feed

Interactive tool output uses append-only blocks. The first row contains the
tool name (`Exec` for `exec`), its command or target, and inline elapsed time.
Tool names are never replaced by action verbs. Shell commands use the shared terminal-safe
syntax highlighter. Status remains visible in a leading marker: green `●` for success, cyan `◉`
for running, amber `●` plus `TIMEOUT` for timeouts, and red `⊗` plus `FAILED`
for failures. Shapes and explicit labels also distinguish states without color.
There is no separate summary/command row. Background process results retain a
running marker and label until the process finishes.

The compact preview contains at most three physical output lines. The first uses
`  └ ` and continuation lines align beneath its text. Additional lines are counted
and shown as `+ N lines (ctrl+o to expand)`. Text payloads from files and MCP content
blocks use the same limit. Long lines are clipped to the terminal width. Ctrl+O
retains complete arguments, output, errors, reasoning and patch diffs in the
existing details view. Plain and JSON event output keep their existing contracts.

`transcript.c` owns tool state and formats the feed. `terminal.c` owns only the
transient active block and its row accounting. On completion it clears that
block before appending the final block to scrollback.

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

## Visual review

Build and replay the deterministic concept fixture in a terminal:

```sh
cmake --build build
build/bin/morph-cli-pty-driver --replay-events tests/fixtures/cli_tool_feed.jsonl
```

Press Enter to close the replay. The fixture includes success, timeout, reasoning
and an active command. It performs no commands or network requests. The PTY
transcript test checks its visible rows at 100 and 40 columns, as well as normal
CLI streaming, details, scrolling and draft restoration. Terminal font, palette
and line height remain controlled by the user's terminal.
