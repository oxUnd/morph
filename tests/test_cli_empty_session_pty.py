"""Start the production CLI with no sessions and exercise its idle polling."""
import http.server
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Terminal


class StartupModel(http.server.BaseHTTPRequestHandler):
    requests = []
    mcp_methods = []
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path == '/mcp':
            method = body.get('method')
            with self.lock:
                self.mcp_methods.append(method)
            if method == 'server/discover':
                self.send_response(400)
                payload = {'error': {'code': -32601, 'message': 'Method not found'}}
            else:
                self.send_response(200)
                payload = {'jsonrpc': '2.0', 'id': body.get('id'), 'result': {
                    'protocolVersion': '2025-06-18', 'capabilities': {},
                    'serverInfo': {'name': 'startup-legacy', 'version': '1'}}}
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            if 'id' in body or method == 'server/discover':
                self.wfile.write(json.dumps(payload).encode())
            self.wfile.flush()
            return
        with self.lock:
            self.requests.append(body)
        systems = [message.get('content', '') for message in body.get('messages', [])
                   if message.get('role') == 'system']
        content = ('{"title":"Startup regression"}' if any(
            text.startswith("Summarize the user's underlying task") for text in systems)
            else 'EMPTY_SESSION_FIRST_TURN_OK')
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        chunk = {'choices': [{'index': 0, 'delta': {'content': content}}]}
        self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
        self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},'
                        b'"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
        self.wfile.flush()


def assert_quiet(terminal, database):
    assert terminal.child.isalive(), terminal.raw[-4000:]
    for error in ('MORPH_RETURN:', 'MORPH_SET_ERR:', 'database error',
                  'No message available', 'No message available on STREAM',
                  'Generating recap', 'Recap:'):
        assert error not in terminal.raw, terminal.raw[-4000:]
    with StartupModel.lock:
        assert not StartupModel.requests, StartupModel.requests
    with sqlite3.connect(database) as db:
        assert db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0] == 0
        assert db.execute('SELECT COUNT(*) FROM messages').fetchone()[0] == 0


def main():
    binary = Path(sys.argv[1]).resolve()
    expect_debug_trace = len(sys.argv) > 2 and sys.argv[2] == '1'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), StartupModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-empty-startup-') as temp:
                directory = Path(temp)
                with StartupModel.lock:
                    StartupModel.requests.clear()
                    StartupModel.mcp_methods.clear()
                mcp_config = f'''\n[[mcp.servers]]
name = "startup-legacy"
transport = "http"
url = "http://127.0.0.1:{server.server_port}/mcp"
auto_connect = true
'''
                terminal = Terminal(binary, directory, server.server_port,
                                    hooks_enabled=False, production=True,
                                    extra_env={'MORPH_DEBUG': '1'},
                                    extra_config=mcp_config)
                database = directory / 'home/.morph/data.db'
                try:
                    terminal.child.setwinsize(24, columns)
                    terminal.screen.resize(lines=24, columns=columns)
                    deadline = time.monotonic() + 8
                    while ('\x1b[?1004h' not in terminal.raw or not database.exists()):
                        assert time.monotonic() < deadline, terminal.raw[-4000:]
                        terminal.pump(.1)
                    terminal.pump(2.2)
                    assert_quiet(terminal, database)
                    with StartupModel.lock:
                        assert StartupModel.mcp_methods == [
                            'server/discover', 'initialize', 'notifications/initialized'
                        ], StartupModel.mcp_methods
                    if expect_debug_trace:
                        log = (directory / 'home/.morph/log/agent.log').read_text()
                        assert 'MORPH_RETURN: protocol error' in log, log[-3000:]
                        assert 'src/mcp/mcp_protocol.c:' in log, log[-3000:]
                    screen = terminal.snapshot(f'empty startup at {columns} columns')
                    assert '0 / 128k' in screen, screen
                    terminal.send('unsent startup draft')
                    terminal.send('\x1b[O')
                    terminal.pump(2.2)
                    assert_quiet(terminal, database)
                    terminal.send('\x1b[I')
                    screen = terminal.snapshot('focus return preserves empty session draft')
                    assert 'unsent startup draft' in screen, screen
                    assert_quiet(terminal, database)
                    terminal.send('\x15Check the startup flow\r')
                    deadline = time.monotonic() + 8
                    while 'EMPTY_SESSION_FIRST_TURN_OK' not in terminal.raw:
                        assert time.monotonic() < deadline, terminal.raw[-4000:]
                        terminal.pump(.1)
                    terminal.pump(1.2)
                    with sqlite3.connect(database) as db:
                        assert db.execute('SELECT COUNT(*) FROM sessions').fetchone()[0] == 1
                        assert db.execute("SELECT COUNT(*) FROM messages WHERE role='user'"
                                          ).fetchone()[0] == 1
                        assert db.execute("SELECT COUNT(*) FROM messages WHERE role='assistant'"
                                          ).fetchone()[0] == 1
                    with StartupModel.lock:
                        requests = list(StartupModel.requests)
                    assert requests
                    assert not any('brief catch-up' in message.get('content', '')
                                   for body in requests for message in body.get('messages', []))
                    assert 'MORPH_RETURN:' not in terminal.raw, terminal.raw[-4000:]
                    assert not terminal.duplicate_prompts
                    terminal.send('/quit\r')
                    terminal.child.expect(__import__('pexpect').EOF, timeout=10)
                    terminal.child.close()
                    assert terminal.child.exitstatus == 0
                finally:
                    terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
