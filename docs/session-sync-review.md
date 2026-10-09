# Session synchronization review and redesign

Status: implemented fixes and conservative session import, 2026-10-09.

## Implemented behavior

- Core provides a canonical transcript; both mobile clients validate their UI
  history against the transcript and a session identity before rendering/saving.
  Missing UI history recovers from bundled display events or core user/assistant
  messages. Read/decode failures are explicit and cannot overwrite source history.
- Core snapshots bundle a WAL-consistent UI projection. Imported display rows
  remain in subsequent snapshots even before the conversation is opened locally.
  UI identity mismatches are excluded; message anchors remain authoritative.
- Android and iOS accept each other's message/attachment encodings. Restoration
  invalidates old view state; mismatched local UI history is archived on replacement.
- **Database backups → data.db → Merge conversations** imports into the active
  database in one transaction. Local conversations are retained. IDs and stored
  relationships are remapped; model history, display events, memory and subagent
  history accompany the session. Repeating an identical source revision is skipped.
  A changed revision becomes a separate conversation; this is intentionally not
  an in-place bidirectional merge. Restore remains a separate replacement action.
- Downloaded snapshots retain existing hash validation. Unsupported newer database
  schemas, malformed records, orphan references and failed writes abort import.
  Configuration, credentials, scheduled jobs and permissions are not activated by
  session import. Import receipts preserve deduplication after re-export where the
  original identity is present; divergent histories are retained as branches.
- Default sync includes uploaded input and portable output paths. Android folder
  providers fail closed on read/list/publication errors; changing the folder resets
  its local sync baseline. Sync operations within a process are serialized.

## Compatibility limits

Old backups that never contained UI events can recover persisted user/assistant
text but cannot reconstruct missing thoughts or tool display records. Media must
also be present in the synchronized input/output folders; absolute paths from a
source device alone are not media data. Legacy UI rows without identity metadata
are checked against transcript anchors. Matching legacy content cannot establish
strong historical provenance. This patch does not implement a CRDT, automatic
in-place reconciliation, or an atomic cross-provider commit of all media blobs.

## Verification

Core regression tests exercise published-backup import, repeat import, collision
remapping, branch preservation, malformed-data rollback, subagent relations, WAL
snapshot display retention, corrupt chunks, provider failures and path mapping.
Mobile tests cover core-only recovery, cross-platform decoding, identity collisions,
unknown UI kinds, generation changes and media paths. Test totals and emulator
verification are reported with the delivered build.

Validation on 2026-10-09: the serial core suite passed 1,377 tests with 6
environment-dependent skips; an additional injected-write-failure rollback test
then passed in normal and ASAN builds. All 35 initial targeted ASAN tests passed,
as did all 5 importer tests after the extra rollback case. Android: 205 unit tests
passed and debug APK built. iOS: 69 selected simulator tests passed and app built.
Two process-timing tests initially failed during parallel host load; each passed
three isolated repeats and the complete serial rerun. No timing-test code changed.
The final Android APK was installed without clearing data on Pixel_9_Pro_XL.
The news conversation rendered after launch, and selecting the kitten conversation
from the drawer displayed its existing body. Backup/import tests used isolated
fixtures; no restore or merge was applied to the user’s real conversation database.

The following findings document the **pre-fix** implementation. The proposed
contract below remains a longer-term protocol direction, not a claim that every
item in that roadmap has been implemented.

## Reported symptom and evidence boundary

After synchronization/import reports success, a conversation appears in the
session list but cannot be opened correctly. The source device, exact import
operation and affected database pair have not been identified. The failures below
are established from code; the user's particular incident has not been reproduced.

## Findings

### P1: list and conversation body have independent authorities

Android `ChatSessionController.refreshSessionList` reads sessions from core.
`switchToSessionId` switches core by name but loads display messages exclusively
from `ui-history.db`, joining by the local integer session ID. A null history is
converted to an empty list. `AndroidUiHistory.load` returns null both for missing
messages and SQLite failures. Therefore missing data, decoding failure and a
legitimate empty session become indistinguishable.

iOS `MorphAgentSessions.switchSession` similarly obtains the session from core
and its display history from `MorphUIHistoryStore.loadPage`.

Restoring only `data.db` can produce a visible session with no display history.
An unrelated UI database with colliding integer IDs can instead display another
conversation. The latter cannot be ruled out by comparing row counts or IDs.

### P1: identical table names conceal incompatible serialization

Both frontends use `ui_messages` and UI schema version 2. Android saves enum names
such as `USER`, `ASK_USER`, `HITL_APPROVAL`; iOS saves `user`, `askUser`,
`hitlApproval`. Both loaders silently skip unrecognized values. A database with
valid rows can consequently render zero messages on the other platform.

Attachments also differ: Android uses uppercase types, `referenceId`, `source`,
`mimeType`, `sizeBytes`; iOS uses lowercase types (including `file`), `reference`,
`origin`, `mime`, `size`. Asset and draft tables differ too (`session_assets` /
`session_drafts` versus `ui_assets` / `ui_session_state`). Merely normalizing
message enum case is insufficient.

Evidence: Android `feature/chat/history/AndroidUiHistory.kt` and `ChatMessage.kt`;
iOS `Storage/MorphUIHistoryStore{Schema,Messages}.swift`,
`MorphUIHistoryStore.swift` and `Models/ChatMessage.swift`.

### P1: current database operation is backup/replacement, not session merge

`src/sync/sync.c:sync_pair` routes existing SQLite files to
`sync_db_backup_create`. `src/sync/db_backup.c` prepares and journals restoration
of an individual database. Android `MorphAgent.restoreSyncBackup` stops core,
applies one restore plan, restarts and commits it. This does not establish a
shared generation across core sessions, display messages and attachments.

