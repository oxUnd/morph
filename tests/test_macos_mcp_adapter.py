"""Offline contracts for the launch-only macOS MCP operation."""

import importlib.util
import json
import subprocess
import sys
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "macos_mcp_adapter", Path(__file__).resolve().parents[1] / "scripts/macos_mcp_adapter.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class MacosMcpAdapterTest(unittest.TestCase):
    def make_adapter(self, replies):
        calls = []

        def exchange(message):
            calls.append(message["params"])
            payload, error = replies[len(calls) - 1]
            return {"id": message["id"], "result": MODULE.result(payload, error)}

        return MODULE.Adapter(exchange), calls

    def test_localized_app_uses_resolved_pid_and_proves_completion(self):
        adapter, calls = self.make_adapter([
            ({"ok": True, "pid": 42, "name": "计算器"}, False),
            ({"ok": True, "verified": True, "pid": 42, "name": "计算器"}, False),
            ({"windows": [{"pid": 42, "id": 8}, {"pid": 99, "id": 9}]}, False),
        ])
        payload = json.loads(adapter.open_app({"app": "Calculator"})["content"][0]["text"])
        self.assertTrue(payload["completed"])
        self.assertEqual(payload["windows"], [{"pid": 42, "id": 8}])
        self.assertEqual([c["arguments"] for c in calls], [
            {"action": "launch", "app": "Calculator"},
            {"action": "activate", "pid": 42}, {"action": "list", "pid": 42}])
        self.assertEqual(len(payload["steps"]), 3)

    def test_lock_stops_without_window_probe_and_preserves_retry_policy(self):
        adapter, calls = self.make_adapter([
            ({"ok": True, "pid": 42}, False),
            ({"ok": False, "reason": "screen_locked", "retry": "never"}, True),
        ])
        reply = adapter.open_app({"app": "Calculator"})
        payload = json.loads(reply["content"][0]["text"])
        self.assertTrue(reply["isError"])
        self.assertFalse(payload["completed"])
        self.assertEqual(payload["reason"], "screen_locked")
        self.assertEqual(payload["retry"], "never")
        self.assertEqual(len(calls), 2)

    def test_localized_launch_timeout_resolves_unique_bundle_tail(self):
        adapter, calls = self.make_adapter([
            ({"ok": False, "reason": "launch_timeout"}, True),
            ({"apps": [{"name": "计算器", "bundle": "com.apple.calculator", "pid": 42}]}, False),
            ({"ok": True, "verified": True}, False),
            ({"windows": [{"pid": 42, "id": 8}]}, False),
        ])
        payload = json.loads(adapter.open_app({"app": "Calculator"})["content"][0]["text"])
        self.assertTrue(payload["completed"])
        self.assertEqual(calls[2]["arguments"]["pid"], 42)

    def test_unverified_activation_and_no_window_are_not_success(self):
        for activation, window_replies in [
            ({"ok": True, "verified": False}, []),
            ({"ok": True, "verified": True}, [({"windows": []}, False)]),
        ]:
            with self.subTest(activation=activation):
                adapter, calls = self.make_adapter([
                    ({"ok": True, "pid": 42}, False), (activation, False), *window_replies])
                reply = adapter.open_app({"app": "Calculator"})
                self.assertTrue(reply["isError"])
                self.assertFalse(json.loads(reply["content"][0]["text"])["completed"])
                self.assertEqual(len(calls), 2 + len(window_replies))

    def test_dry_run_cannot_claim_app_is_visible(self):
        adapter, calls = self.make_adapter([({"ok": True, "dry_run": True}, False)])
        reply = adapter.open_app({"app": "Calculator"})
        self.assertFalse(reply["isError"])
        self.assertFalse(json.loads(reply["content"][0]["text"])["completed"])
        self.assertEqual(len(calls), 1)

    def test_invalid_arguments_do_not_call_upstream(self):
        for args in [{}, {"app": " "}, {"app": 42}, {"app": "Calculator", "pid": 42}]:
            adapter, calls = self.make_adapter([])
            self.assertTrue(adapter.open_app(args)["isError"])
            self.assertEqual(calls, [])

    def test_legacy_activation_includes_window_evidence_without_other_actions(self):
        adapter, calls = self.make_adapter([
            ({"ok": True, "verified": True, "pid": 42}, False),
            ({"windows": [{"pid": 42, "id": 8}]}, False),
        ])
        adapter.available = True
        reply = adapter.handle({"id": 1, "method": "tools/call", "params": {
            "name": "macos_app", "arguments": {"action": "activate", "pid": 42}}})
        payload = json.loads(reply["result"]["content"][0]["text"])
        self.assertTrue(payload["foreground_check"]["completed"])
        self.assertEqual([c["name"] for c in calls], ["macos_app", "macos_window"])

    def test_legacy_activation_refusal_does_not_add_probe(self):
        adapter, calls = self.make_adapter([
            ({"ok": False, "reason": "screen_locked", "retry": "never"}, True)])
        adapter.available = True
        reply = adapter.handle({"id": 1, "method": "tools/call", "params": {
            "name": "macos_app", "arguments": {"action": "activate", "pid": 42}}})
        self.assertTrue(reply["result"]["isError"])
        self.assertEqual(len(calls), 1)

    def test_stdio_transport_preserves_ids_and_closes_child(self):
        child = """import json, sys
for line in sys.stdin:
    m = json.loads(line)
    if 'id' in m:
        print(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'result': {}}), flush=True)
"""
        messages = [{"jsonrpc": "2.0", "id": 7, "method": "initialize"},
                    {"jsonrpc": "2.0", "method": "notifications/initialized"},
                    {"jsonrpc": "2.0", "id": 8, "method": "ping"}]
        proc = subprocess.run(
            [sys.executable, str(Path(MODULE.__file__)), "--", sys.executable, "-c", child],
            input="".join(json.dumps(m) + "\n" for m in messages), capture_output=True,
            text=True, timeout=10, check=True)
        self.assertEqual([json.loads(line)["id"] for line in proc.stdout.splitlines()], [7, 8])
        self.assertEqual(proc.stderr, "")

    def test_respects_upstream_read_only_and_excluded_tools(self):
        for readonly, include_window, expected in [(True, True, False),
                                                   (False, False, False),
                                                   (False, True, True)]:
            specs = [{"name": "macos_app", "annotations": {"readOnlyHint": readonly}}]
            if include_window:
                specs.append({"name": "macos_window"})
            adapter = MODULE.Adapter(lambda m: {"id": m["id"], "result": {"tools": specs}})
            reply = adapter.handle({"id": 1, "method": "tools/list"})
            self.assertEqual(any(t["name"] == "macos_open_app"
                                 for t in reply["result"]["tools"]), expected)
            self.assertEqual(adapter.available, expected)


if __name__ == "__main__":
    unittest.main()
