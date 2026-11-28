"""Pin the browsed turn's question through scroll, restore, resize and panels."""
from pathlib import Path
import json
import re
import sqlite3
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal, pexpect
from test_cli_agents_pty import wait_for
from test_cli_subagents_pty import FeedModel


FIRST = 'FIRST_QUESTION 中文：检查最早的问题'
SECOND = 'SECOND_QUESTION 中文🙂 ' + 'long question ' * 12 + '\nsecond question continuation'
THIRD = 'THIRD_QUESTION 中文：最后一个问题'


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def pinned(terminal):
    return terminal.screen.display[0].startswith(('› ', '> ', '» '))


def stop(terminal):
    terminal.send('\x15/quit\r')
    terminal.child.expect(pexpect.EOF, timeout=10)
    terminal.child.close()
    assert terminal.child.exitstatus == 0


def seed(database):
    with sqlite3.connect(database) as db:
        session, reference = db.execute('SELECT id,display_id FROM sessions').fetchone()
        records = [
            ('first', 'user', FIRST),
            ('first', 'assistant', '\n'.join(f'FIRST_ROW_{i:03}' for i in range(80))),
            ('first', 'user', 'STEER_MUST_NOT_PIN'),
            ('first', 'assistant', '\n'.join(f'FIRST_AFTER_STEER_{i:03}' for i in range(30))),
            ('second', 'user', SECOND),
            ('second', 'assistant', '\n'.join(f'SECOND_ROW_{i:03}' for i in range(70))),
            ('third', 'user', THIRD),
            ('third', 'assistant', '\n'.join(f'THIRD_ROW_{i:03}' for i in range(5))),
        ]
        for sequence, (turn, role, content) in enumerate(records, 1):
            db.execute('INSERT INTO messages(session_id,role,content,turn_id,token_count,'
                       'created_at) VALUES(?,?,?,?,?,?)',
                       (session, role, content, turn, 4, sequence))
            db.execute('INSERT INTO model_history_items(session_id,sequence_no,turn_id,kind,'
                       'role,content,payload_json,active,created_at) VALUES(?,?,?,?,?,?,?,?,?)',
                       (session, sequence, turn, role + '_message', role, content,
                        json.dumps({'input_source': 'user'}) if role == 'user' else None,
                        1, sequence))
        return reference


