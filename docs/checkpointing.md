# Checkpoints and rewind

Morph saves a checkpoint before each primary user turn, before adding that
prompt to history or compacting the conversation. Checkpoints belong to a
session and persist when that session is resumed.

Run `/rewind` while the session is idle to choose a turn and what to restore:

- **Code and conversation:** restore captured files and return the conversation
  to the point before that turn.
- **Code only:** restore captured files while keeping the conversation.
- **Conversation only:** return the conversation to that point while keeping
  current files.

A successful single-mode rewind records a note in model context so retained
files and conversation are not mistaken for matching state.

The picker shows a preview before the mode selection. Escape cancels without
changing files or history. Rewinding does not submit the checkpoint's prompt or
call a model. The CLI reloads the displayed conversation after a conversation
rewind. There is no double-Escape shortcut.

Explicit commands also work without an interactive terminal:

```text
/rewind list
/rewind preview 42
/rewind code 42
/rewind conversation 42
/rewind both 42
/rewind help
```

A bare `/rewind` in non-interactive mode lists checkpoints and command usage.
Restore commands report errors and conflicting paths. An active turn must
finish or be cancelled before rewind can run.

## Captured changes

File contents are captured lazily when the primary agent uses `apply_patch`.
The first tracked edit records the file's preimage; subsequent editing attempts
record its resulting state, including partial failures. Returning to a turn
restores tracked edits from that turn and the later turns being rewound.
If a turn's captured edits have been rewound, or change tracking failed, start a
new user turn before making further captured edits.

This is independent of Git: Morph does not create commits, branches, stashes,
worktrees, or snapshots of the entire repository. Existing uncommitted work is
part of the preimage rather than something to discard.

Shell commands, manual edits, subagent changes, and other tools' writes are
outside this capture mechanism. They are not automatically undone. Before a
code restore, Morph compares affected files with their recorded state. A later
external edit produces a conflict instead of being overwritten; inspect and
resolve that conflict before retrying. Conversation-only rewind leaves files
alone even when a code rewind would conflict.

Checkpoints are retained per session, up to 100 checkpoints and 30 days.
Expired checkpoints cannot be selected. This feature is a convenience for
tracked local edits, not a substitute for version control or backups.

Individual captured files are limited to 16 MiB; a restore is limited to a
128 MiB budget. Symlinks, hardlinks, and non-regular files are rejected rather
than followed or replaced. When a code branch is no longer restorable, the
picker marks code modes unavailable; conversation-only rewind remains available
when that conversation checkpoint is valid.

If restoring files cannot be fully compensated after an error, the CLI names
the affected files and any saved recovery copies. Keep those copies and inspect
both files and session history before continuing.
If restoring succeeds but temporary-file cleanup fails, the CLI still reports
that cleanup failure and any retained recovery copy.

Rewind does not undo network requests, sent messages, database changes made by
tools, running external services, purchases, or model usage and billing. Review
those effects separately.
