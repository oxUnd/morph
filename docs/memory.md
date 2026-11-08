# Memory and search

The runtime stores memory in SQLite. Explicit preferences have their own
authority and inheritance rules. Learned facts, past tasks and reusable tips
provide evidence for future work without becoming user instructions.

## Inspect memory

In an interactive CLI, `/memory`, `/mem`, or `/memory view` opens a temporary
panel on the existing TUI Application. The submitted command stays in chat
history as a plain command line. Closing the panel restores the composer.

| Section | Contents |
| --- | --- |
| Summary | Switches, project scope, counts, saved summaries and background work |
| Your preferences | Effective settings, scope precedence, revisions and sources |
| Saved facts | Explicit notes, current facts and inferred project knowledge |
| Reusable tips | Reusable experience with supporting tool observations |
| Past work | Past tasks, decisions, results, tools and artifacts |
| Not applied | Inferred preferences and legacy rules; these are not active settings |
| Memory updates | Current session's extraction jobs and errors |
| Memory preview | Most recently assembled memory context for the current session |

The inspector uses the same retained list control and list/detail interaction as
the jobs page. Each selected section shows a short explanation above the list.
Select a section with Up/Down, then press Enter to open it. Esc
returns to the list with its selection preserved; another Esc closes the panel.

| Key | Action |
| --- | --- |
| Up/Down, PgUp/PgDn, Home/End | Navigate the list or scroll opened content |
| Enter | Open the selected section |
| `/`, then Enter | Search facts, tips, past work and unapplied settings |
| `r` | Refresh records and job state |
| Esc | Cancel search, return to the list, or close the panel |

Search filters saved facts, tips, past work and unapplied settings, along with
their counts. Your preferences, memory updates and the reply memory preview
remain independently inspectable. Submit an
empty search to restore all project records. Values and sources are terminal
sanitized, and the document controls wrap to the available width.

`/memory show` retains the existing session text view, including in scripts and
JSON command mode. Preference editing remains available through
`/memory set <key> <value> [personal|project|session]` and `/memory unset`.
`/history --last-request` inspects the actual model request; an assembled memory
context is not proof that a request was sent.

## Summary first, details on demand

The active text model receives explicit preferences, up to four compact project
summaries, and a short directory of available records. Directory entries include
an exact type, ID, source session and memory version, plus a preview of at most
120 Unicode characters. The model reads complete records only when they help.
There is no extra foreground model call for input classification, translation,
segmentation, search planning or reranking. Query wording does not change what
is injected: the active model interprets the user's intent in any language.

The `memory` tool supports full record reads, searches, lists and UTF-8 pagination.
For a directory route `[notes#42]`, use:

```json
{"type":"notes","record_id":42,"scope":"project"}
```

For a missing route, the model chooses useful words or phrases. Whitespace
separates alternatives; there is no Chinese tokenizer or maintained vocabulary:

```json
{"type":"all","scope":"project","query":"composer redraw","max_results":20}
```

Bound SQL substring searches cover keys, values, details and sources, using
Unicode case folding and canonical normalization for language-independent matching. Equal
relevance prefers newer records. Search is a lookup primitive; semantic judgment
comes from the active model reading summaries and choosing its queries. No
embedding provider, vector service or SQLite extension is required.

Project reads require the same owner and a matching nonempty project binding.
A session already bound to an owner and nonempty project cannot be reassigned.
Frontend visibility checks run before deduplication, revision selection and
limits. Session-only notes, and summaries that could include them, are not
inherited by other sessions. Explicit `scope=all` reads retain owner and frontend
visibility checks and require an explicit user request.

`max_reply_chars` bounds injected context in Unicode characters. Explicit
preferences retain a minimum budget of 512 characters. Preferences and summaries
are selected as complete records; directory text is an explicitly shortened
preview. Count or size omissions have a tool recovery notice. Full values remain
available through paged reads, independently of the context budget.

## Explicit remember, correct and forget requests

The active text model interprets intent; C code contains no language-specific
preference trigger lists. `memory_preference` saves or unsets an explicitly
requested persistent working preference, in personal, project or session scope.
Temporary instructions, complaints, quotations and inferred wishes must not be
promoted to persistent settings. Ordinary `/memory set` and `/memory unset`
commands remain direct controls.

`memory_update` saves an explicit note, replaces a read record, or deletes one.
Notes default to project scope; `scope=session` keeps a note in the current chat.
Replace/delete require the exact record ID, type, complete current value and
memory version obtained by reading it. For task records and legacy rules, a
correction deletes the old record and saves a new note. Generated summaries are
read-only and are rebuilt from their remaining sources.

