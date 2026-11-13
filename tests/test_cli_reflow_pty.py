"""Optional native Kitty terminal reflow regression for the editable composer.

Kitty runs only its terminal parser in a subprocess. The configured test Python
owns HTTP and the PTY, so Kitty's bundled Python needs no additional packages.
"""
import json
from pathlib import Path
import sys


def native_worker():
    import re
    try:
        from kitty.fast_data_types import Screen
        screen = Screen(None, 32, 200, 10000, 0, 0)
        for name in ('test_create_write_buffer', 'test_commit_write_buffer',
                     'test_parse_written_data', 'resize'):
            if not hasattr(screen, name):
                raise ImportError(f'Kitty Screen lacks {name}')
    except (ImportError, AttributeError, TypeError) as error:
        print(json.dumps({'skip': str(error)}), flush=True)
        return
    synchronized = False
    pending = ''
    print(json.dumps({'ready': True}), flush=True)
    for line in sys.stdin:
        request = json.loads(line)
        if request['op'] == 'feed':
            data = request['data']
            buffer = screen.test_create_write_buffer()
            screen.test_commit_write_buffer(data.encode(), buffer)
            screen.test_parse_written_data()
            pending += data
            for update in re.finditer(r'\x1b\[\?2026([hl])', pending):
                synchronized = update[1] == 'h'
            pending = pending[-16:]
        elif request['op'] == 'resize':
            screen.resize(request['lines'], request['columns'])
        elif request['op'] == 'quit':
            return
        state = {
            'display': [str(screen.line(i)) for i in range(screen.lines)],
            'history': [str(screen.historybuf.line(i))
                        for i in reversed(range(screen.historybuf.count))],
            'cursor': [screen.cursor.x, screen.cursor.y],
            'lines': screen.lines,
            'columns': screen.columns,
            'synchronized': synchronized,
        }
        print(json.dumps(state), flush=True)


if __name__ == '__main__' and '--native-worker' in sys.argv:
    native_worker()
    sys.exit(0)

import argparse
import http.server
import os
import queue
import select
import shutil
import sqlite3
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace

from test_cli_pty import Model, Terminal, pexpect


