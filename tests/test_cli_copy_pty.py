"""Copy exact answers through the production command and clipboard adapters."""
import base64
import http.server
from loopback_http import LoopbackHTTPServer
import os
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_resume_history_pty import seed
from test_cli_transcript_pty import AlternateScreen
import pexpect
import pyte


def frame(terminal, label):
    deadline = time.monotonic() + 5
    while not any(row.rstrip() == '/copy' for row in terminal.screen.display):
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        terminal.pump(.03)
    screen = terminal.snapshot(label)
    rows = terminal.screen.display
    composers = [index for index, row in enumerate(rows)
                 if row.split('Ctrl+T transcript')[0].strip() == '›']
    assert len(composers) == 1, screen
    composer = composers[0]
    heading = max(index for index, row in enumerate(rows) if row.rstrip() == '/copy')
    assert terminal.screen.buffer[heading][0].fg == 'af87ff', screen
    assert not rows[heading + 1].strip(), screen
    assert not rows[composer - 2].strip(), screen
    for row in range(heading, composer - 1):
        assert all(terminal.screen.buffer[row][column].bg != '3a3a3a'
                   for column in range(terminal.screen.columns)), screen
    assert '› /copy' not in screen, screen
    return screen


def resume(terminal, session):
    before = len(terminal.raw)
    terminal.send('/resume ' + session + '\r')
    deadline = time.monotonic() + 8
    marker = 'HISTORY_LAST_QUESTION' if session == '1234abcd' else 'LEGACY_ANSWER'
    while marker not in terminal.raw[before:]:
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        terminal.pump(.03)
    terminal.snapshot('restored session ' + session)


def main():
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    answer = '# Final answer\n\n中文🙂  \n\n```sh\necho "$HOME"  \n```\n'
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-copy-pty-') as temp:
                directory = Path(temp)
                binaries = directory / 'bin'
                binaries.mkdir()
                copied = directory / 'clipboard.txt'
                failed = directory / 'clipboard-fails'
                for backend in ('pbcopy', 'wl-copy', 'xclip', 'xsel'):
                    program = binaries / backend
                    program.write_text('#!/bin/sh\n'
                                       '[ ! -f "$MORPH_PTY_COPY_FAIL" ] || exit 1\n'
                                       '/bin/cat > "$MORPH_PTY_COPY_FILE"\n')
                    program.chmod(0o755)
                terminal = Terminal(binary, directory, server.server_port, production=True,
                                    hooks_enabled=False, extra_env={
                                        'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
                                        'MORPH_PTY_COPY_FILE': str(copied),
                                        'MORPH_PTY_COPY_FAIL': str(failed), 'NO_COLOR': '',
                                        'TMUX': '', 'SSH_CONNECTION': '', 'SSH_TTY': '',
                                        'SSH_CLIENT': ''})
                terminal.child.setwinsize(28, columns)
                terminal.screen = AlternateScreen(columns, 28)
                terminal.stream = pyte.Stream(terminal.screen)
                release = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('/copy\r')
                    screen = frame(terminal, f'{columns}: empty-session copy')
                    assert 'no assistant answer' in screen and not copied.exists(), screen
                    database = directory / 'home' / '.morph' / 'data.db'
                    native, legacy = seed(database)
                    with sqlite3.connect(database) as db:
                        db.execute("UPDATE model_history_items SET content=? WHERE session_id=? "
                                   "AND kind='assistant_message' AND active=1", (answer, native))
                        db.execute("UPDATE messages SET content=? WHERE session_id=? "
                                   "AND role='assistant' AND turn_id='recent-turn'", (answer, native))
                    resume(terminal, '1234abcd')
                    terminal.send('/copy\r')
                    screen = frame(terminal, f'{columns}: resumed answer copied as Markdown')
                    assert 'copied last answer to clipboard' in screen, screen
                    assert copied.read_bytes() == answer.encode(), copied.read_bytes()
                    terminal.send('/cop\t')
                    assert '/copy' in terminal.snapshot(f'{columns}: copy completion')
                    terminal.send('\r')
                    frame(terminal, f'{columns}: completed command copies again')
                    assert copied.read_bytes() == answer.encode()
                    terminal.send('/new copy-empty\r')
                    terminal.send('/copy\r')
                    assert 'no assistant answer' in frame(terminal, f'{columns}: session isolation')
                    assert copied.read_bytes() == answer.encode()
                    resume(terminal, '8765fedc')
                    terminal.send('/copy\r')
                    frame(terminal, f'{columns}: legacy session copy')
                    assert copied.read_bytes() == b'**LEGACY_ANSWER**'
                    failed.touch()
                    before = len(terminal.raw)
                    terminal.send('/copy\r')
                    screen = frame(terminal, f'{columns}: terminal fallback is unconfirmed')
                    assert 'Copy unconfirmed' in screen, screen
                    payloads = re.findall(r'\x1b\]52;c;([^\x07]+)\x07', terminal.raw[before:])
                    assert len(payloads) == 1, terminal.raw[before:]
                    assert base64.b64decode(payloads[0]) == b'**LEGACY_ANSWER**'
                    with sqlite3.connect(database) as db:
                        updated = db.execute("UPDATE messages SET content=? WHERE session_id=? "
                                             "AND role='assistant'", ('x' * 100001, legacy))
                        assert updated.rowcount == 1, updated.rowcount
                    before = len(terminal.raw)
                    terminal.send('/copy\r')
                    screen = frame(terminal, f'{columns}: oversized fallback fails without truncation')
                    assert 'copy failed' in screen, screen
                    assert '\x1b]52;' not in terminal.raw[before:], terminal.raw[before:]
                    assert copied.read_bytes() == b'**LEGACY_ANSWER**'
                    with sqlite3.connect(database) as db:
                        updated = db.execute("UPDATE messages SET content=? WHERE session_id=? "
                                             "AND role='assistant'", ('**LEGACY_ANSWER**', legacy))
                        assert updated.rowcount == 1, updated.rowcount
                    failed.unlink()
                    terminal.send('Provide the next answer\r')
                    _, release = terminal.request()
                    terminal.send('/copy\r')
                    assert 'wait for the active turn' in terminal.snapshot(f'{columns}: busy copy stays local')
                    release.delta = {'content': '\nLIVE_FINAL_ANSWER\n\n```text\nnew code  \n```\n'}
                    release.set()
                    deadline = time.monotonic() + 8
                    while 'LIVE_FINAL_ANSWER' not in terminal.raw:
                        assert time.monotonic() < deadline, terminal.raw[-5000:]
                        terminal.pump(.1)
                    terminal.pump(.6)
                    terminal.send('\x15/copy\r')
                    frame(terminal, f'{columns}: latest generated answer replaces the old copy')
                    with sqlite3.connect(database) as db:
                        latest = db.execute("SELECT content FROM messages WHERE session_id=? "
                                            "AND role='assistant' ORDER BY id DESC LIMIT 1",
                                            (legacy,)).fetchone()[0]
                    assert 'LIVE_FINAL_ANSWER' in latest, latest
                    assert copied.read_bytes() == latest.encode(), copied.read_bytes()
                    terminal.send('/quit\r')
                    terminal.child.expect(pexpect.EOF, timeout=10)
                    terminal.child.close()
                    assert terminal.child.exitstatus == 0
                finally:
                    if release:
                        release.set()
                    terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
