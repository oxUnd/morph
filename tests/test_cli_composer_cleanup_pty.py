"""Accepted lines must not paint another editable prompt into terminal history."""
import http.server
import json
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Terminal, pexpect, pyte
from test_cli_transcript_pty import AlternateScreen


class ComposerModel(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        title = any('underlying task' in item.get('content', '')
                    for item in body.get('messages', []) if item.get('role') == 'system')
        if title:
            self.server.title_started.set()
            self.server.title_release.wait(8)
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
    history = getattr(terminal.screen, 'history', None)
    rows = [''.join(cell.data for _, cell in sorted(row.items()))
            for row in history.top] if history is not None else []
    rows += terminal.screen.display
    return [row for row in rows if row.split('Ctrl+T transcript')[0].strip() in ('>', '›')]


class ComposerFrameStream:
    """Check the resulting terminal cells at every synchronized publication."""

    def __init__(self, terminal, color):
        self.terminal = terminal
        self.color = color
        self.stream = pyte.Stream(terminal.screen)
        self.pending = ''
        self.frames = 0
        self.submissions = 0

    def check(self):
        screen = self.terminal.screen
        rows = screen.display
        footers = [index for index, row in enumerate(rows) if 'perm: ' in row]
        if not footers:
            assert not self.frames or not hasattr(screen, '_main_screen'), '\n'.join(rows)
            return
        self.frames += 1
        diagnostic = (f'frame {self.frames}, cursor={screen.cursor.y},{screen.cursor.x}\n'
                      + '\n'.join(rows))
        assert len(footers) == 1, diagnostic
        # A submitted user bubble uses the same marker as the editable composer.
        # Cursor ownership distinguishes them, even while typing the same text.
        composer = screen.cursor.y
        assert rows[composer].startswith(('› ', '> ')), diagnostic
        prompts = [index for index, row in enumerate(rows)
                   if row.startswith(('› ', '> '))]
        assert composer in prompts, diagnostic
        assert len(prompts) <= self.submissions + 1, diagnostic
        for index in prompts:
            if index != composer:
                assert rows[index].strip() in ('› hello world', '> hello world'), diagnostic
        assert len(empty_prompts(self.terminal)) <= 1, diagnostic
        assert footers[0] >= composer + 2, diagnostic
        assert not rows[composer - 1].strip(), diagnostic
        assert not rows[composer + 1].strip(), diagnostic
        # Every colored band must belong to the current composer or an actual
        # submitted message. Extra blank prompt/padding bands are orphan UI.
        painted = {row for prompt in prompts for row in (prompt - 1, prompt, prompt + 1)
                   if 0 <= row < screen.lines}
        for row in range(screen.lines):
            backgrounds = [screen.buffer[row][column].bg for column in range(screen.columns)]
            if self.color and row in painted:
                assert all(background != 'default' for background in backgrounds), diagnostic
            else:
                assert all(background == 'default' for background in backgrounds), diagnostic

    def feed(self, text):
        self.pending += text
        ending = '\x1b[?2026l'
        while ending in self.pending:
            frame, self.pending = self.pending.split(ending, 1)
            self.stream.feed(frame + ending)
            self.check()
        if '\x1b[?2026h' not in self.pending:
            split = self.pending.rfind('\x1b')
            if split < 0 or self.pending[split:] not in (
                    '\x1b', '\x1b[', '\x1b[?', '\x1b[?2', '\x1b[?20',
                    '\x1b[?202', '\x1b[?2026'):
                self.stream.feed(self.pending)
                self.pending = ''


def restored_composer_snapshot(terminal, label):
    # Output completion and composer restoration can arrive in separate
    # frames. Wait for a live composer; the stream checks every published frame
    # for duplicate prompts and orphan backgrounds before snapshots settle.
    deadline = time.monotonic() + 8
    while True:
        text = terminal.snapshot(label)
        if any(row.split('Ctrl+T transcript')[0].strip() in ('>', '›')
               for row in terminal.screen.display):
            return text
        assert time.monotonic() < deadline, terminal.screen.display


def main():
    binary = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ComposerModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for color in (True, False):
            for width in (120, 80, 60):
                with tempfile.TemporaryDirectory(prefix='morph-composer-cleanup-') as temp:
                    server.title_started = threading.Event()
                    server.title_release = threading.Event()
                    terminal = Terminal(binary, Path(temp), server.server_port,
                                        hooks_enabled=False, production=True,
                                        extra_env={'NO_COLOR': '' if color else '1'})
                    terminal.child.setwinsize(24, width)
                    terminal.screen = AlternateScreen(width, 24)
                    terminal.stream = ComposerFrameStream(terminal, color)
                    try:
                        terminal.wait_for_composer()
                        for turn in range(2):
                            terminal.send('hello world')
                            submitted = len(terminal.raw)
                            terminal.stream.submissions += 1
                            terminal.send('\r')
                            deadline = time.monotonic() + 8
                            while terminal.raw.count('Worked for ') < turn + 1:
                                assert time.monotonic() < deadline, terminal.raw[-4000:]
                                terminal.pump(.05)
                            # The duration row can arrive before the frame's
                            # composer redraw and synchronized-update end marker.
                            restored_composer_snapshot(
                                terminal, f'completed turn {turn + 1} at {width} columns')
                            output = terminal.raw[submitted:]
                            thought = output.index('The user says')
                            committed = output.rfind('hello world', 0, thought)
                            assert committed >= 0, repr(output)
                            assert 'Ctrl+T transcript' not in output[:committed], repr(output)
                            assert len(empty_prompts(terminal)) == 1, terminal.screen.display
                            rows = terminal.screen.display
                            question = max(i for i, row in enumerate(rows)
                                            if row.startswith(('› hello world', '> hello world')))
                            reasoning = max(i for i, row in enumerate(rows) if 'The user says' in row)
                            bottom = question + (1 if color else 0)
                            assert reasoning == bottom + 2, '\n'.join(rows)
                            assert not rows[bottom + 1].strip(), '\n'.join(rows)
                            assert all(terminal.screen.buffer[bottom + 1][column].bg != '3a3a3a'
                                       for column in range(terminal.screen.columns)), '\n'.join(rows)
                            if turn == 0:
                                # Complete metadata after the turn is already idle.
                                assert server.title_started.is_set(), terminal.raw[-4000:]
                                terminal.pump(1.1)
                                server.title_release.set()
                                deadline = time.monotonic() + 8
                                while 'Naming session' in '\n'.join(terminal.screen.display):
                                    assert time.monotonic() < deadline, terminal.screen.display
                                    terminal.pump(.05)
                                restored_composer_snapshot(
                                    terminal, f'metadata completion at {width} columns')
                                assert len(empty_prompts(terminal)) == 1, terminal.screen.display
                                # A window notification restores one composer per frame.
                                start = len(terminal.raw)
                                frame_count = terminal.stream.frames
                                terminal.screen.resize(lines=24, columns=width + 4)
                                terminal.child.setwinsize(24, width + 4)
                                terminal.pump(.3)
                                restored_composer_snapshot(
                                    terminal, f'resize restoration at {width + 4} columns')
                                frames = re.findall(r'\x1b\[\?2026h(.*?)\x1b\[\?2026l',
                                                    terminal.raw[start:], re.S)
                                assert frames, repr(terminal.raw[start:])
                                assert terminal.stream.frames > frame_count
                                assert len(empty_prompts(terminal)) == 1, terminal.screen.display
                                terminal.send('\r\x1b[O\x1b[I')
                                terminal.pump(1.1)
                                restored_composer_snapshot(
                                    terminal, f'focus restoration at {width + 4} columns')
                                assert len(empty_prompts(terminal)) == 1, terminal.screen.display
                        with sqlite3.connect(Path(temp) / 'home/.morph/data.db') as database:
                            assert database.execute(
                                "SELECT COUNT(*) FROM messages WHERE role='user'"
                            ).fetchone()[0] == 2
                        terminal.send('/q')
                        start = len(terminal.raw)
                        terminal.send('\r')
                        terminal.child.expect(pexpect.EOF, timeout=8)
                        if terminal.child.before:
                            terminal.raw += terminal.child.before
                            terminal.stream.feed(terminal.child.before)
                        assert 'Ctrl+T transcript' not in terminal.raw[start:], repr(
                            terminal.raw[start:])
                        assert not empty_prompts(terminal), terminal.screen.display
                        print(f'checked: {width} columns, color={color}, no orphan composer')
                    finally:
                        server.title_release.set()
                        terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
