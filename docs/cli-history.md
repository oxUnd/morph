# History and context inspection

`/copy` copies the current session's last nonempty assistant answer to the system
clipboard. It uses the saved Markdown text, preserving code fences, Unicode,
line endings and trailing spaces. Tool results, preambles, reasoning and internal
summaries are excluded. Answers remain available after resume or compression;
switching to an empty session reports that there is no answer to copy. The command
runs after an active turn finishes, like the other local session commands.

macOS uses `pbcopy`. Linux tries `wl-copy`, `xclip`, then `xsel`; these tools retain
clipboard ownership as needed. Native writes have a five-second deadline. In SSH
or tmux the answer is also forwarded to the terminal with OSC 52. If native copying
fails, terminal forwarding is the fallback. A terminal-only request reports
`Copy unconfirmed`, because OSC 52 has no delivery acknowledgement. Its 100000-byte
limit fails explicitly instead of shortening the answer; native copies preserve
longer answers. Plain/JSON output never includes terminal clipboard escape codes.

`morph resume` starts with the session picker. Select with the arrow keys and Enter,
or cancel with Esc to return to a fresh composer. `morph resume <name|id>` accepts a
session name, display ID or numeric ID and fails if it does not exist. Sessions
remember their working directory: `morph resume <name|id>` restores it
automatically, while an explicit `--workdir` overrides it and rebinds the session.
Configuration options work before or after `resume`:

The picker groups sessions by working directory. The current directory's group
comes first; every other group follows by most recent activity, including a
single `unknown workdir` group for sessions without a remembered directory.
Group headers show a `▾` marker, the abbreviated path, the session count and a
`current` marker; sessions are indented beneath their group with `├`/`└`
branches, and Up/Down and page keys skip the headers. The plain fallback picker
uses the same groups and ordering. `/switch ^N` keeps its existing session
ordering.

```bash
morph resume
morph resume c923843e
morph resume --config ./dev.toml --workdir /path/to/project
```

After `/quit` or `/q`, interactive sessions print both `morph resume ...` and
`morphctl resume ...` commands below the goodbye message. Each command includes the
current session's display ID and configuration file. It also includes the working
directory when the session does not already remember it, with shell quoting for
paths containing spaces or quotes. `morphctl` loads its credentials and forwards
the resume arguments to `morph`. Unsaved fresh sessions have no resume hint.

Interactive session selection through `/resume`, `/resume <name|id>`, `/switch` or
`-s <name>` restores saved conversation text to native terminal scrollback. This
includes inactive messages from before context compression, assistant preambles
and saved tool summaries. Internal summaries and background receipts are omitted.
Ctrl+T inspects the latest restored turn's tool details and saved reasoning. Tools
with no saved result are marked accordingly; saved shell durations are displayed
when available. History restoration never generates a new turn or executes tools.
Saved original tool output is preferred over the shortened copy used by the model.
Older sessions without canonical history fall back to saved user/assistant text.
Plain output and NDJSON session-command contracts remain unchanged.

`/history` opens a read-only inspector in an interactive terminal. It shows the
saved model history: user and assistant messages, tool-call batches, tool results,
compaction summaries, and background receipts. Recent activity is selected first.
The temporary alternate screen leaves the conversation's native scrollback intact.

Use Up/Down, Home/End and PgUp/PgDn to navigate, Enter for details, `/` to search,
Tab to cycle All/Included/Summarized/Excluded, and Esc to return. Search matches
text, tool names, call IDs, entry numbers and stored arguments; ASCII matching is
case-insensitive and Unicode text is preserved. `v` switches a detail between its
stored content/metadata and the adapter-normalized replay fragment. The fragment
is not a complete, standalone request: tool-call batches and their results still
belong together in the replay window. `s` opens the last captured model input.
Filtering and inspecting records never changes their retention or execution.

Memory-consolidation scheduling/progress/completion notices are internal telemetry,
not conversation evidence. They remain UI/log events and are not written into new
model history. Database migration deactivates their old canonical receipts while
preserving the saved text; the replay and compaction paths also exclude imported
or otherwise stale memory-maintenance receipts. The inspector labels them `OUT`
even when older summary provenance references them. Existing summaries and past
request snapshots are historical artifacts and are not rewritten. Other useful
background receipts, such as completed build results, retain their replay behavior.

## Membership labels

| Label | Meaning |
| --- | --- |
| `IN` | A recognized, active item in the current model replay window. |
| `IN / TRIMMED` | An active result shortened before persistence. The unshortened original cannot be recovered from this record. |
| `VIA SUMMARY` | The original was an input to a summary in the active summary ancestry; its original text is not directly replayed. |
| `OUT` | Inactive, not a replayed record kind, or no surviving summary provenance is known. |
| `LEGACY` | Display history has not yet been migrated and validated for model replay. |

The heading says **estimated**. The runtime loads and validates active history
before a turn; new input, model changes and automatic compaction can change the
window. Histories requiring protocol repair are explicitly flagged. Token counts
are the stored history counts, not a forecast of the entire serialized request or
provider billing. Instructions, memory, loaded skills, tool definitions, framing
and the new input are additional. This avoids implying that every saved item, or
every item before a fixed cutoff, will appear verbatim in the next request.

Successful new compactions atomically record the exact source IDs alongside their
summary. Ancestry follows summary-to-summary links across repeated compaction.
Previously inactive/cancelled items are not retroactively claimed as summarized.
Old summaries lack this provenance and do not get invented coverage. The fallback
summary used after a summarizer failure also records no semantic coverage.

## Last submitted model input

Immediately before a structured foreground model dispatch, the agent captures
the adapter-normalized messages/input, instructions and tool definitions. One
replaceable snapshot per session survives restart; timestamps and turn IDs identify
it. Capturing does not mean the provider accepted the request. It is an input
snapshot, not a raw HTTP recording: authentication headers and generation options
are omitted, configured secrets are redacted, and inline binary data is omitted.
Simple legacy model callbacks do not produce structured snapshots. Capture errors
are logged without preventing the model call. Sessions without a snapshot say so.

The view is stable while open, and the next invocation reads a fresh runtime
snapshot. The runtime facade owns history classification and storage access; the
CLI owns only rendering, search and keyboard navigation. It does not duplicate the
agent's message assembly or infer summary coverage from sequence cutoffs.

## Plain output and compatibility

Non-TTY and dumb-terminal input stays line oriented. Explicit limits and flags
print ordinary output rather than opening the viewer, including in JSON event
mode's command-output envelope.

```text
/history 40                    Last 40 saved model-history items with labels
/history --all                 All saved model-history items with labels
/history --entry 318            Estimated replay fragment for one entry
/history --entry 318 --stored   Retained content and metadata for that entry
/history --last-request         Last captured adapter-normalized input
/history --messages             Previous user/assistant display transcript
/history --model [--all]        Existing raw model-history listing
/history --diagnose             Existing protocol diagnostics
/history --repair               Existing explicit history repair
/history --export path          Existing model-history JSON export
```

The inspector handles narrow terminals, resizes, bracketed paste, Ctrl+C and EOF;
it restores terminal modes and the main screen on return. Signal handlers restore
the screen before forwarding termination signals to their previous handlers.
Only changed viewport rows are painted, and idle inspection emits no bytes.

Validation includes runtime and adapter tests plus `test_cli_history_pty.py`,
which exercises 300 saved entries, summary ancestry, actual request capture,
Unicode search, filters, stored/request details, narrow/too-small resize, paste,
Ctrl+C and restoration of scrollback and the Readline composer.
