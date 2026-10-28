# Share a conversation

Run `/share` while idle to save the latest answered turn in the current session
as `~/Downloads/morph-share-YYYYMMDD-HHMMSS-XXXXXX.html`. The command creates
Downloads if needed, never overwrites an existing file, and prints the saved
path. It does not open a browser, contact a model, or upload the conversation.
Open the file locally and take a screenshot if desired.

The document includes the original question and same-turn user additions, the
persisted tool results in execution order, and the final Markdown answer.
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
  runtime execution lock for a consistent snapshot, returning `-EBUSY` during
  an active turn. The query locates the latest answer and reads only its turn,
  without materializing unrelated history.
- `src/sapi/cli/share.c` renders the immutable document; `share_save.c` handles
  atomic publication of a uniquely named file. Markdown uses `morph-markdown-html`, including GFM, footnotes and native MathML.
  Its resource resolver applies the share-specific image and link policy; the CLI
  does not parse or modify the Markdown AST.
- `misc/share/` owns the approved visual tokens and offline theme behavior.
- `/share` uses the normal command dispatcher, including help, completion and
  JSON command events. Submission never becomes a chat message.
