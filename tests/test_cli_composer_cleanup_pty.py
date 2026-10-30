"""Accepted lines must not paint another editable prompt into terminal history."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Terminal, pexpect, pyte


class ComposerModel(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        title = any('underlying task' in item.get('content', '')
                    for item in body.get('messages', []) if item.get('role') == 'system')
        deltas = ([{'content': '{"title":"Greeting"}'}] if title else
                  [{'reasoning_content': 'The user says hello world. Respond briefly.'},
                   {'content': '你好，世界。需要我做什么？'}])
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        for delta in deltas:
            chunk = {'choices': [{'index': 0, 'delta': delta}]}
            self.wfile.write(('data: ' + json.dumps(chunk) + '\n\n').encode())
            self.wfile.flush()
            time.sleep(.03)
        self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n'
                        b'data: [DONE]\n\n')
        self.wfile.flush()


def empty_prompts(terminal):
    rows = [''.join(cell.data for _, cell in sorted(row.items()))
            for row in terminal.screen.history.top] + terminal.screen.display
    return [row for row in rows if row.split('ctrl+o details')[0].strip() in ('>', '›')]


def main():
    binary = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ComposerModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for color in (True, False):
            for width in (120, 80, 60):
                with tempfile.TemporaryDirectory(prefix='morph-composer-cleanup-') as temp:
                    terminal = Terminal(binary, Path(temp), server.server_port,
                                        hooks_enabled=False, production=True,
                                        extra_env={'NO_COLOR': '' if color else '1'})
                    terminal.child.setwinsize(24, width)
                    terminal.screen = pyte.HistoryScreen(width, 24, history=2000)
                    terminal.stream = pyte.Stream(terminal.screen)
                    try:
                        terminal.pump(1)
                        for turn in range(2):
                            terminal.send('hello world')
                            submitted = len(terminal.raw)
                            terminal.send('\r')
                            deadline = time.monotonic() + 8
                            while terminal.raw.count('Worked for ') < turn + 1:
                                assert time.monotonic() < deadline, terminal.raw[-4000:]
                                terminal.pump(.05)
                            terminal.pump(.2)
                            output = terminal.raw[submitted:]
                            thought = output.index('The user says')
                            committed = output.rfind('hello world', 0, thought)
                            assert committed >= 0, repr(output)
                            assert 'ctrl+o details' not in output[:committed], repr(output)
                            assert len(empty_prompts(terminal)) == 1, terminal.screen.display
                        terminal.send('/q')
                        start = len(terminal.raw)
                        terminal.send('\r')
                        terminal.child.expect(pexpect.EOF, timeout=8)
                        assert 'ctrl+o details' not in terminal.raw[start:], repr(
                            terminal.raw[start:])
                        assert not empty_prompts(terminal), terminal.screen.display
                        print(f'checked: {width} columns, color={color}, no orphan composer')
                    finally:
                        terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
