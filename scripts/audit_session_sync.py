#!/usr/bin/env python3
"""Audit offline database copies without reading or printing conversation text.

Use consistent SQLite backups, not a live database copied without its WAL.
Exit 0: no detected inconsistencies (not proof of identity); 1: invalid input;
2: inconsistencies found. Never repairs or changes the input databases.
"""

import argparse
from contextlib import closing
import json
from pathlib import Path
import sqlite3


KINDS = {
    "android": set("USER THOUGHT ACTION OBSERVATION FINAL ERROR IMAGE VIDEO PLAN "
                   "ASK_USER HITL_APPROVAL".split()),
    "ios": set("user thought action observation final error image video plan "
               "askUser hitlApproval".split()),
}


# Both mobile readers accept these legacy wire formats after the sync repair.
KINDS = {platform: set.union(*KINDS.values()) for platform in KINDS}

def open_snapshot(path):
    path = Path(path).resolve(strict=True)
    if any(Path(str(path) + suffix).exists() for suffix in ("-wal", "-journal")):
        raise ValueError("Use a standalone SQLite backup with no pending journal")
    # Immutable prevents SQLite from creating lock, WAL or SHM files.
    return sqlite3.connect(path.as_uri() + "?mode=ro&immutable=1", uri=True)


def audit(core_path, ui_path, platform):
    with closing(open_snapshot(core_path)) as core, closing(open_snapshot(ui_path)) as ui:
        for database in (core, ui):
            if database.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
                raise ValueError("SQLite integrity check failed")
        sessions = {row[0] for row in core.execute("SELECT id FROM sessions")}
        core_counts = dict(core.execute(
            "SELECT session_id, COUNT(*) FROM messages GROUP BY session_id"))
        core_messages = dict(core.execute("SELECT id, session_id FROM messages"))
        ui_counts = {}
        readable = {}
        unsupported = {}
        invalid_links = 0
        for sid, kind, message_id in ui.execute(
                "SELECT session_id, type, core_message_id FROM ui_messages"):
            ui_counts[sid] = ui_counts.get(sid, 0) + 1
            if kind in KINDS[platform]:
                readable[sid] = readable.get(sid, 0) + 1
            else:
                # Do not echo untrusted type values: report only counts.
                unsupported[sid] = unsupported.get(sid, 0) + 1
            if message_id is not None and core_messages.get(message_id) != sid:
                invalid_links += 1
        missing = sorted(sid for sid in sessions
                         if core_counts.get(sid, 0) and not ui_counts.get(sid, 0))
        unreadable = sorted(sid for sid in sessions
                            if ui_counts.get(sid, 0) and not readable.get(sid, 0))
        report = {
            "target_platform": platform,
            "session_count": len(sessions),
            "sessions_with_core_messages_but_no_ui_rows": missing,
            "sessions_with_ui_rows_but_no_readable_rows": unreadable,
            "ui_sessions_missing_from_core": sorted(set(ui_counts) - sessions),
            "core_message_sessions_missing_from_core": sorted(set(core_counts) - sessions),
            "unsupported_ui_message_count": sum(unsupported.values()),
            "invalid_core_message_link_count": invalid_links,
            "identity_verified": False,
            "limitations": "Local integer IDs cannot prove matching database origins. "
                           "Assets, model history and runtime caches are not checked.",
        }
        report["has_findings"] = bool(
            missing or unreadable or report["ui_sessions_missing_from_core"]
            or report["core_message_sessions_missing_from_core"]
            or unsupported or invalid_links)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", help="Standalone data.db backup")
    parser.add_argument("ui", help="Standalone ui-history.db backup")
    parser.add_argument("--platform", required=True, choices=KINDS)
    args = parser.parse_args()
    try:
        report = audit(args.core, args.ui, args.platform)
    except (OSError, sqlite3.Error, ValueError):
        print(json.dumps({"error": "Cannot audit input: check files, schemas, integrity and journals"}))
        return 1
    print(json.dumps(report, indent=2))
    return 2 if report["has_findings"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
