"""Offline fixtures for the session sync review diagnostic."""

import importlib.util
import json
import subprocess
import sys
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

    def test_large_integer_ids_remain_exact(self):
        large = 9007199254740993
        self.insert()
        with closing(sqlite3.connect(self.core)) as db, db:
            db.execute("INSERT INTO sessions VALUES(?)", (large,))
            db.execute("INSERT INTO messages VALUES(?,?)", (large, large))
        self.insert(sid=large + 2, mid=None)
        report = MODULE.audit(self.core, self.ui, "android")
        self.assertEqual([large], report["sessions_with_core_messages_but_no_ui_rows"])
        self.assertEqual([large + 2], report["ui_sessions_missing_from_core"])

    def test_cli_preserves_report_exit_codes_without_echoing_types(self):
        self.insert()
        command = [sys.executable, str(Path(MODULE.__file__)),
                   str(self.core), str(self.ui), "--platform", "ios"]
        valid = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(0, valid.returncode, valid.stderr)
        self.assertFalse(json.loads(valid.stdout)["has_findings"])
        self.insert(kind="PRIVATE_UNSUPPORTED_VALUE")
        findings = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(2, findings.returncode, findings.stderr)
        self.assertEqual(1, json.loads(findings.stdout)["unsupported_ui_message_count"])
        self.assertNotIn("PRIVATE_UNSUPPORTED_VALUE", findings.stdout)
        command[2] = str(self.core.parent / "missing.db")
        invalid = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(1, invalid.returncode)
        self.assertIn("error", json.loads(invalid.stdout))

    def test_read_only_wal_mode_backups_never_create_input_sidecars(self):
        self.insert()
        for path in (self.core, self.ui):
            with closing(sqlite3.connect(path)) as db:
                self.assertEqual("wal", db.execute("PRAGMA journal_mode=WAL").fetchone()[0])
            path.chmod(0o400)
        directory = Path(self.directory.name)
        before = {p.name: p.read_bytes() for p in directory.iterdir()}
        directory.chmod(0o500)
        try:
            self.assertFalse(MODULE.audit(self.core, self.ui, "android")["has_findings"])
            self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})
        finally:
            directory.chmod(0o700)
            for path in (self.core, self.ui):
                path.chmod(0o600)

    def test_native_audit_rejects_journals_missing_paths_and_corruption(self):
        command = [MODULE._audit_executable(), str(self.core), str(self.ui),
                   "--platform", "android"]
        journal = Path(str(self.core) + "-journal")
        journal.touch()
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(1, result.returncode)
        self.assertIn("error", json.loads(result.stdout))
        journal.unlink()
        original = self.ui.read_bytes()
        self.ui.write_bytes(b"not a database")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(1, result.returncode)
        self.assertEqual(b"not a database", self.ui.read_bytes())
        self.ui.write_bytes(original)
        command[1] = str(self.core.parent / "missing.db")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(1, result.returncode)
        self.assertFalse(Path(command[1]).exists())


if __name__ == "__main__":
    unittest.main()
