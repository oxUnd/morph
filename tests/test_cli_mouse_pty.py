"""Select visible retained text and copy through production clipboard adapters."""
import http.server
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time

import pexpect
import pyte

from test_cli_pty import Model, Terminal
from test_cli_resume_history_pty import seed
from test_cli_transcript_pty import AlternateScreen


def mouse(terminal, button, column, row, release=False):
    terminal.send(f'\x1b[<{button};{column + 1};{row + 1}{"m" if release else "M"}')


def find(terminal, text):
    rows = terminal.screen.display
    row = next(index for index, line in enumerate(rows) if text in line)
    return rows[row].index(text), row


def main():
    binary = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-mouse-pty-') as temp:
                directory = Path(temp)
                binaries = directory / 'bin'
                binaries.mkdir()
                copied = directory / 'clipboard.txt'
                for backend in ('pbcopy', 'wl-copy', 'xclip', 'xsel'):
                    program = binaries / backend
                    program.write_text('#!/bin/sh\n/bin/cat > "$MORPH_PTY_COPY_FILE"\n')
                    program.chmod(0o755)
                terminal = Terminal(binary, directory, server.server_port, production=True,
                                    hooks_enabled=False, extra_env={
                                        'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
                                        'MORPH_PTY_COPY_FILE': str(copied), 'NO_COLOR': '',
                                        'TMUX': '', 'SSH_CONNECTION': '', 'SSH_TTY': '',
                                        'SSH_CLIENT': ''})
                terminal.child.setwinsize(28, columns)
                terminal.screen = AlternateScreen(columns, 28)
                terminal.stream = pyte.Stream(terminal.screen)
                try:
                    terminal.wait_for_composer()
                    database = directory / 'home' / '.morph' / 'data.db'
                    _, legacy = seed(database)
                    answer = 'COPY_中文 é END\nSECOND_COPY_LINE'
                    with sqlite3.connect(database) as db:
                        db.execute("UPDATE messages SET content=? WHERE session_id=? "
                                   "AND role='assistant'", (answer, legacy))
                    terminal.send('/resume 8765fedc\r')
                    terminal.send('UNSENT_DRAFT_中文')
                    screen = terminal.snapshot(f'{columns}: draft before selection')
                    assert 'COPY_中文 é END' in screen, screen
                    assert '\x1b[?1002h' in terminal.raw, terminal.raw[-5000:]
                    x1, y1 = find(terminal, 'COPY_')
                    x2, y2 = find(terminal, 'SECOND_COPY_LINE')
                    x2 += len('SECOND_COPY_LINE') - 1
                    composer_y = next(index for index, row in enumerate(terminal.screen.display)
                                      if '› UNSENT_DRAFT_中文' in row)
                    mouse(terminal, 2, x1, y1)
                    assert not copied.exists(), 'right-click without selection copied text'
                    mouse(terminal, 0, x1, y1)
                    mouse(terminal, 32, x2, y2)
                    mouse(terminal, 0, x2, y2, release=True)
                    screen = terminal.snapshot(f'{columns}: selected Unicode and multiple rows')
                    assert terminal.screen.buffer[y1][x1].reverse, screen
                    assert '› UNSENT_DRAFT_中文' in terminal.screen.display[composer_y], screen
                    assert terminal.screen.cursor.y == composer_y, screen
                    mouse(terminal, 2, x2, y2)
                    deadline = time.monotonic() + 6
                    while not copied.exists() or not copied.stat().st_size:
                        assert time.monotonic() < deadline, terminal.snapshot('copy timeout')
                        terminal.pump(.05)
                    content = copied.read_text()
                    assert content.splitlines()[0] == 'COPY_中文 é END', repr(content)
                    assert content.splitlines()[1].strip() == 'SECOND_COPY_LINE', repr(content)
                    assert '\x1b' not in content and not content.endswith(' '), repr(content)
                    assert not Model.requests.qsize(), 'mouse input unexpectedly submitted draft'
                    mouse(terminal, 2, x1, y1)
                    assert copied.read_text() == content
                    terminal.send('X')
                    screen = terminal.snapshot(f'{columns}: typing clears selection')
                    assert not terminal.screen.buffer[y1][x1].reverse, screen
                    assert '› UNSENT_DRAFT_中文X' in screen, screen
                    copied.unlink()
                    mouse(terminal, 2, x1, y1)
                    assert not copied.exists(), 'typing left a stale clipboard selection'
                    mouse(terminal, 0, x1, y1)
                    mouse(terminal, 32, x2, y2)
                    mouse(terminal, 0, x2, y2, release=True)
                    terminal.child.setwinsize(28, 80 if columns != 80 else 60)
                    terminal.pump(.6)
                    mouse(terminal, 2, x1, y1)
                    assert not copied.exists(), 'resize left a stale clipboard selection'
                    terminal.send('\x15/quit\r')
                    terminal.child.expect(pexpect.EOF, timeout=10)
                    terminal.child.close()
                    assert terminal.child.exitstatus == 0, terminal.raw[-4000:]
                finally:
                    terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
