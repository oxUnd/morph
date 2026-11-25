"""Select visible retained text and copy through production clipboard adapters."""
from loopback_http import LoopbackHTTPServer
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
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-mouse-pty-') as temp:
                directory = Path(temp)
                binaries = directory / 'bin'
                binaries.mkdir()
                copied = directory / 'clipboard.txt'
                paste_source = directory / 'paste.txt'
                for backend in ('pbcopy', 'wl-copy', 'xclip', 'xsel'):
                    program = binaries / backend
                    program.write_text('#!/bin/sh\n/bin/cat > "$MORPH_PTY_COPY_FILE"\n')
                    program.chmod(0o755)
                for backend in ('pbpaste', 'wl-paste', 'xclip', 'xsel'):
                    program = binaries / backend
                    program.write_text(
                        '#!/bin/sh\ncase "$*" in *--input*|*" -i"*) '
                        '/bin/cat > "$MORPH_PTY_COPY_FILE";; '
                        '*) /bin/cat "$MORPH_PTY_PASTE_FILE";; esac\n')
                    program.chmod(0o755)
                terminal = Terminal(binary, directory, server.server_port, production=True,
                                    hooks_enabled=False, extra_env={
                                        'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
                                        'MORPH_PTY_COPY_FILE': str(copied),
                                        'MORPH_PTY_PASTE_FILE': str(paste_source), 'NO_COLOR': '',
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
                    assert '\x1b[?1003h' in terminal.raw, terminal.raw[-5000:]
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
                    deadline = time.monotonic() + 6
                    while not copied.exists() or not copied.stat().st_size:
                        assert time.monotonic() < deadline, terminal.snapshot('selection copy timeout')
                        terminal.pump(.05)
                    assert copied.read_text().startswith('COPY_中文 é END\n')
                    mouse(terminal, 2, x2, y2)
                    deadline = time.monotonic() + 6
                    while not copied.exists() or not copied.stat().st_size:
                        assert time.monotonic() < deadline, terminal.snapshot('copy timeout')
                        terminal.pump(.05)
                    content = copied.read_text()
                    assert content.splitlines()[0] == 'COPY_中文 é END', repr(content)
                    assert content.splitlines()[1].strip() == 'SECOND_COPY_LINE', repr(content)
                    assert '\x1b' not in content and not content.endswith(' '), repr(content)
                    assert not terminal.screen.buffer[y1][x1].reverse, (
                        'right-click copy must dismiss the native selection')
                    assert not Model.requests.qsize(), 'mouse input unexpectedly submitted draft'
                    mouse(terminal, 2, x1, y1)
                    assert copied.read_text() == content
                    mouse(terminal, 0, x1, y1)
                    mouse(terminal, 32, x2, y2)
                    mouse(terminal, 0, x2, y2, release=True)
                    # Kitty sends standalone modifier events before the copy chord.
                    for modifier in range(57441, 57455):
                        terminal.send(f'\x1b[{modifier};9:1u'
                                      f'\x1b[{modifier};9:2u'
                                      f'\x1b[{modifier};1:3u')
                    screen = terminal.snapshot(f'{columns}: modifier events preserve selection')
                    assert terminal.screen.buffer[y1][x1].reverse, screen
                    for key in ('\x1b[99;9u', '\x1b[67;6u', '\x1b[2;5~', '\x03'):
                        copied.unlink()
                        terminal.send(key)
                        deadline = time.monotonic() + 6
                        while not copied.exists() or not copied.stat().st_size:
                            assert time.monotonic() < deadline, terminal.snapshot('key copy timeout')
                            terminal.pump(.05)
                        assert copied.read_text() == content
                        screen = terminal.snapshot(f'{columns}: keyboard copy preserves draft')
                        assert 'UNSENT_DRAFT_中文' in screen, screen
                        assert terminal.screen.buffer[y1][x1].reverse, screen
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
                    copied.unlink()
                    resized_columns = 80 if columns != 80 else 60
                    terminal.child.setwinsize(28, resized_columns)
                    terminal.screen.resize(28, resized_columns)
                    terminal.pump(.6)
                    mouse(terminal, 2, x1, y1)
                    assert not copied.exists(), 'resize left a stale clipboard selection'
                    terminal.send('\x03')
                    screen = terminal.snapshot('Ctrl+C without selection clears the draft')
                    assert 'UNSENT_DRAFT' not in screen, screen
                    paste_source.write_text('PASTED_中文🙂\nsecond line\x1b[31mRED\x1b[0m\x03')
                    keys = ('\x16', '\x1b[118;9u', '\x1b[86;6u', '\x1b[2;2~',
                            '\x1b[27;6;118~')
                    undo_keys = ('\x1a', '\x1b[122;9u', '\x1f')
                    for index, key in enumerate(keys):
                        terminal.send('\x15prefix \x01' + key)
                        screen = terminal.snapshot(f'{columns}: Unicode clipboard paste')
                        assert 'PASTED_中文🙂' in screen, screen
                        assert 'second lineRED\\x03prefix ' in screen, screen
                        assert Model.requests.empty(), 'clipboard paste submitted the draft'
                        terminal.send(undo_keys[index % len(undo_keys)])
                        screen = terminal.snapshot('Undo restores the draft before paste')
                        assert 'PASTED_中文🙂' not in screen and 'prefix ' in screen, screen
                    terminal.send('\x15\x1b[200~TERMINAL_PASTE\n中文\x1b[201~')
                    screen = terminal.snapshot('terminal-managed paste remains supported')
                    assert 'TERMINAL_PASTE' in screen and '中文' in screen, screen
                    assert Model.requests.empty(), 'terminal paste submitted the draft'
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
