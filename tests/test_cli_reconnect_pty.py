"""Transport reconnection stays editable, scrollable and cancellable at all widths."""
import http.server
import json
import re
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal
from test_cli_jobs_pty import until


class ConnectionModel(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        title = any(m.get('role') == 'system' and m.get('content', '').startswith(
            "Summarize the user's underlying task") for m in body.get('messages', []))
        with self.server.lock:
            mode = self.server.mode
            if not title:
                self.server.calls += 1
            calls = self.server.calls
        if not title and (mode == 'fail' or (mode == 'recover' and calls <= 2)):
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        if not title and mode == 'auth':
            self.send_response(401)
            self.end_headers()
            self.wfile.write(b'{"error":{"message":"test authentication rejected"}}')
            return
        content = ('{"title":"Reconnect test"}' if title else
                   '\n'.join(f'HISTORY_{n:03d}' for n in range(65)) if mode == 'seed' else
                   'RECOVERED_CONNECTION')
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        payload = {'choices': [{'index': 0, 'delta': {'content': content}}]}
        try:
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def set_mode(server, mode):
    with server.lock:
        server.mode, server.calls = mode, 0


def second_attempt(text):
    return any(re.match(r'^◉ Reconnecting · (?:attempt 2|#2)(?:\s|$)', row)
               for row in text.splitlines())


def run_width(driver, width):
    server = LoopbackHTTPServer(('127.0.0.1', 0), ConnectionModel)
    server.daemon_threads = True
    server.lock = threading.Lock()
    set_mode(server, 'seed')
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix=f'morph-reconnect-{width}-') as temp:
            terminal = Terminal(driver, Path(temp), server.server_port,
                                hooks_enabled=False, retry_count=1)
            terminal.wait_for_composer()
            terminal.child.setwinsize(24, width)
            terminal.screen.resize(24, width)
            terminal.send('seed history\r')
            until(terminal, lambda t: 'Worked for' in t)

            set_mode(server, 'recover')
            terminal.send('recover connection\r')
            until(terminal, lambda t: 'Reconnecting' in t)
            draft = 'DRAFT_START ' + 'wrapped draft ' * 7 + 'DRAFT_END'
            terminal.send(draft)
            text = until(terminal, lambda t: 'DRAFT_END' in ''.join(t.split()))
            assert 'DRAFT_START' in text and 'Reconnecting' in text, text
            terminal.send('\x1b[5~')
            text = until(terminal, lambda t: 'Back to bottom' in t)
            assert server.calls <= 2, server.calls
            terminal.send('\x1b')  # Leave scrollback before testing cancellation.
            until(terminal, lambda t: 'Back to bottom' not in t)
            until(terminal, second_attempt, 7)
            text = until(terminal, lambda t: 'RECOVERED_CONNECTION' in t and
                         'Reconnecting' not in t and 'Worked for' in t, 13)
            assert server.calls == 3, server.calls
            assert 'DRAFT_START' in text and 'DRAFT_END' in ''.join(text.split()), text

            set_mode(server, 'fail')
            terminal.send('\x15cancel connection\r')
            until(terminal, second_attempt, 8)
            terminal.send('CANCEL_DRAFT')
            started = time.monotonic()
            terminal.child.send('\x1b')
            text = until(terminal, lambda t: 'Stopped after' in t and
                         'Reconnecting' not in t, 2)
            assert time.monotonic() - started < 2, text
            assert 'CANCEL_DRAFT' in text, text
            count = server.calls
            terminal.pump(1.1)
            assert server.calls == count, (count, server.calls)

            if width == 80:
                set_mode(server, 'auth')
                terminal.send('\x15reject authentication\r')
                until(terminal, lambda t: 'Failed after' in t)
                assert server.calls == 1, server.calls
                assert 'Reconnecting' not in terminal.snapshot('terminal auth failure')

            set_mode(server, 'success')
            terminal.send('\x15fresh request\r')
            until(terminal, lambda t: 'RECOVERED_CONNECTION' in t and
                  'Working ·' not in t and server.calls == 1)
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=5)
            assert terminal.child.exitstatus in (0, None), terminal.raw[-2000:]
            print(f'PASS: {width} columns, repeated reconnect, draft, scroll, cancel and recovery',
                  flush=True)
    finally:
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    for columns in (120, 80, 60):
        run_width(Path(sys.argv[1]).resolve(), columns)
