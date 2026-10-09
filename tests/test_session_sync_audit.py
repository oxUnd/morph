"""Offline fixtures for the session sync review diagnostic."""

import importlib.util
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "audit_session_sync", Path(__file__).resolve().parents[1] / "scripts/audit_session_sync.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SessionSyncAuditTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.core = Path(self.directory.name) / "data.db"
        self.ui = Path(self.directory.name) / "ui-history.db"
        with closing(sqlite3.connect(self.core)) as db, db:
            db.executescript("CREATE TABLE sessions(id INTEGER PRIMARY KEY);"
                             "CREATE TABLE messages(id INTEGER, session_id INTEGER);"
                             "INSERT INTO sessions VALUES(1);"
                             "INSERT INTO messages VALUES(10,1);")
        with closing(sqlite3.connect(self.ui)) as db, db:
            db.execute("CREATE TABLE ui_messages(session_id INTEGER, type TEXT, core_message_id INTEGER)")

    def insert(self, sid=1, kind="USER", mid=10):
        with closing(sqlite3.connect(self.ui)) as db, db:
            db.execute("INSERT INTO ui_messages VALUES(?,?,?)", (sid, kind, mid))

    def test_core_only_restore_is_not_empty_session(self):
        report = MODULE.audit(self.core, self.ui, "android")
        self.assertEqual([1], report["sessions_with_core_messages_but_no_ui_rows"])

    def test_ios_rows_are_compatible_on_android(self):
        self.insert(kind="user")
        self.assertEqual([], MODULE.audit(self.core, self.ui, "android")[
            "sessions_with_ui_rows_but_no_readable_rows"])
        self.assertFalse(MODULE.audit(self.core, self.ui, "ios")["has_findings"])

    def test_android_rows_are_compatible_on_ios(self):
        self.insert()
        self.assertEqual(0, MODULE.audit(self.core, self.ui, "ios")["unsupported_ui_message_count"])

    def test_orphan_and_wrong_message_identity(self):
        self.insert(sid=2)
        report = MODULE.audit(self.core, self.ui, "android")
        self.assertEqual([2], report["ui_sessions_missing_from_core"])
        self.assertEqual(1, report["invalid_core_message_link_count"])

    def test_consistent_input_is_not_mutated_or_claimed_identity_verified(self):
        self.insert()
        before = {p.name: p.read_bytes() for p in Path(self.directory.name).iterdir()}
        report = MODULE.audit(self.core, self.ui, "android")
        self.assertFalse(report["has_findings"])
        self.assertFalse(report["identity_verified"])
        self.assertEqual(before, {p.name: p.read_bytes() for p in Path(self.directory.name).iterdir()})

    def test_reject_wal_and_missing_input(self):
        Path(str(self.core) + "-wal").touch()
        with self.assertRaises(ValueError):
            MODULE.audit(self.core, self.ui, "android")
        with self.assertRaises(FileNotFoundError):
            MODULE.audit(self.core.parent / "missing.db", self.ui, "android")


if __name__ == "__main__":
    unittest.main()