def automatic_question(binary):
    server = LoopbackHTTPServer(('127.0.0.1', 0), FeedModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    reply = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-first-question-') as temporary:
            terminal = Terminal(binary, Path(temporary), server.server_port,
                                production=True, hooks_enabled=False, extra_env={'NO_COLOR': ''})
            try:
                terminal.wait_for_composer()
                terminal.send('AUTOMATIC_FIRST_QUESTION\r')
                _, reply = terminal.request()
                assert visible(terminal).count('AUTOMATIC_FIRST_QUESTION') == 1
                assert not pinned(terminal), visible(terminal)
                original = next(index for index, row in enumerate(terminal.screen.display)
                                if row.startswith('› AUTOMATIC_FIRST_QUESTION'))
                original_background = [terminal.screen.buffer[original][column].bg
                                       for column in range(terminal.screen.columns)]
                reply.stream({'content': '\n'.join(f'LIVE_ROW_{row:03}' for row in range(70))})
                wait_for(terminal, lambda: 'LIVE_ROW_069' in visible(terminal) and
                         terminal.screen.display[0].rstrip() == '› AUTOMATIC_FIRST_QUESTION')
                assert visible(terminal).count('AUTOMATIC_FIRST_QUESTION') == 1
                pinned_background = [terminal.screen.buffer[0][column].bg
                                     for column in range(terminal.screen.columns)]
                assert pinned_background == original_background, (
                    original_background, pinned_background, visible(terminal))
                assert all(terminal.screen.buffer[0][column].bg == '3a3a3a'
                           for column in range(terminal.screen.columns - 1))
                assert 'Question:' not in terminal.screen.display[0]
                terminal.send('PRESERVED_DRAFT')
                reply.delta = {'content': '\nLIVE_FINISHED'}
                reply.set()
                wait_for(terminal, lambda: 'Worked for' in visible(terminal))
                assert 'PRESERVED_DRAFT' in visible(terminal)
                stop(terminal)
                print('PASS: source question yields to a pinned header while streaming', flush=True)
            finally:
                if reply:
                    reply.set()
                terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


def run(binary, columns, color):
    with tempfile.TemporaryDirectory(prefix='morph-turn-question-') as temporary:
        directory = Path(temporary)
        options = dict(production=True, hooks_enabled=False,
                       extra_env={'NO_COLOR': '' if color else '1'})
        terminal = Terminal(binary, directory, 1, **options)
        try:
            terminal.wait_for_composer()
            assert not pinned(terminal)
            terminal.send('/new Pinned\r')
            wait_for(terminal, lambda: 'created and switched to session: Pinned' in visible(terminal))
            stop(terminal)
            reference = seed(directory / 'home/.morph/data.db')
            terminal = Terminal(binary, directory, 1, **options)
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(lines=24, columns=columns)
            terminal.wait_for_composer()
            terminal.send(f'/resume {reference}\r')
            wait_for(terminal, lambda: 'THIRD_ROW_004' in visible(terminal) and
                     not pinned(terminal))
            assert visible(terminal).count(THIRD) == 1, visible(terminal)
            assert 'THIRD_ROW_004' in visible(terminal)
            terminal.send('PINNED_DRAFT')
            caret = terminal.screen.cursor.x, terminal.screen.cursor.y
            seen = set()
            saw_steer = False
            wheeled_header = False
            for _ in range(30):
                terminal.send('\x1b[5~')
                rows = terminal.screen.display
                header = rows[0]
                if 'SECOND_QUESTION' in header:
                    seen.add('second')
                    assert '…' in header and 'second question continuation' not in header, header
                    assert header.startswith(('› ', '> ')), header
                    if color:
                        assert all(terminal.screen.buffer[0][column].bg == '3a3a3a'
                                   for column in range(columns - 1)), visible(terminal)
                    if not wheeled_header:
                        before = min(map(int, re.findall(r'SECOND_ROW_(\d+)', visible(terminal))))
                        terminal.send('\x1b[<64;1;1M')
                        after = min(map(int, re.findall(r'SECOND_ROW_(\d+)', visible(terminal))))
                        assert after == before - 1, visible(terminal)
                        wheeled_header = True
                if 'FIRST_AFTER_STEER' in '\n'.join(rows[1:]):
                    saw_steer = True
                    assert 'FIRST_QUESTION' in header, visible(terminal)
                if 'FIRST_ROW_000' in '\n'.join(rows[1:]):
                    seen.add('first')
                    assert visible(terminal).count(FIRST) == 1, visible(terminal)
                    break
                assert 'STEER_MUST_NOT_PIN' not in header
                assert (terminal.screen.cursor.x, terminal.screen.cursor.y) == caret
                assert 'PINNED_DRAFT' in rows[caret[1]] and 'perm: ' in rows[-1]
            assert seen == {'first', 'second'} and saw_steer and wheeled_header, (
                seen, saw_steer, wheeled_header, visible(terminal))
            terminal.send('\x1b')
            wait_for(terminal, lambda: THIRD in visible(terminal) and
                     not pinned(terminal) and
                     'Back to bottom' not in visible(terminal))
            assert (terminal.screen.cursor.x, terminal.screen.cursor.y) == caret
            terminal.child.setwinsize(4, columns)
            terminal.screen.resize(lines=4, columns=columns)
            terminal.pump(.3)
            assert 'perm: ' in terminal.screen.display[-1], visible(terminal)
            assert 'PINNED_DRAFT' in terminal.screen.display[terminal.screen.cursor.y]
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(lines=24, columns=columns)
            wait_for(terminal, lambda: THIRD in visible(terminal) and
                     not pinned(terminal))
            terminal.send('\x15/permissions\r')
            wait_for(terminal, lambda: 'Update Model Permissions' in visible(terminal))
            assert visible(terminal).count(THIRD) == 1 and 'perm: ' in terminal.screen.display[-1]
            terminal.send('\x1b')
            terminal.send('/new Empty\r')
            wait_for(terminal, lambda: 'created and switched to session: Empty' in visible(terminal))
            assert not pinned(terminal), visible(terminal)
            terminal.send(f'/resume {reference}\r')
            wait_for(terminal, lambda: THIRD in visible(terminal) and
                     not pinned(terminal))
            if not color:
                assert all(cell.fg == 'default' and cell.bg == 'default'
                           for row in terminal.screen.buffer.values() for cell in row.values())
            stop(terminal)
            print(f'PASS: pinned turn questions at {columns} columns, color={color}', flush=True)
        finally:
            terminal.child.close(force=True)


if __name__ == '__main__':
    binary = Path(sys.argv[1]).resolve()
    automatic_question(binary)
    for columns in (120, 80, 60):
        for color in (True, False):
            run(binary, columns, color)
