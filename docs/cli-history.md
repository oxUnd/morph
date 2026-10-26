# History and context inspection

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

The inspector handles narrow terminals, resizes, bracketed paste, Ctrl-C and EOF;
it restores terminal modes and the main screen on return. Signal handlers restore
the screen before forwarding termination signals to their previous handlers.
Only changed viewport rows are painted, and idle inspection emits no bytes.

Validation includes runtime and adapter tests plus `test_cli_history_pty.py`,
which exercises 300 saved entries, summary ancestry, actual request capture,
Unicode search, filters, stored/request details, narrow/too-small resize, paste,
Ctrl-C and restoration of scrollback and the Readline composer.
