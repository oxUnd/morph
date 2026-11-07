"""Exercise the real runner's bounded host I/O and failure contracts."""

import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


RUNNER = str(Path(sys.argv.pop(1)).resolve())


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        status, body = {
            "/missing": (404, b"missing"),
            "/error": (503, b"unavailable"),
            "/empty": (204, b""),
            "/large": (200, b"x" * (1024 * 1024 + 1)),
            "/unicode": (200, "A🙂中B".encode("utf-8")),
        }.get(self.path, (200, b"hello\x00tail"))
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_args):
        pass


class RunnerHostIoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join()
        cls.server.server_close()

    def run_script(self, source, params=None, **overrides):
        environment = os.environ.copy()
        environment.update({
            "MORPH_DYNAMIC_CAPS": "network,shell",
            "MORPH_DYNAMIC_ALLOWED_COMMANDS": "*",
            "MORPH_DYNAMIC_ALLOWED_NETWORK": self.base,
            "MORPH_DYNAMIC_TIMEOUT": "5",
            "NO_PROXY": "127.0.0.1,localhost,::1",
            "no_proxy": "127.0.0.1,localhost,::1",
        })
        environment.update(overrides)
        with tempfile.TemporaryDirectory(prefix="morph-runner-io-") as directory:
            script = Path(directory) / "tool.js"
            script.write_text(source)
            result = subprocess.run(
                [RUNNER, str(script)],
                input=json.dumps({"jsonrpc": "2.0", "id": 7, "params": params or {}}) + "\n",
                text=True, capture_output=True, env=environment, timeout=10,
            )
        self.assertTrue(result.stdout.strip(), result.stderr)
        response = json.loads(result.stdout)
        self.assertEqual(response["id"], 7)
        return result, response

    def test_exec_preserves_stdout_including_nul(self):
        result, response = self.run_script(
            "function run() { return morph.exec(" + json.dumps("printf 'hello\\000tail'") + "); }"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], "hello\x00tail")

    def test_exec_reports_nonzero_exit(self):
        result, response = self.run_script("function run() { return morph.exec('exit 7'); }")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status 7", response["error"]["message"])

    def test_exec_reports_signal(self):
        result, response = self.run_script(
            "function run() { return morph.exec('kill -TERM $$'); }"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("signal", response["error"]["message"])

    def test_exec_rejects_truncated_capture(self):
        result, response = self.run_script(
            "function run() { return morph.exec('head -c 1048577 /dev/zero'); }"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("command failed", response["error"]["message"])

    def test_exec_obeys_runner_deadline(self):
        started = time.monotonic()
        result, response = self.run_script(
            "function run() { return morph.exec('sleep 3'); }", MORPH_DYNAMIC_TIMEOUT="1"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("command failed", response["error"]["message"])
        self.assertLess(time.monotonic() - started, 2.5)

    def test_fetch_keeps_status_and_body_on_http_errors(self):
        for path, status, body in [("/missing", 404, "missing"), ("/error", 503, "unavailable")]:
            with self.subTest(path=path):
                result, response = self.run_script(
                    "async function run(args) { const r = await morph.fetch(args.url);"
                    "return {ok:r.ok, status:r.status, body:await r.text()}; }",
                    {"url": self.base + path},
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(response["result"], {"ok": False, "status": status, "body": body})

    def test_fetch_preserves_body_length_and_compatibility_helpers(self):
        result, response = self.run_script(
            "function run(args) { const r = morph.fetch(args.url);"
            "return {ok:r.ok, status:r.status, body:r.text(), length:r.length,"
            "tail:r.slice(6), index:r.indexOf('tail')}; }",
            {"url": self.base + "/ok"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], {
            "ok": True, "status": 200, "body": "hello\x00tail", "length": 10,
            "tail": "tail", "index": 6,
        })

    def test_fetch_handles_empty_success(self):
        result, response = self.run_script(
            "function run(args) { const r = morph.fetch(args.url);"
            "return {ok:r.ok, status:r.status, body:r.text()}; }",
            {"url": self.base + "/empty"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], {"ok": True, "status": 204, "body": ""})

    def test_fetch_string_helpers_use_javascript_unicode_indices(self):
        result, response = self.run_script(
            "function run(args) { const r = morph.fetch(args.url);"
            "return {slice:r.slice(1,3), substring:r.substring(1,3), index:r.indexOf('中')}; }",
            {"url": self.base + "/unicode"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], {"slice": "🙂", "substring": "🙂", "index": 3})

    def test_fetch_rejects_transport_failure(self):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            url = f"http://127.0.0.1:{reserved.getsockname()[1]}"
            result, response = self.run_script(
                "function run(args) { return morph.fetch(args.url); }", {"url": url},
                MORPH_DYNAMIC_ALLOWED_NETWORK="*",
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fetch failed", response["error"]["message"])

    def test_fetch_rejects_oversized_response(self):
        result, response = self.run_script(
            "function run(args) { return morph.fetch(args.url); }",
            {"url": self.base + "/large"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fetch failed", response["error"]["message"])

    def test_fetch_preserves_capability_and_allowlist_checks(self):
        for overrides, message in [
            ({"MORPH_DYNAMIC_CAPS": "shell"}, "capability denied"),
            ({"MORPH_DYNAMIC_ALLOWED_NETWORK": self.base + "/allowed"}, "target denied"),
        ]:
            with self.subTest(overrides=overrides):
                result, response = self.run_script(
                    "function run(args) { return morph.fetch(args.url); }",
                    {"url": self.base + "/ok"}, **overrides,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, response["error"]["message"])

    def test_fetch_does_not_treat_files_as_network_requests(self):
        result, response = self.run_script(
            "function run() { return morph.fetch('file:///etc/hosts'); }",
            MORPH_DYNAMIC_ALLOWED_NETWORK="*",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HTTP or HTTPS", response["error"]["message"])


if __name__ == "__main__":
    unittest.main()
