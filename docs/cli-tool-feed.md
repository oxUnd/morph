# CLI tool feed

Interactive tool output uses append-only blocks. A completed block contains a
status dot, tool label, action summary and elapsed time, followed by a muted shell
command and one result preview. Successful calls are green, timeouts amber,
failures red and active calls cyan. Elapsed time comes from the process result
when available, otherwise the tool call's monotonic clock interval.

`exec.description` is an optional short action summary supplied by the model.
Older calls use `Run command`; the actual shell source is always shown separately.
The preview is the first physical output line, clipped to the terminal width.
It does not attempt to infer a conclusion from command output. Other tools show
available result metadata. Ctrl+O retains complete arguments, output, errors and
reasoning in the existing details view. Plain and JSON event output keep their
existing contracts.

`transcript.c` owns tool state and formats the feed. `terminal.c` owns only the
transient active block and its row accounting. On completion it clears that
block before appending the final block to scrollback. Reasoning is committed as
one muted italic preview, with its complete text retained in details.

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