Existing per-file rollback is useful but does not establish cross-file semantic
consistency. Runtime startup success does not prove that listed conversations
have readable display messages or valid media references. No record-level
session merge was found in this path; configuration merge is a separate feature.

### P1: failed reads can be persisted as empty history

After a failed/missing display load is converted to empty, a later session switch
saves the outgoing display. Android history save replaces that session's rows;
iOS save replaces the loaded sequence range. Preserve load failure as a distinct
state and prohibit saving an incomplete/failed view as authoritative history.

### P2: file portability and stale view state remain separate problems

Media paths in UI storage refer to device files, not portable asset identities.
Copying a database cannot make another device's absolute paths usable. Android's
default sync includes omit `input`; an explicitly configured work directory can
place output outside the `.morph` sync source. Restoring a database also needs a
generation signal to invalidate cached session messages and remembered selection.

## Proposed contract

Core runtime owns import/export, identity mapping, validation, publication and
recovery. Frontends render its portable history and retain only local presentation
state. No Android/iOS implementation of independent merge semantics.

1. Introduce stable globally unique session, message, turn and event identifiers.
   SQLite row IDs stay local implementation details. Export lineage/revision IDs;
   never infer identity from a name, timestamp or local integer ID.
2. Define a versioned portable history format with canonical message kinds,
   attachment metadata and structured event payloads. Preserve chronological
   order, tool call/result relations and model history separately from display
   history. Unknown optional events remain stored and render as a generic entry;
   unsupported required schemas fail explicitly.
3. Export immutable session revisions plus content-addressed asset blobs. Use
   logical asset references resolved to local paths. Do not copy source-device
   absolute paths into active references. Include uploaded and generated assets.
4. Publish a manifest with schema version, revision parents, file hashes, required
   objects and a commit marker only after all required objects are available.
   Readers ignore incomplete revisions; provider/listing errors are failures,
   never deletion evidence. Configuration/secrets remain a separate sync policy.
5. Import to staging, validate hashes, parse all required records, map global IDs
   to local IDs and validate relationships. Present counts of new sessions,
   updates, conflicts and missing assets. Preserve the current active data until
   validation succeeds. No success state for a session with missing required body.
6. Commit canonical session and display history in one core-owned transaction.
   Assets are immutable and staged first, so rollback may leave harmless unused
   blobs rather than broken references. Crash recovery exposes either the old or
   the new generation. Frontend caches invalidate only after commit.
7. Merge identical revision IDs idempotently; fast-forward only along known
   ancestry. Divergent conversation continuations create explicit branches, not
   an interleaved transcript or last-writer overwrite. Deletion uses tombstones
   tied to identity/revision; ordinary import must not erase local-only sessions.
8. Serialize manual sync, background sync and restore through runtime. Persist
   baselines per local replica and remote target identity. Switching folders
   starts a new relationship instead of reusing deletion history.

For backups requiring multiple local databases, snapshot them under a shared
write barrier and publish one generation manifest. Restore the complete bundle
with a recovery journal before exposing it. A set of independently timed SQLite
backups is not a consistent bundle.

## Recovery and rollout

First preserve affected databases using SQLite backups with writers quiesced.
Never attempt a destructive merge of the user's active files to diagnose this.

Add a typed history load result (`loaded`, `empty`, `missing`, `unsupported`,
`failed`) and block save on the last three states. Where core still retains a
faithful transcript, offer a marked recovered view through the runtime facade;
do not invent missing thought/tool events or overwrite the original UI history.
Loading a conversation and resuming its model context need separate validation.

Build legacy Android/iOS import adapters into staging. Explicitly map all message
and attachment variants. Assign legacy identities within a documented source
namespace and retain that mapping for repeat imports. Without provenance, keep
imports separate and surface ambiguity rather than matching equal local IDs.
Retain original backups until a validated import can open every imported session.

Only then replace raw UI database sync with the portable protocol. Keep the old
format read-only for migration; do not silently label database replacement as
merge. Report transport, validation and commit outcomes separately in the UI.

## Diagnostic and acceptance criteria

Run against standalone, consistent backups (not live main files without WAL):

```sh
python3 scripts/audit_session_sync.py /backup/data.db /backup/ui-history.db --platform android
python3 -m unittest discover -s tests -p 'test_session_sync_audit.py'
```

The diagnostic prints counts and local IDs, never message text. It detects missing
UI history, unsupported message kinds, orphan session references and invalid core
message links. Exit 2 means findings, 1 means invalid input, 0 means none detected.
It cannot prove matching origins, detect all same-ID collisions, inspect media,
or verify resumability. Synthetic fixtures cover both enum incompatibility
directions, missing UI history, mismatched references and read-only operation;
they are not device end-to-end reproduction.

Release gates for the new implementation:

- Android ↔ iOS, desktop → mobile and same-platform multi-device imports: every
  imported session opens with correct user/final text, ordered events and assets.
- Same local IDs with different origins never mix; renames preserve identity;
  repeated imports do not duplicate; local-only sessions survive.
- Core-only and UI-only imports fail validation or enter explicit recovery.
- Concurrent branches, unknown event kinds, compressed model history and pending
  tool interactions remain explicit; unsafe resume is blocked with a reason.
- Missing/corrupt blobs, failed SAF rename/list, permission loss, interrupted
  transfer and process death before/after commit preserve the prior active data.
- Import while a session is open cannot write stale cached history back afterward.
- Folder changes do not propagate false deletions; simultaneous sync requests are
  serialized; reported counts describe committed sessions rather than file copies.
