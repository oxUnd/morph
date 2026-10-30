"""Exercise /recap, focus reports, editable drafts, and stale responses in a real PTY."""
import http.server
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal


class RecapModel(Model):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        is_recap = any('brief catch-up' in message.get('content', '')
                       for message in body.get('messages', [])
                       if message.get('role') == 'system')
        release = threading.Event()
        if is_recap:
            Model.requests.put((body, release))
        else:
            release.set()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            self.wfile.write(b': waiting\n\n')
            self.wfile.flush()
            if not release.wait(10):
                return
            content = getattr(release, 'answer', 'Main work complete.')
            payload = {'choices': [{'index': 0, 'delta': {'content': content}}]}
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
            self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n'
                            b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), RecapModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-recap-pty-') as temp:
        directory = Path(temp)
        terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
        try:
            terminal.pump(1)
            deadline = time.monotonic() + 8
            while '\x1b[?1004h' not in terminal.raw and time.monotonic() < deadline:
                terminal.pump(.1)
            assert '\x1b[?1004h' in terminal.raw, terminal.raw[-4000:]
            terminal.pump(2)
            assert 'MORPH_RETURN:' not in terminal.raw, terminal.raw[-4000:]
            terminal.send('/recap\r')
            assert 'There is no conversation history to recap' in terminal.raw
            with sqlite3.connect(directory / 'data.db') as db:
                db.execute("INSERT INTO sessions(display_id,name,model,created_at,updated_at) "
                           "VALUES('recap001','recap-test','pty-test',1,1)")
                session_id = db.execute("SELECT id FROM sessions WHERE name='recap-test'").fetchone()[0]
                for index in range(4):
                    for role, text in [('user', f'Fix issue {index}'),
                                       ('assistant', f'Implemented and tested issue {index}')]:
                        db.execute('INSERT INTO messages(session_id,role,content,token_count,created_at) '
                                   'VALUES(?,?,?,?,?)', (session_id, role, text, 5, index + 1))
            terminal.send('/switch recap-test\r')
            terminal.send('draft')
            terminal.send('\x1b[O')
            assert 'draft' in terminal.snapshot('focus loss preserves the editable draft')
            terminal.send('\x1b[I')
            terminal.send('\x0f')
            terminal.send('\x1b[O\x1b[I')
            assert 'esc back' in terminal.snapshot('focus reports preserve transcript view')
            terminal.send('\x0f')
            assert 'draft' in terminal.snapshot('closing transcript restores the draft')
            terminal.send('\x15/recap\r')
            body, release = terminal.request()
            releases.append(release)
            assert 'tools' not in body
            terminal.send('keep this draft')
            assert 'Generating recap' in terminal.snapshot('background recap status with draft')
            release.answer = json.dumps({
                'summary': '已完成修复并通过测试，但尚未安装验证。',
                'next_action': None,
            }, ensure_ascii=False)
            release.set()
            terminal.pump(1)
            screen = terminal.snapshot('recap commits without replacing the draft')
            assert 'Recap:' in screen and 'keep this draft' in screen, screen
            assert 'Next:' not in screen, screen
            assert 'Generating recap' not in screen, screen
            with sqlite3.connect(directory / 'data.db') as db:
                assert db.execute('SELECT COUNT(*) FROM messages WHERE session_id=?',
                                  (session_id,)).fetchone()[0] == 8
                assert db.execute('SELECT COUNT(*) FROM history_compactions').fetchone()[0] == 0

            terminal.send('\x15/recap\r')
            _, stale = terminal.request()
            releases.append(stale)
            terminal.send('Continue with a different task\r')
            terminal.pump(1)
            stale.answer = json.dumps({'summary': 'STALE_RECAP_SHOULD_NOT_APPEAR', 'next_action': None})
            stale.set()
            terminal.pump(1)
            assert 'STALE_RECAP_SHOULD_NOT_APPEAR' not in terminal.raw
            terminal.snapshot('new turn discards the old recap')

            terminal.send('/recap\r')
            _, completed_recap = terminal.request()
            releases.append(completed_recap)
            completed_recap.answer = json.dumps({'summary': 'Completed work recap.', 'next_action': None})
            completed_recap.set()
            terminal.pump(1)
            screen = terminal.snapshot('recap preserves a single completed work status')
            assert screen.count('Worked for ') == 1, screen

            terminal.child.setwinsize(24, 32)
            terminal.screen.resize(lines=24, columns=32)
            terminal.send('/recap\r')
            _, narrow = terminal.request()
            releases.append(narrow)
            narrow.answer = json.dumps({
                'summary': '已完成测试。\u001b[2J尚未验证安装效果。',
                'next_action': '安装后检查结果。',
            }, ensure_ascii=False)
            start = len(terminal.raw)
            narrow.set()
            terminal.pump(1)
            screen = terminal.snapshot('narrow recap wraps and sanitizes terminal controls')
            assert 'Recap:' in screen and 'Next:' in screen, screen
            assert screen.count('Worked for ') <= 1, screen
            assert 'Worked for ' not in terminal.raw[start:], terminal.raw[start:]
            assert '\x1b[2J' not in terminal.raw[start:]
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            assert '\x1b[?1004l' in terminal.raw
            assert not terminal.duplicate_prompts
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
