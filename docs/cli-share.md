# Share a conversation

Run `/share` while idle to save the latest answered turn in the current session
as `~/Downloads/morph-share-YYYYMMDD-HHMMSS-XXXXXX.html`. The command creates
Downloads if needed, never overwrites an existing file, and prints the saved
path. It does not open a browser, contact a model, or upload the conversation.
Open the file locally and take a screenshot if desired.

The document includes the original question and same-turn user additions, the
persisted tool results and received sub-agent messages in execution order, and
the final Markdown answer. Sub-agent mailbox deliveries and hook feedback are
internal process entries, never user questions. Each has its own labeled,
expandable entry; mailbox sender/recipient headers and report text are retained.
Only messages received by the selected parent turn are exported: child-session
transcripts and forked copies of earlier conversations are not appended.
New history records carry an explicit source. Older untagged mailbox deliveries
are recognized only by their steering-record key and complete transport header.
Explicit user provenance always takes priority, even when a user quotes a mailbox
header. Unmarked legacy user steering with the exact same transport envelope is
indistinguishable from old mailbox delivery and is treated as an internal entry.
Interrupted turns are skipped. Inactive records retained after context
compression are included. Legacy sessions without model history can export
their last stored question/answer pair, without reconstructed process details.
An empty session reports an explicit error and creates no file.

The process section starts collapsed. Each tool result can be expanded
individually; storage truncation is identified when present. Internal reasoning,
tool-call arguments, provider requests and system instructions are excluded.
Progress commentary that was not persisted as a tool result cannot be recovered.
Attachments and Markdown images are omitted in this first version. Question and
answer text are not summarized or rewritten.

The project PNG icon, stylesheet and theme controls are embedded in the HTML.
No CDN, network font or image fetch is required. Theme follows the operating
system initially; Light and dark color swatches let the reader switch, with the choice
remembered when browser local storage is available. Process disclosures work
without JavaScript. Layout adapts to narrow screens, and print hides the theme
controls. The footer records the actual share time with the local UTC offset.
Raw HTML in Markdown is disabled and process/question text is escaped.
Only HTTP(S), mailto and fragment links are retained. Image URLs are replaced
through the Markdown exporter resource callback and images remain hidden.

Implementation boundaries:

- `runtime_session_share_current()` owns read-only selection and holds the
  runtime execution lock for a consistent snapshot, returning `NB_ERR_BUSY` during
  an active turn. The query locates the latest answer and reads only its turn,
  without materializing unrelated history.
- The HTML renderer formats the immutable document. The share store publishes a
  uniquely named file atomically. [Renderer](../src/sapi/cli/presentation/share.c) ·
  [File publication](../src/sapi/cli/services/share_store.c)
- Markdown uses `morph-markdown-html`, including GFM, footnotes and native MathML.
  Its resource resolver applies the share-specific image and link policy; the CLI
  does not parse or modify the Markdown AST.
- `misc/share/` owns the approved visual tokens and offline theme behavior.
- `/share` uses the normal command dispatcher, including help, completion and
  JSON command events. Submission never becomes a chat message.

Session restoration context, compaction checkpoints and system-origin prompts
are not human questions and are omitted from the export. A user-authored request
to continue after restoring remains part of YOU. Loading a session with `/resume`
does not itself create a user message.

The same provenance classification is used by CLI history restoration. Received
agent mail restores as a grouped wait/report entry with agent paths and short
report previews, rather than a user chat bubble. Show details preserves the
original transport headers and full body; it starts collapsed.