class ReflowModel(Model):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        systems = [message.get('content', '') for message in body.get('messages', [])
                   if message.get('role') == 'system']
        recap = any('brief catch-up' in text for text in systems)
        title = any('underlying task' in text for text in systems)
        release = threading.Event()
        if recap:
            Model.requests.put((body, release))
        else:
            if not title:
                self.server.turn_requests.put(body)
            release.answer = '{"title":"Native terminal test"}' if title else 'Main work complete.'
            release.set()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            self.wfile.write(b': waiting\n\n')
            self.wfile.flush()
            if not release.wait(60):
                return
            payload = {'choices': [{'index': 0, 'delta': {
                'content': getattr(release, 'answer', '')}}]}
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
            self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}'
                            b'\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class NativeScreen:
    def __init__(self, kitty):
        script = str(Path(__file__).resolve())
        bootstrap = (f'import runpy,sys; sys.argv={[script, "--native-worker"]!r}; '
                     f'runpy.run_path({script!r}, run_name="__main__")')
        self.process = subprocess.Popen([kitty, '+runpy', bootstrap], text=True,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.states = []
        ready = self.receive()
        if 'skip' in ready:
            self.close()
            raise NativeUnavailable(ready['skip'])
        assert ready.get('ready'), ready
        self.exchange({'op': 'state'})

    def receive(self):
        readable, _, _ = select.select([self.process.stdout], [], [], 10)
        assert readable, 'Kitty parser did not respond'
        line = self.process.stdout.readline()
        assert line, 'Kitty parser exited unexpectedly'
        return json.loads(line)

    def exchange(self, request):
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        self.state = self.receive()
        self.display = self.state['display']
        self.cursor = SimpleNamespace(x=self.state['cursor'][0], y=self.state['cursor'][1])
        self.lines = self.state['lines']
        self.columns = self.state['columns']
        self.mode = {2026 << 5} if self.state['synchronized'] else set()

    def feed(self, data):
        self.exchange({'op': 'feed', 'data': data})

    def resize(self, lines, columns):
        self.exchange({'op': 'resize', 'lines': lines, 'columns': columns})

    def capture(self, label):
        state = dict(self.state, label=label)
        self.states.append(state)
        return state['history'] + state['display']

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write('{"op":"quit"}\n')
                self.process.stdin.flush()
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.stderr.close()


class NativeUnavailable(Exception):
    pass


def retained_history(screen, label, turns=3, logical_rows=None):
    rows = logical_rows if logical_rows is not None else screen.state['history'] + screen.display
    text = ''.join(''.join(rows).split())
    diagnostic = label + '\n' + '\n'.join(f'{i:04}: {row}' for i, row in enumerate(rows))
    assert text.count('Mainworkcomplete.') == turns, diagnostic
    assert text.count('Workedfor') == turns, diagnostic
    for turn in range(1, 4):
        assert text.count(f'Nativeterminaltask{turn}') == 1, diagnostic


def one_composer(terminal, screen, label, draft=None, turns=3, logical_rows=None):
    terminal.snapshot(label)
    rows = screen.capture(label)
    if logical_rows is None:
        text = ''.join(''.join(rows).split())
        if (text.count('Mainworkcomplete.') != turns or
                any(text.count(f'Nativeterminaltask{turn}') != 1
                    for turn in range(1, 4))):
            logical_rows = collect_native_history(terminal, screen, label, draft)
    retained_history(screen, label, turns, logical_rows)
    prompts = [i for i, row in enumerate(rows)
               if row.split('Ctrl+T transcript')[0].strip() in ('>', '›')]
    hints = [i for i, row in enumerate(rows) if 'Ctrl+T transcript' in row]
    footers = [i for i, row in enumerate(rows) if 'perm: ' in row]
    diagnostic = (f'{label}\nprompts={prompts} hints={hints} footers={footers}\n' +
                  '\n'.join(f'{i:04}: {row}' for i, row in enumerate(rows)))
    assert len(footers) == 1, diagnostic
    if draft is None:
        assert len(prompts) == 1, diagnostic
        assert len(hints) == 0, diagnostic
        assert screen.cursor.y == prompts[0] - len(screen.state['history']), diagnostic
    else:
        text = ''.join(''.join(rows).split())
        assert text.count('DRAFT_NATIVE') == 1, diagnostic
        start = next(i for i, row in enumerate(rows) if 'DRAFT_NATIVE' in row)
        visible = ''.join(''.join(rows[start:footers[0] - 1]).split()).lstrip('›')
        expected = ''.join(draft.split())
        assert len(visible) >= min(20, len(expected)), diagnostic
        assert visible in expected, diagnostic
        assert not prompts, diagnostic


def collect_native_history(terminal, screen, label, draft=None):
    """Read the retained conversation, including rows above the viewport."""
    composer = next(i for i, row in enumerate(screen.display)
                    if ('DRAFT_NATIVE' in row if draft is not None
                        else row.strip() in ('>', '›')))
    cursor = (screen.cursor.x, screen.cursor.y)
    history = screen.display[:composer - 2]
    previous = history

    def viewport(stage):
        terminal.snapshot(label + ': ' + stage)
        screen.capture(label + ': ' + stage)
        assert (screen.cursor.x, screen.cursor.y) == cursor, screen.display
        assert (('DRAFT_NATIVE' in screen.display[composer]) if draft is not None
                else screen.display[composer].strip() in ('>', '›')), screen.display
        return screen.display[:composer - 2]

    def prepend(page):
        nonlocal history
        overlap = next((count for count in range(len(page), 0, -1)
                        if page[-count:] == history[:count]), 0)
        assert overlap, 'retained history skipped its overlapping boundary'
        history = page[:-overlap] + history

    for pages in range(20):
        terminal.send('\x1b[5~\x1b[<65;1;1M')
        page = viewport('page up with an overlapping row')
        if page == previous:
            break
        prepend(page)
        previous = page
    else:
        raise AssertionError('retained history never reached its beginning')
    terminal.send('\x1b[<64;1;1M')
    first = viewport('first retained row')
    if first != previous:
        prepend(first)
    terminal.send('\x1b[6~' * (pages + 2))
    viewport('return to the idle composer')
    return history


def wait_until(terminal, predicate):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        terminal.pump(.05)
        if predicate() and 2026 << 5 not in terminal.screen.mode:
            return
    raise AssertionError('\n'.join(terminal.screen.display))


def exercise(driver, kitty, server, case, artifacts, columns_sequence=None):
    with tempfile.TemporaryDirectory(prefix='morph-native-reflow-') as temporary:
        screen = NativeScreen(kitty)
        terminal = Terminal(driver, Path(temporary), server.server_port, hooks_enabled=False)
        terminal.child.setwinsize(32, 200)
        terminal.screen = screen
        terminal.stream = screen
        release = None
        try:
            terminal.wait_for_composer()
            for turn in range(3):
                terminal.send(f'Native terminal task {turn + 1}\r')
                wait_until(terminal, lambda: terminal.raw.count('Worked for ') > turn)
                server.turn_requests.get(timeout=5)
            one_composer(terminal, screen, f'{case}: baseline at 200 columns')
            if case == 'idle':
                # Change persisted metadata without typing a command. The idle
                # host timer must repaint the footer and preserve its live hint.
                with sqlite3.connect(Path(temporary) / 'data.db') as db:
                    session = db.execute('SELECT DISTINCT session_id FROM model_history_items')
                    sessions = session.fetchall()
                    assert len(sessions) == 1, sessions
                    owner = sessions[0][0]
                    changed = db.execute(
                        'UPDATE model_history_items SET token_count=token_count+100 '
                        'WHERE id=(SELECT MAX(id) FROM model_history_items '
                        'WHERE session_id=? AND active=1)', (owner,))
                    assert changed.rowcount == 1
                    tokens = db.execute(
                        'SELECT SUM(token_count) FROM model_history_items '
                        'WHERE session_id=? AND active=1', (owner,)).fetchone()[0]
                terminal.pump(1.1)
                wait_until(terminal, lambda: f'context {tokens} /' in '\n'.join(screen.display))
                one_composer(terminal, screen, 'idle footer metadata refresh preserves hint')
            draft = None
            point = 0
            if case == 'draft':
                draft = 'DRAFT_NATIVE_SINGLE ' + 'alpha beta gamma delta ' * 11 + '中文尾部'
                point = len('DRAFT_NATIVE_SINGLE alpha beta ')
            elif case == 'multiline':
                draft = ('DRAFT_NATIVE_MULTI first line 中文\n' +
                         'middle segment ' * 9 + '\nlast segment 尾部')
                point = draft.index('middle') + len('middle')
            if draft is not None:
                terminal.send('\x1b[200~' + draft + '\x1b[201~')
                terminal.send('\x02' * (len(draft) - point))
                one_composer(terminal, screen, f'{case}: draft cursor positioned in middle', draft)
            if case == 'pending':
                terminal.send('\x1b[O/recap\r')
                _, release = terminal.request()
                one_composer(terminal, screen, 'pending recap before native reflow')
            if columns_sequence is None:
                columns_sequence = ((20, 200, 80, 200, 60, 200) if case == 'draft' else
                                    (40, 200, 80, 200, 60, 200) if case == 'multiline' else
                                    (80, 200, 60, 200, 120, 200))
            for index, columns in enumerate(columns_sequence):
                # Resize the native cell grid before delivering SIGWINCH. The
                # retained view then rewraps its logical text, including history
                # above the visible viewport, before the assertions below.
                screen.resize(32, columns)
                screen.capture(f'{case}: immediate native reflow to {columns}')
                terminal.child.setwinsize(32, columns)
                immediate = draft is not None and index == 0
                if immediate:
                    # Bypass pexpect's send delay and write before pumping any
                    # SIGWINCH output. Input must synchronize its own geometry.
                    assert os.write(terminal.child.child_fd, b'Z') == 1
                    draft = draft[:point] + 'Z' + draft[point:]
                    point += 1
                terminal.pump(.4)
                stage = 'immediate input after resize' if immediate else 'process resized'
                one_composer(terminal, screen, f'{case}: {stage} to {columns}', draft)
                if draft is not None and not immediate:
                    # Inserting at the retained middle cursor makes cursor
                    # preservation observable in the final model request.
                    terminal.send('Z')
                    draft = draft[:point] + 'Z' + draft[point:]
                    point += 1
                    one_composer(terminal, screen, f'{case}: cursor insertion at {columns}', draft)
            if release:
                marker = 'NATIVE_REFLOW_RECAP'
                release.answer = json.dumps({
                    'summary': marker + ' ' + '已完成代码验证，保留用户修改并检查终端输入框。' * 10,
                    'next_action': None,
                }, ensure_ascii=False)
                release.set()
                wait_until(terminal, lambda: marker in terminal.raw and
                           'Generating recap' not in '\n'.join(screen.display))
                # Native viewports retain older rows in their scroll view;
                # a growing recap need not keep every turn physically visible.
                logical_rows = collect_native_history(
                    terminal, screen, 'recap completed after native reflow')
                one_composer(terminal, screen, 'recap completed after native reflow',
                             logical_rows=logical_rows)
                terminal.send('\x1b[I')
                one_composer(terminal, screen, 'focus restored after native reflow',
                             logical_rows=logical_rows)
            if draft is not None:
                terminal.send('\r')
                body = server.turn_requests.get(timeout=5)
                users = [message.get('content') for message in body['messages']
                         if message.get('role') == 'user']
                assert users[-1] == draft, (users[-1], draft)
                wait_until(terminal, lambda: any(row.strip() in ('>', '›')
                                               for row in screen.display) and
                           'Working' not in '\n'.join(screen.display) and
                           'Main work complete.' in '\n'.join(screen.display))
                one_composer(terminal, screen, f'{case}: exact draft submitted after reflow', turns=4)
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=10)
        except Exception as error:
            artifacts.mkdir(parents=True, exist_ok=True)
            stem = artifacts / case
            stem.with_suffix('.raw').write_text(terminal.raw)
            screen.capture('first failed check')
            stem.with_suffix('.states.json').write_text(json.dumps(
                screen.states, ensure_ascii=False, indent=2))
            stem.with_suffix('.screen.txt').write_text(
                str(error) + '\n\n' + '\n'.join(screen.capture('failure')))
            print(f'Native reflow failure artifacts: {stem}', flush=True)
            raise
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)
            screen.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('driver', type=Path)
    parser.add_argument('--kitty', default=shutil.which('kitty'))
    parser.add_argument('--case', action='append', choices=('idle', 'pending', 'draft', 'multiline'))
    parser.add_argument('--columns', type=int, action='append')
    parser.add_argument('--artifacts', type=Path, default=Path('/tmp/morph-native-reflow-artifacts'))
    arguments = parser.parse_args()
    if not arguments.kitty:
        print('Native terminal reflow requires an installed Kitty; skipping.')
        return 77
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ReflowModel)
    server.turn_requests = queue.Queue()
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for case in arguments.case or ('idle', 'pending', 'draft', 'multiline'):
            exercise(arguments.driver.resolve(), arguments.kitty, server,
                     case, arguments.artifacts, arguments.columns)
    except NativeUnavailable as error:
        print(f'Native terminal reflow unavailable: {error}')
        return 77
    finally:
        server.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
