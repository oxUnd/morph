#!/usr/bin/env python3
"""Audit offline database copies without reading or printing conversation text.

Use consistent SQLite backups, not a live database copied without its WAL.
Exit 0: no detected inconsistencies (not proof of identity); 1: invalid input;
2: inconsistencies found. Never repairs or changes the input databases.
"""

import argparse
import json
from pathlib import Path
import os
import shutil
import subprocess


KINDS = {
    "android": set("USER THOUGHT ACTION OBSERVATION FINAL ERROR IMAGE VIDEO PLAN "
                   "ASK_USER HITL_APPROVAL".split()),
    "ios": set("user thought action observation final error image video plan "
               "askUser hitlApproval".split()),
}


# Both mobile readers accept these legacy wire formats after the sync repair.
KINDS = {platform: set.union(*KINDS.values()) for platform in KINDS}

def _audit_executable():
    configured = os.environ.get("MORPH_SESSION_SYNC_AUDIT_BINARY")
    if configured:
        return configured
    built = Path(__file__).resolve().parents[1] / "build/bin/morph-session-sync-audit"
    if built.is_file():
        return str(built)
    installed = shutil.which("morph-session-sync-audit")
    if installed:
        return installed
    raise FileNotFoundError("Build morph-session-sync-audit or set MORPH_SESSION_SYNC_AUDIT_BINARY")


def _snapshot_path(path):
    path = Path(path).resolve(strict=True)
    if any(Path(str(path) + suffix).exists() for suffix in ("-wal", "-journal")):
        raise ValueError("Use a standalone SQLite backup with no pending journal")
    return path


def audit(core_path, ui_path, platform):
    if platform not in KINDS:
        raise KeyError(platform)
    core = _snapshot_path(core_path)
    ui = _snapshot_path(ui_path)
    result = subprocess.run(
        [_audit_executable(), str(core), str(ui), "--platform", platform],
        check=False, capture_output=True, text=True)
    if result.returncode not in (0, 2):
        raise ValueError("Cannot audit input: check files, schemas, integrity and journals")
    report = json.loads(result.stdout)
    if not isinstance(report, dict) or "has_findings" not in report:
        raise ValueError("Invalid audit report")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", help="Standalone data.db backup")
    parser.add_argument("ui", help="Standalone ui-history.db backup")
    parser.add_argument("--platform", required=True, choices=KINDS)
    args = parser.parse_args()
    try:
        report = audit(args.core, args.ui, args.platform)
    except (OSError, ValueError):
        print(json.dumps({"error": "Cannot audit input: check files, schemas, integrity and journals"}))
        return 1
    print(json.dumps(report, indent=2))
    return 2 if report["has_findings"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