Both write tools require a quote from the latest actual user event; old messages,
assistant claims and tool output cannot supply that authority. This verifies
provenance, not linguistic intent: the text model decides whether the request
really asks for a persistent change. Tools confirm success only after storage
succeeds. Short connection guards keep concurrent tool writes indivisible.
Corrections/deletions advance the source version, fencing out older learning jobs.

## Two background stages

Both stages reuse `[model.text]` through an independent configured instance.
There is no separate model configuration. Each model request has an output
budget and a 30-second timeout; durable jobs provide retry and backoff instead
of retrying an HTTP request repeatedly inside the worker.

1. **Read recent work.** After a successful turn, capture recent actual user
   inputs and corrections, the answer, and a bounded tail of actions and tool
   observations. Reasoning traces are excluded. Extract useful facts, verified
   tips and task details; preserve scope, evidence and uncertainty. Learned
   settings stay under Not applied and never alter explicit preferences.
2. **Update the saved summary.** Merge current source records into a concise
   Markdown summary and navigation routes. Input is bounded to 128 records and
   24 KB, output to 1,200 Unicode characters. Returned source references must
   identify records actually supplied to the model. Each session contributes its
   own source-bound summary; project context combines only visible summaries.

SQLite remains the single authoritative store for records, summaries, versions
and jobs. `memory_notes` stores explicit notes; `memory_summaries` stores model
summaries with their source snapshot revision. This adopts the summary/directory
organization of coding-agent memories without a second file synchronization
system. Existing records remain readable while their summaries are generated.

Every source insert, update or deletion advances its session's content revision.
A summary whose source revision or memory version no longer matches is excluded
immediately. Writes check both again after the model returns, so an in-flight
summary cannot restore deleted or corrected content. Model text is treated as
historical evidence, and standard history redaction is applied before new memory
text and durable job payloads are stored.

Extraction results and the job's next stage commit in one transaction, so a
crash cannot leave committed learning with an old resume stage. If organizing
fails, recovery retries organizing without repeating the committed extraction
or task insert.
Durable payloads preserve actual tool observations, so a restart does not erase
the evidence needed to validate a tip. Recovery uses conditional claims, process
liveness checks, three attempts and a 30-second backoff. Failed catch-up work
does not create a new job on every input. `/memory retry` explicitly retries
failed updates in the current chat.

On later inputs, at most four changed, visible project sessions are queued for
background catch-up. Current summaries and already queued work do not consume
that budget. Opening or refreshing the panel makes no model request. With
learning disabled, existing notes, summaries and original records remain
inspectable; stale summaries remain excluded.

`/memory clear facts` also clears current-session explicit notes and learned
facts. `/memory clear tips` clears its tips and inferred rules. Any clear removes
the session's generated summary and invalidates older jobs. Clearing all also
removes remaining suggested settings and current-session preference overrides.
Personal/project preferences and other sessions retain their own scope.

## Reference implementations

The design follows the split between compact injected context, on-demand reads
and background organization, while retaining morph's existing storage and scope
contracts. Source inspection used:

- [Codex v2 read instructions](https://github.com/openai/codex/blob/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/ext/memories/templates/memories/read_path_v2.md)
- [Codex background extraction and organization](https://github.com/openai/codex/tree/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/memories/write/src)
- [Codex local substring search](https://github.com/openai/codex/blob/748ab55ca7de0b647d9a7a9d9a8e70e00f28666d/codex-rs/ext/memories/src/local/search.rs)
- [Claude Agent SDK CLI transport](https://github.com/anthropics/claude-agent-sdk-python/blob/2209f2c134fee914c0f75733b631a5fc238ca846/src/claude_agent_sdk/_internal/transport/subprocess_cli.py)
- [Claude Code memory documentation](https://code.claude.com/docs/en/memory)

The public Claude SDK transport delegates to the CLI; it does not expose the
CLI's complete auto-memory implementation. Claude's index/topic-file behavior is
therefore documented behavior, not a claim based on SDK engine source.

## Code names and older data

Memory code uses `task`, `tip`, `search`, `save_turn`, `reply_text` and
`memory_version` for the corresponding operations and records. The source files
are `memory_search.c`, `memory_report.c` and `memory_reply.c`.

New configuration uses `capture_input_enabled`, `save_tasks_enabled`,
`learn_from_chats_enabled`, `max_past_tasks`, `max_tips` and `max_reply_chars`.
Old configuration keys remain readable; the new key wins when both are present.
The memory tool accepts `past_tasks`, `tips` and `user_summary`, as well as the
older type names and `max_episodes` argument. Existing database table names,
queued jobs and model-result keys are decoded at their storage/API boundaries,
so previously saved memory remains usable.
