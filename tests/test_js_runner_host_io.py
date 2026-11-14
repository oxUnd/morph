"""Exercise the real runner's bounded host I/O and failure contracts."""

import http.server
from loopback_http import LoopbackHTTPServer
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
        if self.path == "/range":
            resource = b"0123456789" * 1000
            requested = self.headers.get("Range", "")
            if requested == "bytes=5-10":
                status, body = 206, resource[5:11]
            else:
                status, body = 200, resource
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
    def test_write_errors_use_native_status_descriptions(self):
        with tempfile.TemporaryDirectory(prefix="morph-runner-status-") as directory:
            root = str(Path(directory).resolve())
            result, response = self.run_script(
                "function run(args) { try { morph.fs.writeFile(args.path, 'x'); } "
                "catch (error) { return String(error); } }",
                {"path": root}, MORPH_DYNAMIC_CAPS="fs_write",
                MORPH_DYNAMIC_ALLOWED_WRITE=root, MORPH_DYNAMIC_OUTPUT_DIR=root,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("I/O error", response["result"])
        self.assertNotIn("No such process", response["result"])

    def test_file_reads_own_large_binary_and_text_after_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="morph-runner-read-") as directory:
            directory = str(Path(directory).resolve())
            path = Path(directory) / "payload"
            payload = ("A\x00🙂中" * 65536).encode("utf-8")
            path.write_bytes(payload)
            (Path(directory) / "empty").write_bytes(b"")
            result, response = self.run_script(
                "function run(args) {"
                " const binary = new Uint8Array(morph.fs.readFile(args.path));"
                " const text = morph.fs.readText(args.path);"
                " morph.fs.writeText(args.path, 'replaced');"
                " const empty = morph.fs.readText(args.empty);"
                " return { length: binary.length, first: binary[0], nul: binary[1],"
                " last: binary[binary.length - 1], text, empty }; }",
                {"path": str(path), "empty": str(Path(directory) / "empty")},
                MORPH_DYNAMIC_CAPS="fs_read,fs_write",
                MORPH_DYNAMIC_ALLOWED_READ=directory,
                MORPH_DYNAMIC_ALLOWED_WRITE=directory,
                MORPH_DYNAMIC_OUTPUT_DIR=directory,
            )
            self.assertEqual(result.returncode, 0, (result.stderr, response))
            self.assertEqual(response["result"]["length"], len(payload))
            self.assertEqual(response["result"]["first"], payload[0])
            self.assertEqual(response["result"]["nul"], 0)
            self.assertEqual(response["result"]["last"], payload[-1])
            self.assertEqual(response["result"]["text"], payload.decode("utf-8"))
            self.assertEqual(response["result"]["empty"], "")
            self.assertEqual(path.read_text(), "replaced")

    @classmethod
    def setUpClass(cls):
        cls.server = LoopbackHTTPServer(("127.0.0.1", 0), Handler)
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

    def test_runner_preserves_embedded_nul_in_input_parameters(self):
        result, response = self.run_script(
            "function run(args) { return args.text; }", {"text": "prefix\x00suffix"}
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], "prefix\x00suffix")

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

    def test_capture_budget_can_exceed_the_old_one_megabyte_limit(self):
        for source, params in [
            ("function run() { return morph.exec('head -c 1048577 /dev/zero').length; }", {}),
            ("function run(args) { return morph.fetch(args.url).length; }", {"url": self.base + "/large"}),
        ]:
            with self.subTest(source=source):
                result, response = self.run_script(
                    source, params, MORPH_RUNNER_MAX_CAPTURE_BYTES="2097152"
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(response["result"], 1048577)

    def test_capture_budget_checks_stdout_and_stderr_at_the_boundary(self):
        result, response = self.run_script(
            "function run() { return morph.exec('head -c 16 /dev/zero').length; }",
            MORPH_RUNNER_MAX_CAPTURE_BYTES="16",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], 16)
        for command in ["head -c 17 /dev/zero", "head -c 17 /dev/zero >&2"]:
            with self.subTest(command=command):
                result, response = self.run_script(
                    "function run() { return morph.exec(" + json.dumps(command) + "); }",
                    MORPH_RUNNER_MAX_CAPTURE_BYTES="16",
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("capture limit of 16 bytes", response["error"]["message"])

    def test_fetch_can_read_a_bounded_range_of_a_larger_resource(self):
        result, response = self.run_script(
            "function run(args) { const r = morph.fetch(args.url, {headers: {Range: 'bytes=5-10'}});"
            "return {status: r.status, text: r.text()}; }",
            {"url": self.base + "/range"}, MORPH_RUNNER_MAX_CAPTURE_BYTES="64",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(response["result"], {"status": 206, "text": "567890"})

    def test_fetch_rejects_header_injection_and_nul_urls(self):
        for source, params in [
            ("function run(args) { return morph.fetch(args.url, {headers: args.headers}); }",
             {"url": self.base, "headers": {"Range": "bytes=0-1\r\nX-Injected: yes"}}),
            ("function run(args) { return morph.fetch(args.url + String.fromCharCode(0)); }",
             {"url": self.base}),
            ("function run() { return morph.exec('printf safe' + String.fromCharCode(0)); }", {}),
        ]:
            with self.subTest(source=source):
                result, response = self.run_script(source, params)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error", response)

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
